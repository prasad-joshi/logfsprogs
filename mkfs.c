/*
 * LogFS mkfs
 *
 * Copyright (c) 2007-2008 Joern Engel <joern@logfs.org>
 *
 * License: GPL version 2
 */
#define _LARGEFILE64_SOURCE
#define __USE_FILE_OFFSET64
#include <asm/types.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <libgen.h>
#include <linux/fs.h>
#include <linux/types.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#define __USE_UNIX98
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

static unsigned user_segshift = -1;
static unsigned user_blockshift = -1;
static unsigned user_writeshift = -1;

static u8 segshift = 18;
static u8 blockshift = 12;
static u8 writeshift = 0;
static u32 no_journal_segs = 4;
static u32 bad_seg_reserve = 4;

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
	if (ofs >= 0x100000000ull) {
		struct erase_info_user64 ei;

		ei.start = ofs;
		ei.length = size;

		return ioctl(sb->fd, MEMERASE64, &ei);
	} else {
		struct erase_info_user ei;

		ei.start = ofs;
		ei.length = size;

		return ioctl(sb->fd, MEMERASE, &ei);
	}
}

static int mtd_prepare_sb(struct super_block *sb)
{
	u32 segno;
	int err;

	/* 1st superblock at the beginning */
	segno = get_segment(sb);
	sb->segment_entry[segno].ec_level = ec_level(1, 0);
	sb->segment_entry[segno].valid = cpu_to_be32(RESERVED);
	sb->sb_ofs1 = (u64)segno * sb->segsize;

	/* 2nd superblock at the end */
	for (segno = sb->no_segs - 1; segno > sb->no_segs - 64; segno--) {
		err = mtd_erase(sb, (u64)segno * sb->segsize, sb->segsize);
		if (err)
			continue;
		sb->segment_entry[segno].ec_level = ec_level(1, 0);
		sb->segment_entry[segno].valid = cpu_to_be32(RESERVED);
		sb->sb_ofs2 = (u64)(segno + 1) * sb->segsize - 0x1000;
		break;
	}
	if (segno == sb->no_segs - 64 || sb->sb_ofs2 <= sb->sb_ofs1)
		return -EIO;
	return 0;
}

static int bdev_prepare_sb(struct super_block *sb)
{
	u32 segno;

	/* 1st superblock at the beginning */
	segno = get_segment(sb);
	sb->segment_entry[segno].ec_level = ec_level(1, 0);
	sb->segment_entry[segno].valid = cpu_to_be32(RESERVED);
	sb->sb_ofs1 = (u64)segno * sb->segsize;

	/* 2nd superblock at the end */
	segno = sb->no_segs - 1;
	sb->segment_entry[segno].ec_level = ec_level(1, 0);
	sb->segment_entry[segno].valid = cpu_to_be32(RESERVED);
	sb->sb_ofs2 = (sb->fssize & ~0xfffULL) - 0x1000;
	return 0;
}

int safe_pwrite(int fd, char *buf, size_t size, u64 ofs)
{
	ssize_t ret;
	size_t  remaining;

	remaining = size;
	while (remaining > 0) {
		ret = pwrite(fd, buf, remaining, ofs);
		if (ret < 0) {
			if (errno == EINTR)
				continue;

			fprintf(stderr, "write failed: %s\n", strerror(errno));
			return ret;
		}

		remaining -= ret;
		ofs += ret;
		buf += ret;
	}
	return 0;
}

