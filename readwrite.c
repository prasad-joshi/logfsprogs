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

static unsigned long __get_bits(u64 val, int skip, int no)
{
	u64 ret = val;

	ret >>= skip * no;
	ret <<= 64 - no;
	ret >>= 64 - no;
	return ret;
}

static unsigned long get_bits(struct super_block *sb, u64 bix, u8 level)
{
	return __get_bits(bix, level, sb->blocksize_bits - 3);
}

static int child_no(struct super_block *sb, u64 bix)
{
	return bix & ((sb->blocksize / sizeof(__be64)) - 1);
}

struct inode *find_or_create_inode(struct super_block *sb, u64 ino)
{
	struct inode *inode;
	int err;

	inode = btree_lookup64(&sb->ino_tree, ino);
	if (!inode) {
		inode = zalloc(sizeof(*inode) + sb->blocksize);
		if (!inode)
			return NULL;
		err = btree_insert64(&sb->ino_tree, ino, inode);
		if (err)
			return NULL;
	}
	return inode;
}

static __be64 *find_or_create_block(struct super_block *sb, struct inode *inode,
	       	u64 bix, u8 level)
{
	__be64 *block;
	struct btree_head64 *tree = &inode->block_tree[level];
	int err;

	block = btree_lookup64(tree, bix);
	if (!block) {
		block = zalloc(sb->blocksize);
		if (!block)
			return NULL;
		err = btree_insert64(tree, bix, block);
		if (err)
			return NULL;
	}
	return block;
}

static int write_direct(struct super_block *sb, struct inode *inode, u64 ino,
	       	u64 bix, u8 type, void *buf)
{
	s64 ofs;

	ofs = logfs_segment_write(sb, buf, type, ino, bix, 0);
	if (ofs < 0)
		return ofs;
	inode->di.di_data[bix] = cpu_to_be64(ofs);
	return 0;
}

static u64 bixmask(struct super_block *sb, u8 level)
{
	if (level == 0)
		return 0;
	return (1 << ((sb->blocksize_bits - 3) * level)) - 1;
}

static int write_loop(struct super_block *sb, struct inode *inode, u64 ino,
	       	u64 bix, u8 level, u8 type, void *buf)
{
	u64 parent_bix;
	__be64 *iblock;
	s64 ofs;

	parent_bix = bix | bixmask(sb, level + 1);
	iblock = find_or_create_block(sb, inode, parent_bix, level + 1);
	if (!iblock)
		return -ENOMEM;
	ofs = logfs_segment_write(sb, buf, type, ino, bix, level);
	if (ofs < 0)
		return ofs;
	iblock[get_bits(sb, bix, level)] = cpu_to_be64(ofs);
	return 0;
}

static inline u64 maxbix(u8 height)
{
	return 1ULL << (LOGFS_BLOCK_BITS * height);
}

static void grow_inode(struct inode *inode, u64 bix, u8 level)
{
	if (level != 0)
		return;
	while (bix > maxbix(inode->di.di_height))
		inode->di.di_height++;
}

int logfs_file_write(struct super_block *sb, u64 ino, u64 bix, u8 level,
	       	u8 type, void *buf)
{
	struct inode *inode;

	inode = find_or_create_inode(sb, ino);
	if (!inode)
		return -ENOMEM;

	if (level == 0 && bix < I0_BLOCKS)
		return write_direct(sb, inode, ino, bix, type, buf);

	grow_inode(inode, bix, level);
	return write_loop(sb, inode, ino, bix, level, type, buf);
}

int logfs_file_flush(struct super_block *sb, u64 ino)
{
	struct btree_head64 *tree;
	struct inode *inode;
	__be64 *iblock;
	s64 ofs;
	u64 bix;
	u8 level;
	int err;

	inode = find_or_create_inode(sb, ino);
	BUG_ON(!inode);

	if (inode->di.di_height == 0)
		return 0;

	for (level = 1; level < inode->di.di_height; level++) {
		tree = &inode->block_tree[level];
		for (;;) {
			bix = btree_last64(tree);
			iblock = btree_remove64(tree, bix);
			if (!iblock)
				break;
			err = logfs_file_write(sb, ino, bix, level, OBJ_BLOCK,
					iblock);
			if (err)
				return err;
			free(iblock);
		}
	}
	BUG_ON(level != inode->di.di_height);
	tree = &inode->block_tree[level];
	bix = btree_last64(tree);
	iblock = btree_remove64(tree, bix);
	BUG_ON(!iblock);
	ofs = logfs_segment_write(sb, iblock, OBJ_BLOCK, ino, bix, level);
	if (ofs < 0)
		return ofs;
	inode->di.di_data[INDIRECT_INDEX] = cpu_to_be64(ofs);
	return 0;
}
