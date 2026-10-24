/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "platform/partition_image.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <libfdisk.h>
#include <linux/fs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

enum { SECTOR_BYTES = 512, DISK_BYTES = 64 * 1024 * 1024,
	UEFI_NTFS_TEST_BYTES = 1024 * 1024, INITIAL_POSITION = 17 };
static const char* fault = "none";
static int cancelled, writes, syncs;

int __real_fdisk_write_disklabel(struct fdisk_context*);
int __real_fdisk_add_partition(struct fdisk_context*, struct fdisk_partition*, size_t*);
int __real_fsync(int);

int IsRufusJobCancelled(void) { return cancelled; }

int __wrap_fdisk_add_partition(struct fdisk_context* context, struct fdisk_partition* partition, size_t* number)
{
	int status = __real_fdisk_add_partition(context, partition, number);
	if (strcmp(fault, "cancel-before-write") == 0)
		cancelled = 1;
	return status;
}

int __wrap_fdisk_write_disklabel(struct fdisk_context* context)
{
	writes++;
	int status = __real_fdisk_write_disklabel(context);
	if (strcmp(fault, "cancel-after-write") == 0)
		cancelled = 1;
	return status;
}

int __wrap_fsync(int fd)
{
	syncs++;
	if (strcmp(fault, "sync-fail") == 0) {
		errno = EIO;
		return -1;
	}
	return __real_fsync(fd);
}

static void Require(bool valid, const char* message)
{
	if (valid)
		return;
	fprintf(stderr, "GPT guest failed: %s (errno=%d)\n", message, errno);
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
	struct dirent* entry;
	int count = 0;
	Require(directory != NULL, "descriptor inventory");
	while ((entry = readdir(directory)) != NULL)
		if (entry->d_name[0] != '.')
			count++;
	closedir(directory);
	return count;
}

static GPT_IMAGE_RESULT CheckRequest(const char* name, int fd, GPT_IMAGE_OPTIONS options,
	int expected_error, bool started)
{
	GPT_IMAGE_RESULT result;
	const int descriptor_flags = fcntl(fd, F_GETFD), status_flags = fcntl(fd, F_GETFL);
	const int descriptors = DescriptorCount();
	Require(lseek(fd, INITIAL_POSITION, SEEK_SET) == INITIAL_POSITION, "initial seek");
	fault = name;
	cancelled = strcmp(name, "cancel-before") == 0;
	writes = syncs = 0;
	int error = CreateGptImagePartition(fd, &options, &result);
	printf("RUFUS_GPT_CASE %s ERROR=%d STARTED=%d WRITES=%d SYNCS=%d OFFSET=%" PRIu64
		" SIZE=%" PRIu64 "\n", name, error, result.started, writes, syncs, result.offset, result.size);
	Require(error == expected_error && result.started == started, "request outcome");
	Require(writes == (int)started && syncs == (int)started, "write/flush count");
	Require(fcntl(fd, F_GETFD) == descriptor_flags && fcntl(fd, F_GETFL) == status_flags, "retained flags");
	Require(lseek(fd, 0, SEEK_CUR) == INITIAL_POSITION, "retained seek");
	Require(DescriptorCount() == descriptors, "descriptor leak");
	if (strcmp(name, "sync-fail") == 0)
		Require(result.sync_error == EIO, "reported flush failure");
	return result;
}

static int RunFormatter(char* const arguments[], int inherited_fd)
{
	int status;
	fflush(NULL);
	pid_t child = fork();
	Require(child >= 0, "formatter fork");
	if (child == 0) {
		if (getuid() != 0 || geteuid() != 0)
			_exit(125);
		if (inherited_fd >= 0 && fcntl(inherited_fd, F_SETFD, 0) != 0)
			_exit(126);
		execv(arguments[0], arguments);
		_exit(127);
	}
	pid_t waited;
	do {
		waited = waitpid(child, &status, 0);
	} while (waited < 0 && errno == EINTR);
	Require(waited == child, "formatter reaped");
	Require(WIFEXITED(status), "formatter exited normally");
	return WEXITSTATUS(status);
}

