// SPDX-License-Identifier: GPL-3.0-or-later
// Guest-only formatter handoff/final-sync faults. Never linked into the production helper.
#include "platform/format_command.h"
#include "platform/exfat_format.h"
#include "platform/uefi_ntfs.h"
#include "platform/fat_boot.h"
#include "platform/ntfs_boot.h"
#include "platform/rufus_reporting.h"
#include <QFile>
#include <QtEndian>
#include <cerrno>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

namespace
{
bool formatted;
bool interrupted;
bool writingBootPayload;
QByteArray bootStage;
bool bootWritten;
bool bootSynced;
QByteArray readFile(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

void updateExfatBootCopies(int fd)
{
    constexpr int sectorBytes = 512;
    constexpr int bootRegionBytes = 12 * sectorBytes;
    constexpr int checksumOffset = 11 * sectorBytes;
    constexpr int volumeFlagsOffset = 106;
    constexpr int percentInUseOffset = 112;
    unsigned char region[bootRegionBytes];
    if (pread(fd, region, sizeof(region), 0) != sizeof(region))
        qFatal("Cannot read exFAT fault fixture boot region");
    // Keep both boot regions and their checksums valid. Only the explicit SFD
    // partition-entry check should reject the injected entry at byte 446.
    quint32 checksum = 0;
    for (int i = 0; i < checksumOffset; ++i)
    {
        const bool mutableField = i == volumeFlagsOffset || i == volumeFlagsOffset + 1 || i == percentInUseOffset;
        if (!mutableField)
            checksum = ((checksum << 31) | (checksum >> 1)) + region[i];
    }
    for (int i = checksumOffset; i < bootRegionBytes; i += sizeof(checksum))
        qToLittleEndian(checksum, region + i);
    if (pwrite(fd, region, sizeof(region), 0) != sizeof(region) ||
        pwrite(fd, region, sizeof(region), bootRegionBytes) != sizeof(region))
        qFatal("Cannot update exFAT fault fixture boot copies");
    quint64 capacity = 0;
    if (ioctl(fd, BLKGETSIZE64, &capacity) != 0)
        qFatal("Cannot inspect exFAT fault fixture capacity");
    constexpr int clusterShiftOffset = 109;
    const rufus::ExfatFormatOptions options = {capacity, quint32(sectorBytes << region[clusterShiftOffset]),
        QStringLiteral("Rufus ș😀"), 0};
    if (rufus::CheckExfatVolume(fd, options) != 0)
        qFatal("exFAT entry fault unexpectedly broke volume metadata");
}
}

extern "C" int __real_RunFormatCommand(int, char* const*, FORMAT_RESULT*);
extern "C" int __real_fsync(int);
extern "C" int __real_unshare(int);
extern "C" int __real_mount(const char*, const char*, const char*, unsigned long, const void*);
extern "C" int __real_WriteUefiNtfsImage(const IMAGE_FILE_SOURCE*, const char*, int,
    const GPT_IMAGE_OPTIONS*, RAW_WRITE_RESULT*);
extern "C" int __real_WriteMbrUefiNtfsImage(const IMAGE_FILE_SOURCE*, const char*, int,
    const MBR_IMAGE_OPTIONS*, RAW_WRITE_RESULT*);

extern "C" int __wrap_WriteUefiNtfsImage(const IMAGE_FILE_SOURCE* source, const char* path,
    int target, const GPT_IMAGE_OPTIONS* options, RAW_WRITE_RESULT* result)
{
    // These faults target finalization, after the real boot copy/flush/readback.
    writingBootPayload = true;
    const int error = __real_WriteUefiNtfsImage(source, path, target, options, result);
    writingBootPayload = false;
    return error;
}

extern "C" int __wrap_WriteMbrUefiNtfsImage(const IMAGE_FILE_SOURCE* source, const char* path,
    int target, const MBR_IMAGE_OPTIONS* options, RAW_WRITE_RESULT* result)
{
    writingBootPayload = true;
    const int error = __real_WriteMbrUefiNtfsImage(source, path, target, options, result);
    writingBootPayload = false;
    return error;
}

// Exercise real boot-record transfers under the retained whole-device claim.
void beginBootStage(const char* stage)
{
    if (!QFile::exists(QStringLiteral("/etc/rufus-grub2-fixture")))
        qFatal("GRUB2 fault probe requires its fixture");
    const int competing = open("/dev/sda", O_RDWR | O_EXCL | O_CLOEXEC);
    if (competing >= 0 || errno != EBUSY)
        qFatal("Boot records lost the exclusive disk claim");
    writingBootPayload = true;
    bootStage = stage;
    bootWritten = bootSynced = false;
}

extern "C" int __real_WriteGrub2CoreImage(int, const MBR_IMAGE_OPTIONS*, const unsigned char*, uint64_t, MBR_CORE_RESULT*);
extern "C" int __wrap_WriteGrub2CoreImage(int fd, const MBR_IMAGE_OPTIONS* options,
    const unsigned char* core, uint64_t size, MBR_CORE_RESULT* result)
{
    beginBootStage("core");
    const int error = __real_WriteGrub2CoreImage(fd, options, core, size, result);
    writingBootPayload = false;
    return error;
}

extern "C" int __real_InstallFatDeviceBootRecords(int, uint64_t, uint64_t, int,
    FAT_BOOT_CODE, const FAT_FORMAT_OPTIONS*, FAT_BOOT_RESULT*);
extern "C" int __wrap_InstallFatDeviceBootRecords(int fd, uint64_t device, uint64_t capacity,
    int filesystem, FAT_BOOT_CODE code, const FAT_FORMAT_OPTIONS* options, FAT_BOOT_RESULT* result)
{
    // The same fixture binary is also used for non-GRUB jobs.
    const bool grub = QFile::exists(QStringLiteral("/etc/rufus-grub2-fixture"));
    if (grub) beginBootStage("pbr");
    const int error = __real_InstallFatDeviceBootRecords(fd, device, capacity, filesystem, code, options, result);
    if (grub) writingBootPayload = false;
    return error;
}

extern "C" int __real__ZN5rufus21InstallNtfsBootRecordEiyRKNS_17NtfsFormatOptionsERNS_14NtfsBootResultE(
    int, quint64, const rufus::NtfsFormatOptions&, rufus::NtfsBootResult&);
extern "C" int __wrap__ZN5rufus21InstallNtfsBootRecordEiyRKNS_17NtfsFormatOptionsERNS_14NtfsBootResultE(
    int fd, quint64 device, const rufus::NtfsFormatOptions& options, rufus::NtfsBootResult& result)
{
    beginBootStage("pbr");
    const int error = __real__ZN5rufus21InstallNtfsBootRecordEiyRKNS_17NtfsFormatOptionsERNS_14NtfsBootResultE(
        fd, device, options, result);
    writingBootPayload = false;
    return error;
}

extern "C" ssize_t __real_pwrite64(int, const void*, size_t, off64_t);
extern "C" ssize_t __wrap_pwrite64(int fd, const void* buffer, size_t size, off64_t offset)
{
    const QByteArray fault = writingBootPayload ? readFile(QStringLiteral("/run/format-fault")) : QByteArray();
    const bool cancel = fault == bootStage + "-cancel";
    if (fault == bootStage + "-write-fail")
    {
        errno = EIO;
        return -1;
    }
    // A short write makes cancellation observable inside the transfer loop.
    const ssize_t written = __real_pwrite64(fd, buffer, cancel ? qMin(size, size_t(128)) : size, offset);
    if (written > 0 && writingBootPayload) bootWritten = true;
    if (written > 0 && cancel)
    {
        QFile marker(QStringLiteral("/run/format-boot-blocked"));
        if (!marker.open(QIODevice::WriteOnly)) qFatal("Cannot publish boot cancellation marker");
        marker.close();
        while (!IsRufusJobCancelled()) usleep(10000);
    }
    return written;
}

extern "C" ssize_t __real_pread64(int, void*, size_t, off64_t);
extern "C" ssize_t __wrap_pread64(int fd, void* buffer, size_t size, off64_t offset)
{
    const ssize_t count = __real_pread64(fd, buffer, size, offset);
    const bool readback = writingBootPayload && bootSynced && count > 0;
    if (readback && readFile(QStringLiteral("/run/format-fault")) == bootStage + "-corrupt")
        static_cast<unsigned char*>(buffer)[0] ^= 1;
    return count;
}

extern "C" int __wrap_unshare(int flags)
{
    const bool fixture = geteuid() == 0 &&
        readFile(QStringLiteral("/sys/class/dmi/id/product_name")) == "RufusMBRFixture\n";
    if (!fixture)
        qFatal("namespace fault probe requires the isolated fixture guest");
    if (readFile(QStringLiteral("/run/format-fault")) == "namespace-create")
    {
        errno = EPERM;
        return -1;
    }
    return __real_unshare(flags);
}

extern "C" int __wrap_mount(const char* source, const char* target, const char* type, unsigned long flags, const void* data)
{
    const bool privatePropagation = flags == (MS_REC | MS_PRIVATE);
    if (privatePropagation && readFile(QStringLiteral("/run/format-fault")) == "namespace-private")
    {
        errno = EIO;
        return -1;
    }
    return __real_mount(source, target, type, flags, data);
}

extern "C" int __wrap_RunFormatCommand(int fd, char* const* arguments, FORMAT_RESULT* result)
{
    const bool fixture = geteuid() == 0 &&
        readFile(QStringLiteral("/sys/class/dmi/id/product_name")) == "RufusMBRFixture\n" &&
        readFile(QStringLiteral("/etc/rufus-format-helper-fixture")) == "format-helper\n";
    if (!fixture)
        qFatal("format sync probe requires the isolated fixture guest");
    const int error = __real_RunFormatCommand(fd, arguments, result);
    formatted = error == 0;
    const QByteArray fault = readFile(QStringLiteral("/run/format-fault"));
    if (formatted && (fault == "mbr-changed" || fault == "sfd-table"))
    {
        constexpr off_t diskSignatureOffset = 440;
        constexpr off_t firstPartitionEntryOffset = 446;
        const off_t offset = fault == "sfd-table" ? firstPartitionEntryOffset : diskSignatureOffset;
        const int disk = open("/dev/sda", O_RDWR | O_CLOEXEC);
        unsigned char signature = 0;
        if (disk < 0 || pread(disk, &signature, 1, offset) != 1)
            qFatal("Cannot inspect the MBR signature");
        signature ^= 1;
        if (pwrite(disk, &signature, 1, offset) != 1)
            qFatal("Cannot inject partition-entry change");
        if (fault == "sfd-table" && QFile::exists(QStringLiteral("/etc/rufus-ntfs-helper-fixture")))
        {
            // Keep NTFS's boot copies equal so the SFD entry check must catch this.
            constexpr quint64 sectorBytes = 512;
            quint64 capacity = 0;
            if (ioctl(disk, BLKGETSIZE64, &capacity) != 0 || capacity < sectorBytes)
                qFatal("Cannot inspect NTFS backup position");
            if (pwrite(disk, &signature, 1, off_t(capacity - sectorBytes) + offset) != 1)
                qFatal("Cannot inject matching NTFS backup change");
        }
        if (fault == "sfd-table" && QFile::exists(QStringLiteral("/etc/rufus-exfat-helper-fixture")))
            updateExfatBootCopies(disk);
        if (close(disk) != 0)
            qFatal("Cannot close partition-entry fault descriptor");
    }
    if (formatted && fault == "busy-reacquire")
    {
        // Deliberately retain the competing claim until this disposable helper
        // exits. The caller must fail reacquisition and join that exit.
        const int claim = open("/dev/sda", O_RDWR | O_EXCL | O_CLOEXEC);
        if (claim < 0)
            qFatal("Cannot inject competing disk claim");
    }
    return error;
}

extern "C" int __wrap_fsync(int fd)
{
    if (writingBootPayload)
    {
        const int result = __real_fsync(fd);
        if (!bootStage.isEmpty())
        {
            bootSynced = result == 0;
            const QByteArray fault = readFile(QStringLiteral("/run/format-fault"));
            if (bootSynced && fault == bootStage + "-cancel")
            {
                if (!bootWritten) qFatal("Cancellation did not exercise written boot bytes");
                uprintf("RUFUS_GRUB2_CANCEL_SYNC %s", bootStage.constData());
            }
            if (bootSynced && fault == bootStage + "-sync-fail")
            {
                errno = EIO;
                return -1;
            }
        }
        return result;
    }
    if (!formatted)
        return __real_fsync(fd);
    struct stat info;
    if (fstat(fd, &info) != 0 || !S_ISBLK(info.st_mode))
        qFatal("Final format sync must use the whole disk");
    const QString partition = QStringLiteral("/sys/dev/block/%1:%2/partition")
        .arg(major(info.st_rdev)).arg(minor(info.st_rdev));
    if (QFile::exists(partition))
        qFatal("Final format sync still uses a partition");
    const QByteArray fault = readFile(QStringLiteral("/run/format-fault"));
    if (fault == "sync-eintr" && !interrupted)
    {
        interrupted = true;
        errno = EINTR;
        return -1;
    }
    if (fault == "cancel-sync")
    {
        QFile marker(QStringLiteral("/run/format-final-sync"));
        if (!marker.open(QIODevice::WriteOnly))
            qFatal("Cannot publish final-sync marker");
        marker.close();
        while (!IsRufusJobCancelled())
            usleep(10000);
    }
    const int result = __real_fsync(fd);
    if (result == 0 && fault == "sync-fail")
    {
        errno = EIO;
        return -1;
    }
    uprintf("RUFUS_FORMAT_FINAL_SYNC EINTR=%d CANCEL=%d", interrupted, IsRufusJobCancelled());
    return result;
}
