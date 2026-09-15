/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "grub_inspect.h"
#include "iso_read.h"

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
	RUFUS_IMG_REPORT report = { 0 };
	StrArray filesystems;
	struct stat metadata;
	uint8_t* buf = NULL;
	uint32_t size;
	unsigned long flags;
	char* end;
	int result = 1;

	if (argc < 5 || stat(argv[1], &metadata) != 0 || !S_ISREG(metadata.st_mode))
		return 64;
	flags = strtoul(argv[2], &end, 0);
	if (*end != 0 || end == argv[2] || flags > 255)
		return 64;
	report.has_grub2 = (uint8_t)flags;
	snprintf(report.label, sizeof(report.label), "%s", argv[3]);
	cdio_log_set_handler(LogCdio);
	StrArrayCreate(&filesystems, 8);
	if (filesystems.String == NULL)
		return 1;
	for (int i = 4; i < argc; i++) {
		size = ReadISOFileToBufferExt(argv[1], argv[i], &buf,
			ISO_EXTENSION_ALL & ~ISO_EXTENSION_JOLIET);
		if (size == 0)
			goto out;
		GetGrubVersion((const char*)buf, (size_t)size + 1, argv[i], &report);
		if (!GetGrubFs((const char*)buf, (size_t)size + 1, &filesystems))
			goto out;
		free(buf);
		buf = NULL;
	}
	printf("VERSION=%s\nFS_MASK=%u\nFILESYSTEMS=", report.grub2_version,
		(unsigned int)GetGrubFsMask(&filesystems));
	for (uint32_t i = 0; i < filesystems.Index; i++)
		printf("%s%s", i == 0 ? "" : ",", filesystems.String[i]);
	putchar('\n');
	result = 0;
out:
	free(buf);
	StrArrayDestroy(&filesystems);
	return result;
}
