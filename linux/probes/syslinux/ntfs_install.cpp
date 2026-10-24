// SPDX-License-Identifier: GPL-3.0-or-later
// Guest-only adapter qualification. Never operates outside the disposable VM.
#include "platform/syslinux_ntfs.h"
#include <QCoreApplication>
#include <QFile>
#include <QtEndian>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <linux/fiemap.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <unistd.h>

static const char* fault = "none";
static bool active, completed;
static int partitionFd, rootFd, rawSyncs, mountSyncs, fallbackCalls;
static int attributeReads, attributeWrites;
static int sourceChecks;
static const char* sourcePath;
static quint64 volumeBytes;
static constexpr int sectorBytes = 512;
static constexpr size_t attributeBytes = sizeof(quint32);
static constexpr quint32 readOnlyAttribute = 0x01;
static constexpr quint32 protectedAttributes = readOnlyAttribute | 0x02 | 0x04;
static constexpr char readyFile[] = "/tmp/extlinux-ready";

extern "C" void uprintf(const char*, ...) {}
extern "C" int IsRufusJobCancelled(void)
{
    if (!active)
        return 0;
    const bool before = strcmp(fault, "cancel-before") == 0;
    const bool after = completed && strcmp(fault, "cancel-after") == 0;
    const bool running = strcmp(fault, "cancel-running") == 0 && access(readyFile, F_OK) == 0;
    const bool attributes = strcmp(fault, "attribute-cancel") == 0 && attributeWrites != 0;
    return before || after || running || attributes;
}
extern "C" int IsIsoScanCancelled(void) { return IsRufusJobCancelled(); }

static void changeSourceTime(const char* path)
{
    struct stat info;
    if (stat(path, &info) != 0) qFatal("Cannot stat source fixture");
    timespec times[] = {info.st_atim, info.st_mtim};
    times[1].tv_sec++;
    if (utimensat(AT_FDCWD, path, times, 0) != 0) qFatal("Cannot change source fixture time");
}

extern "C" int __real_CheckImageFileSource(const IMAGE_FILE_SOURCE*, const char*);
extern "C" int __wrap_CheckImageFileSource(const IMAGE_FILE_SOURCE* source, const char* path)
{
    if (active && ++sourceChecks == 2 && strcmp(fault, "source-between") == 0)
        changeSourceTime(path);
    return __real_CheckImageFileSource(source, path);
}

extern "C" ssize_t __real_fgetxattr(int, const char*, void*, size_t);
extern "C" ssize_t __wrap_fgetxattr(int fd, const char* name, void* value, size_t size)
{
    if (!active)
        return __real_fgetxattr(fd, name, value, size);
    attributeReads++;
    const bool readFailed = strcmp(fault, "attribute-get") == 0;
    const bool readbackFailed = strcmp(fault, "attribute-readback") == 0 && attributeReads == 2;
    const bool moduleFailed = strcmp(fault, "attribute-module") == 0 && attributeReads == 3;
    if (readFailed || readbackFailed || moduleFailed)
    {
        errno = EIO;
        return -1;
    }
    if (strcmp(fault, "attribute-unsupported") == 0 || strcmp(fault, "attribute-missing") == 0)
    {
        errno = strcmp(fault, "attribute-missing") == 0 ? ENODATA : EOPNOTSUPP;
        return -1;
    }
    ssize_t result = __real_fgetxattr(fd, name, value, size);
    if (strcmp(fault, "attribute-short") == 0 && result == attributeBytes)
        return attributeBytes - 1;
    if (strcmp(fault, "attribute-final") == 0 && mountSyncs != 0 && result == attributeBytes)
        *static_cast<quint32*>(value) ^= readOnlyAttribute;
    return result;
}

