/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Reuse audit only: original ms-sys MBR writer with ordinary bounded stdio,
 * followed by WriteSBR's byte placement. Regular files only; no device adapter. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include "file.h"
#include "br.h"

enum { SECTOR_BYTES = 512, MAX_CORE_BYTES = 1024 * 1024,
    FIRST_PARTITION_LBA = 454 };

int read_data(FILE* file, uint64_t position, void* data, uint64_t size)
{
    if (position > SECTOR_BYTES || size > SECTOR_BYTES - position)
        return 0;
    if (fseeko(file, (off_t)position, SEEK_SET) != 0)
        return 0;
    return fread(data, 1, (size_t)size, file) == size;
}

int write_data(FILE* file, uint64_t position, const void* data, uint64_t size)
{
    if (position > SECTOR_BYTES || size > SECTOR_BYTES - position)
        return 0;
    if (fseeko(file, (off_t)position, SEEK_SET) != 0)
        return 0;
    return fwrite(data, 1, (size_t)size, file) == size;
}

int contains_data(FILE* file, uint64_t position, const void* data, uint64_t size)
{
    unsigned char buffer[SECTOR_BYTES];
    if (size > sizeof(buffer) || !read_data(file, position, buffer, size))
        return 0;
    return memcmp(buffer, data, (size_t)size) == 0;
}

static FILE* OpenRegular(const char* path, int flags, const char* mode, off_t* size)
{
    const int fd = open(path, flags | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0)
        return NULL;
    struct stat info;
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) {
        close(fd);
        return NULL;
    }
    FILE* file = fdopen(fd, mode);
    if (file == NULL)
        close(fd);
    *size = info.st_size;
    return file;
}

static bool WriteRecords(FILE* target, off_t disk_size, FILE* core, off_t core_size)
{
    unsigned char sector[SECTOR_BYTES];
    const bool payload_size = core_size >= SECTOR_BYTES && core_size <= MAX_CORE_BYTES;
    if (!payload_size || !read_data(target, 0, sector, sizeof(sector)))
        return false;
    uint64_t first_lba = 0;
    for (unsigned i = 0; i < sizeof(uint32_t); i++)
        first_lba |= (uint64_t)sector[FIRST_PARTITION_LBA + i] << (8 * i);
    const uint64_t first_offset = first_lba * SECTOR_BYTES;
    const bool valid_partition = first_offset >= SECTOR_BYTES && first_offset < (uint64_t)disk_size;
    if (!valid_partition || (uint64_t)core_size > first_offset - SECTOR_BYTES)
        return false;
    unsigned char* data = malloc((size_t)core_size);
    if (data == NULL)
        return false;
    bool written = fread(data, 1, (size_t)core_size, core) == (size_t)core_size;
    if (written)
        written = write_grub2_mbr(target) != 0; // Original Rufus function, unmodified.
    if (written)
        written = fseeko(target, SECTOR_BYTES, SEEK_SET) == 0;
    if (written)
        written = fwrite(data, 1, (size_t)core_size, target) == (size_t)core_size;
    free(data);
    return written;
}

int main(int argc, char** argv)
{
    if (argc != 3)
        return 64;
    off_t core_size = 0, disk_size = 0;
    FILE* core = OpenRegular(argv[2], O_RDONLY, "rb", &core_size);
    FILE* target = OpenRegular(argv[1], O_RDWR, "r+b", &disk_size);
    bool written = core != NULL && target != NULL;
    if (written)
        written = WriteRecords(target, disk_size, core, core_size);
    if (target != NULL) {
        if (fflush(target) != 0 || fsync(fileno(target)) != 0)
            written = false;
        if (fclose(target) != 0)
            written = false;
    }
    if (core != NULL && fclose(core) != 0)
        written = false;
    return written ? 0 : 1;
}
