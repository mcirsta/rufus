// SPDX-License-Identifier: GPL-3.0-or-later
// Test-only alternative target: real disk-file writers and extraction, no devices/mounts.
#include "platform/prepare_device.h"
#include "platform/prepare_source.h"
#include "platform/iso_extract.h"
#include "platform/target_io.h"
#include "platform/uefi_ntfs.h"
#include "platform/fat_boot.h"
#include "iso_analyze.h"
#include "platform/rufus_reporting.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QElapsedTimer>
#include <QThread>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <fcntl.h>
#include <linux/fs.h>
#include <linux/msdos_fs.h>
#include <sys/statfs.h>
#include <unistd.h>

namespace
{
int disk = -1, volume = -1;
QByteArray mode;
QString imagePath, corePath;
bool standaloneGrub4dos = false;
constexpr quint64 fixtureDevice = 123;
constexpr size_t sectorBytes = 512, partialBootBytes = 17;
constexpr unsigned fat32BackupSector = 6, biosDriveOffset = 64, singleLargeWim = 0x11;
constexpr int bootWaitTimeoutMs = 5000;
bool activeFatBoot = false, activeFinalMbr = false, bootPaused = false;
uint64_t fatOffset = 0;

void stage(const char* name) { fprintf(stderr, "GRUB_FIXTURE %s\n", name); fflush(stderr); }

void changeFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::Append) || file.write("changed") != 7)
        qFatal("Cannot mutate the retained-input test fixture");
}

void changeDiskSignature(int fd)
{
    constexpr off_t signatureOffset = 440;
    char byte;
    if (pread(fd, &byte, 1, signatureOffset) != 1) qFatal("Cannot read fixture MBR signature");
    byte ^= 1;
    if (pwrite(fd, &byte, 1, signatureOffset) != 1) qFatal("Cannot alter fixture MBR signature");
}
}

extern "C" uid_t __real_getuid(void);
extern "C" uid_t __real_geteuid(void);
extern "C" uid_t __wrap_getuid(void)
{
    return qEnvironmentVariableIsSet("RUFUS_TEST_GRUB_IMAGE") ? 0 : __real_getuid();
}
extern "C" uid_t __wrap_geteuid(void)
{
    return qEnvironmentVariableIsSet("RUFUS_TEST_GRUB_IMAGE") ? 0 : __real_geteuid();
}
extern "C" int __wrap_unshare(int) { stage("namespace"); return 0; }
extern "C" int __wrap_mount(const char*, const char*, const char*, unsigned long, const void*) { return 0; }
extern "C" int __real_fstatfs64(int, struct statfs64*);
extern "C" int __wrap_fstatfs64(int fd, struct statfs64* info)
{
    const int error = __real_fstatfs64(fd, info);
    if (error == 0 && standaloneGrub4dos) info->f_type = MSDOS_SUPER_MAGIC;
    return error;
}

extern "C" int __real_openat64(int, const char*, int, ...);
extern "C" int __wrap_openat64(int fd, const char* path, int flags, ...)
{
    mode_t permissions = 0;
    if (flags & O_CREAT)
    {
        va_list arguments;
        va_start(arguments, flags);
        permissions = mode_t(va_arg(arguments, int));
        va_end(arguments);
    }
    const bool loaderCopy = standaloneGrub4dos && (flags & O_CREAT) && strcmp(path, "grldr") == 0;
    if (loaderCopy && mode == "copy-error") { errno = EACCES; return -1; }
    return __real_openat64(fd, path, flags, permissions);
}

extern "C" int __real_ioctl(int, unsigned long, ...);
extern "C" int __wrap_ioctl(int fd, unsigned long request, ...)
{
    va_list arguments;
    va_start(arguments, request);
    void* value = va_arg(arguments, void*);
    va_end(arguments);
    if (fd == disk && (request == BLKSSZGET || request == BLKPBSZGET))
    {
        *static_cast<int*>(value) = 512;
        return 0;
    }
    return __real_ioctl(fd, request, value);
}

