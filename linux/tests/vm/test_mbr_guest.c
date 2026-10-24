/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "platform/partition_image.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/fs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

enum { SECTOR_BYTES = 512, DISK_BYTES = 65 * 1024 * 1024, INITIAL_POSITION = 17 };
static int cancelled;

int IsRufusJobCancelled(void) { return cancelled; }

static void Require(bool valid, const char* message)
{
	if (valid)
		return;
	fprintf(stderr, "MBR guest failed: %s (errno=%d)\n", message, errno);
	exit(1);
}

static bool Matches(const char* path, const char* expected)
{
	char text[128] = { 0 };
	FILE* file = fopen(path, "r");
	if (file == NULL)
		return false;
	size_t size = fread(text, 1, sizeof(text) - 1, file);
	bool read_ok = !ferror(file);
	fclose(file);
	return read_ok && size == strlen(expected) && strcmp(text, expected) == 0;
}

static int DescriptorCount(void)
{
	DIR* directory = opendir("/proc/self/fd");
	Require(directory != NULL, "descriptor inventory");
	int count = 0;
	struct dirent* entry;
	while ((entry = readdir(directory)) != NULL)
		if (entry->d_name[0] != '.')
			count++;
	closedir(directory);
	return count;
}

static MBR_IMAGE_RESULT CheckRequest(const char* name, int fd, MBR_IMAGE_OPTIONS options,
	int expected_error, bool started)
{
	MBR_IMAGE_RESULT result;
	const int descriptor_flags = fcntl(fd, F_GETFD), status_flags = fcntl(fd, F_GETFL);
	const int descriptors = DescriptorCount();
	Require(lseek(fd, INITIAL_POSITION, SEEK_SET) == INITIAL_POSITION, "initial seek");
	cancelled = strcmp(name, "cancel-before") == 0;
	int error = PrepareMbrTarget(fd, &options, &result);
	printf("RUFUS_MBR_CASE %s ERROR=%d STARTED=%d SYNC_ERROR=%d OFFSET=%" PRIu64
		" SIZE=%" PRIu64 "\n", name, error, result.started, result.sync_error, result.offset, result.size);
	Require(error == expected_error && result.started == started, "request outcome");
	Require(result.sync_error == 0, "flush outcome");
	Require(fcntl(fd, F_GETFD) == descriptor_flags && fcntl(fd, F_GETFL) == status_flags, "retained flags");
	Require(lseek(fd, 0, SEEK_CUR) == INITIAL_POSITION, "retained seek");
	Require(DescriptorCount() == descriptors, "descriptor leak");
	return result;
}

static void CheckKernelPartition(int fd, const MBR_IMAGE_RESULT* partition)
{
	char expected[64];
	Require(ioctl(fd, BLKRRPART, 0) == 0, "refresh guest partition table");
	snprintf(expected, sizeof(expected), "%" PRIu64 "\n", partition->offset / SECTOR_BYTES);
	Require(Matches("/sys/block/sda/sda1/start", expected), "kernel partition start");
	snprintf(expected, sizeof(expected), "%" PRIu64 "\n", partition->size / SECTOR_BYTES);
	Require(Matches("/sys/block/sda/sda1/size", expected), "kernel partition size");
	Require(access("/sys/block/sda/sda2", F_OK) != 0 && errno == ENOENT, "no stale second partition");
	int reopened = open("/dev/sda", O_RDWR | O_EXCL | O_CLOEXEC);
	int open_error = errno;
	if (reopened >= 0)
		close(reopened);
	Require(reopened < 0 && open_error == EBUSY, "retained exclusive whole-disk claim");
	puts("RUFUS_MBR_KERNEL_LAYOUT_AND_CLAIM_OK");
}

int main(void)
{
	// Refuse the host before opening any block node. The VM has only one
	// disposable file-backed USB, with no passthrough or shared host directory.
	if (getuid() != 0 || geteuid() != 0 || getppid() != 1)
		return 77;
	const bool fixture = Matches("/etc/rufus-mbr-fixture", "RUFUS-MBR-TEST\n") &&
		Matches("/sys/class/dmi/id/product_name", "RufusMBRFixture\n");
	if (!fixture)
		return 77;
	int fd = open("/dev/sda", O_RDWR | O_EXCL | O_CLOEXEC);
	Require(fd >= 0, "exclusive USB open");
	struct stat info;
	uint64_t capacity = 0, sequence = 0, after_sequence = 0;
	int sector = 0;
	Require(fstat(fd, &info) == 0 && S_ISBLK(info.st_mode), "block descriptor");
	Require(ioctl(fd, BLKGETSIZE64, &capacity) == 0 && capacity == DISK_BYTES, "fixture capacity");
	Require(ioctl(fd, BLKGETDISKSEQ, &sequence) == 0 && sequence != 0, "disk sequence");
	Require(ioctl(fd, BLKSSZGET, &sector) == 0, "native sector size");
	printf("RUFUS_MBR_DEVICE SECTOR=%d CAPACITY=%" PRIu64 "\n", sector, capacity);
	MBR_IMAGE_OPTIONS options = {DISK_BYTES, SECTOR_BYTES, 63, SECTOR_BYTES,
		0x78563412, (uint64_t)info.st_rdev, FS_FAT32, MBR_BOOT_SYSLINUX, 0};
	if (sector == 4096) {
		CheckRequest("native-4k", fd, options, ENOTSUP, false);
		Require(close(fd) == 0, "4K descriptor close");
		puts("RUFUS_MBR_DONE");
		return 0;
	}
	Require(sector == SECTOR_BYTES, "512-byte fixture");
	Require(access("/sys/block/sda/sda2", F_OK) == 0, "old GPT second partition exists");
	MBR_IMAGE_OPTIONS changed = options;
	changed.device = 0;
	CheckRequest("missing-device", fd, changed, ENOTSUP, false);
	changed.device = options.device + 1;
	CheckRequest("wrong-device", fd, changed, ESTALE, false);
	changed = options;
	changed.disk_size += SECTOR_BYTES;
	CheckRequest("wrong-capacity", fd, changed, ESTALE, false);
	CheckRequest("cancel-before", fd, options, ECANCELED, false);
	int read_only = 1;
	Require(ioctl(fd, BLKROSET, &read_only) == 0, "mark guest disk read-only");
	CheckRequest("native-readonly", fd, options, EROFS, false);
	read_only = 0;
	Require(ioctl(fd, BLKROSET, &read_only) == 0, "restore guest write access");
	int readonly_fd = open("/dev/sda", O_RDONLY | O_CLOEXEC);
	Require(readonly_fd >= 0, "read-only descriptor");
	CheckRequest("readonly-descriptor", readonly_fd, options, EINVAL, false);
	Require(close(readonly_fd) == 0, "read-only descriptor close");
	cancelled = 0;
	MBR_IMAGE_RESULT result;
	Require(CreateMbrImage(fd, &options, &result) == ENOTSUP && !result.started,
		"ordinary creator rejects old GPT without clearing");
	result = CheckRequest("success", fd, options, 0, true);
	Require(CheckMbrImage(fd, &options, &result) == 0, "block MBR readback");
	CheckKernelPartition(fd, &result);
	Require(ioctl(fd, BLKGETDISKSEQ, &after_sequence) == 0 && after_sequence == sequence, "same disk sequence");
	Require(close(fd) == 0, "target descriptor close");
	puts("RUFUS_MBR_DONE");
	return 0;
}
