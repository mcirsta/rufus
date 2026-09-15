/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "embedded_efi.h"
#include "syslinux/libfat/libfatint.h"
#include <cdio/logging.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static unsigned int queries, cancel_after, reads, fail_read, max_blocks, blocks_read;
static bool active, fail_context, fail_open, fail_cache, injected;
static bool fail_seek, fail_root, fail_file;
static unsigned int iso_reads;

void uprintf(const char* format, ...)
{
	va_list args;
	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);
	fputc('\n', stderr);
}

int IsIsoScanCancelled(void)
{
	return cancel_after != 0 && ++queries >= cancel_after;
}

static void LogCdio(cdio_log_level_t level, const char* message)
{
	(void)level;
	uprintf("%s", message);
}

long int __real_iso9660_iso_seek_read(const iso9660_t*, void*, lsn_t, long int);

long int __wrap_iso9660_iso_seek_read(const iso9660_t* iso, void* buffer, lsn_t lsn, long int blocks)
{
	if (active) {
		iso_reads++;
		reads++;
		blocks_read += (unsigned int)blocks;
		if ((unsigned long)blocks > max_blocks)
			max_blocks = (unsigned int)blocks;
		if (reads == fail_read) {
			injected = true;
			return 0;
		}
	}
	return __real_iso9660_iso_seek_read(iso, buffer, lsn, blocks);
}

ssize_t __real_udf_read_block(const udf_dirent_t*, void*, size_t);
bool __real_udf_setpos(udf_dirent_t*, off_t);
udf_dirent_t* __real_udf_get_root(udf_t*, bool, partition_num_t);
udf_dirent_t* __real_udf_fopen(udf_dirent_t*, const char*);

ssize_t __wrap_udf_read_block(const udf_dirent_t* file, void* buffer, size_t blocks)
{
	ssize_t result;
	if (active) {
		reads++;
		blocks_read += (unsigned int)blocks;
		if (blocks > max_blocks)
			max_blocks = (unsigned int)blocks;
	}
	result = __real_udf_read_block(file, buffer, blocks);
	if (active && reads == fail_read) {
		injected = true;
		return result > 0 ? result - 1 : -1;
	}
	return result;
}

bool __wrap_udf_setpos(udf_dirent_t* file, off_t offset)
{
	if (active && fail_seek) {
		injected = true;
		return false;
	}
	return __real_udf_setpos(file, offset);
}

udf_dirent_t* __wrap_udf_get_root(udf_t* udf, bool any, partition_num_t partition)
{
	if (active && fail_root) {
		injected = true;
		return NULL;
	}
	return __real_udf_get_root(udf, any, partition);
}

udf_dirent_t* __wrap_udf_fopen(udf_dirent_t* root, const char* path)
{
	if (active && fail_file) {
		injected = true;
		return NULL;
	}
	return __real_udf_fopen(root, path);
}

void* __real_calloc(size_t, size_t);
void* __real_malloc(size_t);
int __real_posix_memalign(void**, size_t, size_t);

void* __wrap_calloc(size_t count, size_t size)
{
	if (active && fail_context && count == 1 && size >= 32768 && size < 65536) {
		injected = true;
		return NULL;
	}
	return __real_calloc(count, size);
}

void* __wrap_malloc(size_t size)
{
	if (active && fail_open && size == sizeof(struct libfat_filesystem)) {
		injected = true;
		return NULL;
	}
	return __real_malloc(size);
}

int __wrap_posix_memalign(void** ptr, size_t alignment, size_t size)
{
	if (active && fail_cache) {
		injected = true;
		return 12;
	}
	return __real_posix_memalign(ptr, alignment, size);
}

int main(int argc, char** argv)
{
	iso9660_t* iso = NULL;
	udf_t* udf = NULL;
	struct stat metadata;
	bool found = true;
	int result;

	if ((argc != 4 && argc != 5) || stat(argv[1], &metadata) != 0 || !S_ISREG(metadata.st_mode))
		return 64;
	if (sscanf(argv[3], "cancel:%u", &cancel_after) != 1 &&
		sscanf(argv[3], "fail-read:%u", &fail_read) != 1) {
		fail_context = strcmp(argv[3], "fail-context") == 0;
		fail_open = strcmp(argv[3], "fail-open") == 0;
		fail_cache = strcmp(argv[3], "fail-cache") == 0;
		fail_seek = strcmp(argv[3], "fail-seek") == 0;
		fail_root = strcmp(argv[3], "fail-root") == 0;
		fail_file = strcmp(argv[3], "fail-file") == 0;
		if (!fail_context && !fail_open && !fail_cache && !fail_seek && !fail_root && !fail_file &&
			strcmp(argv[3], "none") != 0)
			return 64;
	}
	cdio_log_set_handler(LogCdio);
	if (argc == 5 && strcmp(argv[4], "udf") == 0)
		udf = udf_open(argv[1]);
	else
		iso = iso9660_open_ext(argv[1], ISO_EXTENSION_ALL & ~ISO_EXTENSION_JOLIET);
	if (iso == NULL && udf == NULL)
		return 65;
	// Simulate a prior 4K device operation. Every return must restore all three.
	LIBFAT_SECTOR_SIZE = 4096;
	LIBFAT_SECTOR_MASK = 4095;
	LIBFAT_SECTOR_SHIFT = 12;
	active = true;
	result = InspectEfiImage(iso, udf, argv[2], &found);
	active = false;
	if (iso != NULL)
		iso9660_close(iso);
	if (udf != NULL)
		udf_close(udf);
	if (LIBFAT_SECTOR_SIZE != 4096 || LIBFAT_SECTOR_MASK != 4095 || LIBFAT_SECTOR_SHIFT != 12)
		return 66;
	if (result != ISO_SCAN_OK && found)
		return 67;
	printf("STATUS=%d\nFOUND=%u\nREADS=%u\nBLOCKS=%u\nMAX_BLOCKS=%u\nINJECTED=%u\nISO_READS=%u\n",
		result, (unsigned int)found, reads, blocks_read, max_blocks, (unsigned int)injected, iso_reads);
	return result;
}
