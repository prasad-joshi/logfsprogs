/*
 * LogFS mkfs
 *
 * Copyright (c) 2007 Joern Engel <joern@logfs.org>
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

enum {
	OFS_SB = 0,
	OFS_JOURNAL = 1,
	OFS_ROOTDIR = 3,
	OFS_COUNT
};

static u64 fssize;
static u64 no_segs;

static u32 segsize;
static u32 erasesize;
static u32 blocksize;
static u32 writesize;

static unsigned user_segshift = -1;
static unsigned user_blockshift = -1;
static unsigned user_writeshift = -1;

static unsigned segshift = 17;
static unsigned blockshift = 12;
static unsigned writeshift = 0;

static void *erase_buf;
static void *wbuf;
static u64 wbuf_ofs;

static u64 segment_offset[OFS_COUNT];

static __be32 bb_array[1024];
static int bb_count;

static u16 version;

static int compress_rootdir;
static int quick_bad_block_scan;

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


static inline __be32 logfs_crc32(void *data, size_t len, size_t skip)
{
	return cpu_to_be32(~crc32(0, data+skip, len-skip));
}


////////////////////////////////////////////////////////////////////////////////


struct logfs_device_operations {
	int (*write)(int fd, u64 ofs, size_t size, void *buf);
	int (*erase)(int fd, u64 ofs, size_t size);
};

static int mtd_erase(int fd, u64 ofs, size_t size)
{
	struct erase_info_user ei;

	ei.start = ofs;
	ei.length = size;

	return ioctl(fd, MEMERASE, &ei);
}

static int bdev_write(int fd, u64 ofs, size_t size, void *buf)
{
	ssize_t ret;

	lseek64(fd, ofs, SEEK_SET);
	ret = write(fd, buf, size);
	if (ret != size)
		return -EIO;
	return 0;
}

static int bdev_erase(int fd, u64 ofs, size_t size)
{
	return bdev_write(fd, ofs, size, erase_buf);
}

static const struct logfs_device_operations mtd_ops = {
	.write = bdev_write,
	.erase = mtd_erase,
};

static const struct logfs_device_operations bdev_ops = {
	.write = bdev_write,
	.erase = bdev_erase,
};


////////////////////////////////////////////////////////////////////////////////


static void fail(const char *s) __attribute__ ((__noreturn__));
static void fail(const char *s)
{
	printf("mklogfs: %s\n", s);
	exit(EXIT_FAILURE);
}

/* root inode */

static int buf_write(int fd, const struct logfs_device_operations *ops,
		u64 ofs, void *data, size_t len)
{
	size_t space = len & ~(writesize - 1);
	int err;

	if (space) {
		err = ops->write(fd, ofs, space, data);
		if (err)
			return err;
		ofs += space;
		data += space;
		len -= space;
	}

	if (len)
		memcpy(wbuf, data, len);
	wbuf_ofs = ofs + len;
	return 0;
}

static int make_rootdir(int fd, const struct logfs_device_operations *ops)
{
	struct logfs_segment_header *sh;
	struct logfs_object_header *oh;
	struct logfs_disk_inode *di;
	size_t size = blocksize + LOGFS_HEADERSIZE + LOGFS_SEGMENT_HEADERSIZE;
	int ret;

	sh = calloc(size, 1);
	if (!sh)
		return -ENOMEM;

	oh = (void*)(sh+1);
	di = (void*)(oh+1);

	sh->pad = 0;
	sh->type = OBJ_OSTORE;
	sh->level = LOGFS_MAX_LEVELS;
	sh->segno = cpu_to_be32(segment_offset[OFS_ROOTDIR] >> segshift);
	sh->ec = 0;
	sh->gec = 0;
	sh->crc = logfs_crc32(sh, LOGFS_SEGMENT_HEADERSIZE, 4);

	di->di_flags	= cpu_to_be32(LOGFS_IF_VALID);
	if (compress_rootdir)
		di->di_flags |= cpu_to_be32(LOGFS_IF_COMPRESSED);
	di->di_mode	= cpu_to_be16(S_IFDIR | 0755);
	di->di_refcount	= cpu_to_be32(2);

	oh->len = cpu_to_be16(blocksize);
	oh->type = OBJ_BLOCK;
	oh->compr = COMPR_NONE;
	oh->ino = cpu_to_be64(LOGFS_INO_MASTER);
	oh->bix = cpu_to_be64(LOGFS_INO_ROOT);
	oh->crc = logfs_crc32(oh, LOGFS_HEADERSIZE - 4, 4);
	oh->data_crc = logfs_crc32(di, blocksize, 0);

	ret = buf_write(fd, ops, segment_offset[OFS_ROOTDIR], sh, size);
	free(sh);
	return ret;
}

