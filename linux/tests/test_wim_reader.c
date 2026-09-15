/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "native_input.h"
#include "wimlib/file_io.h"
#include "iso_file.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int failures, checks, calls, injected_result, cancelled;

#define CHECK(expression) do { \
	checks++; \
	if (!(expression)) { \
		fprintf(stderr, "line %d: %s\n", __LINE__, #expression); \
		failures++; \
	} \
} while (0)

int IsIsoScanCancelled(void)
{
	return cancelled;
}

int __wrap_ReadIsoFileRange(ISO_FILE_READER* reader, uint64_t offset, void* data, size_t count)
{
	uint8_t* bytes = data;
	size_t i;

	calls++;
	CHECK(offset <= reader->size && count <= reader->size - offset);
	if (injected_result != ISO_SCAN_OK)
		return injected_result;
	for (i = 0; i < count; i++)
		bytes[i] = (uint8_t)(offset + i);
	return ISO_SCAN_OK;
}

static int DescriptorCount(void)
{
	DIR* directory = opendir("/proc/self/fd");
	int count = 0;

	CHECK(directory != NULL);
	if (directory == NULL)
		return -1;
	while (readdir(directory) != NULL)
		count++;
	closedir(directory);
	return count;
}

static void CheckPosition(struct filedes* fd)
{
	uint8_t data[16];
	uint64_t big = (UINT64_C(1) << 32) + 51;

	CHECK(filedes_is_seekable(fd));
	CHECK(full_read(fd, data, 7) == 0 && fd->offset == 7 && data[0] == 0 && data[6] == 6);
	CHECK(full_read(fd, data, 7) == 0 && fd->offset == 14 && data[0] == 7 && data[6] == 13);
	CHECK(full_pread(fd, data, 9, 200) == 0 && fd->offset == 14 && data[0] == 200 && data[8] == 208);
	CHECK(filedes_seek(fd, 5) == 5);
	CHECK(full_read(fd, data, 3) == 0 && fd->offset == 8 && data[0] == 5 && data[2] == 7);
	CHECK(filedes_seek(fd, (off_t)big) == (off_t)big);
	CHECK(full_read(fd, data, 8) == 0 && fd->offset == (off_t)big + 8 && data[0] == 51);
	CHECK(filedes_seek(fd, (off_t)fd->iso_reader->size) == (off_t)fd->iso_reader->size);
	CHECK(full_read(fd, data, 0) == 0);
}

static void CheckErrors(struct filedes* fd)
{
	uint8_t data[16];
	int before = calls;
	off_t end = fd->offset;

	memset(data, 0x5a, sizeof(data));
	CHECK(full_read(fd, data, 1) == WIMLIB_ERR_UNEXPECTED_END_OF_FILE);
	CHECK(full_pread(fd, data, SIZE_MAX, 1) == WIMLIB_ERR_UNEXPECTED_END_OF_FILE);
	CHECK(full_pread(fd, data, 1, -1) == WIMLIB_ERR_UNEXPECTED_END_OF_FILE);
	CHECK(filedes_seek(fd, -1) == -1 && errno == EINVAL);
	CHECK(filedes_seek(fd, end + 1) == -1 && fd->offset == end);
	CHECK(calls == before && data[0] == 0x5a);
	CHECK(full_write(fd, data, 1) == WIMLIB_ERR_WIM_IS_READONLY && errno == EROFS);
	CHECK(full_pwrite(fd, data, 1, 0) == WIMLIB_ERR_WIM_IS_READONLY && errno == EROFS);
	CHECK(calls == before);
	CHECK(filedes_seek(fd, 7) == 7);
	injected_result = ISO_SCAN_ERROR;
	CHECK(full_read(fd, data, 3) == WIMLIB_ERR_READ && errno == EIO && fd->offset == 7);
	injected_result = ISO_SCAN_CANCELLED;
	CHECK(full_read(fd, data, 3) == WIMLIB_ERR_ABORTED_BY_PROGRESS && errno == ECANCELED && fd->offset == 7);
	injected_result = ISO_SCAN_OK;
	cancelled = 1;
	before = calls;
	CHECK(full_pread(fd, data, 2, 0) == WIMLIB_ERR_ABORTED_BY_PROGRESS && calls == before);
	cancelled = 0;
	CHECK(full_read(fd, data, 3) == 0 && fd->offset == 10 && data[0] == 7);
}

static void CheckOpenAndCleanup(ISO_FILE_READER* reader)
{
	WIMStruct* wim = NULL;
	int i, before = DescriptorCount();

	CHECK(OpenWimFromIsoReader(reader, 0, NULL) == WIMLIB_ERR_INVALID_PARAM);
	CHECK(OpenWimFromIsoReader(NULL, 0, &wim) == WIMLIB_ERR_INVALID_PARAM && wim == NULL);
	CHECK(OpenWimFromIsoReader(reader, -1, &wim) == WIMLIB_ERR_INVALID_PARAM && wim == NULL);
	CHECK(OpenWimFromIsoReader(reader, WIMLIB_OPEN_FLAG_WRITE_ACCESS, &wim) == WIMLIB_ERR_WIM_IS_READONLY);
	cancelled = 1;
	CHECK(OpenWimFromIsoReader(reader, 0, &wim) == WIMLIB_ERR_ABORTED_BY_PROGRESS && wim == NULL);
	cancelled = 0;
	for (i = 0; i < 30; i++)
	{
		// The fake byte pattern is not a WIM. Repeated failures own no fd or reader.
		CHECK(OpenWimFromIsoReader(reader, 0, &wim) != 0 && wim == NULL);
		CHECK(reader->iso != NULL);
	}
	CHECK(DescriptorCount() == before);
}

int main(void)
{
	ISO_FILE_READER reader = { 0 };
	struct filedes fd;
	int result;

	reader.iso = (iso9660_t*)&reader; // Identity only; the wrapped reader never dereferences it.
	reader.size = (UINT64_C(1) << 32) + 100;
	filedes_init(&fd, -1);
	fd.iso_reader = &reader;
	CHECK(wimlib_global_init(WIMLIB_INIT_FLAG_ASSUME_UTF8) == 0);
	/* Check the actual provider and generated configuration after source upgrades. */
	CHECK(wimlib_get_version() == ((1u << 20) | (14u << 10) | 5u));
	CHECK(strcmp(wimlib_get_version_string(), "1.14.5-rufus-reader") == 0);
	CheckPosition(&fd);
	CheckErrors(&fd);
	CheckOpenAndCleanup(&reader);
	wimlib_global_cleanup();
	result = failures == 0 ? 0 : 1;
	printf("Native wimlib reader: %d checks, %d failures.\n", checks, failures);
	return result;
}