extern "C" int __real_InspectWritableTarget(int, uint64_t, WRITABLE_TARGET*);
extern "C" int __wrap_InspectWritableTarget(int fd, uint64_t device, WRITABLE_TARGET* target)
{
    if (device == 0)
        return __real_InspectWritableTarget(fd, device, target);
    if ((fd != disk && fd != volume) || device != fixtureDevice)
        return EPERM;
    const int error = __real_InspectWritableTarget(fd, 0, target);
    if (error == 0)
    {
        target->sector_size = sectorBytes;
        target->physical_sector_size = sectorBytes;
    }
    return error;
}

extern "C" int __real_WriteGrub2CoreImage(int, const MBR_IMAGE_OPTIONS*, const unsigned char*, uint64_t, MBR_CORE_RESULT*);
extern "C" int __wrap_WriteGrub2CoreImage(int fd, const MBR_IMAGE_OPTIONS* options,
    const unsigned char* core, uint64_t size, MBR_CORE_RESULT* result)
{
    stage("core");
    if (mode == "core-error") return EIO;
    return __real_WriteGrub2CoreImage(fd, options, core, size, result);
}

extern "C" int __real_WriteGrub4dosSbrImage(int, const MBR_IMAGE_OPTIONS*, const unsigned char*, uint64_t, MBR_CORE_RESULT*);
extern "C" int __wrap_WriteGrub4dosSbrImage(int fd, const MBR_IMAGE_OPTIONS* options,
    const unsigned char* resource, uint64_t size, MBR_CORE_RESULT* result)
{
    stage("core");
    if (mode == "core-error") return EIO;
    return __real_WriteGrub4dosSbrImage(fd, options, resource, size, result);
}

extern "C" int __real_ExtractIsoTree(const char*, const char*, const ISO_EXTRACT_OPTIONS*, ISO_EXTRACT_RESULT*);
extern "C" int __real_AnalyzeIsoImageWithSource(const char*, uint64_t, const ISO_ANALYSIS_OPTIONS*, ISO_ANALYSIS*, ISO_ANALYSIS_SOURCE*);
extern "C" int __wrap_AnalyzeIsoImageWithSource(const char* path, uint64_t size,
    const ISO_ANALYSIS_OPTIONS* options, ISO_ANALYSIS* analysis, ISO_ANALYSIS_SOURCE* source)
{
    const int status = __real_AnalyzeIsoImageWithSource(path, size, options, analysis, source);
    const bool analyzed = status == ISO_SCAN_OK || status == ISO_SCAN_PARTIAL;
    if (analyzed && (mode == "split-wim" || mode == "split-collision"))
        analysis->report.has_4GB_file = singleLargeWim; // Small real WIM, oversized-file policy fixture.
    return status;
}

extern "C" ssize_t __real_pwrite64(int, const void*, size_t, off64_t);
extern "C" int __real_fsync(int);
extern "C" ssize_t __wrap_pwrite64(int fd, const void* data, size_t size, off64_t offset)
{
    if (activeFinalMbr && fd == disk)
    {
        if (mode == "masquerade-write-error") { errno = EIO; return -1; }
        if (mode == "cancel-masquerade" && !bootPaused)
        {
            const ssize_t count = __real_pwrite64(fd, data, qMin(size, partialBootBytes), offset);
            if (count <= 0) return count;
            bootPaused = true;
            stage("masquerade-wait");
            QElapsedTimer timer; timer.start();
            while (!IsRufusJobCancelled() && timer.elapsed() < bootWaitTimeoutMs) QThread::msleep(5);
            return count;
        }
    }
    if (!activeFatBoot || fd != disk)
        return __real_pwrite64(fd, data, size, offset);
    const bool backupFailure = mode == "fat-boot-backup-error" && uint64_t(offset) == fatOffset + fat32BackupSector * sectorBytes;
    if (mode == "fat-boot-write-error" || backupFailure)
    {
        errno = EIO;
        return -1;
    }
    if (mode != "cancel-fat-boot" || bootPaused)
        return __real_pwrite64(fd, data, size, offset);
    const ssize_t count = __real_pwrite64(fd, data, qMin(size, partialBootBytes), offset);
    if (count <= 0) return count;
    bootPaused = true;
    stage("fat-boot-wait");
    QElapsedTimer timer; timer.start();
    while (!IsRufusJobCancelled() && timer.elapsed() < bootWaitTimeoutMs) QThread::msleep(5);
    return count;
}