static int bdev_write(struct super_block *sb, u64 ofs, size_t size, void *buf)
{
	ssize_t ret;

	ret = safe_pwrite(sb->fd, buf, size, ofs);
	if (ret < 0)
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

static const struct logfs_device_operations mtd_ops = {
	.prepare_sb = mtd_prepare_sb,
	.write = bdev_write,
	.erase = mtd_erase,
};

static const struct logfs_device_operations bdev_ops = {
	.prepare_sb = bdev_prepare_sb,
	.write = bdev_write,
	.erase = bdev_erase,
};


////////////////////////////////////////////////////////////////////////////////


/* root inode */

static void set_segment_header(struct logfs_segment_header *sh, u8 type,
		u8 level, u32 segno)
{
	sh->pad = 0;
	sh->type = type;
	sh->level = level;
	sh->segno = cpu_to_be32(segno);
	sh->ec = 0;
	sh->gec = cpu_to_be64(segno);
	sh->crc = logfs_crc32(sh, LOGFS_SEGMENT_HEADERSIZE, 4);
}

static int write_inode(struct super_block *sb, u64 ino)
{
	struct inode *inode;

	inode = find_or_create_inode(sb, ino);
	if (!inode)
		return -ENOMEM;
	return logfs_file_write(sb, LOGFS_INO_MASTER, ino, 0, OBJ_INODE,
			&inode->di);
}

static int write_segment_file(struct super_block *sb)
{
	struct inode *inode;
	struct logfs_disk_inode *di;
	void *buf;
	int err;
	u64 ofs;

	buf = zalloc(sb->blocksize);
	if (!buf)
		return -ENOMEM;

	inode = find_or_create_inode(sb, LOGFS_INO_SEGFILE);
	if (!inode)
		return -ENOMEM;
	di = &inode->di;
	di->di_flags	= 0;
	di->di_mode	= cpu_to_be16(S_IFREG | 0);
	di->di_refcount	= cpu_to_be32(1);
	di->di_size	= cpu_to_be64(sb->no_segs * 8ull);

	for (ofs = 0; ofs * sb->blocksize < (u64)sb->no_segs * 8; ofs++) {
		err = logfs_file_write(sb, LOGFS_INO_SEGFILE, ofs, 0, OBJ_BLOCK,
				buf);
		if (err)
			return err;
	}
	err = logfs_file_flush(sb, LOGFS_INO_SEGFILE);
	if (err)
		return err;

	return write_inode(sb, LOGFS_INO_SEGFILE);
}

static int write_rootdir(struct super_block *sb)
{
	struct inode *inode;
	struct logfs_disk_inode *di;

	inode = find_or_create_inode(sb, LOGFS_INO_ROOT);
	if (!inode)
		return -ENOMEM;
	di = &inode->di;
	di->di_flags	= 0;
	if (compress_rootdir)
		di->di_flags |= cpu_to_be32(LOGFS_IF_COMPRESSED);
	di->di_mode	= cpu_to_be16(S_IFDIR | 0755);
	di->di_refcount	= cpu_to_be32(1);
	return write_inode(sb, LOGFS_INO_ROOT);
}

/* journal */

static size_t __write_header(struct logfs_journal_header *jh, size_t len,
		size_t datalen, u16 type, u8 compr)
{
	jh->h_len	= cpu_to_be16(len);
	jh->h_type	= cpu_to_be16(type);
	jh->h_datalen	= cpu_to_be16(datalen);
	jh->h_compr	= compr;
	jh->h_pad[0]	= 'h';
	jh->h_pad[1]	= 'e';
	jh->h_pad[2]	= 'a';
	jh->h_pad[3]	= 'd';
	jh->h_pad[4]	= 'r';
	jh->h_crc	= logfs_crc32(jh, len + sizeof(*jh), 4);
	return ALIGN(len, 16) + sizeof(*jh);
}

static size_t write_header(struct logfs_journal_header *h, size_t datalen,
		u16 type)
{
	return __write_header(h, datalen, datalen, type, COMPR_NONE);
}

static size_t je_anchor(struct super_block *sb, void *_da, u16 *type)
{
	struct inode *inode;
	struct logfs_je_anchor *da = _da;
	int i;

	inode = find_or_create_inode(sb, LOGFS_INO_MASTER);
	if (!inode)
		return -ENOMEM;

	memset(da, 0, sizeof(*da));
	da->da_last_ino	= cpu_to_be64(LOGFS_RESERVED_INOS);
	da->da_size	= cpu_to_be64(LOGFS_RESERVED_INOS * sb->blocksize);
	da->da_used_bytes = inode->di.di_used_bytes;
	for (i = 0; i < LOGFS_EMBEDDED_FIELDS; i++)
		da->da_data[i] = inode->di.di_data[i];
	*type = JE_ANCHOR;
	return sizeof(*da);
}

static size_t je_dynsb(struct super_block *sb, void *_dynsb, u16 *type)
{
	struct logfs_je_dynsb *dynsb = _dynsb;

	memset(dynsb, 0, sizeof(*dynsb));
	dynsb->ds_used_bytes	= cpu_to_be64(sb->used_bytes);
	/* Set ds_gec to something beyond anything mkfs would use */
	dynsb->ds_gec		= cpu_to_be64(0x1000);
	*type = JE_DYNSB;
	return sizeof(*dynsb);
}

static size_t je_alias(struct super_block *sb, void *_oa, u16 *type)
{
	struct logfs_obj_alias *oa = _oa;
	struct logfs_segment_entry *se;
	u64 val;
	int i, k;
	int ashift, amask;

	ashift = blockshift - 3; /* 8 bytes per alias */
	amask = (1 << ashift) - 1;
	memset(oa, 0, sb->blocksize);
	k = 0;
	for (i = 0; i < sb->no_segs; i++) {
		se = sb->segment_entry + i;
		if (se->ec_level || se->valid) {
			val = (u64)be32_to_cpu(se->ec_level) << 32 |
				be32_to_cpu(se->valid);
			oa[k].ino = cpu_to_be64(LOGFS_INO_SEGFILE);
			oa[k].bix = cpu_to_be64(i >> ashift);
			oa[k].val = cpu_to_be64(val);
			oa[k].level = 0;
			oa[k].child_no = cpu_to_be16(i & amask);
			k++;
		}
	}

	*type = JE_OBJ_ALIAS;
	return k * sizeof(*oa);
}

static size_t je_commit(struct super_block *sb, void *h, u16 *type)
{
	*type = JE_COMMIT;
	memcpy(h, je_array, no_je * sizeof(__be64));
	return no_je * sizeof(__be64);
}

static size_t write_je(struct super_block *sb,
		size_t jpos, void *scratch, void *header, u32 segno,
		size_t (*write)(struct super_block *sb, void *scratch,
			u16 *type))
{
	u64 ofs = (u64)segno * sb->segsize;
	void *data;
	ssize_t len, max, compr_len, pad_len;
	u16 type;
	u8 compr = COMPR_ZLIB;

	header += jpos;
	data = header + sizeof(struct logfs_journal_header);

	len = write(sb, scratch, &type);
	if (type != JE_COMMIT)
		je_array[no_je++] = cpu_to_be64(ofs + jpos);
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
	u32 seg;

	seg = sb->journal_seg[0];
	/* TODO: add segment to superblock, segfile */
	journal = zalloc(sb->segsize);
	if (!journal)
		return -ENOMEM;

	scratch = zalloc(2 * sb->blocksize);
	if (!scratch)
		return -ENOMEM;

	set_segment_header(journal, SEG_JOURNAL, 0, seg);
	jpos = ALIGN(sizeof(struct logfs_segment_header), 16);
	/* erasecount is not written - implicitly set to 0 */
	/* neither are summary, index, wbuf */
	jpos += write_je(sb, jpos, scratch, journal, seg, je_anchor);
	jpos += write_je(sb, jpos, scratch, journal, seg, je_dynsb);
	jpos += write_je(sb, jpos, scratch, journal, seg, je_alias);
	jpos += write_je(sb, jpos, scratch, journal, seg, je_commit);
	return sb->dev_ops->write(sb, seg * sb->segsize, sb->segsize, journal);
}

/* superblock */

static int make_super(struct super_block *sb)
{
	struct logfs_disk_super _ds, *ds = &_ds;
	void *sector;
	int secsize = ALIGN(sizeof(*ds), sb->writesize);
	int i, ret;

	sector = zalloc(secsize);
	if (!sector)
		return -ENOMEM;

	memset(ds, 0, sizeof(*ds));
	set_segment_header((void *)ds, SEG_SUPER, 0, 0);

	bad_seg_reserve = max(bad_seg_reserve, no_journal_segs);

	ds->ds_magic		= cpu_to_be64(LOGFS_MAGIC);
	ds->ds_ifile_levels	= 3; /* 2+1, 1GiB */
	ds->ds_iblock_levels	= 4; /* 3+1, 512GiB */
	ds->ds_data_levels	= 1; /* old, young, unknown */

	ds->ds_feature_incompat	= 0;
	ds->ds_feature_ro_compat= 0;

	ds->ds_feature_compat	= 0;
	ds->ds_feature_flags	= 0;

	ds->ds_filesystem_size	= cpu_to_be64(sb->fssize);
	ds->ds_segment_shift	= segshift;
	ds->ds_block_shift	= blockshift;
	ds->ds_write_shift	= writeshift;
	ds->ds_bad_seg_reserve	= cpu_to_be32(bad_seg_reserve);

	for (i = 0; i < no_journal_segs; i++)
		ds->ds_journal_seg[i]	= cpu_to_be32(sb->journal_seg[i]);
	ds->ds_super_ofs[0]	= cpu_to_be64(sb->sb_ofs1);
	ds->ds_super_ofs[1]	= cpu_to_be64(sb->sb_ofs2);

	ds->ds_root_reserve	= 0;

	ds->ds_crc = logfs_crc32(ds, sizeof(*ds), LOGFS_SEGMENT_HEADERSIZE + 12);

	memcpy(sector, ds, sizeof(*ds));
	ret = sb->dev_ops->write(sb, sb->sb_ofs1, secsize, sector);
	if (!ret)
		ret = sb->dev_ops->write(sb, sb->sb_ofs2, secsize, sector);
	free(sector);
	return ret;
}

/* main stuff */

static void prepare_journal(struct super_block *sb)
{
	int i;
	u32 segno;

	for (i = 0; i < no_journal_segs; i++) {
		segno = get_segment(sb);
		sb->journal_seg[i] = segno;
		sb->segment_entry[segno].ec_level = ec_level(1, 0);
		sb->segment_entry[segno].valid = cpu_to_be32(RESERVED);
	}
}

static void mkfs(struct super_block *sb)
{
	char answer[4096]; /* I don't care about overflows */
	int ret;

	BUG_ON(!sb);
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
	if (writeshift > 16)
		fail("writeshift too large (max 16)");
	if (segshift < writeshift)
		fail("segment shift must be larger than write shift");
	sb->segsize = 1 << segshift;
	sb->blocksize = 1 << blockshift;
	sb->blocksize_bits = blockshift;
	sb->writesize = 1 << writeshift;

	sb->no_segs = sb->fssize >> segshift;
	sb->fssize = (u64)sb->no_segs << segshift;

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

	sb->segment_entry = zalloc(sb->no_segs * sizeof(sb->segment_entry[0]));
	if (!sb->segment_entry)
		fail("out of memory");

	ret = sb->dev_ops->prepare_sb(sb);
	if (ret)
		fail("could not erase two superblocks");
	prepare_journal(sb);

	ret = write_segment_file(sb);
	if (ret)
		fail("could not write segment file");

	ret = write_rootdir(sb);
	if (ret)
		fail("could not create root inode");

	ret = flush_segments(sb);
	if (ret)
		fail("could not write segments");
	/*
	 * prepare sb
	 * prepare journal
	 * write segment file (create alias)
	 * write inodes (create alias)
	 * flush segments
	 * write journal (including aliases)
	 * write sb
	 */

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
	sb->fd = open(name, O_WRONLY | O_EXCL | O_LARGEFILE);
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
		{
			/* The new "improved" way of doing things */
			char buf[256];
			int fd;

			sprintf(buf, "/sys/class/mtd/%s/size",
					basename((char *)name));
			fd = open(buf, O_RDONLY);
			if (fd >= 0) {
				read(fd, buf, 256);
				sb->fssize = strtoull(buf, NULL, 0);
				close(fd);
			}
		}
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
			{"bad-segment-reserve",	1, NULL, 'B'},
			{"compress",		0, NULL, 'c'},
			{"journal-segments",	1, NULL, 'j'},
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
		case 'B':
			bad_seg_reserve = strtoul(optarg, NULL, 0);
			break;
		case 'c':
			compress_rootdir = 1;
			break;
		case 'j':
			no_journal_segs = strtoul(optarg, NULL, 0);
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
			usage();
			exit(EXIT_FAILURE);
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
