/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Guest-only link wrappers. No fault controls enter the production helper. */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/magic.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <unistd.h>

extern int IsRufusJobCancelled(void);
enum { NTFS_MAGIC = 0x5346544e };
ssize_t __real_write(int, const void*, size_t);
int __real_syncfs(int);
int __real_umount(const char*);
int __real_mount(const char*, const char*, const char*, unsigned long, const void*);

static int Fault(const char* name)
{
	char mode[32] = { 0 };
	if (geteuid() != 0 || access("/etc/rufus-extract-fixture", F_OK) != 0)
		return 0;
	FILE* file = fopen("/home/rufus/extract-fault", "r");
	if (file == NULL)
		return 0;
	fread(mode, 1, sizeof(mode) - 1, file);
	fclose(file);
	return strcmp(mode, name) == 0;
}

ssize_t __wrap_write(int fd, const void* data, size_t size)
{
	static int paused;
	struct stat info;
	struct statfs filesystem;
	if (Fault("winpe-write")) {
		char descriptor[64], path[PATH_MAX];
		snprintf(descriptor, sizeof(descriptor), "/proc/self/fd/%d", fd);
		ssize_t length = readlink(descriptor, path, sizeof(path) - 1);
		if (length >= 0) {
			path[length] = 0;
			const char* name = strrchr(path, '/');
			if (name != NULL && strncmp(name + 1, ".rufus-winpe-", strlen(".rufus-winpe-")) == 0) {
				errno = EIO;
				return -1;
			}
		}
	}
	ssize_t count = __real_write(fd, data, size);
	if (paused || count <= 0 || !Fault("pause"))
		return count;
	if (fstat(fd, &info) != 0 || fstatfs(fd, &filesystem) != 0)
		return count;
	const int target_filesystem = filesystem.f_type == MSDOS_SUPER_MAGIC
		|| filesystem.f_type == NTFS_MAGIC || filesystem.f_type == FUSE_SUPER_MAGIC;
	if (!S_ISREG(info.st_mode) || !target_filesystem)
		return count;
	paused = 1;
	FILE* marker = fopen("/run/rufus-extractor.pid", "w");
	if (marker == NULL)
		return -1;
	fprintf(marker, "%d\n", getpid());
	fclose(marker);
	while (!IsRufusJobCancelled())
		usleep(10000);
	return count;
}

int __wrap_syncfs(int fd)
{
	if (Fault("flush")) {
		errno = EIO;
		return -1;
	}
	return __real_syncfs(fd);
}

int __wrap_umount(const char* target)
{
	if (Fault("unmount")) {
		errno = EBUSY;
		return -1;
	}
	return __real_umount(target);
}

int __wrap_mount(const char* source, const char* target, const char* type, unsigned long flags, const void* data)
{
	const int target_filesystem = type != NULL && (strcmp(type, "vfat") == 0 || strcmp(type, "ntfs") == 0);
	if (target_filesystem && Fault("mount")) {
		errno = EIO;
		return -1;
	}
	return __real_mount(source, target, type, flags, data);
}
