/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "platform/partition_image.h"
#include <libfdisk.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { SECTOR_BYTES = 512, INITIAL_POSITION = 17, SHORT_WRITE_BYTES = 19,
	CLEAR_TAIL_BYTES = 1024 * 1024, PARTITION_PREFIX_OFFSET = 1024 * 1024 };
static const char* fault;
static int cancelled, writes, syncs, boot_writes;
static uint64_t disk_bytes, boot_partition_offset;
int __real_fdisk_add_partition(struct fdisk_context*, struct fdisk_partition*, size_t*);
int __real_fdisk_write_disklabel(struct fdisk_context*);
int __real_fdisk_toggle_partition_flag(struct fdisk_context*, size_t, unsigned long);
int __real_fsync(int);
ssize_t __real_pwrite64(int, const void*, size_t, off_t);
ssize_t __real_pread64(int, void*, size_t, off_t);

int IsRufusJobCancelled(void) { return cancelled; }
void uprintf(const char* format, ...) { (void)format; }

int __wrap_fdisk_add_partition(struct fdisk_context* context, struct fdisk_partition* partition, size_t* number)
{
	if (strcmp(fault, "add-fail") == 0)
		return -ENOMEM;
	if (strcmp(fault, "wrong-offset") == 0)
		fdisk_partition_set_start(partition, fdisk_partition_get_start(partition) + 1);
	int status = __real_fdisk_add_partition(context, partition, number);
	if (status == 0 && strcmp(fault, "active") == 0)
		status = __real_fdisk_toggle_partition_flag(context, *number, DOS_FLAG_ACTIVE);
	if (strcmp(fault, "cancel-before-write") == 0)
		cancelled = 1;
	if (strcmp(fault, "resize-before-write") == 0 && ftruncate(fdisk_get_devfd(context), SECTOR_BYTES) != 0)
		abort();
	return status;
}

int __wrap_fdisk_toggle_partition_flag(struct fdisk_context* context, size_t number, unsigned long flag)
{
	if (strcmp(fault, "inactive") == 0)
		return 0;
	return __real_fdisk_toggle_partition_flag(context, number, flag);
}

int __wrap_fdisk_write_disklabel(struct fdisk_context* context)
{
	writes++;
	if (strcmp(fault, "write-fail") == 0)
		return -EIO;
	int status = __real_fdisk_write_disklabel(context);
	if (strcmp(fault, "cancel-after-table") == 0)
		cancelled = 1;
	return status;
}

ssize_t __wrap_pwrite64(int fd, const void* data, size_t size, off_t offset)
{
	boot_writes++;
	const bool clear_end = disk_bytes >= CLEAR_TAIL_BYTES &&
		(uint64_t)offset >= disk_bytes - CLEAR_TAIL_BYTES;
	const bool clear_failure = strcmp(fault, "clear-write-fail") == 0 ||
		(strcmp(fault, "clear-partial-fail") == 0 && boot_writes > 1) ||
		(strcmp(fault, "clear-end-fail") == 0 && clear_end) ||
		(strcmp(fault, "clear-boot-write-fail") == 0 && (uint64_t)offset == boot_partition_offset);
	if (clear_failure) {
		errno = EIO;
		return -1;
	}
	if (strcmp(fault, "clear-zero") == 0)
		return 0;
	if (strcmp(fault, "boot-write-fail") == 0 ||
		(strcmp(fault, "boot-partial-fail") == 0 && boot_writes > 1)) {
		errno = EIO;
		return -1;
	}
	const bool interrupted = strcmp(fault, "boot-eintr") == 0 || strcmp(fault, "clear-eintr") == 0;
	if (interrupted && boot_writes == 1) {
		errno = EINTR;
		return -1;
	}
	const bool partial = strcmp(fault, "boot-short") == 0 || strcmp(fault, "boot-partial-fail") == 0 ||
		strcmp(fault, "clear-short") == 0 || strcmp(fault, "clear-partial-fail") == 0;
	if (partial && boot_writes == 1 && size > SHORT_WRITE_BYTES)
		size = SHORT_WRITE_BYTES;
	ssize_t result = __real_pwrite64(fd, data, size, offset);
	if (strcmp(fault, "cancel-after-boot") == 0 || strcmp(fault, "cancel-during-clear") == 0)
		cancelled = 1;
	return result;
}

