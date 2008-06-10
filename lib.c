#include "kerncompat.h"
#include "logfs.h"

/*
 * zlib crc32 differs from the kernel variant.  zlib negated both the initial
 * value and the result bitwise.  So for the kernel ~0 is a correct initial
 * value, for zlib 0 is.
 * Better check for such funnies instead of generating bad images.
 */
void check_crc32(void)
{
	u32 c=0;
	if (logfs_crc32(&c, 4, 0) != cpu_to_be32(0xdebb20e3))
		fail("crc32 returns bad results");
}

void fail(const char *s)
{
	printf("mklogfs: %s\n", s);
	exit(EXIT_FAILURE);
}

