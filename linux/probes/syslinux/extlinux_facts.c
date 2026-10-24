/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Guest-only observations of the native extlinux mapping and mount contract. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/fiemap.h>
#include <linux/fs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <unistd.h>
#include "syslxcom.h"

enum { SECTOR_BYTES = 512, MAX_LOADER_BYTES = 1024 * 1024 };

static int fixture(void)
{
    char name[64] = {0};
    FILE* file = fopen("/sys/class/dmi/id/product_name", "r");
    if (file == NULL)
        return 0;
    int valid = fgets(name, sizeof(name), file) != NULL && strcmp(name, "RufusExtlinuxFixture\n") == 0;
    fclose(file);
    return valid && access("/etc/rufus-extlinux-fixture", F_OK) == 0;
}

static int claim(const char* path)
{
    int fd = open(path, O_RDWR | O_EXCL | O_CLOEXEC);
    int error = fd < 0 ? errno : 0;
    printf("CLAIM path=%s error=%d\n", path, error);
    if (fd >= 0)
        close(fd);
    return error;
}

static int map_file(int directory, int device)
{
    int fd = openat(directory, "ldlinux.sys", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        printf("LOADER error=%d\n", errno);
        return 0;
    }
    struct stat info;
    if (fstat(fd, &info) != 0 || info.st_size <= 0 || info.st_size > MAX_LOADER_BYTES) {
        close(fd);
        return 1;
    }
    unsigned count = (info.st_size + SECTOR_BYTES - 1) / SECTOR_BYTES;
    struct fiemap* map = calloc(1, sizeof(*map) + count * sizeof(struct fiemap_extent));
    sector_t* sectors = calloc(count, sizeof(*sectors));
    if (map == NULL || sectors == NULL)
        exit(1);
    map->fm_length = info.st_size;
    map->fm_flags = FIEMAP_FLAG_SYNC;
    map->fm_extent_count = count;
    int result = ioctl(fd, FS_IOC_FIEMAP, map);
    printf("FIEMAP error=%d extents=%u\n", result < 0 ? errno : 0, map->fm_mapped_extents);
    unsigned block_size = 0, block = 0;
    result = ioctl(fd, FIGETBSZ, &block_size);
    printf("FIGETBSZ error=%d bytes=%u\n", result < 0 ? errno : 0, block_size);
    result = ioctl(fd, FIBMAP, &block);
    printf("FIBMAP error=%d block=%u\n", result < 0 ? errno : 0, block);
    errno = 0;
    result = sectmap(fd, sectors, count);
    printf("SECTMAP result=%d error=%d sectors=%u\n", result, result != 0 ? errno : 0, count);
    int matched = 0;
    if (result == 0) {
        unsigned char file_bytes[SECTOR_BYTES], disk_bytes[SECTOR_BYTES];
        for (unsigned i = 0; i < count; ++i) {
            off_t offset = (off_t)i * SECTOR_BYTES;
            size_t bytes = info.st_size - offset < SECTOR_BYTES ? info.st_size - offset : SECTOR_BYTES;
            int file_read = pread(fd, file_bytes, bytes, offset) == (ssize_t)bytes;
            int disk_read = pread(device, disk_bytes, bytes, sectors[i] * SECTOR_BYTES) == (ssize_t)bytes;
            if (!file_read || !disk_read || memcmp(file_bytes, disk_bytes, bytes) != 0)
                break;
            ++matched;
        }
    }
    printf("MAP_READBACK matched=%d sectors=%u bytes=%jd\n", matched, count, (intmax_t)info.st_size);
    free(sectors);
    free(map);
    close(fd);
    return result != 0 || matched != (int)count;
}

int main(int argc, char** argv)
{
    if (argc != 5 || !fixture())
        return 77;
    setvbuf(stdout, NULL, _IONBF, 0);
    int directory = open(argv[1], O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    int device = open(argv[2], O_RDONLY | O_CLOEXEC);
    struct stat path_info, device_info;
    struct statfs filesystem;
    if (directory < 0 || device < 0 || fstat(directory, &path_info) != 0 ||
        fstat(device, &device_info) != 0 || fstatfs(directory, &filesystem) != 0)
        return 1;
    printf("MOUNT magic=%lx same_device=%d\n", (unsigned long)filesystem.f_type,
        path_info.st_dev == device_info.st_rdev);
    const long expected_magic = strcmp(argv[4], "ntfs") == 0 ? 0x5346544e : 0x65735546;
    if (filesystem.f_type != expected_magic || path_info.st_dev != device_info.st_rdev)
        return 1;
    int partition_claim = claim(argv[2]);
    int disk_claim = claim(argv[3]);
    if (partition_claim != EBUSY || disk_claim != EBUSY)
        return 1;
    int writable = open(argv[2], O_RDWR | O_SYNC | O_CLOEXEC);
    printf("RAW_OPEN error=%d\n", writable < 0 ? errno : 0);
    if (writable >= 0)
        close(writable);
    int result = map_file(directory, device);
    close(device);
    close(directory);
    return result;
}
