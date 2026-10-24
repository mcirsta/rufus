/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "platform/ntfs_format.h"
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
#include <time.h>
#include <unistd.h>

static const char* fault;
static int64_t start;
static int syncs, ticks;
enum
{
    MILLISECONDS_PER_SECOND = 1000,
    NANOSECONDS_PER_MILLISECOND = 1000000,
    TIMEOUT_STEP_SECONDS = 61,
    CANCEL_DELAY_MS = 150,
    INITIAL_POSITION = 17
};
extern "C" int __real_fsync(int);
extern "C" int __real_clock_gettime(clockid_t, struct timespec*);

static int64_t Milliseconds(void)
{
    struct timespec now;
    if (__real_clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        abort();
    return (int64_t)now.tv_sec * MILLISECONDS_PER_SECOND + now.tv_nsec / NANOSECONDS_PER_MILLISECOND;
}

extern "C" int __wrap_clock_gettime(clockid_t clock, struct timespec* now)
{
    int result = __real_clock_gettime(clock, now);
    if (result == 0 && strcmp(fault, "timeout") == 0 && clock == CLOCK_MONOTONIC)
        now->tv_sec += TIMEOUT_STEP_SECONDS * ticks++;
    return result;
}

extern "C" int IsRufusJobCancelled(void)
{
    bool cancel_running = strcmp(fault, "cancel-running") == 0 && Milliseconds() - start >= CANCEL_DELAY_MS;
    return strcmp(fault, "cancel-before") == 0 || cancel_running;
}

extern "C" void uprintf(const char* format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    vfprintf(stderr, format, arguments);
    va_end(arguments);
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
    if (strcmp(fault, "corrupt-backup-padding") == 0 && syncs == 1)
    {
        struct stat info;
        const char* logical = getenv("RUFUS_TEST_SECTOR_SIZE");
        const uint32_t sector = logical != nullptr ? uint32_t(strtoul(logical, nullptr, 10)) : 512;
        if (fstat(fd, &info) != 0 || sector <= 512) abort();
        const off_t offset = info.st_size - sector + 512;
        unsigned char byte;
        if (pread(fd, &byte, 1, offset) != 1) abort();
        byte ^= 1;
        if (pwrite(fd, &byte, 1, offset) != 1) abort();
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
    rufus::NtfsFormatOptions options;
    rufus::NtfsFormatResult outcome;
    FORMAT_RESULT& result = outcome.format;
    int fd, error, before, flags, status, child, child_error, leaks, original_flags;
    off_t position;

    if (argc != 8 && argc != 9)
        return 64;
    fault = argv[7];
    options.quickFormat = argc != 9 || strcmp(argv[8], "full") != 0;
    options.partitionOffset = strtoull(argv[3], NULL, 10);
    options.size = strtoull(argv[4], NULL, 10);
    options.clusterSize = (uint32_t)strtoul(argv[5], NULL, 10);
    options.label = QString::fromUtf8(argv[6]);
    const char* logicalGeometry = getenv("RUFUS_TEST_SECTOR_SIZE");
    const char* physicalGeometry = getenv("RUFUS_TEST_PHYSICAL_SECTOR_SIZE");
    if (logicalGeometry != nullptr) options.sectorSize = quint32(strtoul(logicalGeometry, nullptr, 10));
    if (physicalGeometry != nullptr) options.physicalSectorSize = quint32(strtoul(physicalGeometry, nullptr, 10));
    if (strcmp(fault, "label-nul") == 0)
        options.label.append(QChar(u'\0'));
    if (strcmp(fault, "label-surrogate") == 0)
        options.label.append(QChar(0xd800));
    flags = strcmp(fault, "readonly") == 0 ? O_RDONLY : O_RDWR;
    if (strcmp(fault, "append") == 0)
        flags |= O_APPEND;
    fd = open(argv[2], flags | O_CLOEXEC);
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
    original_flags = fcntl(fd, F_GETFD);
    if (strcmp(fault, "replace-path") == 0)
    {
        char retained[4096];
        snprintf(retained, sizeof(retained), "%s.held", argv[2]);
        if (rename(argv[2], retained) != 0)
            abort();
        int replacement = open(argv[2], O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (replacement < 0 || write(replacement, "retained", 8) != 8)
            abort();
        close(replacement);
    }
    before = DescriptorCount();
    const QByteArray originalLocale = qgetenv("LC_ALL");
    start = Milliseconds();
    if (strcmp(fault, "block-request") == 0 || strcmp(fault, "missing-device") == 0)
    {
        const quint64 device = strcmp(fault, "block-request") == 0 ? 1 : 0;
        error = rufus::FormatNtfsDevice(fd, device, QString::fromUtf8(argv[1]), options, outcome);
    }
    else if (strcmp(fault, "native") == 0)
    {
        struct stat info;
        if (fstat(fd, &info) != 0 || !S_ISBLK(info.st_mode))
            return 67;
        error = rufus::FormatNtfsDevice(fd, info.st_rdev, QString::fromUtf8(argv[1]), options, outcome);
    }
    else
        error = rufus::FormatNtfsImage(fd, QString::fromUtf8(argv[1]), options, outcome);
    position = lseek(fd, 0, SEEK_CUR);
    leaks = DescriptorCount() - before;
    child = waitpid(-1, &status, WNOHANG);
    child_error = errno;
    printf("ERROR=%d STARTED=%d EXIT=%d SIGNAL=%d SYNC=%d SYNCS=%d BYTES=%" PRIu64
        " VOLUME_ID=%" PRIu64 " FDS=%d OFFSET=%jd FLAGS=%d CHILD=%d CHILD_ERRNO=%d DIAG=%zu MS=%" PRId64
        " LOCALE=%d\n",
        error, result.started, result.exit_code, result.signal, result.sync_error, syncs,
        result.filesystem_bytes, uint64_t(outcome.volumeId), leaks, (intmax_t)position, fcntl(fd, F_GETFD) == original_flags,
        child, child_error, strlen(result.diagnostics), Milliseconds() - start, qgetenv("LC_ALL") == originalLocale);
    close(fd);
    return 0;
}