static void CheckFormatterConflict(int fd, const GPT_IMAGE_RESULT* partition)
{
	char target[64], offset[32], blocks[32];
	const int descriptors = DescriptorCount();
	snprintf(target, sizeof(target), "/proc/self/fd/%d", fd);
	snprintf(offset, sizeof(offset), "%" PRIu64, partition->offset / SECTOR_BYTES);
	snprintf(blocks, sizeof(blocks), "%" PRIu64, partition->size / 1024);
	// Confirm the retained descriptor still owns the kernel's exclusive claim.
	int reopened = open(target, O_RDWR | O_EXCL | O_CLOEXEC);
	int open_error = errno;
	if (reopened >= 0)
		close(reopened);
	Require(reopened < 0 && open_error == EBUSY, "exclusive claim after GPT");
	char* arguments[] = { "/usr/bin/mkfs.fat", "-F", "32", "--mbr=n",
		"--offset", offset, target, blocks, NULL };
	Require(RunFormatter(arguments, fd) == 1, "stock formatter refused reopen");
	Require(fcntl(fd, F_GETFD) == FD_CLOEXEC, "formatter retained descriptor flags");
	Require(lseek(fd, 0, SEEK_CUR) == INITIAL_POSITION, "formatter retained seek");
	Require(DescriptorCount() == descriptors, "formatter descriptor leak");
	puts("RUFUS_GPT_FORMATTER_BUSY");
}

// Consume the caller's exclusive descriptor, run the native tool, then return
// a newly acquired exclusive descriptor. No descriptor is inherited by the tool.
static int CheckFormatterHandoff(int fd, const GPT_IMAGE_OPTIONS* options, const GPT_IMAGE_RESULT* partition)
{
	char hidden_sectors[32];
	struct stat original_partition, current;
	uint64_t capacity = 0;
	const int descriptors = DescriptorCount();
	enum { GPT_METADATA_SECTORS = 34, PARTITION_WAIT_ATTEMPTS = 100, PARTITION_WAIT_US = 10000 };
	unsigned char primary[GPT_METADATA_SECTORS * SECTOR_BYTES];
	unsigned char backup[sizeof(primary)], readback[sizeof(primary)];
	const off_t backup_offset = options->disk_size - sizeof(backup);

	Require(pread(fd, primary, sizeof(primary), 0) == sizeof(primary), "retain primary GPT");
	Require(pread(fd, backup, sizeof(backup), backup_offset) == sizeof(backup), "retain backup GPT");
	Require(ioctl(fd, BLKRRPART, 0) == 0, "refresh guest partitions");
	for (int attempt = 0; attempt < PARTITION_WAIT_ATTEMPTS; attempt++) {
		if (stat("/dev/sda1", &original_partition) == 0)
			break;
		usleep(PARTITION_WAIT_US);
	}
	Require(stat("/dev/sda1", &original_partition) == 0 && S_ISBLK(original_partition.st_mode), "guest partition");
	Require(close(fd) == 0, "release exclusive claim for formatter");
	snprintf(hidden_sectors, sizeof(hidden_sectors), "%" PRIu64, partition->offset / SECTOR_BYTES);
	char* arguments[] = { "/usr/bin/mkfs.fat", "-F", "32", "-S", "512", "-s", "1",
		"-h", hidden_sectors, "-i", "1234abcd", "-n", "RUFUS TEST", "--mbr=n", "/dev/sda1", NULL };
	Require(RunFormatter(arguments, -1) == 0, "direct formatter after release");
	puts("RUFUS_GPT_FORMATTER_HANDOFF");
	// A mounted volume must be refused by the stock tool; don't bypass its check.
	Require(mount("/dev/sda1", "/target", "vfat", MS_RDONLY, "") == 0, "mount formatted partition");
	Require(RunFormatter(arguments, -1) == 1, "formatter refused mounted partition");
	Require(umount("/target") == 0, "unmount formatted partition");
	puts("RUFUS_GPT_FORMATTER_MOUNTED_REFUSED");
	fd = open("/dev/sda", O_RDWR | O_EXCL | O_SYNC | O_CLOEXEC);
	Require(fd >= 0, "reacquire exclusive USB access");
	Require(fstat(fd, &current) == 0 && (uint64_t)current.st_rdev == options->device, "same disk after formatter");
	Require(ioctl(fd, BLKGETSIZE64, &capacity) == 0 && capacity == options->disk_size, "same disk capacity");
	Require(stat("/dev/sda1", &current) == 0 && current.st_rdev == original_partition.st_rdev, "same partition");
	Require(pread(fd, readback, sizeof(readback), 0) == sizeof(readback), "read primary GPT");
	Require(memcmp(primary, readback, sizeof(primary)) == 0, "primary GPT preserved");
	Require(pread(fd, readback, sizeof(readback), backup_offset) == sizeof(readback), "read backup GPT");
	Require(memcmp(backup, readback, sizeof(backup)) == 0, "backup GPT preserved");
	Require(DescriptorCount() == descriptors, "handoff descriptor leak");
	puts("RUFUS_GPT_REACQUIRED");
	return fd;
}

