#ifndef LOGFS_H
#define LOGFS_H

#include <asm/types.h>
#include <zlib.h>

#include "btree.h"
#include "kerncompat.h"
#include "logfs_abi.h"

struct super_block;
struct logfs_device_operations {
	int (*write)(struct super_block *sb, u64 ofs, size_t size, void *buf);
	int (*erase)(struct super_block *sb, u64 ofs, size_t size);
	s64 (*scan_super)(struct super_block *sb);
};

struct logfs_area {
	u32 segno;
	u32 used_bytes;
	void *buf;
};

struct super_block {
	int fd;

	u64 fssize;
	u32 segsize;
	u32 erasesize;
	u32 blocksize;
	u32 writesize;
	u32 no_segs;
	u32 journal_seg[LOGFS_JOURNAL_SEGS];
	u64 used_bytes;

	u32 lastseg;
	struct logfs_area area[LOGFS_NO_AREAS];
	struct logfs_segment_entry *segment_entry;

	void *erase_buf;
	u64 sb_ofs1;
	u64 sb_ofs2;
	struct btree_head64 ino_tree;
	struct btree_head128 block_tree[LOGFS_NO_AREAS];
	const struct logfs_device_operations *dev_ops;
};

void check_crc32(void);
void fail(const char *s) __attribute__ ((__noreturn__));
struct super_block *open_device(const char *name);

static inline __be32 logfs_crc32(void *data, size_t len, size_t skip)
{
	return cpu_to_be32(~crc32(0, data+skip, len-skip));
}

static inline void *zalloc(size_t bytes)
{
	return calloc(1, bytes);
}

/* readwrite.c */
struct logfs_disk_inode *find_or_create_inode(struct super_block *sb, u64 ino);
int logfs_file_write(struct super_block *sb, u64 ino, u64 bix, u8 type,
	void *buf);
int logfs_file_flush(struct super_block *sb, u64 ino);

/* segment.c */
u32 get_segment(struct super_block *sb);
s64 logfs_segment_write(struct super_block *sb, void *buf, u16 len, u8 type,
		u64 ino, u64 bix, u8 level);
int flush_segments(struct super_block *sb);

static inline __be32 ec_level(u32 ec, u8 level)
{
	return cpu_to_be32((ec << 4) | (level & 0xf));
}

#endif
