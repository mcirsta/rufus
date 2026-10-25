/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "platform/partition_image.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum { SECTOR_BYTES = 512, INITIAL_POSITION = 17, PARTIAL_BYTES = 19,
	SHORT_BYTES = 73, TRANSFER_BYTES = 64 * 1024, MAX_PAYLOAD_BYTES = 2 * 1024 * 1024 };
static const char* fault;
static int target, syncs, writes, reads;
static uint64_t written;
int __real_fsync(int);
ssize_t __real_pread64(int, void*, size_t, off64_t);
ssize_t __real_pwrite64(int, const void*, size_t, off64_t);

int IsRufusJobCancelled(void)
{
	const bool partial = strcmp(fault, "cancel-partial") == 0 && written != 0;
	const bool chunk = strcmp(fault, "cancel-chunk") == 0 && written >= TRANSFER_BYTES;
	const bool synced = strcmp(fault, "cancel-sync") == 0 && syncs != 0;
	return strcmp(fault, "cancel-before") == 0 || partial || chunk || synced;
}

void uprintf(const char* format, ...) { (void)format; }

ssize_t __wrap_pwrite64(int fd, const void* data, size_t size, off64_t offset)
{
	if (fd != target || offset < SECTOR_BYTES)
		abort();
	writes++;
	const bool partial = strcmp(fault, "partial-fail") == 0 || strcmp(fault, "partial-sync-fail") == 0;
	if (strcmp(fault, "write-fail") == 0 || (partial && written != 0)) {
		errno = EIO;
		return -1;
	}
	if (strcmp(fault, "write-zero") == 0)
		return 0;
	if (strcmp(fault, "write-eintr") == 0 && writes == 1) {
		errno = EINTR;
		return -1;
	}
	if (partial || strcmp(fault, "cancel-partial") == 0)
		size = size > PARTIAL_BYTES ? PARTIAL_BYTES : size;
	if (strcmp(fault, "short-write") == 0)
		size = size > SHORT_BYTES ? SHORT_BYTES : size;
	ssize_t count = __real_pwrite64(fd, data, size, offset);
	if (count > 0)
		written += (uint64_t)count;
	return count;
}

ssize_t __wrap_pread64(int fd, void* data, size_t size, off64_t offset)
{
	const bool verifying = syncs != 0 && offset >= SECTOR_BYTES;
	const bool failed = strcmp(fault, "read-fail") == 0 ||
		(verifying && strcmp(fault, "verify-fail") == 0);
	if (failed) {
		errno = EIO;
		return -1;
	}
	if (verifying && strcmp(fault, "verify-eof") == 0)
		return 0;
	if (strcmp(fault, "read-eintr") == 0 && reads++ == 0) {
		errno = EINTR;
		return -1;
	}
	if (strcmp(fault, "short-read") == 0)
		size = size > SHORT_BYTES ? SHORT_BYTES : size;
	ssize_t count = __real_pread64(fd, data, size, offset);
	const bool late = offset >= SECTOR_BYTES + TRANSFER_BYTES;
	const bool corrupt = strcmp(fault, "verify-corrupt") == 0 ||
		(late && strcmp(fault, "verify-late-corrupt") == 0);
	if (count > 0 && verifying && corrupt)
		((unsigned char*)data)[count - 1] ^= 1;
	return count;
}

