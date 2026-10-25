// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/freedos_copy.h"
#include "progress_policy.h"
#include <QFile>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include "platform/filesystem_magic.h"
#include <linux/msdos_fs.h>
#include <sys/ioctl.h>
#include <sys/statfs.h>
#include <unistd.h>

static const char* fault;
static bool native;
static int outputFd = -1, syncs, writes, reads, gets, sets, progress;
static unsigned milestones[5];
static uint32_t attributes;
static quint64 writtenBytes;
static rufus::FreeDosCopyResult* active;
static FILE* memoryStream;
constexpr size_t partialBytes = 17;

extern "C"
{
int __real_fstatfs64(int fd, struct statfs64* info);
int __real_openat64(int fd, const char* path, int flags, ...);
int __real_close(int fd);
int __real_fsync(int fd);
int __real_ioctl(int fd, unsigned long request, ...);
int __real_fstatat64(int fd, const char* path, struct stat64* info, int flags);
FILE* __real_open_memstream(char** output, size_t* length);
int __real_fclose(FILE* stream);
ssize_t __real_write(int fd, const void* data, size_t size);
ssize_t __real_pread64(int fd, void* data, size_t size, off64_t offset);

int IsRufusJobCancelled(void)
{
    const bool partial = strcmp(fault, "cancel-partial") == 0 && writtenBytes != 0;
    const bool completed = strcmp(fault, "cancel-file") == 0 && active->files != 0;
    const bool flushed = strcmp(fault, "cancel-sync") == 0 && syncs != 0;
    const bool finished = strcmp(fault, "cancel-final") == 0 && syncs >= 3;
    return strcmp(fault, "cancel-before") == 0 || partial || completed || flushed || finished;
}

int __wrap_fstatat64(int fd, const char* path, struct stat64* info, int flags)
{
    if (strcmp(fault, "stat-fail") == 0)
    {
        errno = EACCES;
        return -1;
    }
    return __real_fstatat64(fd, path, info, flags);
}

FILE* __wrap_open_memstream(char** output, size_t* length)
{
    if (strcmp(fault, "stream-open-fail") == 0)
    {
        errno = ENOMEM;
        return nullptr;
    }
    memoryStream = __real_open_memstream(output, length);
    return memoryStream;
}

int __wrap_fclose(FILE* stream)
{
    const bool output = stream == memoryStream;
    if (output)
        memoryStream = nullptr;
    const int result = __real_fclose(stream);
    if (output && strcmp(fault, "stream-close-fail") == 0)
    {
        errno = ENOMEM;
        return EOF;
    }
    return result;
}

void UpdateProgress(int operation, float percent)
{
    if (operation != OP_FILE_COPY || percent != -1.0f || progress >= 5)
        abort();
    milestones[progress++] = active->files;
}

int __wrap_fstatfs64(int fd, struct statfs64* info)
{
    int result = __real_fstatfs64(fd, info);
    if (result == 0 && !native && strcmp(fault, "wrong-fs") != 0)
    {
        info->f_type = MSDOS_SUPER_MAGIC;
        if (strcmp(fault, "ntfs") == 0)
            info->f_type = 0x5346544e;
        if (strcmp(fault, "ntfs-3g") == 0)
            info->f_type = FUSE_SUPER_MAGIC;
    }
    return result;
}

int __wrap_openat64(int fd, const char* path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & O_CREAT)
    {
        va_list arguments;
        va_start(arguments, flags);
        mode = mode_t(va_arg(arguments, int));
        va_end(arguments);
    }
    if (strcmp(fault, "open-fail") == 0 && (flags & O_CREAT))
    {
        errno = EACCES;
        return -1;
    }
    if (strcmp(fault, "race-file") == 0 && (flags & O_CREAT))
    {
        const int raced = __real_openat64(fd, path, flags, mode);
        if (raced < 0 || __real_write(raced, "preserve raced file", 19) != 19)
            abort();
        __real_close(raced);
    }
    const int result = __real_openat64(fd, path, flags, mode);
    if (result >= 0 && (flags & O_CREAT))
    {
        outputFd = result;
        attributes = ATTR_ARCH;
    }
    return result;
}

ssize_t __wrap_write(int fd, const void* data, size_t size)
{
    if (fd != outputFd)
        return __real_write(fd, data, size);
    writes++;
    const bool partialFailure = strcmp(fault, "partial-fail") == 0 || strcmp(fault, "partial-sync-fail") == 0;
    if (strcmp(fault, "write-fail") == 0 || (partialFailure && writtenBytes != 0))
    {
        errno = ENOSPC;
        return -1;
    }
    if (strcmp(fault, "write-zero") == 0)
        return 0;
    if (strcmp(fault, "write-eintr") == 0 && writes == 1)
    {
        errno = EINTR;
        return -1;
    }
    if (partialFailure || strcmp(fault, "cancel-partial") == 0 || strcmp(fault, "short-io") == 0)
        size = qMin(size, partialBytes);
    const ssize_t written = __real_write(fd, data, size);
    if (written > 0)
        writtenBytes += quint64(written);
    return written;
}

