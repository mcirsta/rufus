/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Exercise reader failure/ownership contracts on a generated ordinary image. */
#include "iso_scan.h"
#include <cdio/bytesex.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "driver/_cdio_stream.h"

static unsigned int fail_read, reads, failures, checks;
static bool fail_malloc;

#define CHECK(expression) do { \
	checks++; \
	if (!(expression)) { \
		fprintf(stderr, "line %d: %s\n", __LINE__, #expression); \
		failures++; \
	} \
} while (0)

void* __real_malloc(size_t);
ssize_t __real_cdio_stream_read(CdioDataSource_t*, void*, size_t, size_t);

void* __wrap_malloc(size_t size)
{
	if (fail_malloc) {
		fail_malloc = false;
		return NULL;
	}
	return __real_malloc(size);
}

ssize_t __wrap_cdio_stream_read(CdioDataSource_t* stream, void* buffer, size_t size, size_t count)
{
	ssize_t result = __real_cdio_stream_read(stream, buffer, size, count);
	if (++reads == fail_read && result > 0)
		return result - 1;
	return result;
}

static void CheckDirectoryReads(udf_t* image)
{
	udf_dirent_t* root;
	unsigned int point;

	for (point = 1; point <= 2; point++) {
		root = udf_get_root(image, true, 0);
		CHECK(root != NULL);
		if (root == NULL)
			return;
		reads = 0;
		fail_read = point; // Directory extent, then the first file entry.
		root = udf_readdir(root);
		CHECK(root == NULL);
		udf_dirent_free(root);
		fail_read = 0;
	}
	root = udf_get_root(image, true, 0);
	CHECK(root != NULL);
	if (root == NULL)
		return;
	fail_malloc = true; // Directory-sector storage must fail without an I/O to NULL.
	root = udf_readdir(root);
	CHECK(root == NULL && !fail_malloc);
	fail_malloc = false;
	udf_dirent_free(root);
}

static void CheckDirectoryBounds(udf_t* image)
{
	udf_dirent_t* root;
	udf_fileid_desc_t* next;
	uint32_t offset;
	unsigned int point;

	// Only test the corrected reader. The real first entry provides live
	// storage/ownership; constrain the next record to exercise its bounds checks.
	for (point = 0; point < 2; point++) {
		root = udf_get_root(image, true, 0);
		CHECK(root != NULL);
		if (root == NULL)
			return;
		if (point == 0) {
			root->dir_left = 1;
		} else {
			root = udf_readdir(root);
			CHECK(root != NULL && root->fid != NULL);
			if (root == NULL)
				return;
			offset = 4 * ((sizeof(*root->fid) + uint16_from_le(root->fid->u.i_imp_use)
				+ root->fid->i_file_id + 3) / 4);
			next = (udf_fileid_desc_t*)((uint8_t*)root->fid + offset);
			next->u.i_imp_use = uint16_to_le(UINT16_MAX);
		}
		root = udf_readdir(root);
		CHECK(root == NULL);
		udf_dirent_free(root);
	}
}

int main(int argc, char** argv)
{
	udf_t* image;
	uint8_t buffer[UDF_BLOCKSIZE];

	if (argc != 2)
		return 64;
	image = udf_open(argv[1]);
	CHECK(image != NULL);
	if (image == NULL)
		return 1;
	CHECK(udf_read_sectors(image, buffer, 256, 1) == DRIVER_OP_SUCCESS);
	reads = 0;
	fail_read = 1;
	CHECK(udf_read_sectors(image, buffer, 256, 1) == DRIVER_OP_ERROR);
	fail_read = 0;
	CHECK(udf_read_sectors(NULL, buffer, 256, 1) == DRIVER_OP_BAD_PARAMETER);
	CHECK(udf_read_sectors(image, NULL, 256, 1) == DRIVER_OP_BAD_PARAMETER);
	CHECK(udf_read_sectors(image, buffer, 256, 0) == DRIVER_OP_BAD_PARAMETER);
	CheckDirectoryReads(image);
	CheckDirectoryBounds(image);
	udf_close(image);
	printf("UDF reader: %u checks, %u failures\n", checks, failures);
	return failures != 0;
}
