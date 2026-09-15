/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "iso_file.h"

#include <stdio.h>
#include <string.h>

static ISO_FILE_READER reader;
static uint64_t position;
static unsigned int checks, failures, reads, seeks, fail_at, cancel_at;
static bool cancelled, short_read, fragmented;
static uint8_t buffer[2 * ISO_FILE_CACHE_SIZE + 1];

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

static void FillPattern(void* data, uint64_t offset, size_t count)
{
	uint8_t* bytes = data;
	size_t i;

	for (i = 0; i < count; i++)
		bytes[i] = (uint8_t)((offset + i) % 251);
}

long __wrap_iso9660_iso_seek_read(const iso9660_t* iso, void* data, lsn_t lsn, long blocks)
{
	uint64_t offset = ((uint64_t)lsn - reader.lsn) * ISO_BLOCKSIZE;

	CHECK(iso == reader.iso && lsn >= reader.lsn);
	CHECK(blocks > 0 && blocks <= 16);
	CHECK(offset < reader.size && (uint64_t)blocks <= (reader.size - offset + 2047) / 2048);
	reads++;
	if (reads == cancel_at)
		cancelled = true;
	if (reads == fail_at)
		return 0;
	FillPattern(data, offset, (size_t)blocks * ISO_BLOCKSIZE);
	return blocks * ISO_BLOCKSIZE - (short_read ? 1 : 0);
}

bool __wrap_udf_setpos(udf_dirent_t* file, off_t offset)
{
	CHECK(file == reader.udf_file);
	seeks++;
	if (offset < 0 || (uint64_t)offset > reader.size || offset % UDF_BLOCKSIZE != 0)
		return false;
	position = (uint64_t)offset;
	return true;
}

ssize_t __wrap_udf_read_block(const udf_dirent_t* file, void* data, size_t blocks)
{
	uint64_t count = blocks * UDF_BLOCKSIZE;

	CHECK(file == reader.udf_file && blocks > 0 && blocks <= 16);
	CHECK(position < reader.size);
	if (count > reader.size - position)
		count = reader.size - position;
	if (fragmented && count > UDF_BLOCKSIZE)
		count = UDF_BLOCKSIZE;
	reads++;
	if (reads == cancel_at)
		cancelled = true;
	if (reads == fail_at)
		return -1;
	FillPattern(data, position, (size_t)count);
	position += count;
	return (ssize_t)count - (short_read ? 1 : 0);
}

static void ResetReader(bool udf, uint64_t size)
{
	memset(&reader, 0, sizeof(reader));
	reader.size = size;
	reader.lsn = 24;
	if (udf)
		reader.udf_file = (udf_dirent_t*)(uintptr_t)1;
	else
		reader.iso = (iso9660_t*)(uintptr_t)1;
	position = 0;
	reads = seeks = fail_at = cancel_at = 0;
	cancelled = short_read = fragmented = false;
}

static void CheckRange(uint64_t offset, size_t count)
{
	size_t i;
	bool match = true;

	memset(buffer, 0x5a, sizeof(buffer));
	CHECK(ReadIsoFileRange(&reader, offset, buffer, count) == ISO_SCAN_OK);
	for (i = 0; i < count; i++)
		if (buffer[i] != (offset + i) % 251)
			match = false;
	CHECK(match);
	CHECK(buffer[count] == 0x5a);
}

static void CheckRangesAndCache(bool udf)
{
	ResetReader(udf, 3 * ISO_FILE_CACHE_SIZE + 13);
	CheckRange(0, 1);
	CHECK(reads == 1);
	CheckRange(32700, 10);
	CheckRange(7, 21);
	CHECK(reads == 1);
	CheckRange(32767, 2);
	CHECK(reads == 2);
	CheckRange(4095, 65536);
	CheckRange(reader.size - 13, 13);
	CHECK(ReadIsoFileRange(&reader, reader.size, NULL, 0) == ISO_SCAN_OK);
	if (udf)
	{
		ResetReader(true, 3 * ISO_FILE_CACHE_SIZE + 13);
		fragmented = true;
		CheckRange(4095, 65536);
		CHECK(reads > 16);
	}
	// Check address arithmetic beyond 4 GiB without allocating a huge image.
	ResetReader(udf, UINT64_C(6) * 1024 * 1024 * 1024);
	CheckRange(UINT64_C(4294967296) + 17, 100);
	CHECK(reads == 1);
}