int __wrap_fsync(int fd)
{
	syncs++;
	if (strcmp(fault, "sync-fail") == 0 || strcmp(fault, "partial-sync-fail") == 0) {
		errno = ENOSPC;
		return -1;
	}
	if (strcmp(fault, "sync-eintr") == 0 && syncs == 1) {
		errno = EINTR;
		return -1;
	}
	if (strcmp(fault, "resize") == 0) {
		struct stat info;
		if (fstat(fd, &info) != 0 || ftruncate(fd, info.st_size + SECTOR_BYTES) != 0)
			abort();
	}
	if (strcmp(fault, "mbr-changed") == 0) {
		unsigned char byte;
		if (__real_pread64(fd, &byte, 1, 454) != 1)
			abort();
		byte ^= 1;
		if (__real_pwrite64(fd, &byte, 1, 454) != 1)
			abort();
	}
	return __real_fsync(fd);
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

int main(int argc, char** argv)
{
	if (argc != 7 && argc != 8 && argc != 9)
		return 64;
	MBR_BOOT_CODE boot_code = MBR_BOOT_GRUB2;
	if (argc >= 8) {
		if (strcmp(argv[7], "rufus") == 0)
			boot_code = MBR_BOOT_RUFUS;
		else if (strcmp(argv[7], "win7") == 0)
			boot_code = MBR_BOOT_WIN7;
		else if (strcmp(argv[7], "rufus-masquerade") == 0)
			boot_code = MBR_BOOT_RUFUS_MASQUERADE;
		else if (strcmp(argv[7], "syslinux") == 0)
			boot_code = MBR_BOOT_SYSLINUX;
		else if (strcmp(argv[7], "grub4dos") == 0)
			boot_code = MBR_BOOT_GRUB4DOS;
		else if (strcmp(argv[7], "zero") == 0)
			boot_code = MBR_BOOT_ZERO;
		else if (strcmp(argv[7], "grub2") != 0)
			return 64;
	}
	FILE* source = fopen(argv[2], "rb");
	struct stat info;
	if (source == NULL || fstat(fileno(source), &info) != 0 || !S_ISREG(info.st_mode))
		return 65;
	if (info.st_size < 0 || info.st_size > MAX_PAYLOAD_BYTES)
		return 65;
	const size_t size = (size_t)info.st_size;
	unsigned char* core = malloc(size + 1);
	if (core == NULL || fread(core, 1, size, source) != size || fclose(source) != 0)
		return 65;
	fault = argv[6];
	const int file_system = strcmp(argv[4], "ntfs") == 0 ? FS_NTFS :
		strcmp(argv[4], "ext2") == 0 ? FS_EXT2 :
		strcmp(argv[4], "ext3") == 0 ? FS_EXT3 :
		atoi(argv[4]) == 16 ? FS_FAT16 : FS_FAT32;
	MBR_IMAGE_OPTIONS options = { strtoull(argv[3], NULL, 10), SECTOR_BYTES, 63,
		(uint32_t)strtoul(argv[5], NULL, 10), 0x78563412, 0,
		file_system, boot_code, 0 , 0, false, false, false, 0};
	if (strcmp(fault, "wrong-id") == 0)
		options.disk_signature++;
	if (strcmp(fault, "sector") == 0)
		options.sector_size = 4096;
	if (strcmp(fault, "wrong-code") == 0)
		options.boot_code = MBR_BOOT_SYSLINUX;
	if (strcmp(fault, "block-request") == 0)
		options.device = 1;
	int flags = strcmp(fault, "readonly") == 0 ? O_RDONLY : O_RDWR;
	if (strcmp(fault, "append") == 0)
		flags |= O_APPEND;
	if (strcmp(fault, "direct") == 0)
		flags |= O_DIRECT;
	target = open(argv[1], flags | O_CLOEXEC);
	if (target < 0 || lseek(target, INITIAL_POSITION, SEEK_SET) < 0)
		return 65;
	const int before = DescriptorCount();
	const int original_flags = fcntl(target, F_GETFL);
	MBR_CORE_RESULT result = { 0 };
	const bool grub4dos = argc == 9 && strcmp(argv[8], "grub4dos") == 0;
	if (argc == 9 && !grub4dos)
		return 64;
	int (*writer)(int, const MBR_IMAGE_OPTIONS*, const unsigned char*, uint64_t, MBR_CORE_RESULT*) =
		grub4dos ? WriteGrub4dosSbrImage : WriteGrub2CoreImage;
	const int error = writer(target, strcmp(fault, "null-options") == 0 ? NULL : &options,
		strcmp(fault, "null-core") == 0 ? NULL : core,
		strcmp(fault, "overflow") == 0 ? UINT64_MAX : size,
		strcmp(fault, "null-result") == 0 ? NULL : &result);
	printf("ERROR=%d BYTES=%" PRIu64 " SYNC=%d SYNCS=%d WRITES=%d FDS=%d OFFSET=%jd FLAGS=%d\n",
		error, result.bytes_written, result.sync_error, syncs, writes, DescriptorCount() - before,
		(intmax_t)lseek(target, 0, SEEK_CUR),
		fcntl(target, F_GETFL) == original_flags && fcntl(target, F_GETFD) == FD_CLOEXEC);
	close(target);
	free(core);
	return 0;
}
