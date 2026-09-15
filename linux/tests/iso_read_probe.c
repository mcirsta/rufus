/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Emit the shared reader's complete result for byte comparisons in tests. */
#include "iso_read.h"

#include <cdio/logging.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
	iso_extension_mask_t mask = ISO_EXTENSION_ALL;
	struct stat metadata;
	uint8_t* buf = NULL;
	uint32_t size;
	int result = 0;

	if (argc != 4 || stat(argv[1], &metadata) != 0 || !S_ISREG(metadata.st_mode))
		return 64;
	if (strcmp(argv[3], "rr") == 0)
		mask &= ~ISO_EXTENSION_JOLIET;
	else if (strcmp(argv[3], "joliet") == 0)
		mask &= ~ISO_EXTENSION_ROCK_RIDGE;
	else if (strcmp(argv[3], "plain") == 0)
		mask = ISO_EXTENSION_NONE;
	else if (strcmp(argv[3], "all") != 0)
		return 64;
	cdio_log_set_handler(LogCdio);
	size = ReadISOFileToBufferExt(argv[1], argv[2], &buf, mask);
	if (size == 0)
		return buf == NULL ? 1 : 2;
	if (buf == NULL || buf[size] != 0) {
		uprintf("Invalid buffer ownership/termination result");
		result = 2;
	} else if (fwrite(buf, 1, size, stdout) != size) {
		result = 3;
	}
	free(buf);
	return result;
}
