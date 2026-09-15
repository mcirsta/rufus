/* SPDX-License-Identifier: GPL-3.0-or-later */
/* One real UDF block read after a seek; stdout contains only returned bytes. */
#include "iso_scan.h"
#include <cdio/logging.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "driver/_cdio_stream.h"

static bool active, short_read;

ssize_t __real_cdio_stream_read(CdioDataSource_t*, void*, size_t, size_t);

ssize_t __wrap_cdio_stream_read(CdioDataSource_t* stream, void* buffer, size_t size, size_t count)
{
	ssize_t result = __real_cdio_stream_read(stream, buffer, size, count);
	if (active && short_read && result > 0)
		return result - 1;
	return result;
}

static void LogCdio(cdio_log_level_t level, const char* message)
{
	(void)level;
	fprintf(stderr, "%s\n", message);
}

int main(int argc, char** argv)
{
	udf_t* udf = NULL;
	udf_dirent_t *root = NULL, *file = NULL;
	uint8_t buffer[16 * UDF_BLOCKSIZE];
	uint64_t offset;
	unsigned long blocks;
	struct stat metadata;
	ssize_t count;
	int result = 1;

	if ((argc != 5 && argc != 6) || stat(argv[1], &metadata) != 0 || !S_ISREG(metadata.st_mode))
		return 64;
	offset = strtoull(argv[3], NULL, 10);
	blocks = strtoul(argv[4], NULL, 10);
	if (offset > INT64_MAX || blocks == 0 || blocks > 16)
		return 64;
	short_read = argc == 6;
	cdio_log_set_handler(LogCdio);
	udf = udf_open(argv[1]);
	if (udf == NULL)
		goto out;
	root = udf_get_root(udf, true, 0);
	if (root == NULL)
		goto out;
	file = udf_fopen(root, argv[2]);
	if (file == NULL || !udf_setpos(file, (off_t)offset))
		goto out;
	active = true;
	count = udf_read_block(file, buffer, blocks);
	active = false;
	if (count < 0 || (size_t)count > sizeof(buffer))
		goto out;
	if (fwrite(buffer, 1, (size_t)count, stdout) != (size_t)count)
		goto out;
	result = 0;
out:
	udf_dirent_free(file);
	udf_dirent_free(root);
	if (udf != NULL)
		udf_close(udf);
	return result;
}