/* journal */

static size_t __write_header(struct logfs_journal_header *h, size_t len,
		size_t datalen, u16 type, u8 compr)
{
	h->h_len	= cpu_to_be16(len);
	h->h_type	= cpu_to_be16(type);
	h->h_version	= cpu_to_be16(++version);
	h->h_datalen	= cpu_to_be16(datalen);
	h->h_compr	= compr;
	h->h_pad[0]	= 'h';
	h->h_pad[1]	= 'a';
	h->h_pad[2]	= 't';
	h->h_crc	= logfs_crc32(h, len, 4);
	return len;
}

static size_t write_header(struct logfs_journal_header *h, size_t datalen,
		u16 type)
{
	size_t len = datalen + sizeof(*h);
	return __write_header(h, len, datalen, type, COMPR_NONE);
}

static size_t je_wbuf(void *data, u16 *type)
{
	memcpy(data, wbuf, writesize);
	*type = JEG_WBUF + LOGFS_MAX_LEVELS; /* inode level */
	return writesize;
}

static size_t je_badsegments(void *data, u16 *type)
{
	memcpy(data, bb_array, blocksize);
	*type = JE_BADSEGMENTS;
	return blocksize;
}

static size_t je_anchor(void *_da, u16 *type)
{
	struct logfs_je_anchor *da = _da;

	memset(da, 0, sizeof(*da));
	da->da_last_ino	= cpu_to_be64(LOGFS_RESERVED_INOS);
	da->da_size	= cpu_to_be64((LOGFS_INO_ROOT+1) * blocksize);
	da->da_used_bytes = cpu_to_be64(blocksize + LOGFS_HEADERSIZE);
	da->da_data[LOGFS_INO_ROOT] = cpu_to_be64(segment_offset[OFS_ROOTDIR]
				+ LOGFS_SEGMENT_HEADERSIZE);
	*type = JE_ANCHOR;
	return sizeof(*da);
}

static size_t je_dynsb(void *_dynsb, u16 *type)
{
	struct logfs_je_dynsb *dynsb = _dynsb;

	memset(dynsb, 0, sizeof(*dynsb));
	dynsb->ds_used_bytes	= cpu_to_be64(blocksize + LOGFS_HEADERSIZE);
	*type = JE_DYNSB;
	return sizeof(*dynsb);
}

static size_t je_areas(void *_a, u16 *type)
{
	struct logfs_je_areas *a = _a;
	int l = LOGFS_MAX_LEVELS;

	memset(a, 0, sizeof(*a));
	a->used_bytes[l] = cpu_to_be32(wbuf_ofs - segment_offset[OFS_ROOTDIR]);
	a->segno[l] = cpu_to_be32(segment_offset[OFS_ROOTDIR] >> segshift);
	*type = JE_AREAS;
	return sizeof(*a);
}

static size_t je_commit(void *h, u16 *type)
{
	*type = JE_COMMIT;
	return 0;
}

static size_t write_je(size_t jpos, void *scratch, void *header,
		size_t (*write)(void *scratch, u16 *type))
{
	void *data;
	ssize_t len, max, compr_len, pad_len, full_len;
	u16 type;
	u8 compr = COMPR_ZLIB;

	header += jpos;
	data = header + sizeof(struct logfs_journal_header);

	len = write(scratch, &type);
	if (len == 0)
		return write_header(header, 0, type);

	max = blocksize - jpos;
	compr_len = logfs_compress(scratch, data, len, max);
	if ((compr_len < 0) || (type == JE_ANCHOR)) {
		BUG_ON(len > max);
		memcpy(data, scratch, len);
		compr_len = len;
		compr = COMPR_NONE;
	}

	pad_len = ALIGN(compr_len, 16);
	memset(data + compr_len, 0, pad_len - compr_len);
	full_len = pad_len + sizeof(struct logfs_journal_header);

	return __write_header(header, full_len, len, type, compr);
}

