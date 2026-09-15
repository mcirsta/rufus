/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "iso_read.h"
#include "syslinux_version.h"

#include <cdio/logging.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

void uprintf(const char* format, ...)
{
	va_list args;
	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);
	fputc('\n', stderr);
}

static void LogCdio(cdio_log_level_t level, const char* message)
{
	(void)level;
	uprintf("libcdio: %s", message);
}

int main(int argc, char** argv)
{
	struct stat metadata;
	uint8_t* buf = NULL;
	uint32_t size;
	uint16_t version;
	char* ext;

	if (argc != 3 || stat(argv[1], &metadata) != 0 || !S_ISREG(metadata.st_mode))
		return 64;
	cdio_log_set_handler(LogCdio);
	// Use the same name options as Rufus's initial Rock Ridge tree scan.
	size = ReadISOFileToBufferExt(argv[1], argv[2], &buf,
		ISO_EXTENSION_ALL & ~ISO_EXTENSION_JOLIET);
	if (size == 0)
		return 1;
	version = GetSyslinuxVersion((char*)buf, (size_t)size + 1, &ext);
	printf("VERSION=%u\nEXT=%s\n", (unsigned int)version, ext);
	free(buf);
	return 0;
}