extern "C" int __real_fsetxattr(int, const char*, const void*, size_t, int);
extern "C" int __wrap_fsetxattr(int fd, const char* name, const void* value, size_t size, int flags)
{
    if (!active)
        return __real_fsetxattr(fd, name, value, size, flags);
    attributeWrites++;
    if (strcmp(fault, "attribute-set") == 0)
    {
        errno = EIO;
        return -1;
    }
    if (strcmp(fault, "attribute-ignored") == 0)
        return 0;
    if (strcmp(fault, "attribute-extra-bit") == 0 && size == attributeBytes)
    {
        const quint32 changed = *static_cast<const quint32*>(value) ^ 0x20; // Archive must be preserved.
        return __real_fsetxattr(fd, name, &changed, size, flags);
    }
    return __real_fsetxattr(fd, name, value, size, flags);
}

extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd)
{
    if (active)
    {
        rawSyncs++;
        if (strcmp(fault, "device-sync") == 0)
        {
            errno = EIO;
            return -1;
        }
    }
    return __real_fsync(fd);
}
extern "C" int __real_syncfs(int);
extern "C" int __wrap_syncfs(int fd)
{
    if (active)
    {
        mountSyncs++;
        if (strcmp(fault, "mount-sync") == 0 || (strcmp(fault, "sync-eintr") == 0 && mountSyncs == 1))
        {
            errno = strcmp(fault, "sync-eintr") == 0 ? EINTR : EIO;
            return -1;
        }
    }
    return __real_syncfs(fd);
}
extern "C" ssize_t __real_pread64(int, void*, size_t, off_t);
extern "C" ssize_t __wrap_pread64(int fd, void* output, size_t bytes, off_t offset)
{
    if (active && completed && fd == partitionFd && strcmp(fault, "read-error") == 0)
    {
        errno = EIO;
        return -1;
    }
    return __real_pread64(fd, output, bytes, offset);
}

extern "C" int __real_posix_fadvise64(int, off_t, off_t, int);
extern "C" int __wrap_posix_fadvise64(int fd, off_t offset, off_t length, int advice)
{
    if (active && strcmp(fault, "cache-drop") == 0)
        return EIO;
    return __real_posix_fadvise64(fd, offset, length, advice);
}

