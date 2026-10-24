/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "platform/ext_format.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static const char* fault = "none";
static bool formatted;
static int syncs;
enum { INITIAL_POSITION = 17, SUPERBLOCK_OFFSET = 1024, FEATURE_OFFSET = SUPERBLOCK_OFFSET + 92 };

extern "C" int IsRufusJobCancelled(void)
{
    return strcmp(fault, "cancel-before") == 0;
}

extern "C" void uprintf(const char* format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    vfprintf(stderr, format, arguments);
    va_end(arguments);
}

extern "C" int __real_fsync(int);
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
    return __real_fsync(fd);
}

extern "C" int __real_RunFormatCommand(int, char* const*, FORMAT_RESULT*);
extern "C" int __wrap_RunFormatCommand(int fd, char* const* args, FORMAT_RESULT* result)
{
    const int error = __real_RunFormatCommand(fd, args, result);
    formatted = true;
    if (error != 0)
        return error;
    if (strcmp(fault, "shrink") == 0)
    {
        struct stat info;
        if (fstat(fd, &info) != 0 || ftruncate(fd, info.st_size - 512) != 0)
            abort();
    }
    off_t offset = -1;
    if (strcmp(fault, "corrupt-feature") == 0)
        offset = FEATURE_OFFSET;
    if (strcmp(fault, "corrupt-label") == 0)
        offset = SUPERBLOCK_OFFSET + 120;
    if (offset >= 0)
    {
        unsigned char byte;
        if (pread(fd, &byte, 1, offset) != 1)
            abort();
        byte ^= 1;
        if (pwrite(fd, &byte, 1, offset) != 1)
            abort();
    }
    return 0;
}

extern "C" ssize_t __real_pread64(int, void*, size_t, off_t);
extern "C" ssize_t __wrap_pread64(int fd, void* data, size_t size, off_t offset)
{
    if (formatted && strcmp(fault, "read-fail") == 0)
    {
        errno = EIO;
        return -1;
    }
    return __real_pread64(fd, data, size, offset);
}

static int DescriptorCount(void)
{
    DIR* directory = opendir("/proc/self/fd");
    if (directory == nullptr)
        abort();
    int count = 0;
    struct dirent* entry;
    while ((entry = readdir(directory)) != nullptr)
        if (entry->d_name[0] != '.')
            count++;
    closedir(directory);
    return count;
}

int main(int argc, char** argv)
{
    if (argc != 7 && argc != 8 && argc != 9)
        return 64;
    fault = argv[6];
    rufus::ExtFormatOptions options;
    options.size = strtoull(argv[3], nullptr, 10);
    options.fileSystem = strcmp(argv[4], "ext2") == 0 ? FS_EXT2 :
        strcmp(argv[4], "ext3") == 0 ? FS_EXT3 : FS_EXT4;
    options.label = QString::fromUtf8(argv[5]);
    const char* logicalGeometry = getenv("RUFUS_TEST_SECTOR_SIZE");
    const char* physicalGeometry = getenv("RUFUS_TEST_PHYSICAL_SECTOR_SIZE");
    if (logicalGeometry != nullptr) options.sectorSize = quint32(strtoul(logicalGeometry, nullptr, 10));
    if (physicalGeometry != nullptr) options.physicalSectorSize = quint32(strtoul(physicalGeometry, nullptr, 10));
    for (int i = 7; i < argc; i++) {
        if (strcmp(argv[i], "persistence") == 0) options.persistenceConf = true;
        if (strcmp(argv[i], "full") == 0) options.quickFormat = false;
    }
    if (strcmp(fault, "label-nul") == 0)
        options.label.append(QChar(u'\0'));
    if (strcmp(fault, "label-surrogate") == 0)
        options.label.append(QChar(0xd800));
    int flags = strcmp(fault, "readonly") == 0 ? O_RDONLY : O_RDWR;
    if (strcmp(fault, "append") == 0)
        flags |= O_APPEND;
    int fd = open(argv[2], flags | O_CLOEXEC);
    if (fd < 0)
        return 65;
    if (strcmp(fault, "pipe") == 0)
    {
        int ends[2];
        close(fd);
        if (pipe2(ends, O_CLOEXEC) != 0)
            abort();
        fd = ends[0];
        close(ends[1]);
    }
    else if (lseek(fd, INITIAL_POSITION, SEEK_SET) != INITIAL_POSITION)
        return 66;
    const int descriptorFlags = fcntl(fd, F_GETFD);
    const int statusFlags = fcntl(fd, F_GETFL);
    const int descriptors = DescriptorCount();
    FORMAT_RESULT result = {};
    int error;
    if (strcmp(fault, "device-zero") == 0)
        error = rufus::FormatExtDevice(fd, 0, QString::fromUtf8(argv[1]), options, result);
    else if (strcmp(fault, "device-file") == 0)
        error = rufus::FormatExtDevice(fd, 1, QString::fromUtf8(argv[1]), options, result);
    else
        error = rufus::FormatExtImage(fd, QString::fromUtf8(argv[1]), options, result);
    const off_t position = lseek(fd, 0, SEEK_CUR);
    const int remaining = DescriptorCount() - descriptors;
    const bool flagsUnchanged = fcntl(fd, F_GETFD) == descriptorFlags && fcntl(fd, F_GETFL) == statusFlags;
    close(fd);
    int status;
    errno = 0;
    const int child = waitpid(-1, &status, WNOHANG);
    const int childError = errno;
    printf("ERROR=%d STARTED=%d EXIT=%d SIGNAL=%d SYNC=%d SYNCS=%d BYTES=%" PRIu64
           " OFFSET=%lld FLAGS=%d FDS=%d CHILD=%d CHILD_ERRNO=%d DIAG=%zu\n",
        error, result.started, result.exit_code, result.signal, result.sync_error,
        syncs, result.filesystem_bytes, (long long)position, flagsUnchanged, remaining,
        child, childError, strlen(result.diagnostics));
    return 0;
}