extern "C" int __wrap_fsync(int fd)
{
    const int error = __real_fsync(fd);
    if (error == 0 && activeFinalMbr && fd == disk)
    {
        stage("masquerade-sync");
        if (mode == "masquerade-sync-error") { errno = EIO; return -1; }
        if (mode == "masquerade-corrupt") changeDiskSignature(fd);
    }
    if (error != 0 || !activeFatBoot || fd != disk) return error;
    stage("fat-boot-sync");
    if (mode == "fat-boot-sync-error")
    {
        errno = EIO;
        return -1;
    }
    if (mode == "fat-boot-corrupt")
    {
        char byte;
        const off64_t offset = off64_t(fatOffset + biosDriveOffset);
        if (pread(fd, &byte, 1, offset) != 1) qFatal("Cannot read FAT boot corruption byte");
        byte ^= 1;
        if (__real_pwrite64(fd, &byte, 1, offset) != 1 || __real_fsync(fd) != 0)
            qFatal("Cannot inject FAT boot corruption");
    }
    return 0;
}

extern "C" int __real_InstallFatDeviceBootRecords(int, uint64_t, uint64_t, int,
    FAT_BOOT_CODE, const FAT_FORMAT_OPTIONS*, FAT_BOOT_RESULT*);
extern "C" int __wrap_InstallFatDeviceBootRecords(int fd, uint64_t device, uint64_t capacity,
    int fileSystem, FAT_BOOT_CODE code, const FAT_FORMAT_OPTIONS* options, FAT_BOOT_RESULT* result)
{
    stage("fat-boot");
    *result = {};
    if (mode == "fat-boot-error") return EIO;
    if (fd != disk || device != fixtureDevice) qFatal("Unexpected FAT boot target");
    activeFatBoot = true;
    fatOffset = options->offset;
    const int error = __real_InstallFatDeviceBootRecords(fd, device, capacity, fileSystem, code, options, result);
    activeFatBoot = false;
    if (error == 0 && mode == "fat-boot-mbr-change") changeDiskSignature(fd);
    if (error == 0 && mode == "fat-boot-source-change") changeFile(imagePath);
    fprintf(stderr, "GRUB_FIXTURE FAT_BYTES=%llu FAT_SYNC=%d\n",
        static_cast<unsigned long long>(result->bytes_written), result->sync_error);
    return error;
}

extern "C" int __real_InstallLegacyFatDeviceBootRecords(int, uint64_t, uint64_t, FAT_BOOT_CODE, bool,
    const FAT_FORMAT_OPTIONS*, FAT_BOOT_RESULT*);