int main(void)
{
	// Refuse a host invocation before opening any block node.
	if (geteuid() != 0)
		return 77;
	const char* caller = getenv("PKEXEC_UID");
	const bool direct_guest = getppid() == 1 &&
		Matches("/sys/class/dmi/id/product_name", "RufusGPTFixture\n");
	const bool authorized_guest = caller != NULL && strcmp(caller, "1000") == 0 &&
		Matches("/sys/class/dmi/id/product_name", "RufusUSBFixture\n");
	bool fixture = Matches("/etc/rufus-gpt-fixture", "RUFUS-GPT-TEST\n") &&
		(direct_guest || authorized_guest);
	if (!fixture)
		return 77;
	printf("RUFUS_HELPER_UID=%u EUID=%u CALLER=%s\n", (unsigned)getuid(), (unsigned)geteuid(),
		caller != NULL ? caller : "direct");
	int fd = open("/dev/sda", O_RDWR | O_EXCL | O_SYNC | O_CLOEXEC);
	Require(fd >= 0, "exclusive USB open");
	struct stat info;
	uint64_t capacity = 0;
	int sector = 0;
	Require(fstat(fd, &info) == 0 && S_ISBLK(info.st_mode), "block descriptor");
	Require(ioctl(fd, BLKGETSIZE64, &capacity) == 0 && capacity == DISK_BYTES, "fixture capacity");
	Require(ioctl(fd, BLKSSZGET, &sector) == 0, "native sector size");
	GPT_IMAGE_OPTIONS options = { DISK_BYTES, SECTOR_BYTES, 63, SECTOR_BYTES, (uint64_t)info.st_rdev, 0 };
	printf("RUFUS_GPT_DEVICE SECTOR=%d CAPACITY=%" PRIu64 "\n", sector, capacity);
	if (sector == 4096) {
		CheckRequest("native-4k", fd, options, ENOTSUP, false);
		Require(close(fd) == 0, "4K descriptor close");
		puts("RUFUS_GPT_DONE");
		return 0;
	}
	Require(sector == SECTOR_BYTES, "512-byte fixture");
	GPT_IMAGE_OPTIONS changed = options;
	changed.uefi_ntfs_size = UEFI_NTFS_TEST_BYTES;
	CheckRequest("uefi-layout", fd, changed, 0, true);
	changed = options;
	changed.device = 0;
	CheckRequest("missing-device", fd, changed, ENOTSUP, false);
	changed.device = options.device + 1;
	CheckRequest("wrong-device", fd, changed, ESTALE, false);
	changed = options;
	changed.disk_size += SECTOR_BYTES;
	CheckRequest("wrong-capacity", fd, changed, ESTALE, false);
	CheckRequest("cancel-before", fd, options, ECANCELED, false);
	CheckRequest("cancel-before-write", fd, options, ECANCELED, false);
	int read_only = 1;
	Require(ioctl(fd, BLKROSET, &read_only) == 0, "mark guest disk read-only");
	CheckRequest("native-readonly", fd, options, EROFS, false);
	read_only = 0;
	Require(ioctl(fd, BLKROSET, &read_only) == 0, "restore guest disk write access");
	int readonly_fd = open("/dev/sda", O_RDONLY | O_CLOEXEC);
	Require(readonly_fd >= 0, "read-only descriptor");
	CheckRequest("readonly-descriptor", readonly_fd, options, EINVAL, false);
	Require(close(readonly_fd) == 0, "read-only descriptor close");
	CheckRequest("sync-fail", fd, options, EIO, true);
	CheckRequest("cancel-after-write", fd, options, ECANCELED, true);
	GPT_IMAGE_RESULT partition = CheckRequest("success", fd, options, 0, true);
	CheckFormatterConflict(fd, &partition);
	fd = CheckFormatterHandoff(fd, &options, &partition);
	Require(close(fd) == 0, "target descriptor close");
	puts("RUFUS_GPT_DONE");
	return 0;
}
