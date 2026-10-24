// SPDX-License-Identifier: GPL-3.0-or-later
#include "syslinux_install.h"
#include "syslinux_native.h"
#include "syslinux_source.h"
#include "syslinux_payload.h"
#include "format_policy.h"
#include "rufus_reporting.h"
#include <QFile>
#include <QList>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <unistd.h>
extern "C"
{
#include "syslinux/libfat/libfat.h"
#include "syslinux/libfat/fat.h"
}

namespace rufus
{
const char* NativeSyslinuxInstaller(int bootType, int fileSystem)
{
    if (fileSystem == FS_NTFS)
        return bootType == BT_IMAGE || bootType == BT_SYSLINUX_V6 ? RUFUS_NATIVE_EXTLINUX_PATH : nullptr;
    if (fileSystem != FS_FAT16 && fileSystem != FS_FAT32)
        return nullptr;
    if (bootType == BT_SYSLINUX_V4)
        return RUFUS_NATIVE_SYSLINUX4_PATH;
    if (bootType == BT_IMAGE || bootType == BT_SYSLINUX_V6)
        return RUFUS_NATIVE_SYSLINUX_PATH;
    return nullptr;
}

int CheckNativeSyslinuxTools(QString& problem, int bootType, int fileSystem)
{
    problem.clear();
    const char* installer = NativeSyslinuxInstaller(bootType, fileSystem);
    if (installer == nullptr)
    {
        problem = QStringLiteral("Unsupported native Syslinux selection.");
        return ENOTSUP;
    }
    const char* const programs[] = {installer,
        RUFUS_MTOOLS_DIRECTORY "/mcopy", RUFUS_MTOOLS_DIRECTORY "/mattrib"};
    const int count = fileSystem == FS_NTFS ? 1 : 3;
    for (int i = 0; i < count; ++i)
    {
        const char* program = programs[i];
        if (access(program, X_OK) != 0)
        {
            const int error = errno;
            problem = QStringLiteral("Required native Syslinux tool is unavailable: %1").arg(QString::fromUtf8(program));
            return error;
        }
    }
    return 0;
}

namespace
{
constexpr int sectorBytes = 512;
constexpr int fatDirectoryAttribute = 0x10;

struct FatReader
{
    int fd;
    quint64 offset;
    quint64 size;
    int error = 0;
};

struct BootState
{
    QByteArray boot;
    QByteArray backup;
    quint16 backupSector = 0;
};

int checkTarget(const NativeSyslinuxRequest& request, FORMAT_RESULT& geometry)
{
    if (request.deviceLayout == nullptr)
        return request.fileSystem == FS_FAT16 ? CheckFat16Image(request.target, &request.format, &geometry) :
            CheckFat32Image(request.target, &request.format, &geometry);
    const MBR_IMAGE_OPTIONS& options = *request.deviceLayout;
    if (options.boot_code != MBR_BOOT_SYSLINUX || options.file_system != request.fileSystem)
        return EINVAL;
    if (options.device == 0 || request.diskSequence == 0)
        return EINVAL;
    MBR_IMAGE_RESULT layout;
    int error = CheckMbrImage(request.target, &options, &layout);
    if (error != 0)
        return error;
    const bool sameExtent = layout.offset == request.format.offset && layout.size == request.format.size;
    const bool sameCluster = options.cluster_size == request.format.cluster_size;
    if (!sameExtent || !sameCluster)
        return EINVAL;
    uint64_t sequence = 0;
    if (ioctl(request.target, BLKGETDISKSEQ, &sequence) != 0)
        return errno;
    if (sequence != request.diskSequence)
        return ESTALE;
    return request.fileSystem == FS_FAT16
        ? CheckFat16Device(request.target, options.device, options.disk_size, &request.format, &geometry)
        : CheckFat32Device(request.target, options.device, options.disk_size, &request.format, &geometry);
}

int readSector(intptr_t opaque, void* buffer, size_t size, libfat_sector_t sector)
{
    auto* reader = reinterpret_cast<FatReader*>(opaque);
    if (IsRufusJobCancelled())
        reader->error = ECANCELED;
    const bool inVolume = sector < reader->size / sectorBytes;
    if (size != sectorBytes || !inVolume)
        reader->error = EBADMSG;
    if (reader->error != 0)
        return 0;
    ssize_t count;
    do
    {
        count = pread(reader->fd, buffer, size, off_t(reader->offset + sector * sectorBytes));
    } while (count < 0 && errno == EINTR);
    if (count != ssize_t(size))
        reader->error = count < 0 ? errno : EIO;
    return reader->error == 0 ? int(count) : 0;
}

int readBootState(FatReader& reader, int fileSystem, BootState& state)
{
    struct fat_bootsect boot;
    if (readSector(intptr_t(&reader), &boot, sizeof(boot), 0) == 0)
        return reader.error;
    state.boot = QByteArray(reinterpret_cast<const char*>(&boot), sizeof(boot));
    // FAT16 has a fixed root directory and no FAT32 backup-boot-sector field.
    // checkTarget has already validated the selected filesystem's BPB.
    if (fileSystem == FS_FAT16)
        return 0;
    state.backupSector = read16(&boot.u.fat32.bpb_bkbootsec);
    const bool reservedBackup = state.backupSector > 0 && state.backupSector < read16(&boot.bsResSectors);
    if (!reservedBackup)
        return ENOTSUP;
    state.backup.resize(sectorBytes);
    if (readSector(intptr_t(&reader), state.backup.data(), sectorBytes, state.backupSector) == 0)
        return reader.error;
    return 0;
}

// libfat owns directory lookup and FAT traversal. File size bounds allocation;
// the expected cluster count bounds traversal, including the final cluster tail.
int readRootFile(libfat_filesystem* fs, const char* name, int bytes, int clusterBytes,
    QByteArray& contents, QList<uint64_t>& sectors)
{
    struct libfat_direntry entry;
    const int32_t cluster = libfat_searchdir(fs, 0, name, &entry);
    if (cluster == -2)
        return ENOENT;
    if (cluster < 2)
        return EBADMSG;
    struct fat_dirent file;
    memcpy(&file, entry.entry, sizeof(file));
    const bool regularFile = (file.attribute & fatDirectoryAttribute) == 0;
    const bool expectedSize = read32(&file.size) == quint32(bytes);
    if (!regularFile || !expectedSize)
        return EBADMSG;
    contents.resize(bytes);
    sectors.clear();
    const int allocatedSectors = (bytes + clusterBytes - 1) / clusterBytes * (clusterBytes / sectorBytes);
    libfat_sector_t sector = libfat_clustertosector(fs, cluster);
    for (int index = 0; index < allocatedSectors; index++)
    {
        if (IsRufusJobCancelled())
            return ECANCELED;
        const bool invalidSector = sector == 0 || sector == UINT64_MAX;
        if (invalidSector || sectors.contains(sector))
            return EBADMSG;
        sectors.append(sector);
        const int position = index * sectorBytes;
        if (position < bytes)
        {
            const void* data = libfat_get_sector(fs, sector);
            if (data == nullptr)
                return EIO;
            memcpy(contents.data() + position, data, size_t(std::min(sectorBytes, bytes - position)));
        }
        sector = libfat_nextsector(fs, sector);
        libfat_flush(fs);
    }
    return sector == 0 ? 0 : EBADMSG;
}

int checkFreshRoot(libfat_filesystem* fs)
{
    const libfat_sector_t sector = libfat_clustertosector(fs, 0);
    const bool invalidSector = sector == 0 || sector == UINT64_MAX;
    if (invalidSector)
        return EBADMSG;
    const auto* entries = static_cast<const fat_dirent*>(libfat_get_sector(fs, sector));
    if (entries == nullptr)
        return EIO;
    // A newly formatted root starts with an end marker, or a single volume
    // label followed by it. Reject existing/deleted files and LFN records;
    // this entry point does not install over a previously populated volume.
    constexpr unsigned char volumeLabelAttribute = 0x08;
    const bool hasLabel = entries[0].attribute == volumeLabelAttribute;
    const bool emptyRoot = entries[0].name[0] == 0 || (hasLabel && entries[1].name[0] == 0);
    return emptyRoot ? 0 : EEXIST;
}

int checkRootFiles(FatReader& reader, const NativeSyslinuxRequest& request,
    bool installed, QByteArray& loader, QList<uint64_t>& loaderSectors, QString& problem)
{
    libfat_filesystem* fs = libfat_open(readSector, intptr_t(&reader));
    if (fs == nullptr)
        return reader.error != 0 ? reader.error : EBADMSG;
    if (!installed && request.bootType != BT_IMAGE)
    {
        const int error = checkFreshRoot(fs);
        libfat_close(fs);
        if (error != 0)
            problem = QStringLiteral("Standalone Syslinux requires a freshly formatted empty root directory.");
        return reader.error != 0 ? reader.error : error;
    }
    int error;
    if (installed)
    {
        const SyslinuxBootPayload selected = {request.resources ? int(request.resources->loader.size()) : 0,nullptr};
        const SyslinuxBootPayload& payload = request.resources ? selected : request.bootType == BT_SYSLINUX_V4 ? syslinux4Payload : syslinux6Payload;
        error = readRootFile(fs, "LDLINUX SYS", payload.installedBytes, int(request.format.cluster_size),
            loader, loaderSectors);
    }
    else
    {
        struct libfat_direntry entry;
        const int32_t cluster = libfat_searchdir(fs, 0, "LDLINUX SYS", &entry);
        error = cluster == -2 ? 0 : (cluster >= 0 ? EEXIST : EBADMSG);
    }
    if (error != 0)
        problem = installed ? QStringLiteral("Installed ldlinux.sys is missing or has an invalid FAT chain.") :
            QStringLiteral("The freshly extracted target must not contain ldlinux.sys.");
    if (error == 0 && request.bootType != BT_SYSLINUX_V4 && (!request.resources || !request.resources->module.isEmpty()))
    {
        QByteArray module;
        QList<uint64_t> moduleSectors;
        error = readRootFile(fs, "LDLINUX C32", request.resources ? int(request.resources->module.size()) : int(nativeSyslinuxModuleBytes), int(request.format.cluster_size),
            module, moduleSectors);
        if (error == 0) error = request.resources ? (module == request.resources->module ? 0 : EBADMSG) : CheckNativeSyslinuxModule(module,problem);
        else if (!installed && error == ENOENT)
            error = 0;
        else
            problem = QStringLiteral("Root ldlinux.c32 is missing or has an invalid FAT chain.");
        if (installed && error == 0)
        {
            for (uint64_t sector : moduleSectors)
            {
                if (loaderSectors.contains(sector))
                {
                    problem = QStringLiteral("The installed Syslinux files share FAT clusters.");
                    error = EBADMSG;
                    break;
                }
            }
        }
    }
    libfat_close(fs);
    return reader.error != 0 ? reader.error : error;
}

int verifyInstalled(const NativeSyslinuxRequest& request, const BootState& before, QString& problem)
{
    FORMAT_RESULT geometry;
    int error = checkTarget(request, geometry);
    if (error != 0)
        return error;
    FatReader reader = {request.target, request.format.offset, geometry.filesystem_bytes};
    BootState after;
    error = readBootState(reader, request.fileSystem, after);
    if (error != 0)
        return error;
    const bool sameBackup = before.backupSector == after.backupSector && before.backup == after.backup;
    if (!sameBackup)
    {
        problem = QStringLiteral("The installer changed the backup FAT boot sector.");
        return EBADMSG;
    }
    QByteArray installed;
    QList<uint64_t> sectors;
    error = checkRootFiles(reader, request, true, installed, sectors, problem);
    if (error != 0)
        return error;
    return CheckSyslinuxPayload(request.bootType, request.fileSystem, before.boot, after.boot,
        installed, sectors, problem, request.resources);
}
}

static int checkRequest(const NativeSyslinuxRequest& request, QString& problem)
{
    if (request.fileSystem != FS_FAT16 && request.fileSystem != FS_FAT32)
    {
        problem = QStringLiteral("This Syslinux adapter requires FAT16 or FAT32.");
        return ENOTSUP;
    }
    const bool imageInstall = request.bootType == BT_IMAGE;
    const bool standalone = request.bootType == BT_SYSLINUX_V4 || request.bootType == BT_SYSLINUX_V6;
    if (!imageInstall && !standalone)
    {
        problem = QStringLiteral("Only standalone Syslinux 4.07 and 6.04-pre1 are qualified with this adapter.");
        return ENOTSUP;
    }
    const bool hasSource = request.image != nullptr && request.imagePath != nullptr &&
        request.source != nullptr && request.analysis != nullptr;
    const bool noSource = request.image == nullptr && request.imagePath == nullptr &&
        request.source == nullptr && request.analysis == nullptr;
    const bool validSource = imageInstall ? hasSource : noSource;
    if (!validSource)
        return EINVAL;
    const bool absolutePrograms = request.installer.startsWith('/') && request.toolsDirectory.startsWith('/');
    const bool validDirectories = request.temporaryRoot.startsWith('/') && !request.toolsDirectory.contains(':');
    const bool embeddedNull = request.installer.contains(QChar::Null) ||
        request.toolsDirectory.contains(QChar::Null) || request.temporaryRoot.contains(QChar::Null);
    if (!absolutePrograms || !validDirectories || embeddedNull)
        return EINVAL;
    if (request.resources && (!imageInstall || ValidateSyslinuxResources(request.analysis->report,*request.resources) != 0)) return ENOTSUP;
    if (!imageInstall)
        return CheckNativeSyslinuxGeometry(request.fileSystem, sectorBytes, request.format.offset, problem);
    int error = CheckImageFileSource(request.image, request.imagePath);
    if (error == 0)
        error = CheckNativeSyslinuxSource(*request.source, *request.analysis, request.rockRidge,
            request.fileSystem, sectorBytes, request.format.offset, problem);
    return error;
}

static int install(const NativeSyslinuxRequest& request, FORMAT_RESULT& result, QString& problem)
{
    result = {};
    result.exit_code = -1;
    problem.clear();
    int error = checkRequest(request, problem);
    FORMAT_RESULT geometry;
    if (error == 0)
        error = checkTarget(request, geometry);
    if (error != 0)
        return error;
    struct stat before, after;
    if (fstat(request.target, &before) != 0)
        return errno;
    const bool imageInstall = request.bootType == BT_IMAGE;
    const bool targetIsSource = imageInstall && before.st_dev == request.image->info.st_dev &&
        before.st_ino == request.image->info.st_ino;
    if (targetIsSource)
        return EINVAL;
    FatReader reader = {request.target, request.format.offset, geometry.filesystem_bytes};
    BootState boot;
    error = readBootState(reader, request.fileSystem, boot);
    QByteArray unusedLoader;
    QList<uint64_t> unusedSectors;
    if (error == 0)
        error = checkRootFiles(reader, request, false, unusedLoader, unusedSectors, problem);
    if (error == 0 && imageInstall)
        error = CheckImageFileSource(request.image, request.imagePath);
    if (error != 0)
        return error;
    QTemporaryDir directory(request.temporaryRoot + QStringLiteral("/rufus-syslinux-XXXXXX"));
    if (!directory.isValid())
        return EIO;
    directory.setAutoRemove(false);
    QByteArray program = QFile::encodeName(request.installer);
    QByteArray offset = QByteArray::number(request.format.offset);
    QByteArray target = QByteArray("/proc/self/fd/") + QByteArray::number(FORMAT_TARGET_FD);
    QByteArray working = QFile::encodeName(directory.path());
    QByteArray path = QByteArray("PATH=") + QFile::encodeName(request.toolsDirectory);
    QByteArray temporary = QByteArray("TMPDIR=") + working;
    QByteArray home = QByteArray("HOME=") + working;
    char locale[] = "LC_ALL=C.UTF-8";
    char install[] = "--install", offsetOption[] = "--offset";
    QTemporaryFile resourceFile(directory.filePath(QStringLiteral("resources-XXXXXX")));
    QByteArray resourceEnvironment;
    const bool external = request.resources && SL_MAJOR(request.analysis->report.sl_version) >= 5;
    if (external) {
        const QByteArray bytes = EncodeSyslinuxResources(*request.resources);
        if (bytes.isEmpty() || !resourceFile.open() || resourceFile.write(bytes) != bytes.size() || !resourceFile.flush()) {
            if (resourceFile.isOpen()) resourceFile.remove();
            directory.remove(); return EIO;
        }
        resourceEnvironment = QByteArray("RUFUS_SYSLINUX_RESOURCES=")+QFile::encodeName(resourceFile.fileName());
    }
    char* environment[] = {path.data(), temporary.data(), home.data(), locale, external ? resourceEnvironment.data() : nullptr, nullptr};
    char* arguments[] = {program.data(), install, offsetOption, offset.data(), target.data(), nullptr};
    error = RunInstallerCommand(request.target, arguments, environment, working.constData(), &result);
    if (error != 0)
        problem = QStringLiteral("Native Syslinux installation failed (exit %1, signal %2); the target is incomplete. %3").arg(result.exit_code).arg(result.signal).arg(QString::fromUtf8(result.diagnostics));
    if (error == 0 && fstat(request.target, &after) != 0)
        error = errno;
    if (error == 0)
    {
        const bool sameTarget = before.st_dev == after.st_dev && before.st_ino == after.st_ino &&
            before.st_size == after.st_size;
        error = sameTarget ? verifyInstalled(request, boot, problem) : ESTALE;
    }
    if (error == 0 && imageInstall)
        error = CheckImageFileSource(request.image, request.imagePath);
    // RunInstallerCommand has reaped the complete process group before returning.
    // A killed installer may leave its private MTOOLSRC; cleanup is part of success.
    if (resourceFile.isOpen() && !resourceFile.remove() && error == 0) error = EIO;
    if (!directory.remove())
    {
        if (!problem.isEmpty())
            problem += QLatin1Char(' ');
        problem += QStringLiteral("Cannot remove installer temporary directory: ") + directory.path();
        if (error == 0)
            error = EIO;
    }
    return error;
}

int InstallNativeSyslinux(const NativeSyslinuxRequest& request, FORMAT_RESULT& result, QString& problem)
{
    int error = install(request, result, problem);
    if (error == 0 && IsRufusJobCancelled())
        error = ECANCELED;
    if (error != 0 && problem.isEmpty())
        problem = QStringLiteral("Syslinux installation check failed: ") + QString::fromLocal8Bit(strerror(error));
    return error;
}
}
