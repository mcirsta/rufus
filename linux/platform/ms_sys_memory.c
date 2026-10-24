/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "ms-sys/inc/file.h"

enum { MAX_RECORD_BYTES = 8192 }; // Covers each original NTFS/FAT payload region, including FAT32 PE.

/* Rufus's ms-sys writers operate on bounded staged memory, not FAKE_FD or
 * target descriptors. The extra byte belongs to fmemopen's final NUL. */
static int SeekRecordData(FILE* file, uint64_t position, uint64_t length)
{
	if (fseeko(file, 0, SEEK_END) != 0)
		return 0;
	const off_t end = ftello(file);
	if (end <= 0)
		return 0;
	const uint64_t capacity = (uint64_t)end - 1;
	const bool position_fits = position <= capacity;
	const bool data_fits = position_fits && length <= capacity - position;
	if (!data_fits)
		return 0;
	return fseeko(file, (off_t)position, SEEK_SET) == 0;
}

int read_data(FILE* file, uint64_t position, void* data, uint64_t length)
{
	if (!SeekRecordData(file, position, length))
		return 0;
	return fread(data, 1, (size_t)length, file) == length;
}

int contains_data(FILE* file, uint64_t position, const void* data, uint64_t length)
{
	unsigned char actual[MAX_RECORD_BYTES];
	if (length > sizeof(actual))
		return 0;
	return read_data(file, position, actual, length) && memcmp(actual, data, (size_t)length) == 0;
}

int write_data(FILE* file, uint64_t position, const void* data, uint64_t length)
{
	if (!SeekRecordData(file, position, length))
		return 0;
	return fwrite(data, 1, (size_t)length, file) == length;
}
