/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "target_io.h"
#include <stdbool.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <string.h>
#include <sys/ioctl.h>

int InspectWritableTarget(int fd, uint64_t device, WRITABLE_TARGET* target)
{
	memset(target, 0, sizeof(*target));
	const int flags = fcntl(fd, F_GETFL);
	if (flags < 0 || fstat(fd, &target->info) != 0)
		return errno;
	const bool block = device != 0;
	const bool expected_type = block ? S_ISBLK(target->info.st_mode) : S_ISREG(target->info.st_mode);
	if (!expected_type)
		return ENOTSUP;
	const bool writable = (flags & O_ACCMODE) == O_RDWR;
	// Linux pwrite with O_APPEND ignores the supplied offset.
	const bool incompatible_flags = (flags & (O_APPEND | O_DIRECT | O_PATH)) != 0;
	if (!writable || incompatible_flags)
		return EINVAL;
	if (!block) {
		if (target->info.st_size < 0)
			return ESTALE;
		target->size = (uint64_t)target->info.st_size;
		return 0;
	}
	if ((uint64_t)target->info.st_rdev != device)
		return ESTALE;
	int read_only = 1, sector = 0, physical_sector = 0;
	if (ioctl(fd, BLKGETSIZE64, &target->size) != 0 || ioctl(fd, BLKSSZGET, &sector) != 0)
		return errno;
	if (ioctl(fd, BLKPBSZGET, &physical_sector) != 0 || ioctl(fd, BLKROGET, &read_only) != 0)
		return errno;
	if (read_only)
		return EROFS;
	if (sector <= 0 || physical_sector < sector)
		return EINVAL;
	target->sector_size = (unsigned)sector;
	target->physical_sector_size = (unsigned)physical_sector;
	return 0;
}

int CheckEmptyTargetDirectory(int root)
{
	int fd = openat(root, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	int error = 0;
	DIR* directory;
	struct dirent* entry;

	if (fd < 0)
		return errno;
	directory = fdopendir(fd);
	if (directory == NULL) {
		error = errno;
		close(fd);
		return error;
	}
	errno = 0;
	while ((entry = readdir(directory)) != NULL) {
		if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
			error = ENOTEMPTY;
			break;
		}
	}
	if (entry == NULL && errno != 0)
		error = errno;
	if (closedir(directory) != 0 && error == 0)
		error = errno;
	return error;
}

