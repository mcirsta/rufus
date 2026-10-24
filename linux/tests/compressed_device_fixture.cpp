// SPDX-License-Identifier: GPL-3.0-or-later
// Link-only kernel/selection fixtures around the real helper and native job.
// Every descriptor is an owned temporary regular file; no block node is opened.
#include "platform/compressed_write.h"
#include "platform/prepare_device.h"
#include "platform/prepare_source.h"
#include "platform/target_mounts.h"
#include "platform/rufus_reporting.h"

#include <QFile>
#include <QThread>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/file.h>
#include <sys/sysmacros.h>
#include <unistd.h>

extern "C" uid_t __real_getuid();
extern "C" uid_t __real_geteuid();
extern "C" int __real_open64(const char*, int, ...);
extern "C" int __real_close(int);
extern "C" int __real_fstat64(int, struct stat64*);
extern "C" ssize_t __real_pread64(int, void*, size_t, off64_t);
extern "C" ssize_t __real_pwrite64(int, const void*, size_t, off64_t);
extern "C" int __real_fsync(int);
extern "C" int __real_ioctl(int, unsigned long, ...);

namespace
{
constexpr quint64 diskSequence = 19;
constexpr size_t partialBytes = 17;
int diskFd = -1, sourceFd = -1, homeFd = -1;
bool claimed = false, synced = false, unmounted = false, insideBoundary = false;
unsigned writes = 0;
quint64 boundaryDevice = 0;
quint64 transferBytes = 0;

bool mode(const char* name) { return qgetenv("RUFUS_TEST_COMPRESSED_MODE") == name; }
void stage(const char* name) { fprintf(stderr, "COMPRESSED_FIXTURE %s\n", name); fflush(stderr); }

QByteArray root()
{
    const QByteArray path = qgetenv("RUFUS_TEST_COMPRESSED_ROOT");
    const QByteArray basename = path.mid(path.lastIndexOf('/') + 1);
    struct stat info;
    const bool owned = path.startsWith('/') && basename.startsWith("rufus-compressed-device-")
        && lstat(path.constData(), &info) == 0 && S_ISDIR(info.st_mode)
        && info.st_uid == __real_getuid() && (info.st_mode & 0777) == 0700;
    if (!owned || __real_getuid() == 0 || __real_getuid() != __real_geteuid())
        qFatal("Compressed device fixture needs an unprivileged owned private directory");
    return path;
}

quint64 fixtureDevice() { return quint64(makedev(240, 42)); }
unsigned sectorSize() { return qEnvironmentVariableIntValue("RUFUS_TEST_COMPRESSED_SECTOR"); }
bool protectedTarget()
{
    return mode("protected") || mode("home-close-fail") || mode("preflight-close-fail")
        || mode("mount-mismatch") || mode("unmount-error") || mode("native-protection-error");
}

void checkClaim()
{
    if (!claimed || diskFd < 0)
        qFatal("Compressed job released its target claim before I/O finished");
    const int contender = __real_open64((root() + "/disk.bin").constData(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (contender < 0)
        qFatal("Cannot inspect the fixture claim");
    const int acquired = flock(contender, LOCK_EX | LOCK_NB);
    const int error = errno;
    __real_close(contender);
    if (acquired == 0 || (error != EAGAIN && error != EWOULDBLOCK))
        qFatal("Fixture exclusive file lease was not retained");
}
}

extern "C" uid_t __wrap_getuid() { root(); return 0; }
extern "C" uid_t __wrap_geteuid() { root(); return 0; }

extern "C" int __wrap_open64(const char* path, int flags, ...)
{
    mode_t permissions = 0;
    if (flags & O_CREAT)
    {
        va_list arguments;
        va_start(arguments, flags);
        permissions = mode_t(va_arg(arguments, int));
        va_end(arguments);
    }
    const QByteArray name(path);
    const QByteArray directory = root();
    const bool disk = name == directory + "/disk.bin";
    const bool source = name == directory + "/source.zip" || name == directory + "/source.gz" || name == directory + "/source.xz"
        || name == directory + "/source.bz2" || name == directory + "/source.lzma" || name == directory + "/source.Z"
        || name == directory + "/source.zst" || name == directory + "/source.vtsi"
        || name == directory + "/source.vhd";
    const bool writable = (flags & O_ACCMODE) != O_RDONLY;
    if (name.startsWith("/dev/") || (writable && !name.startsWith(directory + '/')))
        qFatal("Fixture refused non-owned target access");
    const bool exclusive = disk && (flags & O_EXCL);
    if (exclusive && mode("claim-error")) { errno = EBUSY; return -1; }
    const int fd = __real_open64(path, flags, permissions);
    if (fd < 0)
        return fd;
    if (disk)
    {
        struct stat64 info;
        if (__real_fstat64(fd, &info) != 0 || !S_ISREG(info.st_mode))
            qFatal("The fixture target must be a regular file");
        diskFd = fd;
        if (exclusive)
        {
            const int required = O_EXCL | O_NOFOLLOW | O_CLOEXEC;
            if ((flags & required) != required || (flags & O_ACCMODE) != O_RDWR || flock(fd, LOCK_EX | LOCK_NB) != 0)
                qFatal("Target acquisition lost its claim flags");
            claimed = true;
            stage("claim");
        }
        else
            stage("readonly-target");
    }
    if (source)
    {
        if ((flags & O_ACCMODE) != O_RDONLY || !(flags & O_NOFOLLOW))
            qFatal("Source ownership preflight lost its read-only/no-follow flags");
        sourceFd = fd;
        stage("source-open");
    }
    return fd;
}

extern "C" int __wrap_fstat64(int fd, struct stat64* info)
{
    const int error = __real_fstat64(fd, info);
    if (error == 0 && fd == diskFd && !(insideBoundary && mode("boundary-regular")))
    {
        info->st_mode = (info->st_mode & ~S_IFMT) | S_IFBLK;
        info->st_rdev = dev_t(boundaryDevice != 0 ? boundaryDevice : fixtureDevice());
    }
    return error;
}

extern "C" int __wrap_ioctl(int fd, unsigned long request, ...)
{
    va_list arguments;
    va_start(arguments, request);
    void* value = va_arg(arguments, void*);
    va_end(arguments);
    if (fd != diskFd)
        return __real_ioctl(fd, request, value);
    struct stat64 info;
    if (__real_fstat64(fd, &info) != 0)
        return -1;
    if (mode("kernel-error")) { errno = EIO; return -1; }
    if (request == BLKGETSIZE64)
        *static_cast<uint64_t*>(value) = uint64_t(info.st_size) + (synced && mode("capacity-change") ? 512 : 0);
    else if (request == BLKGETDISKSEQ)
        *static_cast<uint64_t*>(value) = diskSequence;
    else if (request == BLKSSZGET || request == BLKPBSZGET)
        *static_cast<int*>(value) = int(sectorSize()) + (synced && mode("sector-change") ? int(sectorSize()) : 0);
    else if (request == BLKROGET)
        *static_cast<int*>(value) = mode("readonly") || (synced && mode("readonly-change"));
    else
        qFatal("Unexpected ioctl in compressed raw job");
    return 0;
}

extern "C" ssize_t __wrap_pread64(int fd, void* data, size_t size, off64_t offset)
{
    if (claimed && (fd == diskFd || fd == sourceFd))
        checkClaim();
    if (fd == diskFd)
    {
        if (!synced)
            qFatal("Readback preceded successful fsync");
        if (mode("verify-error")) { errno = EIO; return -1; }
    }
    return __real_pread64(fd, data, size, offset);
}

extern "C" ssize_t __wrap_pwrite64(int fd, const void* data, size_t size, off64_t offset)
{
    if (fd != diskFd)
        qFatal("Compressed helper wrote outside its target");
    checkClaim();
    writes++;
    const bool cancelling = mode("cancel") || mode("cancel-close") || mode("cancel-sync-fail");
    if (mode("write-error") && writes > 1) { errno = ENOSPC; return -1; }
    if (cancelling && writes > 1)
    {
        for (int i = 0; i < 5000 && !IsRufusJobCancelled(); i++) QThread::msleep(1);
        if (!IsRufusJobCancelled()) qFatal("Cancellation did not reach the native worker");
        errno = ECANCELED;
        return -1;
    }
    if ((cancelling || mode("write-error")) && size > partialBytes)
        size = partialBytes;
    const ssize_t count = __real_pwrite64(fd, data, size, offset);
    // Production progress is throttled. Force this test-only, actually written
    // boundary so the parent can deterministically cancel a dirty copy.
    if (cancelling && count > 0 && writes == 1)
        _UpdateProgressWithInfo(RAW_WRITE_COPY, 0, uint64_t(count), transferBytes, 1);
    return count;
}

extern "C" int __wrap_fsync(int fd)
{
    if (fd != diskFd)
        return __real_fsync(fd);
    checkClaim();
    stage("sync");
    if (mode("cancel-sync"))
    {
        for (int i = 0; i < 5000 && !IsRufusJobCancelled(); i++) QThread::msleep(1);
        if (!IsRufusJobCancelled()) qFatal("Sync cancellation did not reach the worker");
    }
    if (mode("sync-error") || mode("cancel-sync-fail")) { errno = EIO; return -1; }
    if (mode("corrupt") || mode("padding-corrupt"))
    {
        const off64_t offset = mode("corrupt") ? 23 : qEnvironmentVariable("RUFUS_TEST_COMPRESSED_DECODED").toLongLong();
        char byte;
        if (__real_pread64(fd, &byte, 1, offset) != 1) qFatal("Cannot inspect fixture byte");
        byte ^= 1;
        if (__real_pwrite64(fd, &byte, 1, offset) != 1) qFatal("Cannot corrupt fixture byte");
    }
    const int error = __real_fsync(fd);
    if (error == 0)
        synced = true;
    return error;
}

extern "C" int __wrap_close(int fd)
{
    const bool disk = fd == diskFd, source = fd == sourceFd, home = fd == homeFd;
    const bool exclusive = disk && claimed;
    const int error = __real_close(fd);
    if (disk) { stage(exclusive ? "close-target" : "close-preflight"); diskFd = -1; claimed = false; }
    if (source) { stage("close-source"); sourceFd = -1; }
    if (home) { stage("close-home"); homeFd = -1; }
    const bool fail = (exclusive && mode("target-close-fail")) || (disk && !exclusive && mode("preflight-close-fail"))
        || (source && (mode("source-close-fail") || mode("cancel-close"))) || (home && mode("home-close-fail"));
    if (fail || (exclusive && mode("target-close-eintr")))
    {
        errno = mode("target-close-eintr") ? EINTR : EIO;
        return -1; // Linux already released this descriptor: never retry it.
    }
    return error;
}

extern "C" int __wrap__ZN5rufus25ReadPrepareDeviceSnapshotERK15QDBusConnectionRKNS_19PrepareImageRequestER4QMapI15QDBusObjectPathS6_I7QStringS6_IS8_8QVariantEEERNS_6DeviceE(
    const QDBusConnection&, const rufus::PrepareImageRequest& request, rufus::UDisksObjects&, rufus::Device& current)
{
    stage("snapshot");
    if (mode("snapshot-error")) return ESTALE;
    current.path = request.path;
    current.number = request.device;
    current.size = request.size;
    current.requiresNativeProtection = protectedTarget();
    return 0;
}

extern "C" int __wrap__ZN5rufus27CheckPrepareDeviceSelectionERK15QDBusConnectionRKNS_19PrepareImageRequestEbPi(
    const QDBusConnection&, const rufus::PrepareImageRequest&, bool, int*)
{
    return synced && mode("final-snapshot-error") ? ESTALE : 0;
}

extern "C" QString __wrap__ZN5rufus22ListUDisksTargetMountsERK4QMapI15QDBusObjectPathS0_I7QStringS0_IS2_8QVariantEEERKNS_6DeviceEyR5QListIS2_Eb(
    const rufus::UDisksObjects&, const rufus::Device&, quint64, QStringList& mounts, bool)
{
    mounts.clear();
    if (protectedTarget() && !unmounted) mounts.append(QStringLiteral("fixture-mount"));
    return {};
}

extern "C" int __wrap__ZN5rufus20OpenNativeCallerHomeEjRi(uid_t caller, int& descriptor)
{
    if (caller != __real_getuid()) return EACCES;
    descriptor = __real_open64(root().constData(), O_PATH | O_DIRECTORY | O_CLOEXEC);
    homeFd = descriptor;
    stage("home");
    return descriptor < 0 ? errno : 0;
}

extern "C" int __wrap__ZN5rufus23CheckNativeTargetMountsEiiRNS_18NativeTargetMountsEi(
    int disk, int source, rufus::NativeTargetMounts& result, int home)
{
    if (disk != diskFd || source != sourceFd || home != homeFd || home < 0)
        qFatal("Native protection lost a retained descriptor");
    if (mode("native-protection-error")) return EBUSY;
    result.device = fixtureDevice();
    result.diskSequence = diskSequence;
    result.volumes.clear();
    if (!unmounted) result.volumes.append({root() + "/mounted", fixtureDevice() + 1, 1});
    return 0;
}

extern "C" QString __wrap__ZN5rufus23MatchUDisksTargetMountsERK4QMapI15QDBusObjectPathS0_I7QStringS0_IS2_8QVariantEEERKNS_6DeviceEyyRKNS_18NativeTargetMountsEb(
    const rufus::UDisksObjects&, const rufus::Device&, quint64, quint64, const rufus::NativeTargetMounts&, bool)
{
    return mode("mount-mismatch") ? QStringLiteral("fixture mount mismatch") : QString();
}

extern "C" int __wrap__ZN5rufus19UnmountNativeTargetEiiRKNS_18NativeTargetMountsERii(
    int disk, int source, const rufus::NativeTargetMounts&, int& count, int home)
{
    if (disk != diskFd || source != sourceFd || home != homeFd)
        qFatal("Unmount lost source/home ownership");
    stage("unmount");
    if (mode("unmount-error")) return EBUSY;
    count = 1;
    unmounted = true;
    return 0;
}

extern "C" int __real_WriteCompressedImageToDevice(const IMAGE_FILE_SOURCE*, const char*, const COMPRESSED_WRITE_PLAN*, int, uint64_t, uint64_t, RAW_WRITE_RESULT*);
extern "C" int __wrap_WriteCompressedImageToDevice(const IMAGE_FILE_SOURCE* source, const char* path,
    const COMPRESSED_WRITE_PLAN* plan, int fd, uint64_t device, uint64_t capacity, RAW_WRITE_RESULT* result)
{
    checkClaim();
    insideBoundary = true;
    transferBytes = plan->target_bytes;
    COMPRESSED_WRITE_PLAN changed = *plan;
    if (mode("boundary-zero-device")) device = 0;
    if (mode("boundary-zero-capacity")) capacity = 0;
    if (mode("boundary-wrong-device")) device++;
    if (mode("boundary-wrong-capacity")) capacity += 512;
    if (mode("boundary-source-device")) { device = uint64_t(source->info.st_dev); boundaryDevice = device; }
    if (mode("boundary-sector"))
    {
        changed.sector_size = plan->sector_size == 512 ? 4096 : 512;
        if (CompressedImageWriteSize(changed.image.decoded_bytes, changed.sector_size, &changed.target_bytes) != 0) qFatal("Fixture geometry");
    }
    if (mode("boundary-capacity"))
    {
        changed.image.decoded_bytes = capacity + 1;
        if (CompressedImageWriteSize(changed.image.decoded_bytes, changed.sector_size, &changed.target_bytes) != 0) qFatal("Fixture geometry");
    }
    if (mode("boundary-append") && fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_APPEND) != 0) qFatal("Fixture append");
    const int error = __real_WriteCompressedImageToDevice(source, path, &changed, fd, device, capacity, result);
    boundaryDevice = 0;
    insideBoundary = false;
    return error;
}
