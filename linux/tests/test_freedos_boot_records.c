/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Reuse probe only: unchanged Rufus ms-sys code operates on bounded memory
 * streams. This is not a production device I/O adapter or a boot test. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "file.h"
#include "fat16.h"
#include "fat32.h"
#include "partition_info.h"

enum { SECTOR_BYTES = 512, PREFIX_BYTES = 4 * SECTOR_BYTES, GUARD_BYTE = 0xa5,
	BPB_START = 11, FAT_LABEL_BYTES = 11 };
static int checks, failures, write_calls, fail_write;

#define CHECK(expression) do { checks++; if (!(expression)) { \
	fprintf(stderr, "line %d: %s\n", __LINE__, #expression); failures++; } } while (0)

/* These are the only ms-sys I/O symbols needed by the unmodified FAT sources.
 * Use real FILE streams, with no Win32 HANDLE or FAKE_FD emulation. */
int read_data(FILE* stream, uint64_t position, void* data, uint64_t length)
{
	if (position > PREFIX_BYTES || length > PREFIX_BYTES - position)
		return 0;
	if (fseeko(stream, (off_t)position, SEEK_SET) != 0)
		return 0;
	return fread(data, 1, (size_t)length, stream) == length;
}

int contains_data(FILE* stream, uint64_t position, const void* data, uint64_t length)
{
	unsigned char buffer[PREFIX_BYTES];
	if (length > sizeof(buffer) || !read_data(stream, position, buffer, length))
		return 0;
	return memcmp(buffer, data, (size_t)length) == 0;
}

int write_data(FILE* stream, uint64_t position, const void* data, uint64_t length)
{
	write_calls++;
	if (write_calls == fail_write)
		return 0;
	if (position > PREFIX_BYTES || length > PREFIX_BYTES - position)
		return 0;
	if (fseeko(stream, (off_t)position, SEEK_SET) != 0)
		return 0;
	return fwrite(data, 1, (size_t)length, stream) == length;
}

static int WriteBootRecord(FILE* stream, bool fat16, bool keep_label)
{
	const int written = fat16 ? write_fat_16_fd_br(stream, keep_label)
		: write_fat_32_fd_br(stream, keep_label);
	if (!written)
		return 0;
	return fat16 ? write_partition_physical_disk_drive_id_fat16(stream)
		: write_partition_physical_disk_drive_id_fat32(stream);
}

static void CheckPreservedBytes(const unsigned char* before, const unsigned char* after,
	bool fat16, bool keep_label)
{
	const unsigned drive_offset = fat16 ? 0x24 : 0x40;
	const unsigned label_offset = fat16 ? 0x2b : 0x47;
	const unsigned bpb_end = fat16 ? 0x3e : 0x52;
	// FAT32 rebuilds the reserved-sector prefix, preserving the FSInfo free
	// cluster count and next-free hint at offsets 488 and 492 within sector 1.
	for (unsigned i = BPB_START; i < PREFIX_BYTES; i++) {
		const bool bpb = i < bpb_end;
		const bool untouched_tail = i >= (fat16 ? SECTOR_BYTES : 3 * SECTOR_BYTES);
		const bool fsinfo = !fat16 && i >= SECTOR_BYTES + 488 && i < SECTOR_BYTES + 496;
		const bool changed_label = !keep_label && i >= label_offset && i < label_offset + FAT_LABEL_BYTES;
		if ((bpb || untouched_tail || fsinfo) && i != drive_offset && !changed_label)
			CHECK(before[i] == after[i]);
	}
	CHECK(after[drive_offset] == 0x80);
	if (!keep_label)
		CHECK(memcmp(after + label_offset, "NO NAME    ", FAT_LABEL_BYTES) == 0);
	static const char kernel_name[] = "KERNEL  SYS";
	CHECK(memmem(after, PREFIX_BYTES, kernel_name, sizeof(kernel_name) - 1) != NULL);
	const unsigned boot_sectors = fat16 ? 1 : 3;
	for (unsigned i = 0; i < boot_sectors; i++) {
		CHECK(after[(i + 1) * SECTOR_BYTES - 2] == 0x55);
		CHECK(after[(i + 1) * SECTOR_BYTES - 1] == 0xaa);
	}
}

static void TestBootRecord(bool fat16, bool keep_label)
{
	unsigned char guarded[PREFIX_BYTES + 2], original[PREFIX_BYTES];
	memset(original, GUARD_BYTE, sizeof(original));
	const unsigned magic_offset = fat16 ? 0x36 : 0x52;
	memcpy(original + magic_offset, fat16 ? "FAT16   " : "FAT32   ", 8);
	int total_writes = 0;
	// First succeed, then fail each original write call in turn. The adapter
	// must not turn a failed payload or drive-ID write into success.
	for (int fault = 0; fault <= total_writes; fault++) {
		memset(guarded, GUARD_BYTE, sizeof(guarded));
		memcpy(guarded + 1, original, sizeof(original));
		FILE* stream = fmemopen(guarded + 1, PREFIX_BYTES, "r+b");
		CHECK(stream != NULL);
		if (stream == NULL)
			return;
		write_calls = 0;
		fail_write = fault;
		CHECK(fat16 ? is_fat_16_fs(stream) : is_fat_32_fs(stream));
		const int written = WriteBootRecord(stream, fat16, keep_label);
		CHECK((written != 0) == (fault == 0));
		if (fault == 0) {
			total_writes = write_calls;
			CHECK(fat16 ? entire_fat_16_fd_br_matches(stream) : entire_fat_32_fd_br_matches(stream));
		} else {
			CHECK(write_calls == fault);
		}
		CHECK(fclose(stream) == 0);
		CHECK(guarded[0] == GUARD_BYTE);
		CHECK(guarded[sizeof(guarded) - 1] == GUARD_BYTE);
		if (fault == 0)
			CheckPreservedBytes(original, guarded + 1, fat16, keep_label);
	}
	printf("FAT%d, keep label %d: native reuse and %d failed-write positions checked\n",
		fat16 ? 16 : 32, keep_label, total_writes);
}

int main(void)
{
	TestBootRecord(true, false);
	TestBootRecord(true, true);
	TestBootRecord(false, false);
	TestBootRecord(false, true);
	printf("%d checks, %d failures\n", checks, failures);
	return failures != 0;
}
