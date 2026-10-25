// SPDX-License-Identifier: GPL-3.0-or-later
// File-only worker; faults deliberately damage disposable test images.
#include "platform/syslinux_install.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>
extern "C"
{
#include "syslinux/libfat/libfat.h"
#include "syslinux/libfat/fat.h"
}

static const char* fault;
static const char* sourcePath;
static int targetFd, syncs;
static bool checking, completed;
static uint64_t volumeOffset;
static int fatBits = 32;
static QByteArray readyPath;
extern "C" void _UpdateProgressWithInfo(int operation, int message, uint64_t processed, uint64_t total, int force)
{
    (void)operation; (void)message; (void)processed; (void)total; (void)force;
}

extern "C" void uprintf(const char*, ...) {}
extern "C" int IsIsoScanCancelled(void)
{
    return checking && strcmp(fault, "cancel-before") == 0;
}
extern "C" int IsRufusJobCancelled(void)
{
    const bool childReady = strcmp(fault, "cancel-native") == 0 &&
        !readyPath.isEmpty() && access(readyPath.constData(), F_OK) == 0;
    return IsIsoScanCancelled() || childReady || (completed && strcmp(fault, "cancel-after") == 0);
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
    return __real_fsync(fd);
}
extern "C" ssize_t __real_pread64(int, void*, size_t, off_t);
extern "C" ssize_t __wrap_pread64(int fd, void* buffer, size_t size, off_t offset)
{
    if (fd == targetFd && completed && strcmp(fault, "read-fail") == 0)
    {
        errno = EIO;
        return -1;
    }
    return __real_pread64(fd, buffer, size, offset);
}

static int readSector(intptr_t fd, void* buffer, size_t bytes, libfat_sector_t sector)
{
    return int(__real_pread64(int(fd), buffer, bytes, off_t(volumeOffset + sector * 512)));
}

static void flipByte(int fd, uint64_t offset)
{
    unsigned char value;
    if (__real_pread64(fd, &value, 1, off_t(offset)) != 1)
        qFatal("Fault read failed");
    value ^= 0x80;
    if (pwrite(fd, &value, 1, off_t(offset)) != 1)
        qFatal("Fault write failed");
}

static void damageTarget(int fd)
{
    if (strcmp(fault, "boot-code") == 0)
        flipByte(fd, volumeOffset + 100);
    if (strcmp(fault, "boot-bpb") == 0)
        flipByte(fd, volumeOffset + (fatBits == 16 ? 36 : 64)); // Drive-number field.
    if (strcmp(fault, "backup") == 0)
        flipByte(fd, volumeOffset + 6 * 512 + 100);
    const bool damageLoader = strncmp(fault, "loader-", 7) == 0;
    const bool damageModule = strcmp(fault, "module-bytes") == 0;
    if (!damageLoader && !damageModule)
        return;
    libfat_filesystem* fs = libfat_open(readSector, fd);
    struct libfat_direntry entry;
    const int32_t cluster = libfat_searchdir(fs, 0, damageModule ? "LDLINUX C32" : "LDLINUX SYS", &entry);
    if (cluster < 2)
        qFatal("Cannot locate fault file");
    libfat_sector_t first = libfat_clustertosector(fs, cluster);
    libfat_close(fs);
    if (strcmp(fault, "loader-bytes") == 0 || damageModule)
        flipByte(fd, volumeOffset + first * 512 + 100);
    else if (strcmp(fault, "loader-size") == 0)
        flipByte(fd, volumeOffset + entry.sector * 512 + entry.offset + offsetof(fat_dirent, size));
    else if (strcmp(fault, "loader-directory") == 0)
    {
        const unsigned char directory = 0x10;
        if (pwrite(fd, &directory, 1, off_t(volumeOffset + entry.sector * 512 + entry.offset + offsetof(fat_dirent, attribute))) != 1)
            qFatal("Cannot change file type");
    }
    else if (strcmp(fault, "loader-cycle") == 0 || strcmp(fault, "loader-short") == 0)
    {
        struct fat_bootsect boot;
        if (readSector(fd, &boot, sizeof(boot), 0) != sizeof(boot))
            qFatal("Cannot read test BPB");
        const bool cycle = strcmp(fault, "loader-cycle") == 0;
        const uint32_t endOfChain = fatBits == 16 ? 0xffff : 0x0fffffff;
        const int entryBytes = fatBits == 16 ? 2 : 4;
        le32_t value;
        write32(&value, cycle ? uint32_t(cluster) : endOfChain);
        const uint32_t fatSectors = fatBits == 16 ? read16(&boot.bsFATsecs) : read32(&boot.u.fat32.bpb_fatsz32);
        for (int copy = 0; copy < 2; copy++)
        {
            const uint64_t fatSector = read16(&boot.bsResSectors) + copy * fatSectors;
            if (pwrite(fd, &value, size_t(entryBytes), off_t(volumeOffset + fatSector * 512 + cluster * entryBytes)) != entryBytes)
                qFatal("Cannot change test FAT");
        }
    }
}