static int make_journal(int fd, const struct logfs_device_operations *ops)
{
	void *journal, *scratch;
	size_t jpos;
	int ret;

	journal = calloc(2*blocksize, 1);
	if (!journal)
		return -ENOMEM;

	scratch = journal + blocksize;

	jpos = 0;
	/* erasecount is not written - implicitly set to 0 */
	/* neither are summary, index, wbuf */
	if (writesize > 1)
		jpos += write_je(jpos, scratch, journal, je_wbuf);
	jpos += write_je(jpos, scratch, journal, je_badsegments);
	jpos += write_je(jpos, scratch, journal, je_anchor);
	jpos += write_je(jpos, scratch, journal, je_dynsb);
	jpos += write_je(jpos, scratch, journal, je_areas);
	jpos += write_je(jpos, scratch, journal, je_commit);
	ret = ops->write(fd, segment_offset[OFS_JOURNAL], blocksize, journal);
	free(journal);
	return ret;
}

/* superblock */

static int make_super(int fd, const struct logfs_device_operations *ops)
{
	struct logfs_disk_super _ds, *ds = &_ds;
	void *sector;
	int secsize = ALIGN(sizeof(*ds), writesize);
	int ret;

	sector = calloc(secsize, 1);
	if (!sector)
		return -ENOMEM;

	memset(ds, 0, sizeof(*ds));

	ds->ds_magic		= cpu_to_be64(LOGFS_MAGIC);
	ds->ds_ifile_levels	= 3; /* 2+1, 1GiB */
	ds->ds_iblock_levels	= 4; /* 3+1, 512GiB */
	ds->ds_data_levels	= 1; /* old, young, unknown */

	ds->ds_feature_incompat	= 0;
	ds->ds_feature_ro_compat= 0;

	ds->ds_feature_compat	= 0;
	ds->ds_flags		= 0;

	ds->ds_filesystem_size	= cpu_to_be64(fssize);
	ds->ds_segment_shift	= segshift;
	ds->ds_block_shift	= blockshift;
	ds->ds_write_shift	= writeshift;

	ds->ds_journal_seg[0]	= cpu_to_be64(1);
	ds->ds_journal_seg[1]	= cpu_to_be64(2);
	ds->ds_journal_seg[2]	= 0;
	ds->ds_journal_seg[3]	= 0;

	ds->ds_root_reserve	= 0;

	ds->ds_crc = logfs_crc32(ds, sizeof(*ds), 12);

	memcpy(sector, ds, sizeof(*ds));
	ret = ops->write(fd, segment_offset[OFS_SB], secsize, sector);
	free(sector);
	return ret;
}

/* main stuff */

/*
 * The superblock doesn't have to be segment-aligned.  So simply search the
 * first non-bad eraseblock, completely ignoring segment size.
 */
