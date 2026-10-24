// SPDX-License-Identifier: GPL-3.0-or-later
// Only linked into the disposable-guest helper probe, never production.
#include "platform/format_command.h"
#include "platform/iso_extract.h"
#include "platform/uefi_ntfs.h"
#include "platform/rufus_reporting.h"
#include <QFile>
#include <QTemporaryDir>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace
{
QByteArray fault;
int target = -1;
bool installing = false;
bool installed = false;
bool ntfs = false;
bool writingUefi = false;

void readFault()
{
    ntfs = QFile::exists(QStringLiteral("/etc/rufus-syslinux-ntfs-fixture"));
    QFile mode(QStringLiteral("/home/rufus/syslinux-fault"));
    if (!mode.open(QIODevice::ReadOnly))
        qFatal("Missing Syslinux fault mode");
    fault = mode.readAll();
}

void changeSourceTime()
{
    struct stat info;
    if (stat("/source.iso", &info) != 0) qFatal("Cannot stat source fixture");
    timespec times[] = {info.st_atim, info.st_mtim};
    times[1].tv_sec++;
    if (utimensat(AT_FDCWD, "/source.iso", times, 0) != 0) qFatal("Cannot change source fixture timestamp");
}
}

extern "C" int __real_ExtractIsoTree(const char*, const char*, const ISO_EXTRACT_OPTIONS*, ISO_EXTRACT_RESULT*);
extern "C" int __wrap_ExtractIsoTree(const char* image, const char* directory,
    const ISO_EXTRACT_OPTIONS* options, ISO_EXTRACT_RESULT* result)
{
    readFault();
    const int error = __real_ExtractIsoTree(image, directory, options, result);
    if (error != 0) return error;
    if (fault == "source-after-copy") changeSourceTime();
    if (fault == "extract-failure") return EIO;
    if (fault == "extract-pause")
    {
        QFile marker(QStringLiteral("/run/syslinux-paused"));
        if (!marker.open(QIODevice::WriteOnly) || marker.write("copy paused\n") != 12)
            qFatal("Cannot signal copy cancellation fixture");
        marker.close();
        while (!IsRufusJobCancelled()) usleep(1000);
        return ECANCELED;
    }
    return 0;
}

extern "C" int __real_WriteMbrUefiNtfsImage(const IMAGE_FILE_SOURCE*, const char*, int,
    const MBR_IMAGE_OPTIONS*, RAW_WRITE_RESULT*);
extern "C" int __wrap_WriteMbrUefiNtfsImage(const IMAGE_FILE_SOURCE* source, const char* path,
    int fd, const MBR_IMAGE_OPTIONS* options, RAW_WRITE_RESULT* result)
{
    readFault();
    writingUefi = true;
    const int error = __real_WriteMbrUefiNtfsImage(source, path, fd, options, result);
    writingUefi = false;
    if (error != 0) return error;
    if (fault == "uefi-failure") return EIO;
    if (fault == "uefi-pause")
    {
        QFile marker(QStringLiteral("/run/syslinux-paused"));
        if (!marker.open(QIODevice::WriteOnly) || marker.write("UEFI paused\n") != 12)
            qFatal("Cannot signal UEFI cancellation fixture");
        marker.close();
        while (!IsRufusJobCancelled()) usleep(1000);
        return ECANCELED;
    }
    return 0;
}

extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd)
{
    const int result = __real_fsync(fd);
    if (writingUefi && fault == "uefi-flush" && result == 0) { errno = EIO; return -1; }
    const bool installerFlush = installing && fault == "flush";
    const bool finalFlush = installed && !installing && fault == "final-flush";
    struct stat info, disk;
    const bool wholeDisk = ntfs && fstat(fd, &info) == 0 && stat("/dev/sda", &disk) == 0 && info.st_rdev == disk.st_rdev;
    const bool selected = installerFlush ? fd == target : ntfs ? wholeDisk : fd == target;
    if (result == 0 && selected && (installerFlush || finalFlush))
    {
        errno = EIO;
        return -1;
    }
    return result;
}

extern "C" int __real_mount(const char*, const char*, const char*, unsigned long, const void*);
extern "C" int __wrap_mount(const char* source, const char* path, const char* type, unsigned long flags, const void* data)
{
    if (path != nullptr && QByteArray(path).startsWith("/run/rufus-"))
    {
        readFault();
        if (fault == "mount") { errno = EPERM; return -1; }
    }
    return __real_mount(source, path, type, flags, data);
}

