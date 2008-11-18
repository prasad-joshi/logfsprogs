/*
 * LogFS mkfs
 *
 * Copyright (c) 2007-2008 Joern Engel <joern@logfs.org>
 *
 * License: GPL version 2
 */

#define __CHECK_ENDIAN__
#define _LARGEFILE64_SOURCE
#define __USE_FILE_OFFSET64
#include <asm/types.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/fs.h>
#include <linux/types.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <zlib.h>

#include "kerncompat.h"
#include <mtd/mtd-abi.h>
#include "logfs_abi.h"
#include "logfs.h"

enum {
	OFS_SB = 0,
	OFS_JOURNAL = 1,
	OFS_ROOTDIR = 3,
	OFS_COUNT
};

static u64 no_segs;

static unsigned user_segshift = -1;
static unsigned user_blockshift = -1;
static unsigned user_writeshift = -1;

static unsigned segshift = 17;
static unsigned blockshift = 12;
static unsigned writeshift = 0;

static u64 wbuf_ofs;

static u64 segment_offset[OFS_COUNT];

static __be32 bb_array[1024];
static int bb_count;

static u16 version;

/* journal entries */
static __be64 je_array[64];
static int no_je;

/* commandline options */
static int compress_rootdir;
static int quick_bad_block_scan;
static int interactice_mode = 1;

////////////////////////////////////////////////////////////////////////////////

static int logfs_compress(void *in, void *out, size_t inlen, size_t outlen)
{
	struct z_stream_s stream;
	int err, ret;

	ret = -EIO;
	memset(&stream, 0, sizeof(stream));
	err = deflateInit(&stream, 3);
	if (err != Z_OK)
		goto error;

	stream.next_in = in;
	stream.avail_in = inlen;
	stream.total_in = 0;
	stream.next_out = out;
	stream.avail_out = outlen;
	stream.total_out = 0;

	err = deflate(&stream, Z_FINISH);
	if (err != Z_STREAM_END)
		goto error;

	err = deflateEnd(&stream);
	if (err != Z_OK)
	goto error;

	if (stream.total_out >= stream.total_in)
		goto error;

	ret = stream.total_out;
error:
	return ret;
}


////////////////////////////////////////////////////////////////////////////////


static int mtd_erase(struct super_block *sb, u64 ofs, size_t size)
{
	struct erase_info_user ei;

	ei.start = ofs;
	ei.length = size;

	return ioctl(sb->fd, MEMERASE, &ei);
}

/*
 * The superblock doesn't have to be segment-aligned.  So simply search the
 * first non-bad eraseblock, completely ignoring segment size.
 */
static s64 mtd_scan_super(struct super_block *sb)
{
	s64 ofs, sb_ofs;
	int err;

	for (ofs = 0; ofs < sb->fssize; ofs += sb->erasesize) {
		printf("\r%lld%% done. ", 100*ofs / sb->fssize);
		err = mtd_erase(sb, ofs, sb->erasesize);
		if (err) {
			printf("Bad block at 0x%llx\n", ofs);
			if ((ofs & (sb->segsize-1)) == 0) {
				/* bad segment */
				if (bb_count > 512)
					return -EIO;
				bb_array[bb_count++] = cpu_to_be32(ofs >> segshift);
			}
		} else {
			if (ofs)
				printf("Superblock is at %llx\n", ofs);
			/* first non-bad block */
			sb_ofs = ofs;
			/* erase remaining blocks in segment */
			for (; ofs & (sb->segsize-1); ofs+=sb->erasesize)
				mtd_erase(sb, ofs, sb->erasesize);
			return sb_ofs;
		}
	}
	/* all bad */
	return -EIO;
}

static int bdev_write(struct super_block *sb, u64 ofs, size_t size, void *buf)
{
	ssize_t ret;

	lseek64(sb->fd, ofs, SEEK_SET);
	ret = write(sb->fd, buf, size);
	if (ret != size)
		return -EIO;
	return 0;
}

static int bdev_erase(struct super_block *sb, u64 ofs, size_t size)
{
	if (!sb->erase_buf) {
		sb->erase_buf = malloc(sb->segsize);
		if (!sb->erase_buf)
			fail("out of memory");
		memset(sb->erase_buf, 0xff, sb->segsize);
	}

	return bdev_write(sb, ofs, size, sb->erase_buf);
}