ssize_t __wrap_pread64(int fd, void* data, size_t size, off64_t offset)
{
    reads++;
    if (strcmp(fault, "read-fail") == 0)
    {
        errno = EIO;
        return -1;
    }
    if (strcmp(fault, "read-eof") == 0)
        return 0;
    if (strcmp(fault, "read-eintr") == 0 && reads == 1)
    {
        errno = EINTR;
        return -1;
    }
    if (strcmp(fault, "short-io") == 0)
        size = qMin(size, partialBytes);
    const ssize_t count = __real_pread64(fd, data, size, offset);
    if (count > 0 && strcmp(fault, "read-corrupt") == 0)
        static_cast<char*>(data)[0] ^= 1;
    return count;
}

int __wrap_ioctl(int fd, unsigned long request, ...)
{
    va_list arguments;
    va_start(arguments, request);
    auto* value = va_arg(arguments, uint32_t*);
    va_end(arguments);
    if (native)
    {
        if (request == FAT_IOCTL_SET_ATTRIBUTES)
            sets++;
        return __real_ioctl(fd, request, value);
    }
    if (request == FAT_IOCTL_GET_ATTRIBUTES)
    {
        gets++;
        if (strcmp(fault, "attribute-get-fail") == 0)
        {
            errno = EIO;
            return -1;
        }
        *value = strcmp(fault, "attribute-mismatch") == 0 && gets % 2 == 0 ? ATTR_ARCH : attributes;
        return 0;
    }
    if (request != FAT_IOCTL_SET_ATTRIBUTES)
        abort();
    if (strcmp(fault, "attribute-set-fail") == 0)
    {
        errno = EACCES;
        return -1;
    }
    sets++;
    attributes = *value;
    return 0;
}

int __wrap_fsync(int fd)
{
    syncs++;
    const bool numberedFailure = strncmp(fault, "sync-", 5) == 0 && syncs == atoi(fault + 5);
    if (numberedFailure || strcmp(fault, "partial-sync-fail") == 0)
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

int __wrap_close(int fd)
{
    const bool output = fd == outputFd;
    if (output)
        outputFd = -1;
    const int result = __real_close(fd);
    if (output && strcmp(fault, "close-fail") == 0)
    {
        errno = EIO;
        return -1;
    }
    return result;
}
}

static int descriptorCount()
{
    DIR* directory = opendir("/proc/self/fd");
    if (directory == nullptr)
        abort();
    int count = 0;
    while (const dirent* entry = readdir(directory))
        if (entry->d_name[0] != '.')
            count++;
    closedir(directory);
    return count;
}

int main(int argc, char** argv)
{
    if (argc != 3 && argc != 5 && argc != 6)
        return 64;
    fault = argv[2];
    native = strcmp(fault, "native") == 0;
    if (native)
    {
        QFile command(QStringLiteral("/proc/cmdline"));
        if (!command.open(QIODevice::ReadOnly) || !command.readAll().contains("rufus.freedos_copy=1"))
            return 77;
    }
    int flags = O_CLOEXEC | O_NOFOLLOW | (strcmp(fault, "opath") == 0 ? O_PATH : O_RDONLY);
    if (strcmp(fault, "not-directory") != 0)
        flags |= O_DIRECTORY;
    int root = open(argv[1], flags);
    if (root < 0)
        return 65;
    if (strcmp(fault, "bad-fd") == 0)
    {
        close(root);
        root = -1;
    }
    const off_t position = lseek(root, 0, SEEK_CUR);
    const int before = descriptorCount();
    static rufus::FreeDosCopyResult result;
    active = &result;
    int error = 0;
    unsigned payloadFiles = 0;
    if (argc == 6)
    {
        if (strcmp(argv[5], "--with-payloads") != 0)
            return 64;
        error = rufus::CopyFreeDosFiles(root, result);
        payloadFiles = result.files;
    }
    if (error == 0)
    {
        if (argc == 5 && strcmp(argv[3], "--grub4dos") == 0)
        {
            QFile loader(QString::fromLocal8Bit(argv[4]));
            if (!loader.open(QIODevice::ReadOnly)) return 66;
            error = rufus::CopyGrub4dosLoader(root, loader.readAll(), result);
        }
        else if (argc >= 5)
        {
            const char* keyboard = strcmp(argv[3], "-") == 0 ? nullptr : argv[3];
            error = rufus::WriteFreeDosStartupFiles(root, keyboard, uint32_t(strtoul(argv[4], nullptr, 10)), result);
        }
        else
            error = rufus::CopyFreeDosFiles(root, result);
    }
    printf("ERROR=%d FILES=%u BYTES=%llu SYNC=%d SYNCS=%d PROGRESS=%d SETS=%d FDS=%d OFFSET=%d FLAGS=%d",
        error, result.files, static_cast<unsigned long long>(result.bytesWritten), result.syncError, syncs, progress, sets,
        descriptorCount() - before, lseek(root, 0, SEEK_CUR) == position,
        fcntl(root, F_GETFD) == FD_CLOEXEC);
    for (int i = 0; i < progress; i++)
        printf(" P%d=%u", i, milestones[i]);
    if (argc >= 5)
        printf(" PAYLOADS=%u", payloadFiles);
    printf(" PATH=%s\n", result.path.toUtf8().constData());
    close(root);
    return 0;
}
