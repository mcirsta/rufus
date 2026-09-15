/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Read requested ranges from one selected view. stdout is exact file bytes. */
#include "iso_file.h"
#include <cdio/logging.h>

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static bool active;
static uint64_t reads, blocks_read, iso_reads, max_blocks;

int IsIsoScanCancelled(void)
{
	return 0;
}

static void LogCdio(cdio_log_level_t level, const char* message)
{
	(void)level;
	fprintf(stderr, "%s\n", message);
}

long __real_iso9660_iso_seek_read(const iso9660_t*, void*, lsn_t, long);
ssize_t __real_udf_read_block(const udf_dirent_t*, void*, size_t);

static void CountRead(size_t blocks)
{
	reads++;
	blocks_read += blocks;
	if (blocks > max_blocks)
		max_blocks = blocks;
}

long __wrap_iso9660_iso_seek_read(const iso9660_t* iso, void* buffer, lsn_t lsn, long blocks)
{
	if (active)
	{
		iso_reads++;
		CountRead((size_t)blocks);
	}
	return __real_iso9660_iso_seek_read(iso, buffer, lsn, blocks);
}

ssize_t __wrap_udf_read_block(const udf_dirent_t* file, void* buffer, size_t blocks)
{
	if (active)
		CountRead(blocks);
	return __real_udf_read_block(file, buffer, blocks);
}

static bool ParseOffset(const char* text, uint64_t* value)
{
	char* end;

	errno = 0;
	if (*text < '0' || *text > '9')
		return false;
	*value = strtoull(text, &end, 10);
	return *end == 0 && errno == 0;
}

int main(int argc, char** argv)
{
	ISO_FILE_READER reader = { 0 };
	iso9660_t* iso = NULL;
	udf_t* udf = NULL;
	uint8_t buffer[4 * ISO_FILE_CACHE_SIZE];
	uint64_t offset, count;
	struct stat metadata;
	int i, result = ISO_SCAN_ERROR;

	// Image, view, path, then one or more offset/count pairs.
	bool valid_arguments = argc >= 6 && argc % 2 == 0;
	if (!valid_arguments)
		return 64;
	if (stat(argv[1], &metadata) != 0 || !S_ISREG(metadata.st_mode))
		return 64;
	cdio_log_set_handler(LogCdio);
	if (strcmp(argv[2], "udf") == 0)
		udf = udf_open(argv[1]);
	else if (strcmp(argv[2], "rr") == 0)
		iso = iso9660_open_ext(argv[1], ISO_EXTENSION_ALL & ~ISO_EXTENSION_JOLIET);
	else if (strcmp(argv[2], "joliet") == 0)
		iso = iso9660_open_ext(argv[1], ISO_EXTENSION_JOLIET);
	else
		return 64;
	if (iso == NULL && udf == NULL)
		goto out;
	result = OpenIsoFileReader(&reader, iso, udf, argv[3]);
	if (result != ISO_SCAN_OK)
		goto out;
	active = true;
	for (i = 4; i < argc; i += 2)
	{
		if (!ParseOffset(argv[i], &offset) || !ParseOffset(argv[i + 1], &count) || count > sizeof(buffer))
		{
			result = 64;
			break;
		}
		result = ReadIsoFileRange(&reader, offset, buffer, (size_t)count);
		if (result != ISO_SCAN_OK)
			break;
		if (fwrite(buffer, 1, (size_t)count, stdout) != count)
		{
			result = 65;
			break;
		}
	}
out:
	active = false;
	fprintf(stderr, "SIZE=%" PRIu64 " READS=%" PRIu64 " BLOCKS=%" PRIu64
		" MAX_BLOCKS=%" PRIu64 " ISO_READS=%" PRIu64 "\n",
		reader.size, reads, blocks_read, max_blocks, iso_reads);
	CloseIsoFileReader(&reader);
	iso9660_close(iso);
	if (udf != NULL)
		udf_close(udf);
	return result;
}
