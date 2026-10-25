/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Writes only the disposable regular files supplied by test_raw_write.py. */
#include "platform/uefi_ntfs.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int active, cancelled, source_fd, target_fd, reads, writes, syncs, synced;
static const char* fault;
static const char* source_path;
static IMAGE_FILE_SOURCE source = { .fd = -1 };
static RAW_WRITE_RESULT result;
static uint64_t target_offset;

enum {
	MAX_TRANSFER_BYTES = 1024 * 1024,
	SHORT_TRANSFER_BYTES = 70013, // Deliberately crosses sectors and buffer boundaries.
	PARTIAL_WRITE_BYTES = 4096,
	SOURCE_POSITION = 7,
	TARGET_POSITION = 13,
	CORRUPTION_OFFSET = 23
};

int IsRufusJobCancelled(void) { return cancelled; }
int IsIsoScanCancelled(void) { return IsRufusJobCancelled(); }
void uprintf(const char* format, ...)
{
	va_list args;
	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);
}
void _UpdateProgressWithInfo(int operation, int message, uint64_t processed, uint64_t total, int forced)
{
	(void)operation; (void)message; (void)processed; (void)total; (void)forced;
}

#if defined(__GLIBC__) && defined(__USE_FILE_OFFSET64)
#define WrappedRead __wrap_pread64
#define RealRead __real_pread64
#define WrappedWrite __wrap_pwrite64
#define RealWrite __real_pwrite64
#else
#define WrappedRead __wrap_pread
#define RealRead __real_pread
#define WrappedWrite __wrap_pwrite
#define RealWrite __real_pwrite
#endif
ssize_t RealRead(int, void*, size_t, off_t);
ssize_t RealWrite(int, const void*, size_t, off_t);
int __real_fsync(int);

ssize_t WrappedRead(int fd, void* buffer, size_t size, off_t offset)
{
	ssize_t count;
	if (!active)
		return RealRead(fd, buffer, size, offset);
	reads++;
	if (size > MAX_TRANSFER_BYTES)
		abort();
	if (strcmp(fault, "eintr") == 0 && reads == 1) {
		errno = EINTR;
		return -1;
	}
	if (strcmp(fault, "short") == 0 && size > SHORT_TRANSFER_BYTES)
		size = SHORT_TRANSFER_BYTES;
	if (strcmp(fault, "read-fail") == 0 && result.bytes_written != 0) {
		errno = EIO;
		return -1;
	}
	if (strcmp(fault, "early-eof") == 0 && result.bytes_written != 0)
		return 0;
	if (fd == target_fd && result.phase == RAW_WRITE_VERIFY) {
		if (!synced)
			abort();
		if (strcmp(fault, "verify-fail") == 0) {
			errno = EIO;
			return -1;
		}
		if (strcmp(fault, "verify-cancel") == 0)
			cancelled = 1;
	}
	count = RealRead(fd, buffer, size, offset);
	return count;
}

ssize_t WrappedWrite(int fd, const void* buffer, size_t size, off_t offset)
{
	ssize_t count;
	if (!active)
		return RealWrite(fd, buffer, size, offset);
	if (fd != target_fd || result.phase != RAW_WRITE_COPY || size > MAX_TRANSFER_BYTES)
		abort();
	writes++;
	if (strcmp(fault, "eintr") == 0 && writes == 1) {
		errno = EINTR;
		return -1;
	}
	if (strcmp(fault, "short") == 0 && size > SHORT_TRANSFER_BYTES)
		size = SHORT_TRANSFER_BYTES;
	if (strcmp(fault, "zero-write") == 0)
		return 0;
	if (strcmp(fault, "write-fail") == 0 || strcmp(fault, "double-fail") == 0) {
		if (writes > 1) {
			errno = ENOSPC;
			return -1;
		}
		if (size > PARTIAL_WRITE_BYTES)
			size = PARTIAL_WRITE_BYTES;
	}
	if (strcmp(fault, "write-cancel") == 0 && size > PARTIAL_WRITE_BYTES)
		size = PARTIAL_WRITE_BYTES;
	count = RealWrite(fd, buffer, size, offset);
	if (strcmp(fault, "write-cancel") == 0)
		cancelled = 1;
	return count;
}

int __wrap_fsync(int fd)
{
	uint8_t byte;
	int error, changed;
	struct timespec times[2] = { source.info.st_atim, source.info.st_mtim };
	if (!active)
		return __real_fsync(fd);
	if (fd != target_fd || result.bytes_written == 0 || result.bytes_verified != 0)
		abort();
	syncs++;
	if (strcmp(fault, "eintr") == 0 && syncs == 1) {
		errno = EINTR;
		return -1;
	}
	if (strcmp(fault, "sync-fail") == 0 || strcmp(fault, "double-fail") == 0) {
		errno = EIO;
		return -1;
	}
	if (strcmp(fault, "sync-cancel") == 0)
		cancelled = 1;
	if (strcmp(fault, "corrupt") == 0) {
		if (RealRead(fd, &byte, 1, target_offset + CORRUPTION_OFFSET) != 1)
			abort();
		byte ^= 1;
		if (RealWrite(fd, &byte, 1, target_offset + CORRUPTION_OFFSET) != 1)
			abort();
	}
	if (strcmp(fault, "source-change") == 0) {
		changed = open(source_path, O_WRONLY);
		if (changed < 0 || RealRead(source_fd, &byte, 1, 0) != 1)
			abort();
		byte ^= 1;
		if (RealWrite(changed, &byte, 1, 0) != 1 || futimens(changed, times) != 0)
			abort();
		close(changed);
	}
	if (strcmp(fault, "target-resize") == 0 && ftruncate(fd, source.info.st_size) != 0)
		abort();
	if (strcmp(fault, "gpt-change") == 0 && RealWrite(fd, "bad", 3, 512) != 3)
		abort();
	if (strcmp(fault, "mbr-change") == 0 && RealWrite(fd, "bad", 3, 466) != 3)
		abort();
	error = __real_fsync(fd);
	if (error == 0)
		synced = 1;
	return error;
}

