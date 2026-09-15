/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "platform/partition_image.h"
#include <libfdisk.h>
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

static const char* fault;
static int cancelled, writes, syncs;
enum {
	SECTOR_BYTES = 512,
	INITIAL_POSITION = 17
};
int __real_fdisk_write_disklabel(struct fdisk_context*);
int __real_fdisk_add_partition(struct fdisk_context*, struct fdisk_partition*, size_t*);
int __real_fsync(int);

int IsRufusJobCancelled(void) { return cancelled; }
void uprintf(const char* format, ...)
{
	va_list args;
	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);
}

int __wrap_fdisk_add_partition(struct fdisk_context* context, struct fdisk_partition* partition, size_t* number)
{
	if (strcmp(fault, "add-fail") == 0)
		return -ENOMEM;
	int status = __real_fdisk_add_partition(context, partition, number);
	if (strcmp(fault, "cancel-before-write") == 0)
		cancelled = 1;
	if (strcmp(fault, "resize-before-write") == 0) {
		int fd = fdisk_get_devfd(context);
		struct stat info;
		if (fstat(fd, &info) != 0 || ftruncate(fd, info.st_size - SECTOR_BYTES) != 0)
			abort();
	}
	return status;
}

int __wrap_fdisk_write_disklabel(struct fdisk_context* context)
{
	int status;
	writes++;
	if (strcmp(fault, "write-fail") == 0)
		return -EIO;
	status = __real_fdisk_write_disklabel(context);
	if (strcmp(fault, "cancel-after-write") == 0)
		cancelled = 1;
	if (strcmp(fault, "damaged-primary") == 0 || strcmp(fault, "damaged-backup") == 0) {
		bool primary = strcmp(fault, "damaged-primary") == 0;
		off_t header_sector = primary ? 1 : (off_t)(fdisk_get_nsectors(context) - 1);
		off_t offset = header_sector * SECTOR_BYTES;
		char bad = 0;
		if (pwrite(fdisk_get_devfd(context), &bad, 1, offset) != 1)
			abort();
	}
	return status;
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
	return __real_fsync(fd);
}

static int DescriptorCount(void)
{
	DIR* directory = opendir("/proc/self/fd");
	struct dirent* entry;
	int count = 0;
	if (directory == NULL)
		abort();
	while ((entry = readdir(directory)) != NULL)
		if (entry->d_name[0] != '.')
			count++;
	closedir(directory);
	return count;
}

int main(int argc, char** argv)
{
	GPT_IMAGE_OPTIONS options = { 0, SECTOR_BYTES, 63, SECTOR_BYTES, 0 };
	GPT_IMAGE_RESULT result;
	int fd, error, before, original_flags, flags;

	if (argc != 6)
		return 64;
	fault = argv[5];
	options.disk_size = strtoull(argv[2], NULL, 10);
	options.cluster_size = (uint32_t)strtoul(argv[3], NULL, 10);
	options.sectors_per_track = (uint32_t)strtoul(argv[4], NULL, 10);
	if (strcmp(fault, "sector-4k") == 0)
		options.sector_size = 4096;
	if (strcmp(fault, "block-request") == 0)
		options.device = 1;
	flags = strcmp(fault, "readonly") == 0 ? O_RDONLY : O_RDWR;
	if (strcmp(fault, "append") == 0)
		flags |= O_APPEND;
	fd = open(argv[1], flags | O_CLOEXEC);
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
		char retained[4096];
		snprintf(retained, sizeof(retained), "%s.held", argv[1]);
		if (rename(argv[1], retained) != 0)
			abort();
		int replacement = open(argv[1], O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
		if (replacement < 0 || write(replacement, "retained", 8) != 8)
			abort();
		close(replacement);
	}
	original_flags = fcntl(fd, F_GETFD);
	before = DescriptorCount();
	cancelled = strcmp(fault, "cancel-before") == 0;
	error = CreateGptImagePartition(fd, &options, &result);
	printf("ERROR=%d STARTED=%d OFFSET=%" PRIu64 " SIZE=%" PRIu64
		" SYNCS=%d SYNC_ERROR=%d WRITES=%d FDS=%d POSITION=%jd FLAGS=%d\n",
		error, result.started, result.offset, result.size, syncs, result.sync_error, writes,
		DescriptorCount() - before, (intmax_t)lseek(fd, 0, SEEK_CUR), fcntl(fd, F_GETFD) == original_flags);
	close(fd);
	return 0;
}