ssize_t __wrap_pread64(int fd, void* data, size_t size, off_t offset)
{
	const bool checking_clear = syncs == 1 && writes == 0;
	const bool tail_read_failure = strcmp(fault, "clear-tail-read-fail") == 0 && disk_bytes >= CLEAR_TAIL_BYTES &&
		(uint64_t)offset >= disk_bytes - CLEAR_TAIL_BYTES;
	const bool boot_read_failure = strcmp(fault, "clear-boot-read-fail") == 0 && (uint64_t)offset == boot_partition_offset;
	if (checking_clear && (strcmp(fault, "clear-read-fail") == 0 || tail_read_failure || boot_read_failure)) {
		errno = EIO;
		return -1;
	}
	return __real_pread64(fd, data, size, offset);
}

int __wrap_fsync(int fd)
{
	syncs++;
	if (strcmp(fault, "sync-fail") == 0) {
		errno = EIO;
		return -1;
	}
	if (strcmp(fault, "sync-eintr") == 0 && syncs == 1) {
		errno = EINTR;
		return -1;
	}
	if (syncs == 1 && strcmp(fault, "cancel-after-clear") == 0)
		cancelled = 1;
	if (syncs == 1 && strcmp(fault, "resize-after-clear") == 0 && ftruncate(fd, SECTOR_BYTES) != 0)
		abort();
	if (syncs == 1 && strcmp(fault, "clear-corrupt") == 0) {
		const unsigned char byte = 1;
		if (__real_pwrite64(fd, &byte, 1, PARTITION_PREFIX_OFFSET) != 1)
			abort();
	}
	if (syncs == 1 && strcmp(fault, "clear-tail-corrupt") == 0) {
		const unsigned char byte = 1;
		if (__real_pwrite64(fd, &byte, 1, disk_bytes - SECTOR_BYTES) != 1)
			abort();
	}
	if (syncs == 1 && strcmp(fault, "clear-boot-corrupt") == 0) {
		const unsigned char byte = 1;
		if (__real_pwrite64(fd, &byte, 1, (off_t)boot_partition_offset) != 1)
			abort();
	}
	const struct { const char* name; off_t offset; } corruptions[] = {
		{ "bad-code", 100 }, { "bad-id", 440 }, { "bad-reserved", 444 },
		{ "bad-active", 446 }, { "bad-type", 450 }, { "bad-offset", 454 },
		{ "bad-inactive", 446 }, { "bad-other-drive", 446 },
		{ "bad-size", 458 }, { "extra-entry", 470 }, { "bad-marker", 510 },
		{ "bad-boot-active", 462 }, { "bad-boot-type", 466 }, { "bad-boot-offset", 470 },
		{ "bad-boot-size", 474 }, { "third-entry", 486 }
	};
	for (size_t i = 0; i < sizeof(corruptions) / sizeof(*corruptions); i++) {
		if (strcmp(fault, corruptions[i].name) != 0)
			continue;
		unsigned char value;
		if (pread(fd, &value, 1, corruptions[i].offset) != 1)
			abort();
		value ^= 1;
		if (strcmp(fault, "bad-inactive") == 0)
			value = 0;
		if (strcmp(fault, "bad-other-drive") == 0)
			value = 0x82;
		if (__real_pwrite64(fd, &value, 1, corruptions[i].offset) != 1)
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
	if (argc < 8 || argc > 13)
		return 64;
	fault = argv[7];
	MBR_IMAGE_OPTIONS options = { strtoull(argv[2], NULL, 10), SECTOR_BYTES,
		(uint32_t)strtoul(argv[4], NULL, 10), (uint32_t)strtoul(argv[3], NULL, 10), 0x78563412, 0, FS_FAT32, MBR_BOOT_SYSLINUX, 0};
	if (argc >= 9) {
		const unsigned bits = (unsigned)strtoul(argv[8], NULL, 10);
		options.file_system = bits == 16 ? FS_FAT16 : (bits == 32 ? FS_FAT32 : FS_UNKNOWN);
		if (strcmp(argv[8], "ntfs") == 0)
			options.file_system = FS_NTFS;
		if (strcmp(argv[8], "exfat") == 0)
			options.file_system = FS_EXFAT;
		if (strcmp(argv[8], "ext2") == 0)
			options.file_system = FS_EXT2;
		if (strcmp(argv[8], "ext3") == 0)
			options.file_system = FS_EXT3;
		if (strcmp(argv[8], "ext4") == 0)
			options.file_system = FS_EXT4;
	}
	const bool uefi_ntfs = argc >= 11 && strcmp(argv[9], "uefi-ntfs") == 0;
	if (argc >= 10) {
		if (strcmp(argv[9], "non-bootable") == 0 || uefi_ntfs)
			options.boot_code = MBR_BOOT_ZERO;
		else if (strcmp(argv[9], "rufus") == 0)
			options.boot_code = MBR_BOOT_RUFUS;
		else if (strcmp(argv[9], "rufus-masquerade") == 0)
			options.boot_code = MBR_BOOT_RUFUS_MASQUERADE;
		else if (strcmp(argv[9], "grub2") == 0)
			options.boot_code = MBR_BOOT_GRUB2;
		else if (strcmp(argv[9], "grub4dos") == 0)
			options.boot_code = MBR_BOOT_GRUB4DOS;
		else if (strcmp(argv[9], "win7") == 0)
			options.boot_code = MBR_BOOT_WIN7;
		else if (strcmp(argv[9], "invalid-code") == 0)
			options.boot_code = (MBR_BOOT_CODE)-1;
		if (argc >= 11)
			options.uefi_ntfs_size = strtoull(argv[10], NULL, 10);
	}
	options.old_bios_fixes = argc >= 12 && strcmp(argv[11], "old-bios") == 0;
	if (argc == 13) options.persistence_size = strtoull(argv[12], NULL, 10);
	disk_bytes = options.disk_size;
	if (options.uefi_ntfs_size != 0) {
		MBR_IMAGE_RESULT planned;
		if (PlanMbrImageLayout(&options, &planned) == 0)
			boot_partition_offset = planned.uefi_ntfs_offset;
	}
	const bool sfd = argc == 10 && strcmp(argv[9], "sfd") == 0;
	if (strcmp(fault, "block-request") == 0)
		options.device = 1;
	options.sector_size = (uint32_t)strtoul(argv[5], NULL, 10);
	MBR_IMAGE_RESULT result = { 0 };
	const SFD_IMAGE_OPTIONS sfd_options = { options.disk_size, options.sector_size, options.device };
	SFD_IMAGE_RESULT sfd_result = { 0 };
	int error, fd = -1, flags = 0, position = INITIAL_POSITION;
	const int before = DescriptorCount();
	const bool planning = strcmp(argv[6], "plan") == 0;
	if (planning)
		error = sfd ? PlanSfdImageLayout(&sfd_options, &sfd_result) : PlanMbrImageLayout(&options, &result);
	else {
		int access = strcmp(fault, "readonly") == 0 ? O_RDONLY : O_RDWR;
		if (strcmp(fault, "append") == 0)
			access |= O_APPEND;
		fd = open(argv[1], access | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
		if (fd < 0)
			return 65;
		if (strcmp(fault, "pipe") == 0) {
			int ends[2];
			close(fd);
			if (pipe2(ends, O_CLOEXEC) != 0)
				abort();
			fd = ends[0];
			close(ends[1]);
		} else if (lseek(fd, INITIAL_POSITION, SEEK_SET) != INITIAL_POSITION)
			return 66;
		if (strcmp(fault, "replace-path") == 0) {
			char held[4096];
			snprintf(held, sizeof(held), "%s.held", argv[1]);
			if (rename(argv[1], held) != 0)
				abort();
			int replacement = open(argv[1], O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
			if (replacement < 0 || write(replacement, "untouched", 9) != 9 || close(replacement) != 0)
				abort();
		}
		int descriptor_flags = fcntl(fd, F_GETFD), access_flags = fcntl(fd, F_GETFL);
		cancelled = strcmp(fault, "cancel-before") == 0;
		if (sfd)
			error = PrepareSfdTarget(fd, &sfd_options, &sfd_result);
		else if (strcmp(argv[6], "check") == 0)
			error = CheckMbrImage(fd, &options, &result);
		else if (strcmp(argv[6], "prepare") == 0)
			error = PrepareMbrTarget(fd, &options, &result);
		else
			error = CreateMbrImage(fd, &options, &result);
		flags = descriptor_flags == fcntl(fd, F_GETFD) && access_flags == fcntl(fd, F_GETFL);
		position = (int)lseek(fd, 0, SEEK_CUR);
		close(fd);
	}
	if (sfd) {
		result.started = sfd_result.started;
		result.sync_error = sfd_result.sync_error;
		result.offset = sfd_result.offset;
		result.size = sfd_result.size;
	}
	printf("ERROR=%d STARTED=%d OFFSET=%" PRIu64 " SIZE=%" PRIu64
		" BOOT_OFFSET=%" PRIu64 " BOOT_SIZE=%" PRIu64
		" COMPAT_OFFSET=%" PRIu64 " COMPAT_SIZE=%" PRIu64
		" WRITES=%d BOOT_WRITES=%d SYNCS=%d SYNC_ERROR=%d FDS=%d POSITION=%d FLAGS=%d\n",
		error, result.started, result.offset, result.size, result.uefi_ntfs_offset, result.uefi_ntfs_size,
		result.compatibility_offset, result.compatibility_size, writes, boot_writes, syncs,
		result.sync_error, DescriptorCount() - before, position, planning ? 1 : flags);
	return 0;
}