extern "C" int __real_RunInstallerCommand(int, char* const*, char* const*, const char*, FORMAT_RESULT*);
extern "C" int __wrap_RunInstallerCommand(int fd, char* const* arguments, char* const* environment,
    const char* directory, FORMAT_RESULT* result)
{
    readyPath = QByteArray(directory) + "/ready";
    const int error = __real_RunInstallerCommand(fd, arguments, environment, directory, result);
    if (error == 0)
    {
        damageTarget(fd);
        if (strcmp(fault, "source-changed") == 0)
        {
            struct stat st;
            if (stat(sourcePath, &st) != 0)
                qFatal("Cannot stat test source");
            struct timespec times[] = {st.st_atim, st.st_mtim};
            times[1].tv_sec++;
            if (utimensat(AT_FDCWD, sourcePath, times, 0) != 0)
                qFatal("Cannot change source timestamp");
        }
        if (strcmp(fault, "target-size") == 0 && ftruncate(fd, 4096) != 0)
            qFatal("Cannot truncate test target");
        if (strcmp(fault, "cleanup-fail") == 0)
        {
            const QByteArray blocked = QByteArray(directory) + "/blocked";
            const QByteArray file = blocked + "/keep";
            if (mkdir(blocked.constData(), 0700) != 0)
                qFatal("Cannot create cleanup fault");
            int fileFd = open(file.constData(), O_CREAT | O_EXCL | O_WRONLY, 0600);
            if (fileFd < 0 || close(fileFd) != 0 || chmod(blocked.constData(), 0) != 0)
                qFatal("Cannot block cleanup");
        }
    }
    completed = true;
    return error;
}

