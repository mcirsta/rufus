/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "platform/exfat_format.h"
#include <blkid/blkid.h>
#include <QCoreApplication>
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

static const char* fault = "none";
static int64_t start;
static int syncs, ticks;
static bool formatted;
static unsigned testSectorBytes = 512;
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
extern "C" int __real_blkid_get_library_version(const char**, const char**);

extern "C" int __wrap_blkid_get_library_version(const char** version, const char** date)
{
    if (fault != nullptr && strcmp(fault, "old-library") == 0)
        return blkid_parse_version_string("2.38.0");
    return __real_blkid_get_library_version(version, date);
}

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
    return __real_fsync(fd);
}

extern "C" int __real_RunFormatCommand(int, char* const*, FORMAT_RESULT*);
extern "C" int __wrap_RunFormatCommand(int fd, char* const* args, FORMAT_RESULT* result)
{
    const int error = __real_RunFormatCommand(fd, args, result);
    formatted = true;
    if (error != 0)
        return error;
    const off_t bootRegionBytes = 12 * testSectorBytes;
    off_t offset = -1;
    if (strcmp(fault, "corrupt-primary") == 0 || strcmp(fault, "corrupt-both") == 0)
        offset = 120; // Boot code participates in the checksum.
    if (strcmp(fault, "corrupt-backup") == 0)
        offset = bootRegionBytes + 120;
    if (offset >= 0)
    {
        unsigned char byte;
        if (pread(fd, &byte, 1, offset) != 1)
            abort();
        byte ^= 1;
        if (pwrite(fd, &byte, 1, offset) != 1)
            abort();
        if (strcmp(fault, "corrupt-both") == 0 && pwrite(fd, &byte, 1, offset + bootRegionBytes) != 1)
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
    QCoreApplication application(argc, argv);
    rufus::ExfatFormatOptions options;
    FORMAT_RESULT result;
    int fd, error, before, flags, status, child, child_error, leaks, original_flags;
    off_t position;

    if (argc == 2 && strcmp(argv[1], "--check-library") == 0)
        return blkid_get_library_version(nullptr, nullptr) >= blkid_parse_version_string("2.39.0") ? 0 : 77;
    if (argc == 4 && strcmp(argv[1], "--clusters") == 0)
    {
        const quint64 diskSize = strtoull(argv[2], nullptr, 10);
        const quint64 volumeSize = strtoull(argv[3], nullptr, 10);
        const RUFUS_CLUSTER_SIZES sizes = rufus::NativeExfatClusterSizes(diskSize, volumeSize);
        printf("%u %u\n", sizes.Allowed, sizes.Default);
        return 0;
    }
    if (argc == 4 && strcmp(argv[1], "--tools") == 0)
    {
        fault = argv[3];
        start = Milliseconds();
        QString problem;
        const int descriptors = DescriptorCount();
        error = rufus::CheckExfatFormatTools(QString::fromUtf8(argv[2]), problem);
        child = waitpid(-1, &status, WNOHANG);
        child_error = errno;
        printf("ERROR=%d FDS=%d CHILD=%d CHILD_ERRNO=%d PROBLEM=%d\n", error,
            DescriptorCount() - descriptors, child, child_error, !problem.isEmpty());
        return 0;
    }
    if (argc != 7 && argc != 8)
        return 64;
    fault = argv[6];
    options.quickFormat = argc != 8 || strcmp(argv[7], "full") != 0;
    options.size = strtoull(argv[3], NULL, 10);
    options.clusterSize = (uint32_t)strtoul(argv[4], NULL, 10);
    options.label = QString::fromUtf8(argv[5]);
    const char* logicalGeometry = getenv("RUFUS_TEST_SECTOR_SIZE");
    const char* physicalGeometry = getenv("RUFUS_TEST_PHYSICAL_SECTOR_SIZE");
    if (logicalGeometry != nullptr) options.sectorSize = quint32(strtoul(logicalGeometry, nullptr, 10));
    testSectorBytes = options.sectorSize;
    if (physicalGeometry != nullptr) options.physicalSectorSize = quint32(strtoul(physicalGeometry, nullptr, 10));
    if (strcmp(fault, "file-offset") == 0)
        options.partitionOffset = 1024 * 1024;
    if (strcmp(fault, "unaligned-offset") == 0)
        options.partitionOffset = 512;
    if (strcmp(fault, "overflow-offset") == 0)
        options.partitionOffset = UINT64_MAX - 1024 * 1024 + 1;
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
    const int originalStatusFlags = fcntl(fd, F_GETFL);
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
    error = rufus::FormatExfatImage(fd, QString::fromUtf8(argv[1]), options, result);
    position = lseek(fd, 0, SEEK_CUR);
    leaks = DescriptorCount() - before;
    child = waitpid(-1, &status, WNOHANG);
    child_error = errno;
    const bool flagsUnchanged = fcntl(fd, F_GETFD) == original_flags && fcntl(fd, F_GETFL) == originalStatusFlags;
    printf("ERROR=%d STARTED=%d EXIT=%d SIGNAL=%d SYNC=%d SYNCS=%d BYTES=%" PRIu64
        " FDS=%d OFFSET=%jd FLAGS=%d CHILD=%d CHILD_ERRNO=%d DIAG=%zu MS=%" PRId64
        " LOCALE=%d\n",
        error, result.started, result.exit_code, result.signal, result.sync_error, syncs,
        result.filesystem_bytes, leaks, (intmax_t)position, flagsUnchanged,
        child, child_error, strlen(result.diagnostics), Milliseconds() - start, qgetenv("LC_ALL") == originalLocale);
    close(fd);
    return 0;
}