static s64 bdev_scan_super(struct super_block *sb)
{
	return bdev_erase(sb, 0, sb->segsize);
}

static const struct logfs_device_operations mtd_ops = {
	.write = bdev_write,
	.erase = mtd_erase,
	.scan_super = mtd_scan_super,
};

static const struct logfs_device_operations bdev_ops = {
	.write = bdev_write,
	.erase = bdev_erase,
	.scan_super = bdev_scan_super,
};


////////////////////////////////////////////////////////////////////////////////


/* root inode */

static int buf_write(struct super_block *sb, u64 ofs, void *data, size_t len)
{
	size_t space = len & ~(sb->writesize - 1);
	int err;

	if (space) {
		err = sb->dev_ops->write(sb, ofs, space, data);
		if (err)
			return err;
		ofs += space;
		data += space;
		len -= space;
	}

	if (len)
		memcpy(sb->wbuf, data, len);
	wbuf_ofs = ofs + len;
	return 0;
}

static void set_segment_header(struct logfs_segment_header *sh, u8 type,
		u8 level, u32 segno)
{
	sh->pad = 0;
	sh->type = type;
	sh->level = level;
	sh->segno = cpu_to_be32(segno);
	sh->ec = 0;
	sh->gec = cpu_to_be64(1);
	sh->crc = logfs_crc32(sh, LOGFS_SEGMENT_HEADERSIZE, 4);
}

static int make_rootdir(struct super_block *sb)
{
	struct logfs_segment_header *sh;
	struct logfs_object_header *oh;
	struct logfs_disk_inode *di;
	size_t size = sb->blocksize + LOGFS_OBJECT_HEADERSIZE + LOGFS_SEGMENT_HEADERSIZE;
	int ret;

	sh = calloc(size, 1);
	if (!sh)
		return -ENOMEM;

	oh = (void*)(sh+1);
	di = (void*)(oh+1);

	set_segment_header(sh, SEG_OSTORE, LOGFS_MAX_LEVELS,
			segment_offset[OFS_ROOTDIR] >> segshift);

	di->di_flags	= 0;
	if (compress_rootdir)
		di->di_flags |= cpu_to_be32(LOGFS_IF_COMPRESSED);
	di->di_mode	= cpu_to_be16(S_IFDIR | 0755);
	di->di_refcount	= cpu_to_be32(2);

	oh->len = cpu_to_be16(sb->blocksize);
	oh->type = OBJ_BLOCK;
	oh->compr = COMPR_NONE;
	oh->ino = cpu_to_be64(LOGFS_INO_MASTER);
	oh->bix = cpu_to_be64(LOGFS_INO_ROOT);
	oh->crc = logfs_crc32(oh, LOGFS_OBJECT_HEADERSIZE - 4, 4);
	oh->data_crc = logfs_crc32(di, sb->blocksize, 0);

	ret = buf_write(sb, segment_offset[OFS_ROOTDIR], sh, size);
	free(sh);
	return ret;
}

/* journal */

static size_t __write_header(struct logfs_journal_header *jh, size_t len,
		size_t datalen, u16 type, u8 compr)
{
	jh->h_len	= cpu_to_be16(len);
	jh->h_type	= cpu_to_be16(type);
	jh->h_version	= cpu_to_be16(++version);
	jh->h_datalen	= cpu_to_be16(datalen);
	jh->h_compr	= compr;
	jh->h_pad[0]	= 'h';
	jh->h_pad[1]	= 'a';
	jh->h_pad[2]	= 't';
	jh->h_crc	= logfs_crc32(jh, len + sizeof(*jh), 4);
	return ALIGN(len, 16) + sizeof(*jh);
}

static size_t write_header(struct logfs_journal_header *h, size_t datalen,
		u16 type)
{
	return __write_header(h, datalen, datalen, type, COMPR_NONE);
}

static size_t je_wbuf(struct super_block *sb, void *data, u16 *type)
{
	memcpy(data, sb->wbuf, sb->writesize);
	*type = JEG_WBUF + LOGFS_MAX_LEVELS; /* inode level */
	return sb->writesize;
}

static size_t je_badsegments(struct super_block *sb, void *data, u16 *type)
{
	memcpy(data, bb_array, sb->blocksize);
	*type = JE_BADSEGMENTS;
	return sb->blocksize;
}

