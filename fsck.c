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


static void usage(void)
{
	printf(
"logfsck <options> <device>\n"
"\n"
"Options:\n"
"  -h --help            display this help\n"
"\n");
}

int main(int argc, char **argv)
{
	check_crc32();
	for (;;) {
		int oi = 1;
		char short_opts[] = "h";
		static const struct option long_opts[] = {
			{"help",		0, NULL, 'h'},
			{ }
		};
		int c = getopt_long(argc, argv, short_opts, long_opts, &oi);
		if (c == -1)
			break;
		switch (c) {
		case 'h':
			usage();
			exit(EXIT_SUCCESS);
		default:
			fail("unknown option\n");
		}
	}

	if (optind != argc - 1) {
		usage();
		exit(EXIT_FAILURE);
	}

	printf("foo\n");

	return 0;
}