static int DescriptorCount(void)
{
	DIR* directory = opendir("/proc/self/fd");
	int count = 0;
	if (!directory)
		abort();
	while (readdir(directory)) count++;
	closedir(directory);
	return count;
}

int main(int argc, char** argv)
{
	int error, before = DescriptorCount(), flags = O_RDWR;
	GPT_IMAGE_OPTIONS options = { 0, 512, 63, 4096, 0, 0 , 0, false, false, 0};
	GPT_IMAGE_RESULT layout;
	MBR_IMAGE_OPTIONS mbr = {0};
	const bool exfat_payload = argc == 9 && strcmp(argv[4], "uefi-exfat-mbr") == 0;
	const bool mbr_payload = exfat_payload || (argc == 9 && strcmp(argv[4], "uefi-ntfs-mbr") == 0);
	if (argc < 3 || (argc > 6 && argc != 9))
		return 64;
	if (argc == 9) {
		if (!mbr_payload && strcmp(argv[4], "uefi-ntfs") != 0)
			return 64;
		options.disk_size = strtoull(argv[5], NULL, 10);
		options.cluster_size = (uint32_t)strtoul(argv[6], NULL, 10);
		options.sectors_per_track = (uint32_t)strtoul(argv[7], NULL, 10);
		options.uefi_ntfs_size = strtoull(argv[8], NULL, 10);
		if (mbr_payload) {
			mbr = (MBR_IMAGE_OPTIONS){options.disk_size, options.sector_size, options.sectors_per_track,
				options.cluster_size, 0, 0, exfat_payload ? FS_EXFAT : FS_NTFS, MBR_BOOT_ZERO, options.uefi_ntfs_size, 0, false, false, false, 0};
			MBR_IMAGE_RESULT planned;
			if (PlanMbrImageLayout(&mbr, &planned) == 0)
				target_offset = planned.uefi_ntfs_offset;
		} else if (PlanGptImageLayout(&options, &layout) == 0)
			target_offset = layout.uefi_ntfs_offset;
	} else if (argc == 6) {
		target_offset = strtoull(argv[4], NULL, 10);
	}
	source_path = argv[1];
	fault = argc >= 4 ? argv[3] : "none";
	error = OpenImageFileSource(source_path, &source);
	if (error != 0)
		return 65;
	source_fd = source.fd;
	if (strcmp(fault, "readonly") == 0)
		flags = O_RDONLY;
	if (strcmp(fault, "writeonly") == 0)
		flags = O_WRONLY;
	if (strcmp(fault, "append") == 0)
		flags |= O_APPEND;
	target_fd = open(argv[2], flags | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW);
	if (target_fd < 0)
		return 66;
	if (lseek(source_fd, SOURCE_POSITION, SEEK_SET) != SOURCE_POSITION)
		abort();
	// A FIFO is deliberately passed through to the operation's type check.
	if (strcmp(fault, "fifo") != 0 && lseek(target_fd, TARGET_POSITION, SEEK_SET) != TARGET_POSITION)
		abort();
	if (argc == 5 && strcmp(fault, "source-replace") == 0 && rename(argv[4], source_path) != 0)
		abort();
	if (strcmp(fault, "cancel-before") == 0)
		cancelled = 1;
	if (strcmp(fault, "block-request") == 0)
		options.device = mbr.device = 1;
	active = 1;
	if (mbr_payload)
		error = WriteMbrUefiNtfsImage(&source, source_path, target_fd, &mbr, &result);
	else if (argc == 9)
		error = WriteUefiNtfsImage(&source, source_path, target_fd, &options, &result);
	else if (argc == 6)
		error = WriteRawImageToExtent(&source, source_path, target_fd, target_offset,
			strtoull(argv[5], NULL, 10), &result);
	else
		error = WriteRawImage(&source, source_path, target_fd, &result);
	active = 0;
	if (lseek(source_fd, 0, SEEK_CUR) != SOURCE_POSITION)
		abort();
	if (strcmp(fault, "fifo") != 0 && lseek(target_fd, 0, SEEK_CUR) != TARGET_POSITION)
		abort();
	printf("{\"error\":%d,\"phase\":%d,\"written\":%llu,\"verified\":%llu,"
		"\"sync_error\":%d,\"syncs\":%d}\n", error, result.phase,
		(unsigned long long)result.bytes_written, (unsigned long long)result.bytes_verified,
		result.sync_error, syncs);
	CloseImageFileSource(&source);
	if (close(target_fd) != 0 || DescriptorCount() != before)
		abort();
	return 0;
}
