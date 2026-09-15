/* SPDX-License-Identifier: GPL-3.0-or-later */
/* API doubles test reader ownership/dispatch without large allocations or UDF
 * tooling. Real ISO decoding is covered by the separate generated-image test. */
#include "iso_read.h"
#include <cdio/udf.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, cases;
static bool use_udf, fail_open, fail_root, fail_lookup, directory, fail_alloc;
static uint64_t file_size;
static ssize_t read_result;
static size_t allocation_size, requested_blocks;
static int allocations, reads, udf_opens, iso_opens;
static int live_image, live_root, live_file;
static iso_extension_mask_t seen_mask;
static uint64_t image_handle;
static udf_dirent_t udf_root, udf_file;
static iso9660_stat_t iso_file;

#define CHECK(expression) do { \
	if (!(expression)) { \
		fprintf(stderr, "case %d, line %d: %s\n", cases, __LINE__, #expression); \
		failures++; \
	} \
} while (0)

void* __real_malloc(size_t size);

void* __wrap_malloc(size_t size)
{
	allocations++;
	allocation_size = size;
	return fail_alloc ? NULL : __real_malloc(size);
}

void uprintf(const char* format, ...)
{
	va_list args;
	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);
	fputc('\n', stderr);
}

udf_t* udf_open(const char* path)
{
	CHECK(strcmp(path, "image") == 0);
	udf_opens++;
	if (!use_udf || fail_open)
		return NULL;
	live_image++;
	return (udf_t*)&image_handle;
}

udf_dirent_t* udf_get_root(udf_t* image, bool any_partition, partition_num_t partition)
{
	CHECK(image == (udf_t*)&image_handle && any_partition && partition == 0);
	if (fail_root)
		return NULL;
	live_root++;
	return &udf_root;
}

udf_dirent_t* udf_fopen(udf_dirent_t* root, const char* path)
{
	CHECK(root == &udf_root && strcmp(path, "/entry") == 0);
	if (fail_lookup)
		return NULL;
	live_file++;
	return &udf_file;
}

bool udf_is_dir(const udf_dirent_t* file)
{
	CHECK(file == &udf_file);
	return directory;
}

uint64_t udf_get_file_length(const udf_dirent_t* file)
{
	CHECK(file == &udf_file);
	return file_size;
}

ssize_t udf_read_block(const udf_dirent_t* file, void* buf, size_t count)
{
	CHECK(file == &udf_file);
	reads++;
	requested_blocks = count;
	if (read_result != -2)
		return read_result;
	CHECK(count * UDF_BLOCKSIZE + 1 == allocation_size);
	memset(buf, 0xa5, count * UDF_BLOCKSIZE);
	return (ssize_t)file_size;
}

bool udf_dirent_free(udf_dirent_t* file)
{
	if (file == &udf_root)
		live_root--;
	else if (file == &udf_file)
		live_file--;
	else
		CHECK(file == NULL);
	return true;
}

bool udf_close(udf_t* image)
{
	CHECK(image == (udf_t*)&image_handle);
	live_image--;
	return true;
}

iso9660_t* iso9660_open_ext(const char* path, iso_extension_mask_t mask)
{
	CHECK(strcmp(path, "image") == 0);
	iso_opens++;
	seen_mask = mask;
	if (fail_open)
		return NULL;
	live_image++;
	return (iso9660_t*)&image_handle;
}

iso9660_stat_t* iso9660_ifs_stat_translate(iso9660_t* image, const char* path)
{
	CHECK(image == (iso9660_t*)&image_handle && strcmp(path, "/entry") == 0);
	if (fail_lookup)
		return NULL;
	iso_file.type = directory ? _STAT_DIR : _STAT_FILE;
	iso_file.total_size = file_size;
	iso_file.lsn = 123;
	live_file++;
	return &iso_file;
}

long int iso9660_iso_seek_read(const iso9660_t* image, void* buf, lsn_t start, long int count)
{
	CHECK(image == (iso9660_t*)&image_handle && start == 123);
	reads++;
	requested_blocks = (size_t)count;
	if (read_result != -2)
		return (long int)read_result;
	CHECK((size_t)count * ISO_BLOCKSIZE + 1 == allocation_size);
	memset(buf, 0xa5, (size_t)count * ISO_BLOCKSIZE);
	return count * ISO_BLOCKSIZE;
}

void iso9660_stat_free(iso9660_stat_t* file)
{
	if (file != NULL) {
		CHECK(file == &iso_file);
		live_file--;
	}
}

bool iso9660_close(iso9660_t* image)
{
	if (image != NULL) {
		CHECK(image == (iso9660_t*)&image_handle);
		live_image--;
	}
	return true;
}

static void ResetRead(bool udf, uint64_t size)
{
	use_udf = udf;
	file_size = size;
	fail_open = fail_root = fail_lookup = directory = fail_alloc = false;
	read_result = -2;
	allocations = reads = udf_opens = iso_opens = 0;
	allocation_size = requested_blocks = 0;
	CHECK(live_image == 0 && live_root == 0 && live_file == 0);
}

