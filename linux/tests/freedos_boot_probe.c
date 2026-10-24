/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "platform/fat_boot.h"
#include "image_report.h"
#include "winpe_setup.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "ms-sys/inc/file.h"

static const char* fault;
static int target, syncs, writes, patches, fail_patch, interrupted_reads;
static size_t written, prefix_size;
static int staging_patches;
static uint64_t volume_offset;
enum { INITIAL_POSITION = 17, PARTIAL_BYTES = 17, SHORT_WRITE_BYTES = 73,
	SECTOR_BYTES = 512, BACKUP_SECTOR = 6, WINDOWS_TAIL_SECTOR = 12,
	FAT32_DRIVE_OFFSET = 64, FSINFO_HINT_OFFSET = SECTOR_BYTES + 488,
	FAT16_PATCHES = 4, FAT32_PATCHES = 10, WINDOWS_FAT32_PATCHES = 12 };

int __real_fsync(int fd);
ssize_t __real_pwrite64(int fd, const void* buffer, size_t size, off64_t offset);
ssize_t __real_pread64(int fd, void* buffer, size_t size, off64_t offset);
int __real_write_data(FILE* file, uint64_t position, const void* data, uint64_t length);

int IsRufusJobCancelled(void)
{
	const bool after_record = strcmp(fault, "cancel-record") == 0 && written >= prefix_size;
	const bool after_partial = strcmp(fault, "cancel-partial") == 0 && written != 0;
	const bool after_sync = strcmp(fault, "cancel-sync") == 0 && syncs != 0;
	const bool after_staging = strcmp(fault, "cancel-staged") == 0 && patches >= staging_patches;
	const bool after_tail = strcmp(fault, "cancel-tail") == 0 && written >= 2 * prefix_size + SECTOR_BYTES;
	return strcmp(fault, "cancel-before") == 0 || after_record || after_partial || after_sync || after_staging || after_tail;
}

int IsIsoScanCancelled(void) { return IsRufusJobCancelled(); }
void _UpdateProgressWithInfo(int operation, int message, uint64_t processed, uint64_t total, int force)
{
	(void)operation; (void)message; (void)processed; (void)total; (void)force;
}

void uprintf(const char* format, ...)
{
	va_list arguments;
	va_start(arguments, format);
	vfprintf(stderr, format, arguments);
	va_end(arguments);
}

int __wrap_write_data(FILE* file, uint64_t position, const void* data, uint64_t length)
{
	patches++;
	return patches == fail_patch ? 0 : __real_write_data(file, position, data, length);
}

int __wrap_fsync(int fd)
{
	syncs++;
	const bool fail = strcmp(fault, "sync-fail") == 0 || strcmp(fault, "partial-sync-fail") == 0;
	if (fail) {
		errno = ENOSPC;
		return -1;
	}
	if (strcmp(fault, "sync-eintr") == 0 && syncs == 1) {
		errno = EINTR;
		return -1;
	}
	if (strcmp(fault, "resize") == 0) {
		struct stat info;
		if (fstat(fd, &info) != 0 || ftruncate(fd, info.st_size + 512) != 0)
			abort();
	}
	return __real_fsync(fd);
}

ssize_t __wrap_pwrite64(int fd, const void* buffer, size_t size, off64_t offset)
{
	if (fd != target)
		abort();
	writes++;
	const bool partial_failure = strcmp(fault, "partial-fail") == 0 || strcmp(fault, "partial-sync-fail") == 0;
	const bool backup_failure = strcmp(fault, "backup-fail") == 0 &&
		(uint64_t)offset == volume_offset + BACKUP_SECTOR * SECTOR_BYTES;
	const bool tail_failure = strcmp(fault, "tail-fail") == 0 &&
		(uint64_t)offset == volume_offset + WINDOWS_TAIL_SECTOR * SECTOR_BYTES;
	const bool backup_tail_failure = strcmp(fault, "backup-tail-fail") == 0 &&
		(uint64_t)offset == volume_offset + (WINDOWS_TAIL_SECTOR + BACKUP_SECTOR) * SECTOR_BYTES;
	const bool partial_tail = strcmp(fault, "partial-tail-fail") == 0 && written >= 2 * prefix_size;
	const bool after_partial_tail = partial_tail && written > 2 * prefix_size;
	if (strcmp(fault, "write-fail") == 0 || (partial_failure && written != 0) || backup_failure ||
		tail_failure || backup_tail_failure || after_partial_tail) {
		errno = EIO;
		return -1;
	}
	if (strcmp(fault, "write-zero") == 0)
		return 0;
	if (strcmp(fault, "write-eintr") == 0 && writes == 1) {
		errno = EINTR;
		return -1;
	}
	if (partial_failure || partial_tail || strcmp(fault, "cancel-partial") == 0)
		size = size > PARTIAL_BYTES ? PARTIAL_BYTES : size;
	if (strcmp(fault, "short-write") == 0)
		size = size > SHORT_WRITE_BYTES ? SHORT_WRITE_BYTES : size;
	ssize_t count = __real_pwrite64(fd, buffer, size, offset);
	if (count > 0)
		written += (size_t)count;
	return count;
}