static size_t je_anchor(struct super_block *sb, void *_da, u16 *type)
{
	struct logfs_je_anchor *da = _da;

	memset(da, 0, sizeof(*da));
	da->da_last_ino	= cpu_to_be64(LOGFS_RESERVED_INOS);
	da->da_size	= cpu_to_be64((LOGFS_INO_ROOT+1) * sb->blocksize);
	da->da_used_bytes = cpu_to_be64(sb->blocksize + LOGFS_OBJECT_HEADERSIZE);
	da->da_data[LOGFS_INO_ROOT] = cpu_to_be64(segment_offset[OFS_ROOTDIR]
				+ LOGFS_SEGMENT_HEADERSIZE);
	*type = JE_ANCHOR;
	return sizeof(*da);
}

static size_t je_dynsb(struct super_block *sb, void *_dynsb, u16 *type)
{
	struct logfs_je_dynsb *dynsb = _dynsb;

	memset(dynsb, 0, sizeof(*dynsb));
	dynsb->ds_used_bytes	= cpu_to_be64(sb->blocksize + LOGFS_OBJECT_HEADERSIZE);
	*type = JE_DYNSB;
	return sizeof(*dynsb);
}

static size_t je_areas(struct super_block *sb, void *_a, u16 *type)
{
	struct logfs_je_areas *a = _a;
	int l = LOGFS_MAX_LEVELS;

	memset(a, 0, sizeof(*a));
	a->used_bytes[l] = cpu_to_be32(wbuf_ofs - segment_offset[OFS_ROOTDIR]);
	a->segno[l] = cpu_to_be32(segment_offset[OFS_ROOTDIR] >> segshift);
	*type = JE_AREAS;
	return sizeof(*a);
}

static size_t je_commit(struct super_block *sb, void *h, u16 *type)
{
	*type = JE_COMMIT;
	memcpy(h, je_array, no_je * sizeof(__be64));
	return no_je * sizeof(__be64);
}

static size_t write_je(struct super_block *sb,
		size_t jpos, void *scratch, void *header,
		size_t (*write)(struct super_block *sb, void *scratch,
			u16 *type))
{
	void *data;
	ssize_t len, max, compr_len, pad_len;
	u16 type;
	u8 compr = COMPR_ZLIB;

	/* carefule: segment_offset[OFS_JOURNAL] must match make_journal() */
	header += jpos;
	data = header + sizeof(struct logfs_journal_header);

	len = write(sb, scratch, &type);
	if (type != JE_COMMIT)
		je_array[no_je++] = cpu_to_be64(segment_offset[OFS_JOURNAL] + jpos);
	if (len == 0)
		return write_header(header, 0, type);

	max = sb->blocksize - jpos;
	compr_len = logfs_compress(scratch, data, len, max);
	if ((compr_len < 0) || (type == JE_COMMIT)) {
		BUG_ON(len > max);
		memcpy(data, scratch, len);
		compr_len = len;
		compr = COMPR_NONE;
	}

	pad_len = ALIGN(compr_len, 16);
	memset(data + compr_len, 0, pad_len - compr_len);

	return __write_header(header, compr_len, len, type, compr);
}

static int make_journal(struct super_block *sb)
{
	void *journal, *scratch;
	size_t jpos;
	int ret;

	journal = calloc(2*sb->blocksize, 1);
	if (!journal)
		return -ENOMEM;

	scratch = journal + sb->blocksize;

	set_segment_header(journal, SEG_JOURNAL, 0,
			segment_offset[OFS_JOURNAL] >> segshift);
	jpos = ALIGN(sizeof(struct logfs_segment_header), 16);
	/* erasecount is not written - implicitly set to 0 */
	/* neither are summary, index, wbuf */
	if (sb->writesize > 1)
		jpos += write_je(sb, jpos, scratch, journal, je_wbuf);
	jpos += write_je(sb, jpos, scratch, journal, je_badsegments);
	jpos += write_je(sb, jpos, scratch, journal, je_anchor);
	jpos += write_je(sb, jpos, scratch, journal, je_dynsb);
	jpos += write_je(sb, jpos, scratch, journal, je_areas);
	jpos += write_je(sb, jpos, scratch, journal, je_commit);
	ret = sb->dev_ops->write(sb, segment_offset[OFS_JOURNAL], sb->blocksize, journal);
	free(journal);
	return ret;
}

/* superblock */