static void CheckRead(uint32_t expected_size)
{
	uint8_t stale = 0;
	uint8_t* buf = &stale;
	uint32_t size;
	iso_extension_mask_t mask = ISO_EXTENSION_ROCK_RIDGE;

	cases++;
	size = ReadISOFileToBufferExt("image", "/entry", &buf, mask);
	CHECK(size == expected_size);
	CHECK((size == 0) == (buf == NULL));
	CHECK(live_image == 0 && live_root == 0 && live_file == 0);
	CHECK(udf_opens == 1);
	CHECK(iso_opens == ((!use_udf || fail_open) ? 1 : 0));
	if (iso_opens != 0)
		CHECK(seen_mask == mask);
	if (expected_size == 0) {
		CHECK(buf == NULL);
	} else if (buf != NULL && buf != &stale) {
		CHECK(buf[size] == 0);
		for (uint32_t i = 0; i < size; i++)
			CHECK(buf[i] == 0xa5);
	}
	CHECK(buf != &stale);
	if (buf != &stale)
		free(buf);
}

static void CheckLimitedRead(bool udf)
{
	uint8_t* buf;
	const uint32_t limit = 65536;

	cases++;
	ResetRead(udf, limit + 1);
	CHECK(ReadISOFileToBufferLimited("image", "/entry", &buf, ISO_EXTENSION_ALL, limit) == 0);
	CHECK(buf == NULL && allocations == 0 && reads == 0);
	CHECK(live_image == 0 && live_root == 0 && live_file == 0);
	cases++;
	ResetRead(udf, limit);
	CHECK(ReadISOFileToBufferLimited("image", "/entry", &buf, ISO_EXTENSION_ALL, limit) == limit);
	CHECK(buf != NULL && allocations == 1 && allocation_size == limit + 1 && reads == 1);
	if (buf != NULL)
		CHECK(buf[limit] == 0 && buf[limit - 1] == 0xa5);
	free(buf);
	CHECK(live_image == 0 && live_root == 0 && live_file == 0);
	cases++;
	ResetRead(udf, 1);
	CHECK(ReadISOFileToBufferLimited("image", "/entry", &buf, ISO_EXTENSION_ALL, 0) == 0);
	CHECK(ReadISOFileToBufferLimited("image", "/entry", &buf, ISO_EXTENSION_ALL, ISO_READ_MAX_SIZE + 1) == 0);
	CHECK(buf == NULL && udf_opens == 0 && iso_opens == 0 && allocations == 0);
}

static void CheckBackend(bool udf)
{
	ResetRead(udf, 2049);
	CheckRead(2049);
	CHECK(allocations == 1 && allocation_size == 4097 && reads == 1 && requested_blocks == 2);
	ResetRead(udf, 2049);
	fail_lookup = true;
	CheckRead(0);
	CHECK(allocations == 0 && reads == 0);
	ResetRead(udf, 2049);
	directory = true;
	CheckRead(0);
	CHECK(allocations == 0 && reads == 0);
	ResetRead(udf, 0);
	CheckRead(0);
	CHECK(allocations == 0 && reads == 0);
	ResetRead(udf, ISO_READ_MAX_SIZE + UINT64_C(1));
	CheckRead(0);
	CHECK(allocations == 0 && reads == 0);
	ResetRead(udf, UINT64_MAX);
	CheckRead(0);
	CHECK(allocations == 0 && reads == 0);
	ResetRead(udf, ISO_READ_MAX_SIZE);
	fail_alloc = true;
	CheckRead(0);
	CHECK(allocations == 1 && allocation_size == (size_t)ISO_READ_MAX_SIZE + 1 && reads == 0);
	for (int i = -1; i < 2; i++) {
		ResetRead(udf, 2049);
		read_result = i;
		CheckRead(0);
		CHECK(allocations == 1 && reads == 1);
	}
	// Recovery within this process after all failure/cleanup paths.
	ResetRead(udf, 1);
	CheckRead(1);
}

int main(void)
{
	uint8_t* buf;
	CheckBackend(false);
	CheckBackend(true);
	CheckLimitedRead(false);
	CheckLimitedRead(true);
	ResetRead(true, 1);
	fail_root = true;
	CheckRead(0);
	CHECK(allocations == 0 && reads == 0);
	ResetRead(false, 1);
	fail_open = true;
	CheckRead(0);
	CHECK(allocations == 0 && reads == 0);
	ResetRead(false, 1);
	CHECK(ReadISOFileToBufferExt("image", "/entry", NULL, ISO_EXTENSION_ALL) == 0);
	CHECK(ReadISOFileToBufferExt(NULL, "/entry", &buf, ISO_EXTENSION_ALL) == 0 && buf == NULL);
	CHECK(ReadISOFileToBufferExt("image", NULL, &buf, ISO_EXTENSION_ALL) == 0 && buf == NULL);
	CHECK(udf_opens == 0 && iso_opens == 0 && allocations == 0);
	printf("%d read scenarios plus null arguments: %d failures\n", cases, failures);
	return failures != 0;
}
