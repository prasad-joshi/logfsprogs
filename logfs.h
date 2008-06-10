#ifndef LOGFS_H
#define LOGFS_H

#include <zlib.h>

void check_crc32(void);
void fail(const char *s) __attribute__ ((__noreturn__));

static inline __be32 logfs_crc32(void *data, size_t len, size_t skip)
{
	return cpu_to_be32(~crc32(0, data+skip, len-skip));
}

#endif