extern "C" int __real_syncfs(int);
extern "C" int __wrap_syncfs(int fd)
{
    const int result = __real_syncfs(fd);
    if (installed && fault == "mount-flush") { errno = EIO; return -1; }
    return result;
}

extern "C" int __real_umount(const char*);
extern "C" int __wrap_umount(const char* path)
{
    if (installed && fault == "unmount") { errno = EBUSY; return -1; }
    return __real_umount(path);
}

extern "C" int __real_rmdir(const char*);
extern "C" int __wrap_rmdir(const char* path)
{
    if (installed && ntfs && fault == "cleanup") { errno = EIO; return -1; }
    return __real_rmdir(path);
}

extern "C" int __real_fsetxattr(int, const char*, const void*, size_t, int);
extern "C" int __wrap_fsetxattr(int fd, const char* name, const void* value, size_t size, int flags)
{
    if (installed && fault == "attributes") { errno = EIO; return -1; }
    return __real_fsetxattr(fd, name, value, size, flags);
}

extern "C" ssize_t __real_pread64(int, void*, size_t, off_t);
extern "C" ssize_t __wrap_pread64(int fd, void* buffer, size_t size, off_t offset)
{
    if (installed && fd == target && fault == "read")
    {
        errno = EIO;
        return -1;
    }
    return __real_pread64(fd, buffer, size, offset);
}

extern "C" bool __real__ZN13QTemporaryDir6removeEv(QTemporaryDir*);
extern "C" bool __wrap__ZN13QTemporaryDir6removeEv(QTemporaryDir* directory)
{
    if (installed && fault == "cleanup" && directory->path().startsWith(QStringLiteral("/run/rufus-syslinux-")))
        return false;
    return __real__ZN13QTemporaryDir6removeEv(directory);
}

extern "C" int __real_RunInstallerCommand(int, char* const*, char* const*, const char*, FORMAT_RESULT*);
extern "C" int __wrap_RunInstallerCommand(int fd, char* const* arguments, char* const* environment,
    const char* directory, FORMAT_RESULT* result)
{
    QFile marker(QStringLiteral("/etc/rufus-syslinux-fixture"));
    QFile product(QStringLiteral("/sys/class/dmi/id/product_name"));
    if (geteuid() != 0 || !marker.open(QIODevice::ReadOnly))
        qFatal("Syslinux fault probe requires its guest fixture");
    const QByteArray version = marker.readAll();
    if (version != "4\n" && version != "6\n")
        qFatal("Unknown Syslinux guest version");
    if (!product.open(QIODevice::ReadOnly) || product.readAll() != "RufusMBRFixture\n")
        qFatal("Syslinux fault probe requires the disposable VM");
    struct stat selected, disk;
    readFault();
    if (fstat(fd, &selected) != 0 || stat(ntfs ? "/dev/sda1" : "/dev/sda", &disk) != 0)
        qFatal("Cannot inspect guest target");
    const bool expectedDisk = S_ISBLK(selected.st_mode) && selected.st_rdev == disk.st_rdev;
    if (!expectedDisk)
        qFatal("Fault target is not the guest USB");
    target = fd;
    installing = true;
    const char* program = fault == "no-op" ? "/usr/bin/true" : "/usr/bin/false";
    char* substitute[] = {const_cast<char*>(program), nullptr};
    const bool replaceCommand = fault == "no-op" || fault == "failure";
    const int error = __real_RunInstallerCommand(fd, replaceCommand ? substitute : arguments,
        environment, directory, result);
    uprintf("RUFUS_SYSLINUX_INSTALLER ERROR=%d EXIT=%d SYNC=%d: %s", error,
        result->exit_code, result->sync_error, result->diagnostics);
    installing = false;
    installed = true;
    if (error == 0 && fault == "source-after-install")
        changeSourceTime();
    if (error == 0 && fault == "corrupt")
    {
        // The real argument list is program, --install, --offset, bytes, fd.
        const off_t bootCode = (ntfs ? 0 : off_t(strtoull(arguments[3], nullptr, 10))) + 100;
        char byte;
        if (__real_pread64(fd, &byte, 1, bootCode) != 1)
            qFatal("Cannot read guest boot code");
        byte ^= 1;
        if (pwrite(fd, &byte, 1, bootCode) != 1 || __real_fsync(fd) != 0)
            qFatal("Cannot corrupt guest boot code");
    }
    return error;
}
