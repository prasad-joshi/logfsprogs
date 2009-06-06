/*
 * segment.c
 *
 * Copyright (c) 2007-2008 Joern Engel <joern@logfs.org>
 *
 * License: GPL version 2
 */
#include <asm/types.h>
#include <errno.h>

#include "kerncompat.h"
#include "logfs_abi.h"
#include "logfs.h"

#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
static inline void hexdump(const char *prefix, void *buf, size_t len)
{
	unsigned char *c = buf;
	int i;

	printf("%s", prefix);
	for (i = 0; i < len; i++) {
		printf("%02x ", c[i]);
		if ((i & 0xf) == 0xf)
			printf("\n");
	}
	printf("\n");
}

static void copybuf(struct logfs_area *area, void *buf, size_t len)
{
	memcpy(area->buf + area->used_bytes, buf, len);
	area->used_bytes += len;
}

u32 get_segment(struct super_block *sb)
{
	u64 ofs;
	u32 segno;
	int err;

	do {
		segno = sb->lastseg;
		ofs = (u64)segno * sb->segsize;
		sb->lastseg += 1;
		if (sb->lastseg > sb->no_segs)
			fail("no more free segments");
		err = sb->dev_ops->erase(sb, ofs, sb->segsize);
		if (err) {
			/* bad segment */
			sb->segment_entry[segno].ec_level = cpu_to_be32(BADSEG);
			sb->segment_entry[segno].valid = cpu_to_be32(RESERVED);
			printf("Bad block at 0x%llx\n", ofs);
		}
	} while (err);
	return segno;
}

static void __init_area(struct super_block *sb, struct logfs_area *area,
		u8 level)
{
	struct logfs_segment_header *sh = area->buf;

	memset(area->buf, 0xff, sb->segsize);
	area->segno = get_segment(sb);
	area->used_bytes = sizeof(*sh);
	sh->pad = 0;
	sh->type = SEG_OSTORE;
	sh->level = level;
	sh->segno = cpu_to_be32(area->segno);
	sh->ec = cpu_to_be32(1);
	sh->gec = cpu_to_be64(area->segno);
	sh->crc = logfs_crc32(sh, LOGFS_SEGMENT_HEADERSIZE, 4);
}

static void init_area(struct super_block *sb, struct logfs_area *area,
		u8 level)
{
	if (area->buf)
		return;

	area->buf = malloc(sb->segsize);
	__init_area(sb, area, level);
}

static int finish_area(struct super_block *sb, struct logfs_area *area,
		int final, u8 level)
{
	u64 ofs = (u64)area->segno * sb->segsize;
	int err;

	err = sb->dev_ops->write(sb, ofs, sb->segsize, area->buf);
	if (err)
		return err;

	sb->segment_entry[area->segno].ec_level = ec_level(1, level);
	sb->segment_entry[area->segno].valid =
		cpu_to_be32(area->used_bytes - LOGFS_SEGMENT_HEADERSIZE);
	if (final)
		return 0;

	__init_area(sb, area, level);
	return 0;
}

static int grow_inode(struct super_block *sb, u64 ino, size_t len)
{
	struct inode *inode = find_or_create_inode(sb, ino);
	if (!inode)
		return -ENOMEM;

	inode->di.di_used_bytes =
		cpu_to_be64(len + be64_to_cpu(inode->di.di_used_bytes));
	sb->used_bytes += len;
	return 0;
}

static int obj_len(struct super_block *sb, int obj_type)
{
	switch (obj_type) {
	case OBJ_DENTRY:
		return sizeof(struct logfs_disk_dentry);
	case OBJ_INODE:
		return sizeof(struct logfs_disk_inode);
	case OBJ_BLOCK:
		return sb->blocksize;
	default:
		BUG();
	}
}

s64 logfs_segment_write(struct super_block *sb, void *buf, u8 type,
		u64 ino, u64 bix, u8 level)
{
	struct logfs_object_header oh;
	struct logfs_area *area;
	int err;
	s64 ofs;
	u16 len = obj_len(sb, type);

	if (ino == LOGFS_INO_MASTER)
		level += LOGFS_MAX_LEVELS;
	area = sb->area + level;

	memset(&oh, 0, sizeof(oh));
	oh.len = cpu_to_be16(len);
	oh.type = type;
	oh.compr = COMPR_NONE;
	oh.ino = cpu_to_be64(ino);
	oh.bix = cpu_to_be64(bix);
	oh.crc = logfs_crc32(&oh, LOGFS_OBJECT_HEADERSIZE - 4, 4);
	oh.data_crc = logfs_crc32(buf, len, 0);

	init_area(sb, area, level);
	if (area->used_bytes + sizeof(oh) * sb->blocksize > sb->segsize) {
		err = finish_area(sb, area, 0, level);
		if (err)
			return err;
	}
	ofs = area->segno * sb->segsize + area->used_bytes;
	copybuf(area, &oh, sizeof(oh));
	copybuf(area, buf, len);
	err = grow_inode(sb, ino, sizeof(oh) + len);
	if (err)
		return err;
	printf("logfs_segment_write(%llx, %llx %x) type %x len %x to %llx\n",
			ino, bix, level, type, len, ofs);
	return ofs;
}

int flush_segments(struct super_block *sb)
{
	struct logfs_area *area;
	int i, err;

	for (i = 0; i < LOGFS_NO_AREAS; i++) {
		area = sb->area + i;
		if (area->buf) {
			err = finish_area(sb, area, 1, i);
			if (err)
				return err;
		}
	}
	return 0;
}