extern "C" int __real_ioctl(int, unsigned long, ...);
extern "C" int __wrap_ioctl(int fd, unsigned long command, ...)
{
    va_list args;
    va_start(args, command);
    void* argument = va_arg(args, void*);
    va_end(args);
    if (!active || !completed)
        return __real_ioctl(fd, command, argument);
    const bool fib = strncmp(fault, "fib-", 4) == 0;
    if (command == FS_IOC_FIEMAP && (fib || strcmp(fault, "fiemap-io") == 0))
    {
        errno = fib ? EOPNOTSUPP : EIO;
        return -1;
    }
    if (command == FIGETBSZ)
        fallbackCalls++;
    int result = __real_ioctl(fd, command, argument);
    if (command == FS_IOC_FIEMAP && (strcmp(fault, "sync-eintr") == 0 || strcmp(fault, "none") == 0))
    {
        const fiemap* map = static_cast<fiemap*>(argument);
        struct stat info;
        fstat(fd, &info);
        printf("MAPPING result=%d count=%u size=%ld links=%lu dev=%lu\n", result,
            map->fm_mapped_extents, long(info.st_size), long(info.st_nlink), long(info.st_dev));
        for (unsigned i = 0; result == 0 && i < map->fm_mapped_extents; i++)
        {
            const fiemap_extent& extent = map->fm_extents[i];
            printf("EXTENT logical=%llu physical=%llu length=%llu flags=%x\n",
                extent.fe_logical, extent.fe_physical, extent.fe_length, extent.fe_flags);
        }
    }
    if (command == BLKGETDISKSEQ && strcmp(fault, "device-changed") == 0 && result == 0)
        ++*static_cast<quint64*>(argument);
    if (command == FIGETBSZ && strcmp(fault, "fib-zero-block") == 0)
        *static_cast<unsigned*>(argument) = 0;
    if (command == FIBMAP && strcmp(fault, "fib-hole") == 0)
        *static_cast<unsigned*>(argument) = 0;
    if (command == FIBMAP && strcmp(fault, "fib-outside") == 0)
        *static_cast<unsigned*>(argument) = UINT32_MAX;
    if (command != FS_IOC_FIEMAP || strncmp(fault, "map-", 4) != 0)
        return result;
    fiemap* map = static_cast<fiemap*>(argument);
    if (result != 0)
    {
        // Construct a genuine first extent through FIBMAP on NTFS-3G, then
        // perturb the response. This stays in the observer, never production.
        unsigned blockBytes = 0, first = 0;
        if (__real_ioctl(fd, FIGETBSZ, &blockBytes) != 0 || __real_ioctl(fd, FIBMAP, &first) != 0)
            qFatal("Cannot construct FUSE map fixture");
        map->fm_mapped_extents = 1;
        map->fm_extents[0] = {};
        map->fm_extents[0].fe_physical = uint64_t(first) * blockBytes;
        map->fm_extents[0].fe_length = (map->fm_length + blockBytes - 1) / blockBytes * blockBytes;
        map->fm_extents[0].fe_flags = FIEMAP_EXTENT_LAST;
    }
    fiemap_extent& extent = map->fm_extents[0];
    if (strcmp(fault, "map-count") == 0) map->fm_mapped_extents = map->fm_extent_count + 1;
    if (strcmp(fault, "map-empty") == 0) map->fm_mapped_extents = 0;
    if (strcmp(fault, "map-hole") == 0) extent.fe_logical = sectorBytes;
    if (strcmp(fault, "map-overflow") == 0) extent.fe_logical = UINT64_MAX - sectorBytes + 1;
    if (strcmp(fault, "map-outside") == 0) extent.fe_physical = volumeBytes;
    if (strcmp(fault, "map-length") == 0) extent.fe_length = UINT64_MAX - sectorBytes + 1;
    if (strcmp(fault, "map-empty-extent") == 0) extent.fe_length = 0;
    if (strcmp(fault, "map-alignment") == 0) extent.fe_physical++;
    if (strcmp(fault, "map-encoded") == 0) extent.fe_flags |= FIEMAP_EXTENT_ENCODED;
    if (strcmp(fault, "map-incomplete") == 0) extent.fe_flags &= ~FIEMAP_EXTENT_LAST;
    if (strcmp(fault, "map-short") == 0) extent.fe_length = sectorBytes;
    if (strcmp(fault, "map-duplicate") == 0)
    {
        map->fm_mapped_extents = 2;
        map->fm_extents[1] = extent;
        map->fm_extents[1].fe_logical = sectorBytes;
        extent.fe_length = sectorBytes;
        extent.fe_flags &= ~FIEMAP_EXTENT_LAST;
    }
    return 0;
}

extern "C" int __real_fstatfs64(int, struct statfs64*);
extern "C" int __wrap_fstatfs64(int fd, struct statfs64* info)
{
    int result = __real_fstatfs64(fd, info);
    if (result == 0 && active && completed)
    {
        if (strcmp(fault, "readonly-mount") == 0) info->f_flags |= ST_RDONLY;
        if (strcmp(fault, "wrong-filesystem") == 0) info->f_type = 0;
    }
    return result;
}

static void flip(int fd, off_t offset)
{
    char byte;
    if (__real_pread64(fd, &byte, 1, offset) != 1)
        qFatal("Fault read failed");
    byte ^= 0x40;
    if (pwrite(fd, &byte, 1, offset) != 1)
        qFatal("Fault write failed");
}