static void CheckBounds(bool udf)
{
	static const struct { uint64_t offset; size_t count; } ranges[] = {
		{ 32768, 1 }, { 32769, 0 }, { UINT64_MAX, 1 },
		{ UINT64_MAX - 10, 20 }, { 1, SIZE_MAX }
	};
	size_t i;

	ResetReader(udf, 32768);
	for (i = 0; i < sizeof(ranges) / sizeof(ranges[0]); i++)
	{
		memset(buffer, 0x5a, sizeof(buffer));
		CHECK(ReadIsoFileRange(&reader, ranges[i].offset, buffer, ranges[i].count) == ISO_SCAN_ERROR);
		CHECK(buffer[0] == 0x5a && reads == 0 && seeks == 0);
	}
	CHECK(ReadIsoFileRange(&reader, 0, NULL, 1) == ISO_SCAN_ERROR);
	CHECK(ReadIsoFileRange(NULL, 0, buffer, 1) == ISO_SCAN_ERROR);
	ResetReader(udf, 0);
	CHECK(ReadIsoFileRange(&reader, 0, NULL, 0) == ISO_SCAN_OK && reads == 0);
	CHECK(ReadIsoFileRange(&reader, 0, buffer, 1) == ISO_SCAN_ERROR && reads == 0);
	if (!udf)
	{
		ResetReader(false, 32768);
		reader.lsn = INT32_MAX;
		CHECK(ReadIsoFileRange(&reader, 0, buffer, 1) == ISO_SCAN_PARTIAL && reads == 0);
	}
}

static void CheckFailureAndRecovery(bool udf)
{
	ResetReader(udf, 3 * ISO_FILE_CACHE_SIZE);
	fail_at = 2;
	CHECK(ReadIsoFileRange(&reader, 0, buffer, 2 * ISO_FILE_CACHE_SIZE) == ISO_SCAN_PARTIAL);
	CHECK(reader.cache_size == 0 && reads == 2);
	fail_at = 0;
	CheckRange(0, 65536);
	CHECK(reads == 4);
	ResetReader(udf, 32768);
	short_read = true;
	CHECK(ReadIsoFileRange(&reader, 0, buffer, 10) == ISO_SCAN_PARTIAL);
	CHECK(reader.cache_size == 0);
	short_read = false;
	CheckRange(0, 10);
}

static void CheckCancellation(bool udf)
{
	ResetReader(udf, 65536);
	cancelled = true;
	CHECK(ReadIsoFileRange(&reader, 0, buffer, 1) == ISO_SCAN_CANCELLED && reads == 0);
	cancelled = false;
	CheckRange(0, 1);
	cancelled = true;
	CHECK(ReadIsoFileRange(&reader, 1, buffer, 1) == ISO_SCAN_CANCELLED && reads == 1);
	cancelled = false;
	cancel_at = 2;
	CHECK(ReadIsoFileRange(&reader, 32768, buffer, 1) == ISO_SCAN_CANCELLED);
	CHECK(reader.cache_size == 0 && reads == 2);
	cancelled = false;
	cancel_at = 0;
	CheckRange(32768, 1);
	CHECK(reads == 3);
	if (udf)
	{
		ResetReader(true, 65536);
		fragmented = true;
		cancel_at = 3;
		CHECK(ReadIsoFileRange(&reader, 0, buffer, 32768) == ISO_SCAN_CANCELLED);
		CHECK(reader.cache_size == 0 && reads == 3);
	}
}

int main(void)
{
	int udf;

	for (udf = 0; udf <= 1; udf++)
	{
		CheckRangesAndCache(udf != 0);
		CheckBounds(udf != 0);
		CheckFailureAndRecovery(udf != 0);
		CheckCancellation(udf != 0);
	}
	memset(&reader, 0, sizeof(reader));
	CHECK(OpenIsoFileReader(NULL, NULL, NULL, NULL) == ISO_SCAN_ERROR);
	CHECK(OpenIsoFileReader(&reader, NULL, NULL, "file") == ISO_SCAN_ERROR);
	CHECK(OpenIsoFileReader(&reader, (iso9660_t*)(uintptr_t)1,
		(udf_t*)(uintptr_t)1, "file") == ISO_SCAN_ERROR);
	CHECK(OpenIsoFileReader(&reader, (iso9660_t*)(uintptr_t)1, NULL, NULL) == ISO_SCAN_ERROR);
	CHECK(ReadIsoFileRange(&reader, 0, buffer, 1) == ISO_SCAN_ERROR);
	CloseIsoFileReader(NULL);
	CloseIsoFileReader(&reader);
	CloseIsoFileReader(&reader);
	printf("%u ISO/UDF range, cache, cancellation and failure checks; %u failures\n", checks, failures);
	return failures != 0;
}