static s64 superblock_scan(int fd, const struct logfs_device_operations *ops)
{
	s64 ofs, sb_ofs;
	int err;

	if (ops != &mtd_ops) {
		sb_ofs = 0;
		return 0;
	}

	for (ofs=0; ofs<fssize; ofs+=erasesize) {
		printf("\r%lld%% done. ", 100*ofs / fssize);
		err = ops->erase(fd, ofs, erasesize);
		if (err) {
			printf("Bad block at 0x%llx\n", ofs);
			if ((ofs & (segsize-1)) == 0) {
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
			for (; ofs & (segsize-1); ofs+=erasesize)
				ops->erase(fd, ofs, erasesize);
			return sb_ofs;
		}
	}
	/* all bad */
	return -EIO;
}

static int bad_block_scan(int fd, const struct logfs_device_operations *ops)
{
	int seg, err;
	s64 ofs, sb_ofs;

	seg = 0;
	bb_count = 0;
	sb_ofs = superblock_scan(fd, ops);
	if (sb_ofs < 0)
		return -EIO;

	segment_offset[seg++] = sb_ofs;
	for (ofs=ALIGN(sb_ofs+1, segsize); ofs<fssize; ofs+=segsize) {
		printf("\r%lld%% done. ", 100*ofs / fssize);
		err = ops->erase(fd, ofs, segsize);
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

static void mkfs(int fd, const struct logfs_device_operations *ops)
{
	char answer[4096]; /* I don't care about overflows */
	int ret;

	if (user_segshift + 1)
		segshift = user_segshift;
	if (user_blockshift + 1)
		segshift = user_blockshift;
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
	segsize = 1 << segshift;
	blocksize = 1 << blockshift;
	writesize = 1 << writeshift;

	no_segs = fssize >> segshift;
	fssize = no_segs << segshift;

	printf("Will create filesystem with the following details:\n");
	printf("              hex:   decimal:\n");
	printf("fssize=   %8llx %10lld\n", fssize, fssize);
	printf("segsize=  %8x %10d\n", segsize, segsize);
	printf("blocksize=%8x %10d\n", blocksize, blocksize);
	printf("writesize=%8x %10d\n", writesize, writesize);
	printf("\n");
	printf("Do you wish to continue (yes/no)\n");

	scanf("%s", answer);
	if (strcmp(answer, "yes"))
		fail("aborting...");

	wbuf = malloc(writesize);
	if (!wbuf)
		fail("out of memory");

	erase_buf = malloc(segsize);
	if (!erase_buf)
		fail("out of memory");
	memset(erase_buf, 0xff, segsize);

	ret = bad_block_scan(fd, ops);
	if (ret)
		fail("bad block scan failed");

	ret = make_rootdir(fd, ops);
	if (ret)
		fail("could not create root inode");

	ret = make_journal(fd, ops);
	if (ret)
		fail("could not create journal");

	ret = make_super(fd, ops);
	if (ret)
		fail("could not create superblock");

	fsync(fd);
	printf("\nFinished generating LogFS\n");
}

static void open_device(const char *name)
{
	const struct logfs_device_operations *ops = &bdev_ops;
	struct mtd_info_user mtd;
	struct stat stat;
	int fd, err;

	fd = open(name, O_WRONLY);
	if (fd == -1)
		fail("could not open device");

	err = fstat(fd, &stat);
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
		err = ioctl(fd, MEMGETINFO, &mtd);
		if (err)
			fail("mtd ioctl failed");

		erasesize = mtd.erasesize;
		segshift = ffs(mtd.erasesize) - 1;
		if (mtd.erasesize != 1 << segshift)
			fail("device erasesize must be a power of 2");

		writeshift = ffs(mtd.writesize) - 1;
		if (mtd.writesize != 1 << writeshift)
			fail("device writesize must be a power of 2");

		fssize = mtd.size;
		break;
	case S_IFREG:
		fssize = stat.st_size;
		break;
	case S_IFBLK:
		err = ioctl(fd, BLKGETSIZE64, &fssize);
		if (err)
			fail("block ioctl failed");
		break;
	}

	mkfs(fd, ops);
}

static void usage(void)
{
	printf(
"mklogfs <options> <device>\n"
"\n"
"Options:\n"
"  -c --compress        turn compression on\n"
"  -h --help            display this help\n"
"  -q --quick		skip bad block scan\n"
"  -s --segshift        segment shift in bits\n"
"  -w --writeshift      write shift in bits\n"
"\n"
"Segment size and write size are powers of two.  To specify them, the\n"
"appropriate power is specified with the \"-s\" or \"-w\" options, instead\n"
"of the actual size.  E.g. \"mklogfs -w8\" will set a writesize\n"
"of 256 Bytes (2^8).\n\n");
}

/*
 * zlib crc32 differs from the kernel variant.  zlib negated both the initial
 * value and the result bitwise.  So for the kernel ~0 is a correct initial
 * value, for zlib 0 is.
 * Better check for such funnies instead of generating bad images.
 */
static void check_crc32(void)
{
	u32 c=0;
	if (logfs_crc32(&c, 4, 0) != cpu_to_be32(0xdebb20e3))
		fail("crc32 returns bad results");
}

int main(int argc, char **argv)
{
	check_crc32();
	for (;;) {
		int oi = 1;
		char short_opts[] = "chqs:w:";
		static const struct option long_opts[] = {
			{"compress",	0, NULL, 'c'},
			{"help",	0, NULL, 'h'},
			{"quick",	0, NULL, 'q'},
			{"segshift",	1, NULL, 's'},
			{"writeshift",	1, NULL, 'w'},
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

	open_device(argv[optind]);

	return 0;
}
