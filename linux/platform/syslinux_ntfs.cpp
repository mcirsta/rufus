// SPDX-License-Identifier: GPL-3.0-or-later
#include "syslinux_ntfs.h"
#include "ntfs_attributes.h"
#include "syslinux_native.h"
#include "syslinux_source.h"
#include "syslinux_payload.h"
#include "target_io.h"
#include "rufus_reporting.h"
#include "format_policy.h"
#include "syslinux/libinstaller/syslxint.h"
#include <QFile>
#include <QTemporaryFile>
#include <QtEndian>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/fiemap.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/xattr.h>
#include <unistd.h>

namespace rufus
{
namespace
{
constexpr int sectorBytes = syslinuxSectorBytes;
constexpr long ntfsMagic = 0x5346544e;
constexpr long fuseMagic = 0x65735546;
constexpr int maximumFileSectors = (512 * 1024 + 1024) / sectorBytes;
static_assert(syslinux6Payload.installedBytes / sectorBytes <= maximumFileSectors);

int fileAttributes(int fd, bool loader, bool update)
{
    // Both native drivers expose this four-byte value in CPU byte order.
    constexpr char name[] = "system.ntfs_attrib";
    constexpr quint32 readOnly = 0x01, hidden = 0x02, system = 0x04, normal = 0x80;
    constexpr quint32 protection = readOnly | hidden | system;
    quint32 attributes = 0;
    ssize_t size = fgetxattr(fd, name, &attributes, sizeof(attributes));
    if (size < 0)
        return errno;
    if (size != sizeof(attributes))
        return EBADMSG;
    // Windows explicitly protects ldlinux.sys; the freshly downloaded c32 is
    // copied normally. Preserve unrelated flags (including the archive bit).
    quint32 expected = attributes & ~protection;
    if (loader)
        expected = (expected | protection) & ~normal;
    if (!update || attributes == expected)
        return attributes == expected ? 0 : EBADMSG;
    if (IsRufusJobCancelled())
        return ECANCELED;
    if (fsetxattr(fd, name, &expected, sizeof(expected), 0) != 0)
        return errno;
    size = fgetxattr(fd, name, &attributes, sizeof(attributes));
    if (size < 0)
        return errno;
    return size == sizeof(attributes) && attributes == expected ? 0 : EBADMSG;
}

int setBootAttributes(const NativeSyslinuxNtfsRequest& request)
{
    const char* names[] = {"ldlinux.sys", "ldlinux.c32"};
    const SyslinuxBootPayload selected = {request.resources ? int(request.resources->loader.size()) : syslinux6Payload.loaderBytes,nullptr};
    const int sizes[] = {selected.installedBytes,request.resources ? int(request.resources->module.size()) : int(nativeSyslinuxModuleBytes)};
    for (int i = 0; i < 2; i++)
    {
        if (i == 1 && request.resources && request.resources->module.isEmpty()) continue;
        int fd = openat(request.directory, names[i], O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        if (fd < 0)
            return errno;
        struct stat info;
        int error = fstat(fd, &info) == 0 ? 0 : errno;
        if (error == 0)
        {
            const bool regular = S_ISREG(info.st_mode) && info.st_nlink == 1;
            const bool expected = info.st_size == sizes[i] && quint64(info.st_dev) == request.device;
            error = regular && expected ? fileAttributes(fd, i == 0, true) : EBADMSG;
        }
        if (close(fd) != 0 && error == 0)
            error = errno;
        if (error != 0)
            return error;
    }
    return 0;
}

int checkMount(const NativeSyslinuxNtfsRequest& request)
{
    WRITABLE_TARGET target;
    int error = InspectWritableTarget(request.partition, request.device, &target);
    if (error != 0)
        return error;
    if (target.sector_size != sectorBytes || target.size != request.format.size)
        return ESTALE;
    quint64 sequence = 0;
    if (ioctl(request.partition, BLKGETDISKSEQ, &sequence) != 0)
        return errno;
    if (sequence != request.diskSequence)
        return ESTALE;
    struct stat held, path, parent;
    struct statfs filesystem;
    const QByteArray name = QFile::encodeName(request.mountPath);
    if (fstat(request.directory, &held) != 0 || lstat(name.constData(), &path) != 0 ||
        fstatat(request.directory, "..", &parent, AT_SYMLINK_NOFOLLOW) != 0 ||
        fstatfs(request.directory, &filesystem) != 0)
        return errno;
    const bool directories = S_ISDIR(held.st_mode) && S_ISDIR(path.st_mode);
    const bool sameRoot = held.st_dev == path.st_dev && held.st_ino == path.st_ino;
    const bool mountedPartition = held.st_dev == target.info.st_rdev && parent.st_dev != held.st_dev;
    if (!directories || !sameRoot || !mountedPartition)
        return ESTALE;
    if (filesystem.f_type != ntfsMagic && filesystem.f_type != fuseMagic)
        return ENOTSUP;
    if (filesystem.f_flags & ST_RDONLY)
        return EROFS;
    return IsRufusJobCancelled() ? ECANCELED : 0;
}

int readBytes(int fd, void* output, size_t size, quint64 offset)
{
    if (IsRufusJobCancelled())
        return ECANCELED;
    ssize_t count;
    do {
        count = pread(fd, output, size, off_t(offset));
    } while (count < 0 && errno == EINTR);
    if (count < 0)
        return errno;
    return count == ssize_t(size) ? 0 : EIO;
}

int readBoot(const NativeSyslinuxNtfsRequest& request, QByteArray& boot, QByteArray& backup)
{
    boot.resize(sectorBytes);
    backup.resize(sectorBytes);
    int error = readBytes(request.partition, boot.data(), sectorBytes, 0);
    if (error == 0)
        error = readBytes(request.partition, backup.data(), sectorBytes, request.format.size - sectorBytes);
    return error;
}

int checkImageRoot(const NativeSyslinuxNtfsRequest& request, QString& problem)
{
    struct stat info;
    if (fstatat(request.directory, "ldlinux.sys", &info, AT_SYMLINK_NOFOLLOW) == 0)
    {
        problem = QStringLiteral("The extracted NTFS root must not contain ldlinux.sys before installation.");
        return EEXIST;
    }
    if (errno != ENOENT)
        return errno;
    if (request.resources && request.resources->module.isEmpty()) return 0;
    const int moduleBytes = request.resources ? int(request.resources->module.size()) : int(nativeSyslinuxModuleBytes);
    // extlinux replaces the root companion. Missing is allowed; an existing
    // file must be ordinary, unshared and identical to that replacement.
    int fd = openat(request.directory, "ldlinux.c32", O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0)
        return errno == ENOENT ? 0 : errno;
    int error = fstat(fd, &info) == 0 ? 0 : errno;
    if (error == 0)
    {
        const bool regular = S_ISREG(info.st_mode) && info.st_nlink == 1;
        const bool expected = info.st_size == moduleBytes && quint64(info.st_dev) == request.device;
        if (!regular || !expected)
            error = EBADMSG;
    }
    QByteArray module;
    if (error == 0)
    {
        module.resize(moduleBytes);
        error = readBytes(fd, module.data(), size_t(module.size()), 0);
    }
    if (error == 0) error = request.resources ? (module == request.resources->module ? 0 : EBADMSG) : CheckNativeSyslinuxModule(module,problem);
    if (close(fd) != 0 && error == 0)
        error = errno;
    return error;
}

int checkBoot(const NativeSyslinuxNtfsRequest& request, const QByteArray& bytes)
{
    ntfs_boot_sector boot;
    memcpy(&boot, bytes.constData(), sizeof(boot));
    const bool ntfs = memcmp(boot.bsOemName, "NTFS    ", sizeof(boot.bsOemName)) == 0;
    const bool signature = qFromLittleEndian(boot.bsSignature) == 0xaa55;
    const bool sectors = qFromLittleEndian(boot.bsBytesPerSec) == sectorBytes;
    const bool clusters = boot.bsSecPerClust * sectorBytes == request.format.clusterSize;
    const bool size = qFromLittleEndian(boot.bsTotalSectors) == request.format.size / sectorBytes - 1;
    const bool offset = qFromLittleEndian(boot.bsUnused_2) == request.format.partitionOffset / sectorBytes;
    const bool serial = qFromLittleEndian(boot.bsVolSerialNr) == request.volumeId;
    if (!ntfs || !signature || !ntfs_check_zero_fields(&boot))
        return EBADMSG;
    return sectors && clusters && size && offset && serial ? 0 : ESTALE;
}

int mapFiemap(int fd, int fileBytes, quint64 volumeBytes, QList<uint64_t>& sectors)
{
    // Fixed bound: only the pinned loader/companion reach this function.
    alignas(fiemap) unsigned char storage[sizeof(fiemap) + maximumFileSectors * sizeof(fiemap_extent)] = {};
    fiemap* map = reinterpret_cast<fiemap*>(storage);
    map->fm_length = fileBytes;
    map->fm_flags = FIEMAP_FLAG_SYNC;
    map->fm_extent_count = maximumFileSectors;
    if (ioctl(fd, FS_IOC_FIEMAP, map) != 0)
        return errno;
    const unsigned count = map->fm_mapped_extents;
    if (count == 0 || count > maximumFileSectors)
        return EBADMSG;
    const int wanted = (fileBytes + sectorBytes - 1) / sectorBytes;
    for (unsigned i = 0; i < count; i++)
    {
        const fiemap_extent& extent = map->fm_extents[i];
        const bool last = (extent.fe_flags & FIEMAP_EXTENT_LAST) != 0;
        const bool supported = (extent.fe_flags & ~(FIEMAP_EXTENT_LAST | FIEMAP_EXTENT_MERGED)) == 0;
        const bool aligned = (extent.fe_logical | extent.fe_physical) % sectorBytes == 0;
        const bool consecutive = extent.fe_logical == quint64(sectors.size()) * sectorBytes;
        const bool physicalStart = extent.fe_physical >= sectorBytes && extent.fe_physical < volumeBytes - sectorBytes;
        if (!supported || !aligned || !consecutive || last != (i + 1 == count))
            return EBADMSG;
        if (!physicalStart || extent.fe_length == 0 || extent.fe_length > volumeBytes - sectorBytes - extent.fe_physical)
            return EBADMSG;
        // Like upstream sectmap_fie, round only the last extent: the kernel
        // NTFS driver reports the companion's exact, non-sector-aligned EOF.
        const uint64_t mappedBytes = last ? (extent.fe_length + sectorBytes - 1) / sectorBytes * sectorBytes : extent.fe_length;
        const bool lengthAligned = mappedBytes % sectorBytes == 0;
        const bool lengthFits = mappedBytes <= volumeBytes - sectorBytes - extent.fe_physical;
        if (!lengthAligned || !lengthFits)
            return EBADMSG;
        const int remaining = wanted - int(sectors.size());
        if (remaining <= 0)
            return EBADMSG;
        const int mapped = int(std::min(uint64_t(remaining), mappedBytes / sectorBytes));
        const uint64_t first = extent.fe_physical / sectorBytes;
        for (int n = 0; n < mapped; n++)
            sectors.append(first + n);
    }
    return sectors.size() == wanted ? 0 : EBADMSG;
}

int mapFile(int fd, int fileBytes, const NtfsFormatOptions& format, QList<uint64_t>& sectors)
{
    int error = mapFiemap(fd, fileBytes, format.size, sectors);
    const bool unsupported = error == EOPNOTSUPP || error == ENOTTY || error == ENOSYS;
    if (!unsupported)
        return error;
    // NTFS-3G has no FIEMAP. Reuse its FIGETBSZ/FIBMAP interface; never fall back
    // after a malformed map or an I/O error. All arithmetic stays bounded here.
    unsigned blockBytes = 0;
    if (ioctl(fd, FIGETBSZ, &blockBytes) != 0)
        return errno;
    const bool blockSize = blockBytes >= sectorBytes && blockBytes <= format.clusterSize;
    const bool powerOfTwo = blockBytes != 0 && (blockBytes & (blockBytes - 1)) == 0;
    if (!blockSize || !powerOfTwo)
        return EBADMSG;
    const int wanted = (fileBytes + sectorBytes - 1) / sectorBytes;
    unsigned logical = 0;
    while (sectors.size() < wanted)
    {
        if (IsRufusJobCancelled())
            return ECANCELED;
        unsigned block = logical++;
        if (ioctl(fd, FIBMAP, &block) != 0)
            return errno;
        const uint64_t first = uint64_t(block) * (blockBytes / sectorBytes);
        const int count = std::min(int(blockBytes / sectorBytes), wanted - int(sectors.size()));
        for (int i = 0; i < count; i++)
            sectors.append(first + i);
    }
    return 0;
}

int readMappedFile(const NativeSyslinuxNtfsRequest& request, const char* name, int bytes,
    QList<uint64_t>& used, QByteArray& contents, QList<uint64_t>& sectors)
{
    int fd = openat(request.directory, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0)
        return errno;
    struct stat before, after;
    int error = fstat(fd, &before) == 0 ? 0 : errno;
    if (error == 0)
    {
        const bool regular = S_ISREG(before.st_mode) && before.st_nlink == 1;
        const bool expected = before.st_size == bytes && quint64(before.st_dev) == request.device;
        error = regular && expected ? 0 : EBADMSG;
    }
    if (error == 0)
        error = fileAttributes(fd, strcmp(name, "ldlinux.sys") == 0, false);
    if (error == 0)
        error = mapFile(fd, bytes, request.format, sectors);
    if (error == 0)
    {
        contents.resize(bytes);
        error = readBytes(fd, contents.data(), size_t(bytes), 0);
    }
    for (int i = 0; error == 0 && i < sectors.size(); i++)
    {
        const uint64_t sector = sectors[i];
        const bool fits = sector != 0 && sector < request.format.size / sectorBytes - 1;
        if (!fits || used.contains(sector))
        {
            error = EBADMSG;
            break;
        }
        used.append(sector);
        char raw[sectorBytes];
        const int count = std::min(sectorBytes, bytes - i * sectorBytes);
        error = readBytes(request.partition, raw, size_t(count), sector * sectorBytes);
        if (error == 0 && memcmp(raw, contents.constData() + i * sectorBytes, size_t(count)) != 0)
            error = EBADMSG;
    }
    if (error == 0 && fstat(fd, &after) != 0)
        error = errno;
    if (error == 0)
    {
        const bool sameSize = before.st_size == after.st_size && before.st_nlink == after.st_nlink;
        const bool sameModification = before.st_mtim.tv_sec == after.st_mtim.tv_sec &&
            before.st_mtim.tv_nsec == after.st_mtim.tv_nsec;
        const bool sameChange = before.st_ctim.tv_sec == after.st_ctim.tv_sec &&
            before.st_ctim.tv_nsec == after.st_ctim.tv_nsec;
        if (!sameSize || !sameModification || !sameChange)
            error = ESTALE;
    }
    if (close(fd) != 0 && error == 0)
        error = errno;
    return error;
}

int verifyInstalled(const NativeSyslinuxNtfsRequest& request,
    const QByteArray& beforeBoot, const QByteArray& beforeBackup, QString& problem)
{
    QByteArray boot, backup, loader, module;
    QList<uint64_t> used, loaderSectors, moduleSectors;
    int error = checkMount(request);
    if (error == 0)
        error = readBoot(request, boot, backup);
    if (error == 0 && beforeBackup != backup)
    {
        problem = QStringLiteral("The installer changed the backup NTFS boot sector.");
        error = EBADMSG;
    }
    if (error == 0)
    {
        const SyslinuxBootPayload selected = {request.resources ? int(request.resources->loader.size()) : syslinux6Payload.loaderBytes,nullptr};
        error = readMappedFile(request, "ldlinux.sys", selected.installedBytes, used, loader, loaderSectors);
        if (error != 0)
            problem = QStringLiteral("Cannot verify the installed NTFS ldlinux.sys file and sector map: ") +
                QString::fromLocal8Bit(strerror(error));
    }
    if (error == 0 && (!request.resources || !request.resources->module.isEmpty()))
    {
        error = readMappedFile(request, "ldlinux.c32", request.resources ? int(request.resources->module.size()) : int(nativeSyslinuxModuleBytes), used, module, moduleSectors);
        if (error != 0)
            problem = QStringLiteral("Cannot verify the installed NTFS ldlinux.c32 file and sector map: ") +
                QString::fromLocal8Bit(strerror(error));
    }
    if (error == 0) error = request.resources ? (module == request.resources->module ? 0 : EBADMSG) : CheckNativeSyslinuxModule(module,problem);
    if (error == 0)
        error = CheckSyslinuxPayload(request.bootType, FS_NTFS, beforeBoot, boot, loader, loaderSectors, problem, request.resources);
    if (error == 0)
        error = checkMount(request);
    return error;
}

int install(const NativeSyslinuxNtfsRequest& request, FORMAT_RESULT& result, QString& problem)
{
    const bool imageInstall = request.bootType == BT_IMAGE;
    if (!imageInstall && request.bootType != BT_SYSLINUX_V6)
        return ENOTSUP;
    const bool hasSource = request.image != nullptr && request.imagePath != nullptr &&
        request.source != nullptr && request.analysis != nullptr;
    const bool noSource = request.image == nullptr && request.imagePath == nullptr &&
        request.source == nullptr && request.analysis == nullptr;
    const bool validSource = imageInstall ? hasSource : noSource;
    if (!validSource)
        return EINVAL;
    const bool identity = request.device != 0 && request.diskSequence != 0;
    const bool absolute = request.installer.startsWith('/') && request.mountPath.startsWith('/');
    const bool embeddedNull = request.installer.contains(QChar::Null) || request.mountPath.contains(QChar::Null);
    if (!identity || !absolute || embeddedNull)
        return EINVAL;
    int error = CheckNtfsFormatOptions(request.format);
    if (!error && request.resources) error = imageInstall ? ValidateSyslinuxResources(request.analysis->report,*request.resources) : EINVAL;
    if (error == 0 && imageInstall)
    {
        error = CheckImageFileSource(request.image, request.imagePath);
        if (error == 0 && quint64(request.image->info.st_dev) == request.device)
            error = EINVAL;
        if (error == 0)
            error = CheckNativeSyslinuxSource(*request.source, *request.analysis, request.rockRidge,
                FS_NTFS, sectorBytes, request.format.partitionOffset, problem);
    }
    if (error == 0)
        error = checkMount(request);
    if (error == 0)
        error = imageInstall ? checkImageRoot(request, problem) : CheckEmptyTargetDirectory(request.directory);
    QByteArray boot, backup;
    if (error == 0)
        error = readBoot(request, boot, backup);
    if (error == 0)
        error = checkBoot(request, boot);
    if (error == 0 && boot != backup)
        error = EBADMSG;
    if (error == 0 && imageInstall)
        error = CheckImageFileSource(request.image, request.imagePath);
    if (error != 0)
        return error;
    QByteArray program = QFile::encodeName(request.installer);
    QByteArray directory = QFile::encodeName(request.mountPath);
    QByteArray target = QByteArray("/proc/self/fd/") + QByteArray::number(FORMAT_TARGET_FD);
    char path[] = "PATH=/usr/bin", locale[] = "LC_ALL=C.UTF-8";
    char install[] = "--install", reset[] = "--reset-adv", device[] = "--device";
    QTemporaryFile resourceFile(QStringLiteral("/run/rufus-syslinux-resources-XXXXXX"));
    QByteArray resourceEnvironment;
    if (request.resources) {
        const QByteArray bytes = EncodeSyslinuxResources(*request.resources);
        if (bytes.isEmpty() || !resourceFile.open() || resourceFile.write(bytes) != bytes.size() || !resourceFile.flush()) return EIO;
        resourceEnvironment = QByteArray("RUFUS_SYSLINUX_RESOURCES=")+QFile::encodeName(resourceFile.fileName());
    }
    char* environment[] = {path, locale, request.resources ? resourceEnvironment.data() : nullptr, nullptr};
    char* arguments[] = {program.data(), install, reset, device, target.data(), directory.data(), nullptr};
    bool restoreCompression = false;
    if (request.format.compression && request.format.clusterSize <= NtfsCompressionClusterBytes)
        error = PrepareNtfsUncompressedCreation(request.directory, &restoreCompression);
    if (error == 0) error = RunInstallerCommand(request.partition, arguments, environment, directory.constData(), &result);
    if (restoreCompression) {
        const int restored = SetNtfsDirectoryCompression(request.directory, true);
        if (error == 0) error = restored;
    }
    if (error == 0)
        error = checkMount(request);
    if (error == 0)
    {
        error = setBootAttributes(request);
        if (error != 0)
            problem = QStringLiteral("Cannot set the Syslinux NTFS file attributes: ") +
                QString::fromLocal8Bit(strerror(error));
    }
    if (result.started)
    {
        // The runner has reaped every child and flushed the raw descriptor.
        // Flush filesystem metadata too, then the device, even on cancellation.
        while (syncfs(request.directory) != 0)
        {
            if (errno == EINTR)
                continue;
            if (result.sync_error == 0)
                result.sync_error = errno;
            break;
        }
        while (fsync(request.partition) != 0)
        {
            if (errno == EINTR)
                continue;
            if (result.sync_error == 0)
                result.sync_error = errno;
            break;
        }
    }
    if (error == 0)
        error = result.sync_error;
    // Filesystem writes and raw block reads use different page-cache mappings.
    // After both flushes, discard clean raw pages before checking current data.
    if (error == 0)
        error = posix_fadvise(request.partition, 0, off_t(request.format.size), POSIX_FADV_DONTNEED);
    if (error == 0)
        error = verifyInstalled(request, boot, backup, problem);
    if (error == 0 && imageInstall)
        error = CheckImageFileSource(request.image, request.imagePath);
    return error;
}
}

int InstallNativeSyslinuxNtfs(const NativeSyslinuxNtfsRequest& request,
    FORMAT_RESULT& result, QString& problem)
{
    result = {};
    result.exit_code = -1;
    problem.clear();
    int error = install(request, result, problem);
    if (error == 0 && IsRufusJobCancelled())
        error = ECANCELED;
    if (error != 0 && problem.isEmpty())
        problem = QStringLiteral("Syslinux NTFS installation check failed: ") + QString::fromLocal8Bit(strerror(error));
    return error;
}
}
