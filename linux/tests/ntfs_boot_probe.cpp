/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "platform/ntfs_boot.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char* fault;
static int target, syncs, writes, reads;
static quint64 written, volume_size;
enum { BOOT_RECORD_BYTES = 4136 };
extern "C" int __real_fsync(int);
extern "C" ssize_t __real_pread64(int, void*, size_t, off64_t);
extern "C" ssize_t __real_pwrite64(int, const void*, size_t, off64_t);
extern "C" int __real_write_data(FILE*, uint64_t, const void*, uint64_t);

extern "C" int IsRufusJobCancelled(void)
{
    return strcmp(fault, "cancel-before") == 0 ||
        (strcmp(fault, "cancel-partial") == 0 && written != 0) ||
        (strcmp(fault, "cancel-after-write") == 0 && written >= BOOT_RECORD_BYTES);
}

extern "C" void uprintf(const char* format, ...) { (void)format; }

extern "C" int __wrap_write_data(FILE* stream, uint64_t position, const void* data, uint64_t length)
{
    if (strcmp(fault, "patch-fail") == 0)
        return 0;
    return __real_write_data(stream, position, data, length);
}

extern "C" ssize_t __wrap_pwrite64(int fd, const void* data, size_t size, off64_t offset)
{
    if (fd != target || offset < 0)
        abort();
    writes++;
    if (strcmp(fault, "write-fail") == 0 ||
        (strcmp(fault, "partial-fail") == 0 && written != 0))
    {
        errno = EIO;
        return -1;
    }
    if (strcmp(fault, "write-zero") == 0)
        return 0;
    if (strcmp(fault, "write-eintr") == 0 && writes == 1)
    {
        errno = EINTR;
        return -1;
    }
    if (strcmp(fault, "short-write") == 0)
        size = size > 73 ? 73 : size;
    if (strcmp(fault, "partial-fail") == 0 || strcmp(fault, "cancel-partial") == 0)
        size = size > 17 ? 17 : size;
    const ssize_t count = __real_pwrite64(fd, data, size, offset);
    if (count > 0)
        written += quint64(count);
    return count;
}

extern "C" ssize_t __wrap_pread64(int fd, void* data, size_t size, off64_t offset)
{
    if (fd != target)
        abort();
    reads++;
    const bool verifying = syncs != 0;
    if (strcmp(fault, "read-fail") == 0 ||
        (strcmp(fault, "verify-fail") == 0 && verifying))
    {
        errno = EIO;
        return -1;
    }
    if (strcmp(fault, "verify-eof") == 0 && verifying)
        return 0;
    if (strcmp(fault, "read-eintr") == 0 && reads == 1)
    {
        errno = EINTR;
        return -1;
    }
    if (strcmp(fault, "short-read") == 0 && reads > 4)
        size = size > 73 ? 73 : size;
    const ssize_t count = __real_pread64(fd, data, size, offset);
    const bool backup = quint64(offset) == volume_size - 512;
    if (count > 0 && verifying &&
        (strcmp(fault, "verify-corrupt") == 0 ||
         (strcmp(fault, "backup-corrupt") == 0 && backup)))
        static_cast<unsigned char*>(data)[count - 1] ^= 1;
    return count;
}

extern "C" int __wrap_fsync(int fd)
{
    syncs++;
    if (strcmp(fault, "sync-fail") == 0)
    {
        errno = EIO;
        return -1;
    }
    if (strcmp(fault, "sync-eintr") == 0 && syncs == 1)
    {
        errno = EINTR;
        return -1;
    }
    if (strcmp(fault, "resize") == 0 && ftruncate(fd, off_t(volume_size + 512)) != 0)
        abort();
    return __real_fsync(fd);
}

static int descriptorCount(void)
{
    DIR* directory = opendir("/proc/self/fd");
    if (directory == nullptr)
        abort();
    int count = 0;
    dirent* entry;
    while ((entry = readdir(directory)) != nullptr)
        if (entry->d_name[0] != '.')
            count++;
    closedir(directory);
    return count;
}

int main(int argc, char** argv)
{
    if (argc != 6)
        return 64;
    fault = argv[5];
    rufus::NtfsFormatOptions options;
    options.partitionOffset = strtoull(argv[2], nullptr, 10);
    options.size = volume_size = strtoull(argv[3], nullptr, 10);
    options.clusterSize = uint32_t(strtoul(argv[4], nullptr, 10));
    options.label = QStringLiteral("Rufus test");
    const char* logicalGeometry = getenv("RUFUS_TEST_SECTOR_SIZE");
    const char* physicalGeometry = getenv("RUFUS_TEST_PHYSICAL_SECTOR_SIZE");
    if (logicalGeometry != nullptr) options.sectorSize = quint32(strtoul(logicalGeometry, nullptr, 10));
    if (physicalGeometry != nullptr) options.physicalSectorSize = quint32(strtoul(physicalGeometry, nullptr, 10));
    if (strcmp(fault, "wrong-offset") == 0)
        options.partitionOffset += 512;
    if (strcmp(fault, "wrong-cluster") == 0)
        options.clusterSize *= 2;
    int flags = strcmp(fault, "readonly") == 0 ? O_RDONLY : O_RDWR;
    if (strcmp(fault, "append") == 0)
        flags |= O_APPEND;
    if (strcmp(fault, "direct") == 0)
        flags |= O_DIRECT;
    target = open(argv[1], flags | O_CLOEXEC);
    if (target < 0 || lseek(target, 17, SEEK_SET) != 17)
        return 65;
    const int descriptors = descriptorCount();
    const int originalFlags = fcntl(target, F_GETFL);
    rufus::NtfsBootResult result;
    const quint64 device = strcmp(fault, "block-request") == 0 ? 1 : 0;
    const int error = rufus::InstallNtfsBootRecord(target, device, options, result);
    printf("ERROR=%d BYTES=%" PRIu64 " SYNC=%d SYNCS=%d WRITES=%d FDS=%d OFFSET=%jd FLAGS=%d\n",
        error, uint64_t(result.bytesWritten), result.syncError, syncs, writes,
        descriptorCount() - descriptors, intmax_t(lseek(target, 0, SEEK_CUR)),
        fcntl(target, F_GETFL) == originalFlags && fcntl(target, F_GETFD) == FD_CLOEXEC);
    close(target);
    return 0;
}