ssize_t __wrap_pread64(int fd, void* buffer, size_t size, off64_t offset)
{
	const bool readback = syncs != 0;
	const bool backup = (uint64_t)offset == volume_offset + BACKUP_SECTOR * SECTOR_BYTES && size == prefix_size;
	const bool tail = (uint64_t)offset == volume_offset + WINDOWS_TAIL_SECTOR * SECTOR_BYTES && size == SECTOR_BYTES;
	const bool backup_tail = (uint64_t)offset == volume_offset +
		(WINDOWS_TAIL_SECTOR + BACKUP_SECTOR) * SECTOR_BYTES && size == SECTOR_BYTES;
	const bool tail_read_failure = !readback && ((strcmp(fault, "tail-read-fail") == 0 && tail) ||
		(strcmp(fault, "backup-tail-read-fail") == 0 && backup_tail));
	const bool tail_verify_failure = readback && ((strcmp(fault, "verify-tail-fail") == 0 && tail) ||
		(strcmp(fault, "verify-backup-tail-fail") == 0 && backup_tail));
	const bool read_failure = strcmp(fault, "read-fail") == 0 ||
		(strcmp(fault, "backup-read-fail") == 0 && backup) ||
		(strcmp(fault, "verify-fail") == 0 && readback) ||
		(strcmp(fault, "verify-backup-fail") == 0 && readback && backup);
	if (read_failure || tail_read_failure || tail_verify_failure) {
		errno = EIO;
		return -1;
	}
	if (strcmp(fault, "verify-eof") == 0 && readback)
		return 0;
	if (strcmp(fault, "read-eintr") == 0 && size == prefix_size && interrupted_reads++ == 0) {
		errno = EINTR;
		return -1;
	}
	// This fault is specific to the FAT32 staged prefix, not the formatter's
	// sector reader. The adapter must continue after a short positive read.
	if (strcmp(fault, "short-read") == 0 && size > 512)
		size = 512;
	ssize_t count = __real_pread64(fd, buffer, size, offset);
	const bool primary = (uint64_t)offset == volume_offset && size == prefix_size;
	const bool corrupt_drive = (strcmp(fault, "verify-drive-corrupt") == 0 && primary) ||
		(strcmp(fault, "verify-backup-drive-corrupt") == 0 && backup);
	const bool corrupt_hints = (strcmp(fault, "verify-hints-corrupt") == 0 && primary) ||
		(strcmp(fault, "verify-backup-hints-corrupt") == 0 && backup);
	if (readback && count > FAT32_DRIVE_OFFSET && corrupt_drive)
		((unsigned char*)buffer)[FAT32_DRIVE_OFFSET] ^= 1;
	if (readback && count > FSINFO_HINT_OFFSET && corrupt_hints)
		((unsigned char*)buffer)[FSINFO_HINT_OFFSET] ^= 1;
	const bool corrupt = strcmp(fault, "verify-corrupt") == 0 ||
		(strcmp(fault, "verify-backup-corrupt") == 0 && backup) ||
		(strcmp(fault, "verify-tail-corrupt") == 0 && tail) ||
		(strcmp(fault, "verify-backup-tail-corrupt") == 0 && backup_tail);
	if (count > 0 && readback && corrupt)
		((unsigned char*)buffer)[count - 1] ^= 1;
	return count;
}

static int DescriptorCount(void)
{
	DIR* directory = opendir("/proc/self/fd");
	if (directory == NULL)
		abort();
	int count = 0;
	struct dirent* entry;
	while ((entry = readdir(directory)) != NULL)
		if (entry->d_name[0] != '.')
			count++;
	closedir(directory);
	return count;
}

static int CheckMemoryBounds(void)
{
	unsigned char buffer[19], actual[16];
	memset(buffer, 0xa5, sizeof(buffer));
	FILE* stream = fmemopen(buffer + 1, 17, "r+b");
	if (stream == NULL)
		return 1;
	const char data[] = "four";
	const bool rejects_overflow = !write_data(stream, UINT64_MAX, data, 4) &&
		!read_data(stream, 1, actual, UINT64_MAX);
	const bool rejects_end = !write_data(stream, 16, data, 1) && !read_data(stream, 15, actual, 2);
	const bool accepts_last = write_data(stream, 12, data, 4) && contains_data(stream, 12, data, 4);
	const bool closes = fclose(stream) == 0;
	const bool guards_intact = buffer[0] == 0xa5 && buffer[18] == 0xa5;
	const bool bytes_match = memcmp(buffer + 13, data, 4) == 0;
	return rejects_overflow && rejects_end && accepts_last && closes && guards_intact && bytes_match ? 0 : 1;
}