extern "C" int __wrap_InstallLegacyFatDeviceBootRecords(int fd, uint64_t device, uint64_t capacity,
    FAT_BOOT_CODE code, bool masquerade, const FAT_FORMAT_OPTIONS* options, FAT_BOOT_RESULT* result)
{
    stage("winpe-fat-boot");
    constexpr off_t hintsOffset = sectorBytes + 488;
    constexpr size_t hintsBytes = 8;
    unsigned char hints[hintsBytes];
    for (unsigned copy : {0u, fat32BackupSector})
    {
        const off_t offset = off_t(options->offset) + off_t(copy * sectorBytes) + hintsOffset;
        if (pread(fd, hints, sizeof(hints), offset) != sizeof(hints)) qFatal("Cannot read final FAT allocation hints");
        fprintf(stderr, "GRUB_FIXTURE WINPE_HINTS_%u=", copy);
        for (unsigned char byte : hints) fprintf(stderr, "%02x", byte);
        fputc('\n', stderr);
    }
    *result = {};
    if (mode == "fat-boot-error") return EIO;
    if (fd != disk || device != fixtureDevice) qFatal("Unexpected WinPE FAT target");
    activeFatBoot = true;
    fatOffset = options->offset;
    const int error = __real_InstallLegacyFatDeviceBootRecords(fd, device, capacity, code, masquerade, options, result);
    activeFatBoot = false;
    return error;
}

extern "C" int __real_CreateMbrImage(int, const MBR_IMAGE_OPTIONS*, MBR_IMAGE_RESULT*);
extern "C" int __wrap_CreateMbrImage(int fd, const MBR_IMAGE_OPTIONS* options, MBR_IMAGE_RESULT* result)
{
    if (!options->masquerade_disk && options->boot_code != MBR_BOOT_RUFUS_MASQUERADE)
        return __real_CreateMbrImage(fd, options, result);
    stage("masquerade");
    *result = {};
    if (mode == "masquerade-error") return EIO;
    activeFinalMbr = true;
    const int error = __real_CreateMbrImage(fd, options, result);
    activeFinalMbr = false;
    return error;
}

extern "C" int __real_WriteMbrUefiNtfsImage(const IMAGE_FILE_SOURCE*, const char*, int, const MBR_IMAGE_OPTIONS*, RAW_WRITE_RESULT*);
extern "C" int __wrap_WriteMbrUefiNtfsImage(const IMAGE_FILE_SOURCE* source, const char* path,
    int fd, const MBR_IMAGE_OPTIONS* layout, RAW_WRITE_RESULT* result)
{
    stage("payload");
    if (mode == "payload-error")
    {
        *result = {};
        return EIO;
    }
    const int error = __real_WriteMbrUefiNtfsImage(source, path, fd, layout, result);
    if (error == 0 && mode == "payload-sync-error")
    {
        result->sync_error = EIO;
        return EIO;
    }
    return error;
}

extern "C" int __wrap_ExtractIsoTree(const char* source, const char* destination,
    const ISO_EXTRACT_OPTIONS* options, ISO_EXTRACT_RESULT* result)
{
    stage("copy");
    if (mode == "cancel-copy")
    {
        stage("copy-wait");
        QElapsedTimer timer; timer.start();
        while (!IsRufusJobCancelled() && timer.elapsed() < bootWaitTimeoutMs) QThread::msleep(5);
        return IsRufusJobCancelled() ? ECANCELED : ETIMEDOUT;
    }
    if (mode == "copy-error") return EIO;
    const int error = __real_ExtractIsoTree(source, destination, options, result);
    if (error == 0 && mode == "postcopy-source-change") changeFile(imagePath);
    return error;
}

