/*
 * readwrite.c
 *
 * Copyright (c) 2007-2008 Joern Engel <joern@logfs.org>
 *
 * License: GPL version 2
 */
#include <asm/types.h>
#include <errno.h>

#include "btree.h"
#include "kerncompat.h"
#include "logfs_abi.h"
#include "logfs.h"

static int child_no(struct super_block *sb, u64 bix)
{
	return bix & (sb->blocksize - 1);
}

struct logfs_disk_inode *find_or_create_inode(struct super_block *sb, u64 ino)
{
	struct logfs_disk_inode *di;
	int err;

	di = btree_lookup64(&sb->ino_tree, ino);
	if (!di) {
		di = calloc(1, sb->blocksize);
		if (!di)
			return NULL;
		err = btree_insert64(&sb->ino_tree, ino, di);
		if (err)
			return NULL;
	}
	return di;
}

static __be64 *find_or_create_block(struct super_block *sb, u64 ino, u64 bix,
		u8 level)
{
	__be64 *block;
	struct btree_head128 *tree = &sb->block_tree[level];
	int err;

	block = btree_lookup128(tree, ino, bix);
	if (!block) {
		block = calloc(1, sb->blocksize);
		if (!block)
			return NULL;
		err = btree_insert128(tree, ino, bix, block);
		if (err)
			return NULL;
	}
	return block;
}

static int write_direct(struct super_block *sb, u64 ino, u64 bix, u8 type,
		void *buf)
{
	struct logfs_disk_inode *di;
	s64 ofs;

	di = find_or_create_inode(sb, ino);
	if (!di)
		return -ENOMEM;
	ofs = logfs_segment_write(sb, buf, sb->blocksize, type, ino, bix, 0);
	if (ofs < 0)
		return ofs;
	di->di_data[bix] = cpu_to_be64(ofs);
	return 0;
}

static int write_loop(struct super_block *sb, u64 ino, u64 bix, u8 type,
		void *buf)
{
	__be64 *iblock;
	s64 ofs;

	iblock = find_or_create_block(sb, ino, bix, 1);
	if (!iblock)
		return -ENOMEM;
	ofs = logfs_segment_write(sb, buf, sb->blocksize, type, ino, bix, 0);
	if (ofs < 0)
		return ofs;
	iblock[bix & child_no(sb, bix)] = cpu_to_be64(ofs);
	return 0;
}

int logfs_file_write(struct super_block *sb, u64 ino, u64 bix, u8 type,
	void *buf)
{
	if (bix < I0_BLOCKS)
		return write_direct(sb, ino, bix, type, buf);
	return write_loop(sb, ino, bix, type, buf);
}

int logfs_file_flush(struct super_block *sb, u64 ino)
{
	__be64 *iblock;
	return 0;
}