int main(int argc, char** argv)
{
	if (argc == 2 && strcmp(argv[1], "--memory-bounds") == 0)
		return CheckMemoryBounds();
	if (argc != 8 && argc != 9)
		return 64;
	FAT_FORMAT_OPTIONS options = { 0 };
	options.offset = strtoull(argv[2], NULL, 10);
	options.size = strtoull(argv[3], NULL, 10);
	options.cluster_size = (uint32_t)strtoul(argv[4], NULL, 10);
	options.volume_id = 0x1234abcd;
	const char* logical_geometry = getenv("RUFUS_TEST_SECTOR_SIZE");
	const char* physical_geometry = getenv("RUFUS_TEST_PHYSICAL_SECTOR_SIZE");
	if (logical_geometry != NULL) options.sector_size = (uint32_t)strtoul(logical_geometry, NULL, 10);
	if (physical_geometry != NULL) options.physical_sector_size = (uint32_t)strtoul(physical_geometry, NULL, 10);
	snprintf(options.label, sizeof(options.label), "%s", argv[5]);
	const int bits = atoi(argv[6]);
	const int file_system = bits == 16 ? FS_FAT16 : bits == 32 ? FS_FAT32 : FS_NTFS;
	prefix_size = bits == 16 ? SECTOR_BYTES : 3 * SECTOR_BYTES;
	volume_offset = options.offset;
	fault = argv[7];
	if (strncmp(fault, "patch-", 6) == 0)
		fail_patch = atoi(fault + 6);
	int flags = strcmp(fault, "readonly") == 0 ? O_RDONLY : O_RDWR;
	if (strcmp(fault, "append") == 0)
		flags |= O_APPEND;
	if (strcmp(fault, "direct") == 0)
		flags |= O_DIRECT;
	target = open(argv[1], flags | O_CLOEXEC);
	if (target < 0 || lseek(target, INITIAL_POSITION, SEEK_SET) < 0)
		return 65;
	if (strcmp(fault, "serial") == 0)
		options.volume_id++;
	const int descriptors = DescriptorCount();
	const int original_flags = fcntl(target, F_GETFL);
	FAT_BOOT_RESULT result = { 0 };
	FAT_BOOT_CODE boot_code = argc == 9 ? FAT_BOOT_STANDARD : FAT_BOOT_FREEDOS;
	if (argc == 9 && strcmp(argv[8], "windows") == 0)
		boot_code = FAT_BOOT_WINDOWS;
	if (argc == 9 && strcmp(argv[8], "winpe") == 0)
		boot_code = FAT_BOOT_WINPE;
	const char* provider = argc == 9 ? argv[8] : "";
	const bool winpe_i386 = strcmp(provider, "winpe-bootmgr-i386") == 0;
	const bool winpe_amd64 = strcmp(provider, "winpe-bootmgr-amd64") == 0;
	const bool winpe_minint = strcmp(provider, "winpe-bootmgr-minint") == 0;
	const bool winpe_bootmgr = winpe_i386 || winpe_amd64 || winpe_minint;
	if (winpe_bootmgr)
		boot_code = FAT_BOOT_WINDOWS;
	const bool windows_tail = boot_code == FAT_BOOT_WINDOWS || boot_code == FAT_BOOT_WINPE;
	staging_patches = bits == 16 ? FAT16_PATCHES :
		windows_tail ? WINDOWS_FAT32_PATCHES : FAT32_PATCHES;
	if (strcmp(fault, "invalid-code") == 0)
		boot_code = (FAT_BOOT_CODE)-1;
	const FAT_FORMAT_OPTIONS* request = strcmp(fault, "null-options") == 0 ? NULL : &options;
	FAT_BOOT_RESULT* output = strcmp(fault, "null-result") == 0 ? NULL : &result;
	int error;
	if (winpe_bootmgr) {
		const uint16_t family = winpe_minint ? WINPE_MININT : winpe_amd64 ? WINPE_AMD64 : WINPE_I386;
		const WINPE_SETUP_PLAN plan = GetWinPeSetupPlan(family, winpe_minint);
		error = InstallWinPeFatBootRecords(target, plan.masquerade_disk, request, output);
	} else {
		error = InstallFatBootRecords(target, file_system, boot_code, request, output);
	}
	printf("ERROR=%d BYTES=%" PRIu64 " SYNC=%d SYNCS=%d WRITES=%d PATCHES=%d FDS=%d OFFSET=%jd FLAGS=%d\n",
		error, result.bytes_written, result.sync_error, syncs, writes, patches,
		DescriptorCount() - descriptors, (intmax_t)lseek(target, 0, SEEK_CUR),
		fcntl(target, F_GETFL) == original_flags && fcntl(target, F_GETFD) == FD_CLOEXEC);
	close(target);
	return 0;
}