namespace rufus
{
PrepareDeviceTarget::PrepareDeviceTarget(const PrepareImageRequest& request, Table table)
    : request_(request), table_(table)
{
    mode = qgetenv("RUFUS_TEST_GRUB_IMAGE");
    imagePath = request.imagePath;
    corePath = request.grub2CorePath;
    standaloneGrub4dos = request.grub4dos;
}

PrepareDeviceTarget::~PrepareDeviceTarget()
{
    if (directory_ >= 0) close(directory_);
    if (partition_ >= 0) close(partition_);
    if (fd_ >= 0) close(fd_);
    disk = volume = -1;
}

int PrepareDeviceTarget::open(uid_t caller, const PrepareIsoSource* source)
{
    const QFileInfo path(request_.path);
    const bool fixture = path.fileName() == QStringLiteral("disk.bin")
        && path.dir().dirName().startsWith(QStringLiteral("test_prepare_grub2_image-"));
    const bool imageJob = (request_.grub2 || request_.windowsBios)
        && !request_.isSourceFree() && table_ == Table::Mbr;
    const bool standaloneJob = request_.grub4dos && request_.isSourceFree()
        && (request_.fileSystem == FS_FAT16 || request_.fileSystem == FS_FAT32 || request_.fileSystem == FS_NTFS)
        && table_ == Table::Mbr && source == nullptr;
    if (!fixture || (!standaloneJob && (!imageJob || source == nullptr)) || mode.isEmpty() || caller != __real_getuid())
        return EPERM;
    fd_ = ::open(QFile::encodeName(request_.path).constData(), O_RDWR | O_NOFOLLOW | O_CLOEXEC);
    if (fd_ < 0) return errno;
    struct stat info;
    if (fstat(fd_, &info) != 0) return errno;
    if (!S_ISREG(info.st_mode) || info.st_uid != caller || info.st_size != off_t(request_.size)) return EPERM;
    disk = fd_;
    stage("target-open");
    return 0;
}

int PrepareDeviceTarget::check()
{
    struct stat info;
    if (fstat(fd_, &info) != 0) return errno;
    return S_ISREG(info.st_mode) && info.st_size == off_t(request_.size) ? 0 : ESTALE;
}

int PrepareDeviceTarget::open(uid_t, const PrepareIsoSource*, const PrepareIsoSource*) { return ENOTSUP; }

int PrepareDeviceTarget::finish()
{
    if (directory_ >= 0) return EBUSY;
    if (partition_ >= 0) {
        const int partition = partition_; partition_ = -1;
        if (::close(partition) != 0) return errno;
    }
    const int descriptor = fd_; fd_ = -1;
    return descriptor >= 0 && ::close(descriptor) != 0 ? errno : 0;
}

int PrepareDeviceTarget::finishRaw()
{
    if (table_ != Table::Raw)
        return EINVAL;
    const int descriptor = fd_;
    fd_ = -1;
    if (descriptor >= 0 && ::close(descriptor) != 0)
        return errno;
    return 0;
}

int PrepareDeviceTarget::checkMountSupport()
{
    stage("mount-support");
    if (mode == "prewrite-source-change") changeFile(imagePath);
    return mode == "mount-support-error" ? ENOTSUP : 0;
}

int PrepareDeviceTarget::format(const FAT32_FORMAT_OPTIONS& options, const char* mkfs, FAT32_FORMAT_RESULT& result)
{
    stage("format");
    if (mode == "format-error") return EIO;
    const int error = request_.fileSystem == FS_FAT16 ? FormatFat16Image(fd_, mkfs, &options, &result)
        : FormatFat32Image(fd_, mkfs, &options, &result);
    if (error == 0 && mode == "core-substitution") changeFile(corePath);
    if (error == 0 && mode == "source-change") changeFile(imagePath);
    if (error == 0 && mode == "fat-boot-mbr-before") changeDiskSignature(fd_);
    return error;
}

int PrepareDeviceTarget::formatNtfs(const GPT_IMAGE_RESULT&, const NtfsFormatOptions& options,
    const QString& mkfs, NtfsFormatResult& result)
{
    stage("format");
    if (mode == "format-error") return EIO;
    const QByteArray path = QFile::encodeName(request_.path + QStringLiteral(".ntfs"));
    partition_ = ::open(path.constData(), O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (partition_ < 0) return errno;
    if (ftruncate(partition_, off_t(options.size)) != 0) return errno;
    volume = partition_;
    const int error = FormatNtfsImage(partition_, mkfs, options, result);
    if (error == 0 && mode == "core-substitution") changeFile(corePath);
    if (error == 0 && mode == "source-change") changeFile(imagePath);
    return error;
}

int PrepareDeviceTarget::installNtfsBoot(const NtfsFormatOptions& options, NtfsBootResult& result)
{
    stage("ntfs-boot");
    if (mode == "ntfs-boot-error") return EIO;
    return InstallNtfsBootRecord(partition_, fixtureDevice, options, result);
}

int PrepareDeviceTarget::mount(quint64, quint64)
{
    stage("mount");
    constexpr off_t activeOffset = 446;
    unsigned char active;
    if (pread(fd_, &active, 1, activeOffset) != 1 || active != 0x80) qFatal("Unexpected active flag during mount");
    fprintf(stderr, "GRUB_FIXTURE MOUNT_ACTIVE=%02x\n", active);
    if (mode == "mount-error") return EIO;
    mountPath_ = QFile::encodeName(request_.path + QStringLiteral(".tree"));
    if (!QDir().mkpath(QString::fromLocal8Bit(mountPath_))) return EIO;
    directory_ = ::open(mountPath_.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    return directory_ < 0 ? errno : 0;
}

int PrepareDeviceTarget::finishMount(int& syncError)
{
    stage("finish");
    syncError = 0;
    if (directory_ >= 0)
    {
        if (fsync(directory_) != 0) syncError = errno;
        close(directory_); directory_ = -1;
    }
    if (mode == "masquerade-mbr-before") changeDiskSignature(fd_);
    if (mode == "sync-error") syncError = EIO;
    return mode == "cleanup-error" ? EBUSY : 0;
}

// This fixture cannot run any other preparation route.
int PrepareDeviceTarget::formatPersistence(const GPT_IMAGE_RESULT&, bool, const QString&, FORMAT_RESULT&) { return ENOTSUP; }
int PrepareDeviceTarget::formatExfat(const ExfatFormatOptions&, const QString&, FORMAT_RESULT&) { return ENOTSUP; }
int PrepareDeviceTarget::formatExt(const GPT_IMAGE_RESULT&, const ExtFormatOptions&, const QString&, FORMAT_RESULT&) { return ENOTSUP; }
int PrepareDeviceTarget::formatUdf(const GPT_IMAGE_RESULT&, const UdfFormatOptions&, const QString&, FORMAT_RESULT&) { return ENOTSUP; }
int PrepareDeviceTarget::applyWindowsImage(WIMStruct*, int, const PrepareIsoSource&, const GPT_IMAGE_RESULT&, WindowsApplyResult&) { return ENOTSUP; }
int PrepareDeviceTarget::formatWindowsEsp(const GPT_IMAGE_RESULT&, const char*, FAT32_FORMAT_RESULT&) { return ENOTSUP; }
int PrepareDeviceTarget::mountWindowsEsp(const GPT_IMAGE_RESULT&) { return ENOTSUP; }
int PrepareDeviceTarget::checkFfuMountSupport() { return ENOTSUP; }
int PrepareDeviceTarget::prepareFfuPartitions() { return ENOTSUP; }
int PrepareDeviceTarget::openFfuPartition(quint32, quint64, quint64, quint64&) { return ENOTSUP; }
int PrepareDeviceTarget::mountFfuPartition(quint32, quint64, quint64) { return ENOTSUP; }
int PrepareDeviceTarget::checkFfuPartitionTable() { return ENOTSUP; }
}

extern "C" int __real__ZN5rufus23ReadEmbeddedGrub4dosSbrEyR10QByteArray(quint64, QByteArray&);
extern "C" int __wrap__ZN5rufus23ReadEmbeddedGrub4dosSbrEyR10QByteArray(quint64 offset, QByteArray& resource)
{
    // The target fixture is constructed before source/resource preflight.
    fprintf(stderr, "GRUB_FIXTURE resource\n");
    if (qgetenv("RUFUS_TEST_GRUB_IMAGE") == "resource-error") return EIO;
    return __real__ZN5rufus23ReadEmbeddedGrub4dosSbrEyR10QByteArray(offset, resource);
}