static int make_super(struct super_block *sb)
{
	struct logfs_disk_super _ds, *ds = &_ds;
	void *sector;
	int secsize = ALIGN(sizeof(*ds), sb->writesize);
	int ret;

	sector = calloc(secsize, 1);
	if (!sector)
		return -ENOMEM;

	memset(ds, 0, sizeof(*ds));
	set_segment_header((void *)ds, SEG_SUPER, 0, 0);

	ds->ds_magic		= cpu_to_be64(LOGFS_MAGIC);
	ds->ds_ifile_levels	= 3; /* 2+1, 1GiB */
	ds->ds_iblock_levels	= 4; /* 3+1, 512GiB */
	ds->ds_data_levels	= 1; /* old, young, unknown */

	ds->ds_feature_incompat	= 0;
	ds->ds_feature_ro_compat= 0;

	ds->ds_feature_compat	= 0;
	ds->ds_flags		= 0;

	ds->ds_filesystem_size	= cpu_to_be64(sb->fssize);
	ds->ds_segment_shift	= segshift;
	ds->ds_block_shift	= blockshift;
	ds->ds_write_shift	= writeshift;

	ds->ds_journal_seg[0]	= cpu_to_be64(1);
	ds->ds_journal_seg[1]	= cpu_to_be64(2);
	ds->ds_journal_seg[2]	= 0;
	ds->ds_journal_seg[3]	= 0;

	ds->ds_root_reserve	= 0;

	ds->ds_crc = logfs_crc32(ds, sizeof(*ds), LOGFS_SEGMENT_HEADERSIZE + 12);

	memcpy(sector, ds, sizeof(*ds));
	ret = sb->dev_ops->write(sb, segment_offset[OFS_SB], secsize, sector);
	free(sector);
	return ret;
}

/* main stuff */

static int bad_block_scan(struct super_block *sb)
{
	int seg, err;
	s64 ofs, sb_ofs;

	seg = 0;
	bb_count = 0;
	sb_ofs = sb->dev_ops->scan_super(sb);
	if (sb_ofs < 0)
		return -EIO;

	segment_offset[seg++] = sb_ofs;
	for (ofs = ALIGN(sb_ofs+1, sb->segsize); ofs < sb->fssize; ofs += sb->segsize) {
		printf("\r%lld%% done. ", 100*ofs / sb->fssize);
		err = sb->dev_ops->erase(sb, ofs, sb->segsize);
		if (err) {
			/* bad segment */
			if (bb_count > 512)
				return -EIO;
			bb_array[bb_count++] = cpu_to_be32(ofs >> segshift);
			printf("Bad block at 0x%llx\n", ofs);
		} else {
			/* good segment */
			if (seg < OFS_COUNT)
				segment_offset[seg++] = ofs;
			else if (quick_bad_block_scan)
				return 0;
		}
	}
	return 0;
}

static void mkfs(struct super_block *sb)
{
	char answer[4096]; /* I don't care about overflows */
	int ret;

	if (user_segshift + 1)
		segshift = user_segshift;
	if (user_blockshift + 1)
		blockshift = user_blockshift;
	if (user_writeshift + 1)
		writeshift = user_writeshift;

	if (segshift > 30)
		fail("segment shift too large (max 30)");
	if (segshift <= blockshift)
		fail("segment shift must be larger than block shift");
	if (blockshift != 12)
		fail("blockshift must be 12");
	if (writeshift >= 12)
		fail("writeshift too large (max 12)");
	sb->segsize = 1 << segshift;
	sb->blocksize = 1 << blockshift;
	sb->writesize = 1 << writeshift;

	no_segs = sb->fssize >> segshift;
	sb->fssize = no_segs << segshift;

	printf("Will create filesystem with the following details:\n");
	printf("              hex:   decimal:\n");
	printf("fssize=   %8llx %10lld\n", sb->fssize, sb->fssize);
	printf("segsize=  %8x %10d\n", sb->segsize, sb->segsize);
	printf("blocksize=%8x %10d\n", sb->blocksize, sb->blocksize);
	printf("writesize=%8x %10d\n", sb->writesize, sb->writesize);
	printf("\n");

	if (interactice_mode) {
		printf("Do you wish to continue (yes/no)\n");
		scanf("%s", answer);
		if (strcmp(answer, "yes"))
			fail("aborting...");
	}
	if (quick_bad_block_scan) {
		printf("mklogfs won't erase filesystem.  This may oops your kernel.\n");
		scanf("%s", answer);
		if (strcmp(answer, "yes"))
			fail("aborting...");
	}

	ret = bad_block_scan(sb);
	if (ret)
		fail("bad block scan failed");

	ret = make_rootdir(sb);
	if (ret)
		fail("could not create root inode");

	ret = make_journal(sb);
	if (ret)
		fail("could not create journal");

	ret = make_super(sb);
	if (ret)
		fail("could not create superblock");

	fsync(sb->fd);
	printf("\nFinished generating LogFS\n");
}

