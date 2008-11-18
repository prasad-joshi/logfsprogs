#ifndef LOGFS_H
#define LOGFS_H

#define __CHECK_ENDIAN__

#include <zlib.h>
#include "kerncompat.h"

struct super_block;
struct logfs_device_operations {
	int (*write)(struct super_block *sb, u64 ofs, size_t size, void *buf);
	int (*erase)(struct super_block *sb, u64 ofs, size_t size);
	s64 (*scan_super)(struct super_block *sb);
};

struct super_block {
	int fd;

	u64 fssize;
	u32 segsize;
	u32 erasesize;
	u32 blocksize;
	u32 writesize;

	void *erase_buf;
	void *wbuf;
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

#endif