static int descriptorCount()
{
    DIR* directory = opendir("/proc/self/fd");
    if (directory == nullptr)
        qFatal("Cannot count descriptors");
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
    QCoreApplication app(argc, argv);
    if (argc != 10 && argc != 11)
        return 64;
    if (argc == 11)
        fatBits = atoi(argv[10]);
    sourcePath = argv[1];
    const bool standalone4 = strcmp(sourcePath, "--standalone4") == 0;
    const bool standalone = standalone4 || strcmp(sourcePath, "--standalone") == 0;
    fault = argv[6];
    volumeOffset = strtoull(argv[7], nullptr, 10);
    if (strcmp(fault, "no-subreaper") != 0 && prctl(PR_SET_CHILD_SUBREAPER, 1) != 0)
        return 65;
    const int before = descriptorCount();
    IMAGE_FILE_SOURCE retained = {};
    ISO_ANALYSIS_SOURCE source = {};
    ISO_ANALYSIS analysis = {};
    if (!standalone)
    {
        if (OpenImageFileSource(sourcePath, &retained) != 0)
            return 66;
        char path[64];
        snprintf(path, sizeof(path), "/proc/self/fd/%d", retained.fd);
        const ISO_ANALYSIS_OPTIONS options = {true, false, {0x0407, 0x0604}};
        const int scanned = AnalyzeIsoImageWithSource(path, uint64_t(retained.info.st_size), &options, &analysis, &source);
        if (scanned != ISO_SCAN_OK && scanned != ISO_SCAN_PARTIAL)
            return 67;
    }
    const int access = strcmp(fault, "read-only") == 0 ? O_RDONLY : O_RDWR;
    const int append = strcmp(fault, "append") == 0 ? O_APPEND : 0;
    targetFd = open(argv[2], access | append | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (targetFd < 0 || lseek(targetFd, 17, SEEK_SET) != 17)
        return 68;
    const int flags = fcntl(targetFd, F_GETFL), fdFlags = fcntl(targetFd, F_GETFD);
    rufus::NativeSyslinuxRequest request;
    request.target = targetFd;
    request.fileSystem = fatBits == 16 ? FS_FAT16 : FS_FAT32;
    request.format = {volumeOffset, strtoull(argv[8], nullptr, 10), uint32_t(strtoul(argv[9], nullptr, 10)), 0x1234abcd, "RUFUS_TEST", 0, 0, 0};
    if (strcmp(fault, "no-label") == 0)
        strcpy(request.format.label, "NO NAME");
    request.bootType = standalone ? BT_SYSLINUX_V6 : BT_IMAGE;
    if (standalone4)
        request.bootType = BT_SYSLINUX_V4;
    if (!standalone)
    {
        request.image = &retained;
        request.imagePath = sourcePath;
        request.source = &source;
        request.analysis = &analysis;
    }
    request.installer = QString::fromLocal8Bit(argv[3]);
    request.toolsDirectory = QString::fromLocal8Bit(argv[4]);
    request.temporaryRoot = QString::fromLocal8Bit(argv[5]);
    if (strcmp(fault, "incomplete-source") == 0)
        analysis.tree_complete = false;
    if (strcmp(fault, "wrong-geometry") == 0)
        request.format.cluster_size *= 2;
    if (strcmp(fault, "wrong-volume-id") == 0)
        request.format.volume_id ^= 1;
    if (strcmp(fault, "wrong-filesystem") == 0)
        request.fileSystem = fatBits == 16 ? FS_FAT32 : FS_FAT16;
    if (strcmp(fault, "unsupported-filesystem") == 0)
        request.fileSystem = FS_NTFS;
    if (strcmp(fault, "unsupported-selection") == 0)
        request.bootType = BT_GRUB2;
    if (strcmp(fault, "image-version4") == 0)
        analysis.report.sl_version = 0x0407;
    if (strcmp(fault, "invalid-selection") == 0)
        request.bootType = BT_MAX;
    if (strcmp(fault, "wrong-source-mode") == 0)
        request.bootType = standalone ? BT_IMAGE : BT_SYSLINUX_V6;
    if (strcmp(fault, "source-image") == 0)
        request.image = standalone ? &retained : nullptr;
    if (strcmp(fault, "source-path") == 0)
        request.imagePath = standalone ? sourcePath : nullptr;
    if (strcmp(fault, "source-reader") == 0)
        request.source = standalone ? &source : nullptr;
    if (strcmp(fault, "source-analysis") == 0)
        request.analysis = standalone ? &analysis : nullptr;
    if (strcmp(fault, "unaligned-offset") == 0)
        request.format.offset++;
    if (strcmp(fault, "large-offset") == 0)
        request.format.offset = uint64_t(UINT32_MAX) + 1;
    const QByteArray facts(reinterpret_cast<const char*>(&analysis), sizeof(analysis));
    checking = true;
    FORMAT_RESULT result;
    QString problem;
    int error = rufus::InstallNativeSyslinux(request, result, problem);
    checking = false;
    const bool unchanged = memcmp(facts.constData(), &analysis, sizeof(analysis)) == 0;
    const bool sameFlags = flags == fcntl(targetFd, F_GETFL) && fdFlags == fcntl(targetFd, F_GETFD);
    const int position = int(lseek(targetFd, 0, SEEK_CUR));
    close(targetFd);
    if (!standalone)
    {
        CloseIsoAnalysisSource(&source);
        CloseImageFileSource(&retained);
    }
    int status;
    const int child = waitpid(-1, &status, WNOHANG), childError = errno;
    QJsonObject output;
    output.insert(QStringLiteral("error"), error);
    output.insert(QStringLiteral("problem"), problem);
    output.insert(QStringLiteral("diagnostics"), QString::fromLocal8Bit(result.diagnostics));
    output.insert(QStringLiteral("exit_code"), result.exit_code);
    output.insert(QStringLiteral("started"), result.started);
    output.insert(QStringLiteral("syncs"), syncs);
    output.insert(QStringLiteral("sync_error"), result.sync_error);
    output.insert(QStringLiteral("fds"), descriptorCount() - before);
    output.insert(QStringLiteral("position"), position);
    output.insert(QStringLiteral("flags"), sameFlags);
    output.insert(QStringLiteral("facts"), unchanged);
    output.insert(QStringLiteral("child"), child);
    output.insert(QStringLiteral("child_error"), childError);
    puts(QJsonDocument(output).toJson(QJsonDocument::Compact).constData());
    return 0;
}