static struct super_block *__open_device(const char *name)
{
	struct super_block *sb;
	const struct logfs_device_operations *ops = &bdev_ops;
	struct mtd_info_user mtd;
	struct stat stat;
	int err;

	sb = zalloc(sizeof(*sb));
	sb->fd = open(name, O_WRONLY);
	if (sb->fd == -1)
		fail("could not open device");

	err = fstat(sb->fd, &stat);
	if (err)
		fail("could not stat device");

	switch (stat.st_mode & S_IFMT) {
	case S_IFSOCK:
	case S_IFLNK:
	case S_IFDIR:
	case S_IFIFO:
		fail("wrong device type");
	case S_IFCHR:
		if (major(stat.st_rdev) != 90)
			fail("non-mtd character device");
		ops = &mtd_ops;
		err = ioctl(sb->fd, MEMGETINFO, &mtd);
		if (err)
			fail("mtd ioctl failed");

		sb->erasesize = mtd.erasesize;
		segshift = ffs(mtd.erasesize) - 1;
		if (mtd.erasesize != 1 << segshift)
			fail("device erasesize must be a power of 2");

		writeshift = ffs(mtd.writesize) - 1;
		if (mtd.writesize != 1 << writeshift)
			fail("device writesize must be a power of 2");

		sb->fssize = mtd.size;
		break;
	case S_IFREG:
		sb->fssize = stat.st_size;
		break;
	case S_IFBLK:
		err = ioctl(sb->fd, BLKGETSIZE64, &sb->fssize);
		if (err)
			fail("block ioctl failed");
		break;
	}

	sb->wbuf = malloc(sb->writesize);
	if (!sb->wbuf)
		fail("out of memory");

	sb->dev_ops = ops;
	return sb;
}

static void usage(void)
{
	printf(
"mklogfs <options> <device>\n"
"\n"
"Options:\n"
"  -c --compress        turn compression on\n"
"  -h --help            display this help\n"
"  -s --segshift        segment shift in bits\n"
"  -w --writeshift      write shift in bits\n"
"     --demo-mode	skip bad block scan; don't erase device\n"
"     --non-interactive turn off safety question before writing\n"
"\n"
"Segment size and write size are powers of two.  To specify them, the\n"
"appropriate power is specified with the \"-s\" or \"-w\" options, instead\n"
"of the actual size.  E.g. \"mklogfs -w8\" will set a writesize\n"
"of 256 Bytes (2^8).\n\n");
}

int main(int argc, char **argv)
{
	struct super_block *sb;

	check_crc32();
	for (;;) {
		int oi = 1;
		char short_opts[] = "chs:w:";
		static const struct option long_opts[] = {
			{"compress",		0, NULL, 'c'},
			{"help",		0, NULL, 'h'},
			{"non-interactive",	0, NULL, 'n'},
			{"demo-mode",		0, NULL, 'q'},
			{"segshift",		1, NULL, 's'},
			{"writeshift",		1, NULL, 'w'},
			{ }
		};
		int c = getopt_long(argc, argv, short_opts, long_opts, &oi);
		if (c == -1)
			break;
		switch (c) {
		case 'b':
			user_blockshift = strtoul(optarg, NULL, 0);
			break;
		case 'c':
			compress_rootdir = 1;
			break;
		case 'h':
			usage();
			exit(EXIT_SUCCESS);
		case 'n':
			interactice_mode = 0;
			break;
		case 'q':
			quick_bad_block_scan = 1;
			break;
		case 's':
			user_segshift = strtoul(optarg, NULL, 0);
			break;
		case 'w':
			user_writeshift = strtoul(optarg, NULL, 0);
			break;
		default:
			fail("unknown option\n");
		}
	}

	if (optind != argc - 1) {
		usage();
		exit(EXIT_FAILURE);
	}

	sb = __open_device(argv[optind]);
	mkfs(sb);

	return 0;
}