static void damage()
{
    if (strcmp(fault, "boot-code") == 0) flip(partitionFd, 100);
    if (strcmp(fault, "boot-bpb") == 0) flip(partitionFd, 72);
    if (strcmp(fault, "backup") == 0) flip(partitionFd, off_t(volumeBytes - sectorBytes + 100));
    const bool loader = strncmp(fault, "loader-", 7) == 0;
    const bool module = strcmp(fault, "module-bytes") == 0;
    if (!loader && !module)
        return;
    const char* name = module ? "ldlinux.c32" : "ldlinux.sys";
    struct stat info;
    if (fstatat(rootFd, name, &info, AT_SYMLINK_NOFOLLOW) != 0)
        qFatal("Cannot stat fault file");
    if (!(info.st_mode & S_IWUSR) && fchmodat(rootFd, name, 0700, 0) != 0)
        qFatal("Cannot clear fixture permissions");
    int fd = openat(rootFd, name, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        qFatal("Cannot open fault file");
    if (strcmp(fault, "loader-bytes") == 0 || module) flip(fd, 100);
    if (strcmp(fault, "loader-adv") == 0) flip(fd, 69631);
    if (strcmp(fault, "loader-size") == 0 && ftruncate(fd, 512) != 0)
        qFatal("Cannot truncate fault file");
    close(fd);
    if (strcmp(fault, "loader-symlink") == 0 || strcmp(fault, "loader-directory") == 0)
    {
        if (unlinkat(rootFd, name, 0) != 0)
            qFatal("Cannot replace fault file");
        int result = strcmp(fault, "loader-symlink") == 0 ? symlinkat("ldlinux.c32", rootFd, name) :
            mkdirat(rootFd, name, 0700);
        if (result != 0)
            qFatal("Cannot create fault file");
    }
    if (strcmp(fault, "loader-link") == 0 && linkat(rootFd, name, rootFd, "extra", 0) != 0)
        qFatal("Cannot create fault hardlink");
}

extern "C" int __real_RunInstallerCommand(int, char* const*, char* const*, const char*, FORMAT_RESULT*);
extern "C" int __wrap_RunInstallerCommand(int fd, char* const* arguments, char* const* environment,
    const char* directory, FORMAT_RESULT* result)
{
    const int error = __real_RunInstallerCommand(fd, arguments, environment, directory, result);
    if (error == 0)
        damage();
    if (error == 0 && strcmp(fault, "source-after") == 0)
        changeSourceTime(sourcePath);
    completed = true;
    return error;
}

static int descriptorCount()
{
    DIR* directory = opendir("/proc/self/fd");
    if (directory == nullptr) qFatal("Cannot enumerate descriptors");
    int count = 0;
    while (readdir(directory) != nullptr) count++;
    closedir(directory);
    return count;
}

static void reset(const QByteArray& boot)
{
    active = completed = false;
    const char* names[] = {"ldlinux.sys", "ldlinux.c32", "extra"};
    for (const char* name : names)
    {
        // Remove Windows read-only through its real attribute before fixture
        // cleanup. Do not assume synthetic POSIX modes describe FUSE flags.
        int fd = openat(rootFd, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        if (fd >= 0)
        {
            struct stat info;
            quint32 attributes = 0;
            if (fstat(fd, &info) == 0 && S_ISREG(info.st_mode) &&
                fgetxattr(fd, "system.ntfs_attrib", &attributes, sizeof(attributes)) == sizeof(attributes))
            {
                attributes &= ~protectedAttributes;
                if (fsetxattr(fd, "system.ntfs_attrib", &attributes, sizeof(attributes), 0) != 0)
                    qFatal("Cannot clear fixture attributes");
            }
            close(fd);
        }
        if (unlinkat(rootFd, name, 0) != 0 && errno != ENOENT)
            if (unlinkat(rootFd, name, AT_REMOVEDIR) != 0) qFatal("Cannot clean fixture file");
    }
    if (pwrite(partitionFd, boot.constData(), sectorBytes, 0) != sectorBytes ||
        pwrite(partitionFd, boot.constData(), sectorBytes, off_t(volumeBytes - sectorBytes)) != sectorBytes)
        qFatal("Cannot restore fixture boot sectors");
    if (syncfs(rootFd) != 0 || fsync(partitionFd) != 0) qFatal("Cannot sync reset");
    unlink(readyFile);
}

struct Case { const char* name; int error; bool started = true; };
static const Case imageCases[] = {
    {"missing-image", EINVAL, false}, {"missing-path", EINVAL, false},
    {"missing-view", EINVAL, false}, {"missing-analysis", EINVAL, false},
    {"standalone-source", EINVAL, false}, {"unsupported-selection", ENOTSUP, false},
    {"version4", ENOTSUP, false}, {"unknown-version", ENOTSUP, false},
    {"incomplete-source", ENOTSUP, false}, {"wrong-view", EINVAL, false},
    {"missing-config", ENOTSUP, false}, {"stale-source", ESTALE, false},
    {"source-between", ESTALE, false}, {"source-after", ESTALE},
    {"existing-loader", EEXIST, false}, {"existing-loader-directory", EEXIST, false},
    {"existing-loader-symlink", EEXIST, false},
    {"existing-module-directory", EBADMSG, false}, {"existing-module-symlink", ELOOP, false},
    {"existing-module-link", EBADMSG, false}, {"existing-module-size", EBADMSG, false},
    {"existing-module-bytes", ENOTSUP, false}, {"no-root-module", 0}, {"inferred-version", 0}
};
static const Case cases[] = {
    {"cancel-before", ECANCELED, false}, {"wrong-device", ESTALE, false},
    {"wrong-sequence", ESTALE, false}, {"wrong-size", ESTALE, false},
    {"wrong-serial", ESTALE, false}, {"wrong-cluster", ESTALE, false},
    {"wrong-mount", ESTALE, false}, {"nonempty", ENOTEMPTY, false},
    {"missing-program", ENOENT, false}, {"provider-failed", EIO},
    {"cancel-running", ECANCELED}, {"cancel-after", ECANCELED},
    {"device-sync", EIO}, {"mount-sync", EIO}, {"sync-eintr", 0}, {"cache-drop", EIO},
    {"attribute-get", EIO}, {"attribute-readback", EIO}, {"attribute-module", EIO},
    {"attribute-unsupported", EOPNOTSUPP}, {"attribute-missing", ENODATA}, {"attribute-short", EBADMSG},
    {"attribute-set", EIO}, {"attribute-ignored", EBADMSG}, {"attribute-extra-bit", EBADMSG},
    {"attribute-final", EBADMSG}, {"attribute-cancel", ECANCELED},
    {"read-error", EIO}, {"device-changed", ESTALE}, {"readonly-mount", EROFS},
    {"wrong-filesystem", ENOTSUP}, {"boot-code", EBADMSG}, {"boot-bpb", EBADMSG}, {"backup", EBADMSG},
    {"loader-bytes", EBADMSG}, {"loader-adv", EBADMSG}, {"module-bytes", ENOTSUP},
    {"loader-size", EBADMSG}, {"loader-symlink", ELOOP}, {"loader-directory", EBADMSG}, {"loader-link", EBADMSG},
    {"fiemap-io", EIO}, {"fib-zero-block", EBADMSG}, {"fib-hole", EBADMSG}, {"fib-outside", EBADMSG},
    {"map-count", EBADMSG}, {"map-empty", EBADMSG}, {"map-hole", EBADMSG}, {"map-overflow", EBADMSG},
    {"map-outside", EBADMSG}, {"map-length", EBADMSG}, {"map-empty-extent", EBADMSG},
    {"map-alignment", EBADMSG}, {"map-encoded", EBADMSG}, {"map-incomplete", EBADMSG},
    {"map-short", EBADMSG}, {"map-duplicate", EBADMSG}, {"fib-valid", 0}, {"none", 0},
};

static void prepareImageCase(rufus::NativeSyslinuxNtfsRequest& request,
    ISO_ANALYSIS& analysis, IMAGE_FILE_SOURCE& image)
{
    analysis = *request.analysis;
    image = *request.image;
    request.analysis = &analysis;
    request.image = &image;
    if (strcmp(fault, "missing-image") == 0) request.image = nullptr;
    if (strcmp(fault, "missing-path") == 0) request.imagePath = nullptr;
    if (strcmp(fault, "missing-view") == 0) request.source = nullptr;
    if (strcmp(fault, "missing-analysis") == 0) request.analysis = nullptr;
    if (strcmp(fault, "standalone-source") == 0) request.bootType = BT_SYSLINUX_V6;
    if (strcmp(fault, "unsupported-selection") == 0) request.bootType = BT_SYSLINUX_V4;
    if (strcmp(fault, "version4") == 0) analysis.report.sl_version = 0x0407;
    if (strcmp(fault, "unknown-version") == 0) analysis.report.sl_version = 0;
    if (strcmp(fault, "incomplete-source") == 0) analysis.tree_complete = false;
    if (strcmp(fault, "wrong-view") == 0) analysis.filesystem = ISO_FILESYSTEM_NONE;
    if (strcmp(fault, "missing-config") == 0) strcpy(analysis.report.cfg_path, "/absent.cfg");
    if (strcmp(fault, "stale-source") == 0) image.info.st_size++;
    if (strcmp(fault, "inferred-version") == 0)
    {
        analysis.report.sl_version_inferred = true;
        analysis.report.sl_version_ext[0] = 0;
    }
    const bool loader = strncmp(fault, "existing-loader", 15) == 0;
    const bool module = strncmp(fault, "existing-module", 15) == 0;
    if (!loader && !module && strcmp(fault, "no-root-module") != 0)
        return;
    if (module || strcmp(fault, "no-root-module") == 0)
        if (unlinkat(rootFd, "ldlinux.c32", 0) != 0) qFatal("Cannot remove fixture module");
    if (strcmp(fault, "no-root-module") == 0)
        return;
    const char* name = loader ? "ldlinux.sys" : "ldlinux.c32";
    if (strstr(fault, "-directory") != nullptr)
    {
        if (mkdirat(rootFd, name, 0700) != 0) qFatal("Cannot create reserved directory");
    }
    else if (strstr(fault, "-symlink") != nullptr)
    {
        if (symlinkat("absent", rootFd, name) != 0) qFatal("Cannot create reserved symlink");
    }
    else
    {
        QFile original(request.mountPath + QStringLiteral("/boot/x86_64/loader/ldlinux.c32"));
        if (!original.open(QIODevice::ReadOnly)) qFatal("Cannot read preserved module");
        QByteArray bytes = original.readAll();
        if (strcmp(fault, "existing-module-bytes") == 0) bytes[100] ^= 0x40;
        if (strcmp(fault, "existing-module-size") == 0) bytes.chop(1);
        int fd = openat(rootFd, name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd < 0 || write(fd, bytes.constData(), size_t(bytes.size())) != bytes.size() || close(fd) != 0)
            qFatal("Cannot create reserved file");
        if (strcmp(fault, "existing-module-link") == 0 && linkat(rootFd, name, rootFd, "extra", 0) != 0)
            qFatal("Cannot link reserved module");
    }
}

static bool runCase(const Case& test, const rufus::NativeSyslinuxNtfsRequest& original)
{
    rufus::NativeSyslinuxNtfsRequest request = original;
    fault = test.name;
    ISO_ANALYSIS analysis = {};
    IMAGE_FILE_SOURCE image = {};
    if (original.bootType == BT_IMAGE)
        prepareImageCase(request, analysis, image);
    const ISO_ANALYSIS beforeAnalysis = analysis;
    if (strcmp(fault, "wrong-device") == 0) request.device++;
    if (strcmp(fault, "wrong-sequence") == 0) request.diskSequence++;
    if (strcmp(fault, "wrong-size") == 0) request.format.size += sectorBytes;
    if (strcmp(fault, "wrong-serial") == 0) request.volumeId++;
    if (strcmp(fault, "wrong-cluster") == 0) request.format.clusterSize *= 2;
    if (strcmp(fault, "wrong-mount") == 0) request.mountPath = QStringLiteral("/tmp");
    if (strcmp(fault, "missing-program") == 0) request.installer = QStringLiteral("/no-extlinux");
    if (strcmp(fault, "provider-failed") == 0) request.installer = QStringLiteral("/tmp/fail-installer");
    if (strcmp(fault, "cancel-running") == 0) request.installer = QStringLiteral("/tmp/wait-installer");
    if (strcmp(fault, "nonempty") == 0)
    {
        int fd = openat(rootFd, "extra", O_CREAT | O_EXCL | O_WRONLY, 0600);
        if (fd < 0 || close(fd) != 0) qFatal("Cannot create nonempty root");
    }
    if (lseek(partitionFd, 17, SEEK_SET) != 17) qFatal("Cannot set borrowed position");
    const int before = descriptorCount(), flags = fcntl(partitionFd, F_GETFL), fdFlags = fcntl(partitionFd, F_GETFD);
    rawSyncs = mountSyncs = fallbackCalls = 0;
    attributeReads = attributeWrites = 0;
    sourceChecks = 0;
    active = true;
    FORMAT_RESULT result;
    QString problem;
    const int error = rufus::InstallNativeSyslinuxNtfs(request, result, problem);
    active = false;
    int status;
    const int child = waitpid(-1, &status, WNOHANG), childError = errno;
    const bool borrowed = flags == fcntl(partitionFd, F_GETFL) && fdFlags == fcntl(partitionFd, F_GETFD) &&
        lseek(partitionFd, 0, SEEK_CUR) == 17;
    const int mountExpected = strcmp(fault, "sync-eintr") == 0 ? 2 : int(test.started);
    const bool syncs = mountSyncs == mountExpected && rawSyncs == 2 * int(test.started);
    const bool children = child == -1 && childError == ECHILD;
    const bool noFallback = strncmp(fault, "map-", 4) != 0 && strcmp(fault, "fiemap-io") != 0;
    const bool mapping = noFallback || fallbackCalls == 0;
    const bool syncError = result.sync_error == ((strcmp(fault, "mount-sync") == 0 ||
        strcmp(fault, "device-sync") == 0) ? EIO : 0);
    const bool pass = error == test.error && result.started == test.started && borrowed && syncs && syncError &&
        children && mapping && descriptorCount() == before && memcmp(&analysis, &beforeAnalysis, sizeof(analysis)) == 0;
    printf("RUFUS_NTFS_ADAPTER %s %s error=%d expected=%d started=%d sync=%d/%d fallback=%d %s\n",
        fault, pass ? "PASS" : "FAIL", error, test.error, result.started, rawSyncs, mountSyncs,
        fallbackCalls, problem.toUtf8().constData());
    return pass;
}

static int checkAttributes(const char* path)
{
    const char* names[] = {"ldlinux.sys", "ldlinux.c32"};
    int directory = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory < 0) return 1;
    int error = 0;
    for (int i = 0; i < 2; i++)
    {
        int fd = openat(directory, names[i], O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) { error = 1; break; }
        quint32 native = 0, bigEndian = 0;
        const bool read = fgetxattr(fd, "system.ntfs_attrib", &native, sizeof(native)) == sizeof(native) &&
            fgetxattr(fd, "system.ntfs_attrib_be", &bigEndian, sizeof(bigEndian)) == sizeof(bigEndian);
        const quint32 expected = i == 0 ? protectedAttributes : 0;
        const bool correct = read && native == qFromBigEndian(bigEndian) && (native & protectedAttributes) == expected;
        printf("RUFUS_NTFS_ATTRIBUTES %s %s value=%08x\n", names[i], correct ? "PASS" : "FAIL", native);
        if (close(fd) != 0 || !correct) error = 1;
    }
    close(directory);
    return error;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QFile fixture(QStringLiteral("/sys/class/dmi/id/product_name"));
    const bool readback = argc == 3 && strcmp(argv[1], "--attributes") == 0;
    if ((!readback && argc != 4 && argc != 5) || !fixture.open(QIODevice::ReadOnly) || fixture.readAll().trimmed() != "RufusExtlinuxFixture" ||
        access("/etc/rufus-extlinux-fixture", F_OK) != 0)
        return 77;
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (readback)
        return checkAttributes(argv[2]);
    if (prctl(PR_SET_CHILD_SUBREAPER, 1) != 0) return 1;
    partitionFd = open(argv[2], O_RDWR | O_CLOEXEC);
    rootFd = open(argv[1], O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    struct stat info;
    quint64 sequence = 0;
    QByteArray boot(sectorBytes, '\0');
    if (partitionFd < 0 || rootFd < 0 || fstat(partitionFd, &info) != 0 ||
        ioctl(partitionFd, BLKGETSIZE64, &volumeBytes) != 0 || ioctl(partitionFd, BLKGETDISKSEQ, &sequence) != 0 ||
        pread(partitionFd, boot.data(), sectorBytes, 0) != sectorBytes)
        return 1;
    rufus::NativeSyslinuxNtfsRequest request;
    request.partition = partitionFd;
    request.directory = rootFd;
    request.device = info.st_rdev;
    request.diskSequence = sequence;
    request.format.size = volumeBytes;
    request.format.clusterSize = uchar(boot[13]) * sectorBytes;
    request.format.partitionOffset = quint64(qFromLittleEndian<quint32>(boot.constData() + 28)) * sectorBytes;
    request.volumeId = qFromLittleEndian<quint64>(boot.constData() + 72);
    request.mountPath = QString::fromLocal8Bit(argv[1]);
    request.installer = QString::fromLocal8Bit(argv[3]);
    IMAGE_FILE_SOURCE image = {};
    ISO_ANALYSIS_SOURCE source = {};
    ISO_ANALYSIS analysis = {};
    QByteArray module;
    if (argc == 5)
    {
        sourcePath = argv[4];
        if (OpenImageFileSource(sourcePath, &image) != 0) qFatal("Cannot open ISO fixture");
        char path[64];
        snprintf(path, sizeof(path), "/proc/self/fd/%d", image.fd);
        const ISO_ANALYSIS_OPTIONS options = {true, false, {0x0407, 0x0604}};
        if (AnalyzeIsoImageWithSource(path, uint64_t(image.info.st_size), &options, &analysis, &source) != ISO_SCAN_OK)
            qFatal("Cannot analyze ISO fixture");
        request.bootType = BT_IMAGE;
        request.image = &image;
        request.imagePath = sourcePath;
        request.source = &source;
        request.analysis = &analysis;
        QFile companion(request.mountPath + QStringLiteral("/ldlinux.c32"));
        if (!companion.open(QIODevice::ReadOnly)) qFatal("Cannot retain extracted companion");
        module = companion.readAll();
    }
    bool passed = true;
    const size_t imageCount = argc == 5 ? sizeof(imageCases) / sizeof(imageCases[0]) : 0;
    const size_t count = imageCount + sizeof(cases) / sizeof(cases[0]);
    for (size_t i = 0; i < count; i++)
    {
        Case test = i < imageCount ? imageCases[i] : cases[i - imageCount];
        reset(boot);
        if (argc == 5)
        {
            // Keep the extracted tree throughout, restoring only the two files
            // under test. Timestamp faults require a fresh retained stat.
            if (fstat(image.fd, &image.info) != 0) qFatal("Cannot refresh fixture identity");
            int fd = openat(rootFd, "ldlinux.c32", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
            if (fd < 0 || write(fd, module.constData(), size_t(module.size())) != module.size() || close(fd) != 0)
                qFatal("Cannot restore extracted companion");
            if (strcmp(test.name, "nonempty") == 0)
            {
                test.error = 0;
                test.started = true;
            }
        }
        if (!runCase(test, request))
        {
            passed = false;
            break;
        }
    }
    CloseIsoAnalysisSource(&source);
    if (argc == 5) CloseImageFileSource(&image);
    close(rootFd);
    close(partitionFd);
    return passed ? 0 : 1;
}
