// SPDX-License-Identifier: GPL-3.0-or-later
#include "prepare_image_protocol.h"
#include "platform/udf_format.h"
#include "platform/media_appearance.h"
#include "unattend.h"
#include "dos_locale_policy.h"
#include "platform/partition_image.h"
#include "platform/compressed_write.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <limits>

namespace rufus
{
namespace
{
bool unsignedValue(const QJsonValue& value, quint64& result)
{
    bool valid = false;
    const QString text = value.toString();
    result = text.toULongLong(&valid);
    return valid && QString::number(result) == text;
}

bool exactDosLocale(const PrepareImageRequest& request)
{
    const QByteArray keyboard = request.dosKeyboard.toLatin1();
    const DOS_LOCALE locale = SelectDOSLocale(keyboard.constData(), request.dosCodepage, !request.msDos);
    const bool exactKeyboard = !locale.keyboard_fallback &&
        request.dosKeyboard == QString::fromLatin1(locale.keyboard);
    const bool exactCodepage = !locale.codepage_fallback && request.dosCodepage == locale.codepage;
    return exactKeyboard && exactCodepage;
}

bool validImageSource(const PrepareImageRequest& request)
{
    const bool path = QDir::isAbsolutePath(request.imagePath) && !request.imagePath.contains(QChar::Null);
    const bool identity = request.imageInode != 0 && request.imageSize != 0
        && !request.imageModified.isEmpty() && !request.imageChanged.isEmpty();
    const bool filesystem = request.fileSystem == FS_FAT16 || request.fileSystem == FS_FAT32
        || request.fileSystem == FS_NTFS;
    return path && identity && filesystem;
}

QJsonObject encodeSilentInstall(const PrepareImageRequest& request)
{
    const WindowsRegionOptions& region = request.windowsRegion;
    const QJsonObject regional = {{"input", region.inputLocale}, {"system", region.systemLocale},
        {"user", region.userLocale}, {"display", region.uiLanguage},
        {"fallback", region.uiLanguageFallback}, {"timezone", region.timezone}};
    return {{"archive", request.windowsInstallPath}, {"edition", QString::number(request.windowsEdition)},
        {"region", regional}, {"confirmed", request.windowsSilentConfirmed}};
}

bool decodeSilentInstall(const QJsonObject& object, PrepareImageRequest& request)
{
    quint64 edition = 0;
    if (object.size() != 4 || !unsignedValue(object.value("edition"), edition))
        return false;
    const bool validEdition = edition > 0 && edition <= std::numeric_limits<int>::max();
    if (!validEdition || !object.value("confirmed").isBool() || !object.value("archive").isString())
        return false;
    request.windowsInstallPath = object.value("archive").toString();
    if (request.windowsInstallPath.isEmpty() || request.windowsInstallPath.contains(QChar::Null))
        return false;
    request.windowsEdition = int(edition);
    request.windowsSilentConfirmed = object.value("confirmed").toBool();
    const QJsonObject regional = object.value("region").toObject();
    const char* const keys[] = {"input", "system", "user", "display", "fallback", "timezone"};
    WindowsRegionOptions& region = request.windowsRegion;
    QString* const fields[] = {&region.inputLocale, &region.systemLocale, &region.userLocale,
        &region.uiLanguage, &region.uiLanguageFallback, &region.timezone};
    const size_t fieldCount = sizeof(keys) / sizeof(keys[0]);
    if (regional.size() != int(fieldCount))
        return false;
    for (size_t i = 0; i < fieldCount; ++i)
    {
        const QJsonValue value = regional.value(keys[i]);
        if (!value.isString() || value.toString().contains(QChar::Null))
            return false;
        *fields[i] = value.toString();
    }
    return true;
}
}

bool ValidatePrepareGrub2Core(const PrepareImageRequest& request)
{
    if (!request.hasGrub2Core())
        return !request.grub4dos || !request.imagePath.isEmpty();
    // Image GRUB4DOS supplies its own loader through retained ISO extraction.
    if (request.grub4dos && !request.imagePath.isEmpty())
        return false;
    const bool windowsFilesystem = request.fileSystem == FS_NTFS || request.fileSystem == FS_FAT16 || request.fileSystem == FS_FAT32;
    const bool windowsSbr = request.windowsBios && request.enhancedInstall
        && windowsFilesystem && !request.uefiNtfs;
    const bool syslinuxSbr = request.syslinux && request.syslinuxSelection == BT_IMAGE
        && validImageSource(request);
    const bool grubJob = (request.grub2 || request.grub4dos || windowsSbr || syslinuxSbr)
        && (request.mbr || syslinuxSbr) && request.isDevice;
    const bool absolutePath = QDir::isAbsolutePath(request.grub2CorePath) && !request.grub2CorePath.contains(QChar::Null);
    const bool bounded = request.grub2CoreSize >= PrepareCoreSectorBytes && request.grub2CoreSize <= PrepareCoreMaximumBytes;
    const bool fingerprint = request.grub2CoreSha256.size() == 32;
    return grubJob && absolutePath && bounded && fingerprint;
}

bool ValidatePrepareArchive(const PrepareImageRequest& request)
{
    const PrepareArchiveSource& archive = request.archive;
    if (!archive.present()) return true;
    const bool filesystem = request.fileSystem >= FS_FAT16 && request.fileSystem < FS_EXT2;
    const bool path = QDir::isAbsolutePath(archive.path) && !archive.path.contains(QChar::Null);
    const bool identity = archive.inode != 0 && archive.size >= 22 && archive.size <= quint64(INT64_MAX)
        && !archive.modified.isEmpty() && !archive.changed.isEmpty();
    return request.isDevice && !request.rawWrite && request.zeroMode == ZERO_NONE && filesystem && path && identity;
}

bool ValidatePrepareWindowsToGo(const PrepareImageRequest& request)
{
    if (!request.windowsToGo) return request.windowsToGoTarget == TT_UEFI;
    const bool firmware = request.windowsToGoTarget == TT_UEFI
        || (request.windowsToGoTarget == TT_BIOS && request.mbr);
    const bool source = request.isDevice && validImageSource(request) && request.fileSystem == FS_NTFS;
    const bool otherMode = request.rawWrite || request.nonBootable || request.sfd || request.isoEsp
        || request.syslinux || request.isDos() || request.reactOs || request.kolibriOs || request.grub2 || request.grub4dos || request.windowsBios || request.kolibriOs
        || request.hasGrub2Core() || request.hasCompressedImage() || request.persistenceSize != 0;
    const QString& archive = request.windowsInstallPath;
    const bool internalPath = archive.isEmpty() || (archive.startsWith(QLatin1Char('/'))
        && !archive.contains(QChar::Null) && QDir::cleanPath(archive) == archive);
    const WindowsRegionOptions& region = request.windowsRegion;
    const bool regionEmpty = region.inputLocale.isEmpty() && region.systemLocale.isEmpty()
        && region.userLocale.isEmpty() && region.uiLanguage.isEmpty()
        && region.uiLanguageFallback.isEmpty() && region.timezone.isEmpty();
    const bool edition = internalPath && request.windowsEdition > 0 && !request.windowsSilentConfirmed;
    const bool installerFlags = request.windowsFlags & (UNATTEND_SILENT_INSTALL | UNATTEND_DUPLICATE_LOCALE | UNATTEND_WINDOWS_TO_GO);
    const bool bootOptions = (!request.uefiNtfs || request.mbr)
        && (request.mbr || request.windowsRufusMbr);
    return firmware && source && !otherMode && edition && regionEmpty && !installerFlags && bootOptions;
}

quint64 PrepareRawWriteBytes(const PrepareImageRequest& request)
{
    if (!request.rawWrite)
        return 0;
    if (request.zeroMode != ZERO_NONE)
        return request.size;
    if (!request.hasCompressedImage())
        return request.imageSize;
    uint64_t size = 0;
    const bool supported = request.imageCompression == BLED_COMPRESSION_GZIP
        || request.imageCompression == BLED_COMPRESSION_XZ || request.imageCompression == BLED_COMPRESSION_BZIP2
        || request.imageCompression == BLED_COMPRESSION_LZMA || request.imageCompression == BLED_COMPRESSION_LZW
        || request.imageCompression == BLED_COMPRESSION_ZIP || request.imageCompression == BLED_COMPRESSION_ZSTD
        || request.imageCompression == BLED_COMPRESSION_VTSI
        || request.imageCompression == IMG_COMPRESSION_FFU
        || request.imageCompression == IMG_COMPRESSION_VHD || request.imageCompression == IMG_COMPRESSION_VHDX;
    const quint64 required = request.ffuMinimumSize != 0 ? request.ffuMinimumSize : request.decodedImageSize;
    if (!supported || CompressedImageWriteSize(required, request.imageSectorSize, &size) != 0)
        return 0;
    return size;
}

bool ValidatePrepareCompressedImage(const PrepareImageRequest& request)
{
    if (!request.hasCompressedImage())
        return true;
    if (request.ffuStore > 65535 || (request.ffuStore != 0 && request.imageCompression != IMG_COMPRESSION_FFU))
        return false;
    if (request.ffuMinimumSize != 0 && (request.imageCompression != IMG_COMPRESSION_FFU
        || request.ffuMinimumSize > request.decodedImageSize)) return false;
    const bool supported = request.imageCompression == BLED_COMPRESSION_GZIP
        || request.imageCompression == BLED_COMPRESSION_XZ || request.imageCompression == BLED_COMPRESSION_BZIP2
        || request.imageCompression == BLED_COMPRESSION_LZMA || request.imageCompression == BLED_COMPRESSION_LZW
        || request.imageCompression == BLED_COMPRESSION_ZIP || request.imageCompression == BLED_COMPRESSION_ZSTD
        || request.imageCompression == BLED_COMPRESSION_VTSI
        || request.imageCompression == IMG_COMPRESSION_FFU
        || request.imageCompression == IMG_COMPRESSION_VHD || request.imageCompression == IMG_COMPRESSION_VHDX;
    const bool compressedDd = request.rawWrite && request.isDevice && supported;
    if (!compressedDd || !validImageSource(request))
        return false;
    const bool boundedSource = request.imageSize <= quint64(std::numeric_limits<qint64>::max());
    const bool fullPrefix = request.decodedImageSize >= COMPRESSED_BOOT_SECTOR_BYTES;
    const bool fingerprint = request.decodedImageSha256.size() == SHA256_HASHSIZE;
    const bool layout = request.imageCompression == BLED_COMPRESSION_VTSI || request.imageCompression == IMG_COMPRESSION_FFU;
    const bool layoutFingerprint = layout ? request.imageLayoutSha256.size() == SHA256_HASHSIZE
        : request.imageLayoutSha256.isEmpty();
    if (!boundedSource || !fullPrefix || !fingerprint || !layoutFingerprint)
        return false;
    if (GetImageCompressionType(QFile::encodeName(request.imagePath).constData()) != request.imageCompression)
        return false;
    const quint64 written = PrepareRawWriteBytes(request);
    if (written == 0 || written > request.size)
        return false;
    return request.size % request.imageSectorSize == 0;
}

bool ValidatePreparePersistence(const PrepareImageRequest& request)
{
    if (request.persistenceSize == 0) return request.persistenceExtVersion == 3;
    const bool source = request.isDevice && validImageSource(request) && !request.isSourceFree();
    const bool compatible = !request.rawWrite && !request.sfd && !request.nonBootable && !request.windowsBios && !request.kolibriOs
        && request.windowsFlags == 0 && request.windowsUsername.isEmpty() && !request.hasWindowsInstallSettings();
    const bool filesystem = request.fileSystem == FS_FAT16 || request.fileSystem == FS_FAT32 || request.fileSystem == FS_NTFS;
    const bool version = request.persistenceExtVersion >= 2 && request.persistenceExtVersion <= 4;
    const bool size = request.persistenceSize >= MinimumPersistenceBytes && request.persistenceSize < request.size
        && request.persistenceSize % PersistenceUnitBytes == 0;
    return source && compatible && filesystem && version && size;
}

static bool ValidOldBiosFixes(const PrepareImageRequest& request)
{
    if (!request.oldBiosFixes) return true;
    const bool bios = request.syslinux || request.isDos() || request.reactOs || request.kolibriOs || request.grub2 || request.grub4dos || request.windowsBios || request.kolibriOs
        || (request.windowsToGo && request.windowsToGoTarget == TT_BIOS);
    return request.isDevice && !request.rawWrite && !request.sfd && !request.nonBootable
        && (request.mbr || request.syslinux) && bios;
}

static bool ValidIsoEsp(const PrepareImageRequest& request)
{
    if (!request.isoEsp) return true;
    const bool fat = request.fileSystem == FS_FAT16 || request.fileSystem == FS_FAT32;
    const bool bios = request.syslinux || request.isDos() || request.reactOs || request.kolibriOs || request.grub2 || request.grub4dos || request.windowsBios || request.kolibriOs;
    const bool fat32Quick = request.fileSystem != FS_FAT32 || request.quickFormat;
    return request.isDevice && !request.rawWrite && !request.nonBootable && !request.sfd && !request.mbr
        && !request.oldBiosFixes && !request.uefiNtfs && !bios && fat && fat32Quick && !request.imagePath.isEmpty();
}

static bool ValidRuntimeValidation(const PrepareImageRequest& request)
{
    if (!request.runtimeValidation) return true;
    const bool filesystem = request.fileSystem == FS_FAT16 || request.fileSystem == FS_FAT32 || request.fileSystem == FS_NTFS;
    const bool copiedImage = request.isDevice && validImageSource(request) && !request.isSourceFree()
        && !request.rawWrite && !request.windowsToGo && !request.sfd && !request.hasCompressedImage();
    const bool forcedWindowsBios = request.windowsBios && !request.enhancedInstall;
    return filesystem && copiedImage && !forcedWindowsBios;
}

bool ValidFormatGeometry(const PrepareImageRequest& request)
{
    const quint32 logical = request.logicalSectorSize, physical = request.physicalSectorSize;
    const bool logicalValid = logical >= 512 && logical <= 4096 && (logical & (logical - 1)) == 0;
    const bool physicalValid = physical >= logical && physical <= 65536 && (physical & (physical - 1)) == 0;
    if (!logicalValid || !physicalValid) return false;
    // Original BIOS/Syslinux/SBR and bundled UEFI:NTFS bytes address 512-byte sectors.
    const bool legacy = request.syslinux || request.isDos() || request.reactOs || request.kolibriOs
        || request.grub2 || request.grub4dos || request.windowsBios || request.hasGrub2Core()
        || (request.windowsToGo && request.windowsToGoTarget == TT_BIOS);
    return logical == 512 || (!legacy && !request.usesUefiNtfs());
}

static QJsonObject encodePrepareContent(const PrepareImageRequest& request)
{
    if (request.rawWrite && (request.logicalSectorSize != 512 || request.physicalSectorSize != 512)) return {};
    if (!request.rawWrite && request.deviceOperation == 0 && !ValidFormatGeometry(request)) return {};
    const bool ntfsFormat = request.isDevice && request.fileSystem == FS_NTFS && !request.rawWrite && request.zeroMode == ZERO_NONE;
    const bool udfFormat = request.isDevice && request.fileSystem == FS_UDF && request.nonBootable
        && !request.rawWrite && request.zeroMode == ZERO_NONE;
    if (request.ntfsCompression && !ntfsFormat) return {};
    if (request.fileIndexing && !ntfsFormat && !udfFormat) return {};
    const bool ntfsOptions = ntfsFormat && (request.ntfsCompression || request.fileIndexing);
    const bool newBoot = request.msDos || request.reactOs;
    if (newBoot) {
        const bool fat = request.fileSystem == FS_FAT16 || request.fileSystem == FS_FAT32;
        const bool layout = request.isDevice && request.mbr && fat && !request.sfd;
        const bool conflict = request.freeDos || (request.msDos && request.reactOs) || request.nonBootable
            || request.rawWrite || request.uefiNtfs || request.syslinux || request.grub2 || request.grub4dos
            || request.windowsBios || request.kolibriOs || request.windowsToGo || !request.imagePath.isEmpty();
        if (!layout || conflict) return {};
    }
    const bool dosImage = QDir::isAbsolutePath(request.dosImagePath) && !request.dosImagePath.contains(QChar::Null);
    if (request.msDos && (!dosImage || (request.sizeChecks && request.clusterSize >= 65536))) return {};
    if (!request.msDos && !request.dosImagePath.isEmpty()) return {};

    if (!ValidatePrepareWindowsToGo(request)) return {};
    if (!ValidOldBiosFixes(request)) return {};
    if (!ValidIsoEsp(request)) return {};
    if (!ValidRuntimeValidation(request)) return {};
    const bool appearanceFilesystem = request.fileSystem == FS_FAT16 || request.fileSystem == FS_FAT32
        || request.fileSystem == FS_NTFS || request.fileSystem == FS_EXFAT || request.fileSystem == FS_UDF;
    if (request.extendedLabel && (!request.isDevice || request.rawWrite || !appearanceFilesystem || !ValidMediaLabel(request.displayLabel))) return {};
    if (!request.extendedLabel && !request.displayLabel.isEmpty()) return {};
    if (!request.quickFormat && (request.rawWrite || !SupportsFullFormat(request.fileSystem, request.size))) return {};
    if (request.badBlocksPattern < -1 || request.badBlocksPattern >= BADLOCKS_PATTERN_TYPES
        || (request.badBlocksPattern >= 0 && !request.isDevice)) return {};
    const bool zero = request.zeroMode == ZERO_FULL || request.zeroMode == ZERO_FAST;
    if (request.zeroMode != ZERO_NONE && !zero) return {};
    if (zero)
    {
        const bool sourceMetadata = !request.imagePath.isEmpty() || request.imageDevice != 0 || request.imageInode != 0
            || request.imageSize != 0 || !request.imageModified.isEmpty() || !request.imageChanged.isEmpty();
        if (!request.rawWrite || !request.isDevice || sourceMetadata || request.hasCompressedImage() || request.ignoreBootMarker)
            return {};
    }
    if (!ValidatePreparePersistence(request)) return {};
    if (!ValidatePrepareArchive(request)) return {};
    if (!ValidatePrepareCompressedImage(request))
        return {};
    if (!ValidatePrepareGrub2Core(request))
        return {};
    if (request.hasSyslinuxResources()) {
        const bool job = request.syslinux && !request.isSourceFree() && request.isDevice && !request.rawWrite;
        const bool path = QDir::isAbsolutePath(request.syslinuxResourcesPath) && !request.syslinuxResourcesPath.contains(QChar::Null);
        const bool bytes = request.syslinuxResourcesSize >= 540 && request.syslinuxResourcesSize <= PrepareCoreMaximumBytes;
        if (!job || !path || !bytes || request.syslinuxResourcesSha256.size() != 32) return {};
    }
    const bool grubImageUefi = request.uefiNtfs && (request.grub2 || request.grub4dos)
        && request.fileSystem == FS_NTFS && validImageSource(request);
    if (request.grub4dos)
    {
        const bool image = !request.imagePath.isEmpty();
        const bool fat = request.fileSystem == FS_FAT16 || request.fileSystem == FS_FAT32;
        const bool loaderFilesystem = fat || request.fileSystem == FS_NTFS;
        const bool nativeLoader = request.isDevice && request.mbr && loaderFilesystem;
        const bool incompatibleMode = request.rawWrite || request.nonBootable || request.sfd
            || request.syslinux || request.isDos() || request.reactOs || request.kolibriOs || request.grub2 || request.windowsBios
            || (request.uefiNtfs && !grubImageUefi);
        const bool invalidSource = image && !validImageSource(request);
        const bool sourceOptions = invalidSource || request.enhancedInstall
            || request.windowsFlags != 0 || !request.windowsUsername.isEmpty() || request.hasWindowsInstallSettings();
        if (!nativeLoader || incompatibleMode || sourceOptions)
            return {};
    }
    if (request.kolibriOs)
    {
        const bool filesystem = request.fileSystem == FS_FAT32 || request.fileSystem == FS_NTFS;
        const bool source = request.isDevice && request.mbr && validImageSource(request);
        const bool conflict = request.rawWrite || request.nonBootable || request.sfd || request.syslinux
            || request.isDos() || request.reactOs || request.grub2 || request.grub4dos || request.windowsBios
            || request.windowsToGo || request.enhancedInstall || request.hasGrub2Core()
            || (request.uefiNtfs && request.fileSystem != FS_NTFS)
            || request.windowsFlags != 0 || !request.windowsUsername.isEmpty() || request.hasWindowsInstallSettings();
        if (!filesystem || !source || conflict) return {};
    }
    const bool standaloneSyslinux = request.isStandaloneSyslinux();
    if (!standaloneSyslinux && request.syslinuxSelection != BT_IMAGE)
        return {};
    if (request.windowsBios)
    {
        const bool windowsFilesystem = request.fileSystem == FS_NTFS || request.fileSystem == FS_FAT16 || request.fileSystem == FS_FAT32;
        const bool nativeWindows = request.isDevice && request.mbr && windowsFilesystem
            && validImageSource(request);
        const bool incompatibleMode = request.rawWrite || request.nonBootable || request.sfd
            || request.syslinux || request.isDos() || request.reactOs || request.kolibriOs || request.grub2;
        const bool customizedCore = request.hasGrub2Core() && (request.windowsFlags != 0
            || !request.windowsUsername.isEmpty() || request.hasWindowsInstallSettings());
        const bool inconsistentDualMode = request.uefiNtfs && !request.enhancedInstall;
        if (!nativeWindows || incompatibleMode || inconsistentDualMode || customizedCore)
            return {};
    }
    else if (!request.windowsToGo && !request.windowsRufusMbr)
    {
        const bool sourceMbr = request.isDevice && validImageSource(request)
            && !request.isSourceFree() && (request.mbr || request.syslinux);
        if (!sourceMbr) return {};
    }
    if (request.isDos() || request.reactOs || request.grub2)
    {
        const bool fat = request.fileSystem == FS_FAT16 || request.fileSystem == FS_FAT32;
        const bool extGrub2 = request.grub2 && (request.fileSystem == FS_EXT2 || request.fileSystem == FS_EXT3);
        const bool nativeMbr = request.isDevice && request.mbr &&
            (fat || (request.grub2 && request.fileSystem == FS_NTFS) || extGrub2);
        const bool incompatibleLayout = request.rawWrite || request.nonBootable || request.sfd ||
            request.syslinux || (request.uefiNtfs && !grubImageUefi);
        const bool invalidSource = !request.imagePath.isEmpty()
            && (!request.grub2 || !validImageSource(request));
        const bool imageOptions = invalidSource || request.enhancedInstall ||
            request.windowsFlags != 0 || !request.windowsUsername.isEmpty() || request.hasWindowsInstallSettings();
        if (!nativeMbr || incompatibleLayout || imageOptions)
            return {};
        if (request.isDos() && request.grub2)
            return {};
    }
    if (request.isDos())
    {
        if (!exactDosLocale(request))
            return {};
    }
    else if (!request.dosKeyboard.isEmpty() || request.dosCodepage != 0 || !request.dosRufusMbr)
        return {};
    if (!ValidDeviceClass(request.deviceClass) || request.deviceClass == DeviceClass::Optical
        || (request.deviceClass != DeviceClass::Usb && !request.isDevice))
        return {};
    if (request.allowFixedUsb && !request.isDevice)
        return {};
    const bool syslinuxImageUefi = request.uefiNtfs && request.syslinux &&
        request.syslinuxSelection == BT_IMAGE && request.fileSystem == FS_NTFS && !request.imagePath.isEmpty();
    if (request.uefiNtfs)
    {
        const bool nativeFileSystem = request.fileSystem == FS_NTFS || request.fileSystem == FS_EXFAT;
        const bool incompatibleLayout = request.rawWrite || request.nonBootable || request.sfd;
        const bool standalone = !request.syslinux && !request.grub2 && !request.windowsBios && request.imagePath.isEmpty();
        const bool supportedSource = standalone || syslinuxImageUefi || grubImageUefi || request.windowsBios || request.kolibriOs || request.windowsToGo;
        if (!request.isDevice || !nativeFileSystem || incompatibleLayout || !supportedSource)
            return {};
    }
    if (request.rawWrite)
    {
        const bool layoutOptions = request.nonBootable || request.sfd || request.mbr || request.syslinux;
        const bool formatOptions = request.clusterSize != 512 || request.volumeId != 0
            || !request.label.isEmpty() || request.fileSystem != FS_FAT32;
        const bool customization = request.enhancedInstall || request.windowsFlags != 0
            || !request.windowsUsername.isEmpty() || request.hasWindowsInstallSettings();
        if (!request.isDevice || (!zero && request.imagePath.isEmpty()) || layoutOptions || formatOptions || customization)
            return {};
    }
    const bool supportedFileSystem = request.fileSystem == FS_FAT16 ||
        request.fileSystem == FS_FAT32 || request.fileSystem == FS_NTFS || request.fileSystem == FS_EXFAT ||
        ((request.grub2 || request.nonBootable) && (request.fileSystem == FS_EXT2 || request.fileSystem == FS_EXT3))
        || (request.nonBootable && request.fileSystem == FS_UDF);
    if (!supportedFileSystem)
        return {};
    const bool exfat = request.fileSystem == FS_EXFAT;
    const bool udf = request.fileSystem == FS_UDF;
    const bool ext = request.fileSystem == FS_EXT2 || request.fileSystem == FS_EXT3;
    const bool nativeDataFormat = request.nonBootable && (udf || ext);
    if (!udf && request.udfRevision != 0x0201) return {};
    if (udf && request.clusterSize != UdfDefaultCluster) return {};
    const bool formatOnly = request.nonBootable || (exfat && !request.uefiNtfs);
    const bool fat = request.fileSystem == FS_FAT16 || request.fileSystem == FS_FAT32;
    const bool supportedSfd = request.nonBootable && (fat || request.fileSystem == FS_NTFS || exfat || udf || ext) && !request.mbr;
    if (request.sfd && !supportedSfd)
        return {};
    if (request.mbr && !request.nonBootable && !request.uefiNtfs && !request.isDos() && !request.reactOs && !request.grub2 && !request.grub4dos && !request.windowsBios && !request.kolibriOs && !request.windowsToGo)
        return {}; // ISO-backed MBR creation is separate from standalone UEFI:NTFS.
    const bool deviceOnly = request.fileSystem == FS_NTFS || ext || formatOnly;
    if (deviceOnly && !request.isDevice)
        return {};
    if (!request.isDevice && !request.imagePath.isEmpty())
        return {};
    const bool hasSource = request.isDevice && !request.imagePath.isEmpty();
    if (formatOnly && hasSource)
        return {};
    const bool nativeSerial = exfat || request.fileSystem == FS_NTFS || ext || udf;
    if (nativeSerial && request.volumeId != 0)
        return {}; // Native NTFS/exFAT formatters generate their own serial.
    if (ext && request.clusterSize != RUFUS_EXT_DEFAULT_CLUSTER)
        return {}; // Rufus's ext layout uses the default cluster sentinel.
    if (request.enhancedInstall && !hasSource)
        return {};
    const bool silent = request.windowsFlags & UNATTEND_SILENT_INSTALL;
    if (!silent && !request.windowsToGo && request.hasWindowsInstallSettings())
        return {};
    const bool customized = request.windowsFlags != 0 || !request.windowsUsername.isEmpty();
    const bool ntfsSyslinux = request.fileSystem == FS_NTFS &&
        (request.syslinuxSelection == BT_IMAGE || request.syslinuxSelection == BT_SYSLINUX_V6);
    const bool incompatibleSyslinux = !(fat || ntfsSyslinux) || request.enhancedInstall || customized || request.hasWindowsInstallSettings();
    if (request.syslinux)
    {
        const bool validSource = standaloneSyslinux ? !hasSource : hasSource;
        const bool incompatibleMode = request.rawWrite || request.nonBootable || request.sfd || (request.uefiNtfs && !syslinuxImageUefi);
        if (!request.isDevice || !validSource || incompatibleSyslinux || incompatibleMode)
            return {};
    }
    if (customized && (!request.isDevice || request.imagePath.isEmpty()))
        return {};
    const char* operation = request.isDevice ? "prepare-device-v1" : "prepare-image-v1";
    if (request.fileSystem == FS_FAT16)
        operation = request.isDevice ? "prepare-fat16-device-v1" : "prepare-fat16-image-v1";
    if (request.fileSystem == FS_NTFS)
        operation = "prepare-ntfs-device-v1";
    if (exfat)
        operation = request.uefiNtfs ? "prepare-uefi-exfat-device-v1" : "format-exfat-device-v1";
    else if (formatOnly && request.fileSystem == FS_NTFS)
        operation = "format-ntfs-device-v1";
    else if (formatOnly)
        operation = request.fileSystem == FS_FAT16 ? "format-fat16-device-v1" : "format-fat32-device-v1";
    if (request.mbr)
    {
        operation = request.fileSystem == FS_FAT16 ? "format-fat16-mbr-device-v1" : "format-fat32-mbr-device-v1";
        if (request.fileSystem == FS_NTFS)
            operation = "format-ntfs-mbr-device-v1";
        else if (exfat)
            operation = "format-exfat-mbr-device-v1";
        if (request.uefiNtfs)
            operation = exfat ? "prepare-uefi-exfat-mbr-device-v1" : "prepare-uefi-ntfs-mbr-device-v1";
    }
    if (request.sfd)
    {
        operation = request.fileSystem == FS_FAT16 ? "format-fat16-sfd-device-v1" : "format-fat32-sfd-device-v1";
        if (request.fileSystem == FS_NTFS)
            operation = "format-ntfs-sfd-device-v1";
        else if (exfat)
            operation = "format-exfat-sfd-device-v1";
    }
    if (request.syslinux)
    {
        operation = request.fileSystem == FS_FAT16 ? "prepare-fat16-syslinux-device-v1" : "prepare-syslinux-device-v1";
        if (ntfsSyslinux)
            operation = syslinuxImageUefi ? "prepare-ntfs-syslinux-uefi-device-v1" : "prepare-ntfs-syslinux-device-v1";
    }
    if (standaloneSyslinux)
    {
        operation = request.fileSystem == FS_FAT16 ? "prepare-syslinux6-fat16-device-v1" : "prepare-syslinux6-fat32-device-v1";
        if (request.syslinuxSelection == BT_SYSLINUX_V4)
            operation = request.fileSystem == FS_FAT16 ? "prepare-syslinux4-fat16-device-v1" : "prepare-syslinux4-fat32-device-v1";
        if (ntfsSyslinux)
            operation = "prepare-syslinux6-ntfs-device-v1";
    }
    if (request.isDos())
        operation = request.fileSystem == FS_FAT16 ? "prepare-freedos-fat16-device-v1" : "prepare-freedos-fat32-device-v1";
    if (request.msDos)
        operation = request.fileSystem == FS_FAT16 ? "prepare-msdos-fat16-device-v1" : "prepare-msdos-fat32-device-v1";
    if (request.reactOs)
        operation = request.fileSystem == FS_FAT16 ? "prepare-reactos-fat16-device-v1" : "prepare-reactos-fat32-device-v1";
    if (request.grub2)
        operation = request.fileSystem == FS_FAT16 ? "prepare-grub2-fat16-device-v1" : "prepare-grub2-fat32-device-v1";
    if (request.grub2 && request.fileSystem == FS_NTFS)
        operation = grubImageUefi ? "prepare-grub2-ntfs-uefi-device-v1" : "prepare-grub2-ntfs-device-v1";
    if (request.grub2 && ext)
        operation = request.fileSystem == FS_EXT2 ? "prepare-grub2-ext2-device-v1" : "prepare-grub2-ext3-device-v1";
    if (request.windowsBios)
    {
        if (request.fileSystem == FS_FAT16)
            operation = "prepare-windows-fat16-bios-device-v1";
        else if (request.fileSystem == FS_FAT32)
            operation = "prepare-windows-fat32-bios-device-v1";
        else
            operation = request.uefiNtfs ? "prepare-windows-ntfs-dual-device-v1" : "prepare-windows-ntfs-bios-device-v1";
    }
    if (request.grub4dos)
        operation = request.fileSystem == FS_NTFS
            ? (grubImageUefi ? "prepare-grub4dos-ntfs-uefi-device-v1" : "prepare-grub4dos-ntfs-device-v1")
            : request.fileSystem == FS_FAT16 ? "prepare-grub4dos-fat16-device-v1" : "prepare-grub4dos-fat32-device-v1";
    if (request.kolibriOs)
        operation = request.fileSystem == FS_NTFS
            ? (request.uefiNtfs ? "prepare-kolibri-ntfs-uefi-device-v1" : "prepare-kolibri-ntfs-device-v1")
            : "prepare-kolibri-fat32-device-v1";
    if (request.rawWrite)
        operation = zero ? (request.zeroMode == ZERO_FAST ? "fast-zero-device-v1" : "zero-device-v1")
            : request.hasCompressedImage() ? "compressed-write-device-v1" : "raw-write-device-v1";
    if (request.windowsToGo)
        operation = request.mbr ? "prepare-windows-togo-mbr-device-v1" : "prepare-windows-togo-gpt-device-v1";
    if (nativeDataFormat) operation = "format-native-device-v1";
    QJsonObject object = {{"operation", operation}, {"path", request.path},
        {"device", QString::number(request.device)},
        {"size", QString::number(request.size)}, {"cluster", QString::number(request.clusterSize)},
        {"volume", QString::number(request.volumeId)}, {"label", request.label}};
    if (request.logicalSectorSize != 512 || request.physicalSectorSize != 512)
        object.insert("geometry", QJsonObject{{"logical", int(request.logicalSectorSize)}, {"physical", int(request.physicalSectorSize)}});
    if (nativeDataFormat) {
        QJsonObject format = {{"filesystem", request.fileSystem},
            {"layout", request.sfd ? "sfd" : request.mbr ? "mbr" : "gpt"}};
        if (udf) format.insert("revision", int(request.udfRevision));
        object.insert("nativeFormat", format);
    }
    if (request.rawWrite)
    {
        object.remove("cluster");
        object.remove("volume");
        object.remove("label");
    }
    if (request.isDevice)
    {
        if (request.runtimeValidation) object.insert("runtimeValidation", true);
        if (request.deviceClass != DeviceClass::Usb) object.insert("deviceClass", double(DeviceClassMask(request.deviceClass)));
        if (request.allowFixedUsb) object.insert("allowFixedUsb", true);
        object.insert("sequence", QString::number(request.diskSequence));
        object.insert("object", request.objectPath);
        object.insert("owner", request.serviceOwner);
        object.insert("identity", QJsonArray::fromStringList(request.identity));
        if (!request.imagePath.isEmpty())
            object.insert("source", QJsonObject{{"path", request.imagePath},
                {"device", QString::number(request.imageDevice)}, {"inode", QString::number(request.imageInode)},
                {"size", QString::number(request.imageSize)}, {"modified", request.imageModified},
                {"changed", request.imageChanged}, {"rockRidge", request.rockRidge}, {"joliet", request.joliet}});
    }
    else
        object.insert("inode", QString::number(request.inode));
    if (request.hasSyslinuxResources())
        object.insert("syslinuxResources",QJsonObject{{"path",request.syslinuxResourcesPath},{"size",QString::number(request.syslinuxResourcesSize)},{"sha256",QString::fromLatin1(request.syslinuxResourcesSha256.toHex())}});
    if (request.hasGrub2Core())
        object.insert("grubCore", QJsonObject{{"path", request.grub2CorePath},
            {"size", QString::number(request.grub2CoreSize)}, {"sha256", QString::fromLatin1(request.grub2CoreSha256.toHex())}});
    if (request.msDos) object.insert("dosImage", request.dosImagePath);
    if (request.isDos())
        object.insert("dos", QJsonObject{{"keyboard", request.dosKeyboard},
            {"codepage", QString::number(request.dosCodepage)}, {"rufusMbr", request.dosRufusMbr}});
    if (request.windowsBios || request.windowsToGo || !request.windowsRufusMbr)
        object.insert("rufusMbr", request.windowsRufusMbr);
    if (request.windowsToGo)
        object.insert("toGo", QJsonObject{{"archive", request.windowsInstallPath},
            {"edition", QString::number(request.windowsEdition)}, {"target", request.windowsToGoTarget}, {"uefiNtfs", request.uefiNtfs}});
    if (request.preserveTimestamps && !request.rawWrite && object.contains("source")) {
        QJsonObject source = object.value("source").toObject(); source.insert("preserveTimes", true); object.insert("source", source);
    }
    if (request.rawWrite)
    {
        QJsonObject source = object.value("source").toObject();
        source.remove("rockRidge");
        source.remove("joliet");
        if (request.hasCompressedImage())
        {
            QJsonObject compression = {{"type", request.imageCompression},
                {"decoded", QString::number(request.decodedImageSize)}, {"sector", int(request.imageSectorSize)},
                {"sha256", QString::fromLatin1(request.decodedImageSha256.toHex())}, {"ignoreMarker", request.ignoreBootMarker}};
            if (request.imageCompression == BLED_COMPRESSION_VTSI || request.imageCompression == IMG_COMPRESSION_FFU)
                compression.insert("layoutSha256", QString::fromLatin1(request.imageLayoutSha256.toHex()));
            if (request.ffuStore != 0)
                compression.insert("store", int(request.ffuStore));
            if (request.ffuMinimumSize != 0)
                compression.insert("minimum", QString::number(request.ffuMinimumSize));
            source.insert("compression", compression);
        }
        else if (request.ignoreBootMarker)
            source.insert("ignoreMarker", true);
        if (!zero) object.insert("source", source);
    }
    if (request.enhancedInstall)
        object.insert("enhancedInstall", true);
    if (customized)
    {
        QJsonObject windows = {{"flags", QString::number(request.windowsFlags)}, {"username", request.windowsUsername}};
        if (silent)
            windows.insert("silent", encodeSilentInstall(request));
        object.insert("windows", windows);
    }
    if (request.extendedLabel) object.insert("autorun", QJsonObject{{"label", request.displayLabel}});
    if (ntfsOptions) object.insert("ntfsOptions", QJsonObject{{"compression", request.ntfsCompression}, {"indexing", request.fileIndexing}});
    if (udfFormat && request.fileIndexing) object.insert("udfIndexing", true);
    if (!request.sizeChecks) object.insert("skipSizeChecks", true);
    if (request.oldBiosFixes) object.insert("oldBiosFixes", true);
    if (request.isoEsp) object.insert("isoEsp", true);
    if (!request.quickFormat) object.insert("fullFormat", true);
    if (request.badBlocksPattern >= 0) object.insert("badBlocks", QJsonObject{
        {"pattern", request.badBlocksPattern}, {"detectFakes", request.detectFakes}});
    if (request.persistenceSize != 0)
        object.insert("persistence", QJsonObject{{"size", QString::number(request.persistenceSize)}, {"ext", request.persistenceExtVersion}});
    if (request.archive.present()) {
        const PrepareArchiveSource& archive = request.archive;
        object.insert("archive", QJsonObject{{"path", archive.path}, {"device", QString::number(archive.device)},
            {"inode", QString::number(archive.inode)}, {"size", QString::number(archive.size)},
            {"modified", archive.modified}, {"changed", archive.changed}});
    }
    return object;
}

static bool decodePrepareContent(const QJsonObject& object, PrepareImageRequest& request)
{
    PrepareImageRequest decoded;
    quint64 cluster = decoded.clusterSize, volume = 0;
    const bool compressed = object.value("operation") == "compressed-write-device-v1";
    const bool fullZero = object.value("operation") == "zero-device-v1";
    const bool fastZero = object.value("operation") == "fast-zero-device-v1";
    const bool zero = fullZero || fastZero;
    const bool raw = zero || compressed || object.value("operation") == "raw-write-device-v1";
    const bool fat16Image = object.value("operation") == "prepare-fat16-image-v1";
    const bool fat16Device = object.value("operation") == "prepare-fat16-device-v1";
    const bool fat16Syslinux = object.value("operation") == "prepare-fat16-syslinux-device-v1";
    const bool syslinux4Fat16 = object.value("operation") == "prepare-syslinux4-fat16-device-v1";
    const bool syslinux4 = syslinux4Fat16 || object.value("operation") == "prepare-syslinux4-fat32-device-v1";
    const bool standaloneSyslinux16 = syslinux4Fat16 || object.value("operation") == "prepare-syslinux6-fat16-device-v1";
    const bool syslinuxNtfs = object.value("operation") == "prepare-syslinux6-ntfs-device-v1";
    const bool syslinuxNtfsUefi = object.value("operation") == "prepare-ntfs-syslinux-uefi-device-v1";
    const bool syslinuxNtfsImage = syslinuxNtfsUefi || object.value("operation") == "prepare-ntfs-syslinux-device-v1";
    const bool standaloneSyslinux = syslinux4 || standaloneSyslinux16 || syslinuxNtfs || object.value("operation") == "prepare-syslinux6-fat32-device-v1";
    const bool msDos16 = object.value("operation") == "prepare-msdos-fat16-device-v1";
    const bool msDos = msDos16 || object.value("operation") == "prepare-msdos-fat32-device-v1";
    const bool reactOs16 = object.value("operation") == "prepare-reactos-fat16-device-v1";
    const bool reactOs = reactOs16 || object.value("operation") == "prepare-reactos-fat32-device-v1";
    const bool freeDos16 = object.value("operation") == "prepare-freedos-fat16-device-v1";
    const bool freeDos = freeDos16 || object.value("operation") == "prepare-freedos-fat32-device-v1";
    const bool dos = freeDos || msDos;
    if (object.contains("dosImage") != msDos) return false;
    const bool grub2Fat16 = object.value("operation") == "prepare-grub2-fat16-device-v1";
    const bool grub2NtfsUefi = object.value("operation") == "prepare-grub2-ntfs-uefi-device-v1";
    const bool grub2Ntfs = grub2NtfsUefi || object.value("operation") == "prepare-grub2-ntfs-device-v1";
    const bool grub2Ext2 = object.value("operation") == "prepare-grub2-ext2-device-v1";
    const bool grub2Ext3 = object.value("operation") == "prepare-grub2-ext3-device-v1";
    const bool grub2 = grub2Fat16 || grub2Ntfs || grub2Ext2 || grub2Ext3 ||
        object.value("operation") == "prepare-grub2-fat32-device-v1";
    const bool grub4dosFat16 = object.value("operation") == "prepare-grub4dos-fat16-device-v1";
    const bool grub4dosNtfsUefi = object.value("operation") == "prepare-grub4dos-ntfs-uefi-device-v1";
    const bool grub4dosNtfs = grub4dosNtfsUefi || object.value("operation") == "prepare-grub4dos-ntfs-device-v1";
    const bool grub4dos = grub4dosFat16 || grub4dosNtfs || object.value("operation") == "prepare-grub4dos-fat32-device-v1";
    const bool windowsDual = object.value("operation") == "prepare-windows-ntfs-dual-device-v1";
    const bool windowsNtfs = windowsDual || object.value("operation") == "prepare-windows-ntfs-bios-device-v1";
    const bool windowsFat32 = object.value("operation") == "prepare-windows-fat32-bios-device-v1";
    const bool windowsFat16 = object.value("operation") == "prepare-windows-fat16-bios-device-v1";
    const bool windowsBios = windowsNtfs || windowsFat32 || windowsFat16;
    const bool kolibriNtfsUefi = object.value("operation") == "prepare-kolibri-ntfs-uefi-device-v1";
    const bool kolibriNtfs = kolibriNtfsUefi || object.value("operation") == "prepare-kolibri-ntfs-device-v1";
    const bool kolibriOs = kolibriNtfs || object.value("operation") == "prepare-kolibri-fat32-device-v1";
    const bool toGoMbr = object.value("operation") == "prepare-windows-togo-mbr-device-v1";
    const bool toGo = toGoMbr || object.value("operation") == "prepare-windows-togo-gpt-device-v1";
    if (object.contains("toGo") != toGo) return false;
    const bool alternateSourceMbr = object.contains("rufusMbr") && !windowsBios && !toGo;
    if (!object.contains("rufusMbr") && (windowsBios || toGo)) return false;
    if (alternateSourceMbr && (!object.value("rufusMbr").isBool()
        || object.value("rufusMbr").toBool() || !object.contains("source"))) return false;
    if ((windowsBios || toGo) && (!object.value("rufusMbr").isBool() || !object.contains("source")))
        return false;
    if (object.contains("dos") != dos)
        return false;
    const bool formatMbr16 = object.value("operation") == "format-fat16-mbr-device-v1";
    const bool formatMbr32 = object.value("operation") == "format-fat32-mbr-device-v1";
    const bool formatSfd16 = object.value("operation") == "format-fat16-sfd-device-v1";
    const bool formatSfd32 = object.value("operation") == "format-fat32-sfd-device-v1";
    const bool formatFat16 = formatSfd16 || formatMbr16 || object.value("operation") == "format-fat16-device-v1";
    const bool formatFat32 = formatSfd32 || formatMbr32 || object.value("operation") == "format-fat32-device-v1";
    const bool fat16 = fat16Image || fat16Device || fat16Syslinux || standaloneSyslinux16 || formatFat16 || freeDos16 || msDos16 || reactOs16 || grub2Fat16 || grub4dosFat16 || windowsFat16;
    const bool imageOperation = fat16Image || object.value("operation") == "prepare-image-v1";
    const bool fullFormat = object.contains("fullFormat");
    const bool badBlocks = object.contains("badBlocks");
    const bool appearance = object.contains("autorun");
    const bool geometry = object.contains("geometry");
    const bool image = object.size() == 8 + (fullFormat ? 1 : 0) + (geometry ? 1 : 0) && imageOperation;
    const bool withSource = object.contains("source");
    const bool customized = object.contains("windows");
    const bool enhanced = object.contains("enhancedInstall");
    const bool formatMbrNtfs = object.value("operation") == "format-ntfs-mbr-device-v1";
    const bool formatSfdNtfs = object.value("operation") == "format-ntfs-sfd-device-v1";
    const bool formatNtfs = formatSfdNtfs || formatMbrNtfs || object.value("operation") == "format-ntfs-device-v1";
    const bool uefiMbrNtfs = object.value("operation") == "prepare-uefi-ntfs-mbr-device-v1";
    const bool ntfs = toGo || formatNtfs || uefiMbrNtfs || syslinuxNtfs || syslinuxNtfsImage || grub2Ntfs || grub4dosNtfs || windowsNtfs || kolibriNtfs
        || object.value("operation") == "prepare-ntfs-device-v1";
    const bool formatMbrExfat = object.value("operation") == "format-exfat-mbr-device-v1";
    const bool formatSfdExfat = object.value("operation") == "format-exfat-sfd-device-v1";
    const bool formatExfat = formatSfdExfat || formatMbrExfat || object.value("operation") == "format-exfat-device-v1";
    const bool uefiMbrExfat = object.value("operation") == "prepare-uefi-exfat-mbr-device-v1";
    const bool uefiExfat = uefiMbrExfat || object.value("operation") == "prepare-uefi-exfat-device-v1";
    const bool exfat = formatExfat || uefiExfat;
    const bool nativeDataFormat = object.value("operation") == "format-native-device-v1";
    if (object.contains("nativeFormat") != nativeDataFormat) return false;
    const bool formatOnly = formatExfat || formatFat16 || formatFat32 || formatNtfs || nativeDataFormat;
    const bool syslinux = standaloneSyslinux || syslinuxNtfsImage || fat16Syslinux || object.value("operation") == "prepare-syslinux-device-v1";
    const bool nativeOperation = raw || ntfs || uefiExfat || formatOnly || syslinux || dos || reactOs || grub2 || grub4dos || windowsFat32 || windowsFat16 || kolibriOs || fat16Device || object.value("operation") == "prepare-device-v1";
    const bool syslinuxResources = object.contains("syslinuxResources");
    if (syslinuxResources && (!syslinux || standaloneSyslinux || !withSource)) return false;
    const bool coreSource = object.contains("grubCore");
    if (grub4dos && (coreSource == withSource))
        return false;
    if (coreSource && !grub2 && !grub4dos && !windowsBios && (!syslinux || standaloneSyslinux))
        return false;
    if (coreSource && windowsBios && customized)
        return false;
    const bool ntfsOptions = object.contains("ntfsOptions");
    const bool udfIndexing = object.contains("udfIndexing");
    const bool skipSizeChecks = object.contains("skipSizeChecks");
    const bool deviceClass = object.contains("deviceClass");
    const bool fixedUsb = object.contains("allowFixedUsb");
    const bool oldBios = object.contains("oldBiosFixes");
    const bool isoEsp = object.contains("isoEsp");
    const bool runtimeValidation = object.contains("runtimeValidation");
    const bool archive = object.contains("archive");
    const int deviceFields = (zero ? 8 : raw ? 9 : (withSource ? 12 : 11) + (customized ? 1 : 0) + (enhanced ? 1 : 0))
        + (deviceClass ? 1 : 0) + (fixedUsb ? 1 : 0) + (dos ? 1 : 0) + (msDos ? 1 : 0) + (coreSource ? 1 : 0) + (windowsBios || alternateSourceMbr ? 1 : 0) + (toGo ? 2 : 0) + (object.contains("persistence") ? 1 : 0) + (fullFormat ? 1 : 0) + (badBlocks ? 1 : 0) + (appearance ? 1 : 0) + (oldBios ? 1 : 0) + (isoEsp ? 1 : 0) + (archive ? 1 : 0) + (nativeDataFormat ? 1 : 0) + (skipSizeChecks ? 1 : 0) + (ntfsOptions ? 1 : 0) + (udfIndexing ? 1 : 0) + (syslinuxResources ? 1 : 0);
    const bool device = object.size() == deviceFields + (runtimeValidation ? 1 : 0) + (geometry ? 1 : 0) && nativeOperation;
    if (!image && !device)
        return false;
    if (geometry) {
        const QJsonObject value = object.value("geometry").toObject();
        if (value.size() != 2 || !value.value("logical").isDouble() || !value.value("physical").isDouble()) return false;
        const int logical = value.value("logical").toInt(-1), physical = value.value("physical").toInt(-1);
        if (logical < 512 || physical < logical || value.value("logical").toDouble() != logical
            || value.value("physical").toDouble() != physical || (logical == 512 && physical == 512)) return false;
        decoded.logicalSectorSize = quint32(logical); decoded.physicalSectorSize = quint32(physical);
    }
    if (skipSizeChecks) {
        const auto value = object.value("skipSizeChecks");
        if (!device || !msDos || !value.isBool() || !value.toBool()) return false;
        decoded.sizeChecks = false;
    }
    if (deviceClass)
    {
        const auto value = object.value("deviceClass");
        const int kind = value.toInt(0);
        if (!device || !value.isDouble() || value.toDouble() != kind)
            return false;
        decoded.deviceClass = DeviceClass(kind);
        if (!ValidDeviceClass(decoded.deviceClass) || decoded.deviceClass == DeviceClass::Usb)
            return false;
    }
    if (fixedUsb)
    {
        const QJsonValue value = object.value("allowFixedUsb");
        if (!device || !value.isBool() || !value.toBool())
            return false;
        decoded.allowFixedUsb = true;
    }
    if (appearance)
    {
        const QJsonObject metadata = object.value("autorun").toObject();
        if (!device || raw || metadata.size() != 1 || !metadata.value("label").isString()) return false;
        decoded.extendedLabel = true;
        decoded.displayLabel = metadata.value("label").toString();
        if (!ValidMediaLabel(decoded.displayLabel)) return false;
    }
    if (badBlocks)
    {
        const QJsonObject test = object.value("badBlocks").toObject();
        const int pattern = test.value("pattern").toInt(-1);
        const bool shape = test.size() == 2 && test.value("pattern").isDouble()
            && pattern >= 0 && pattern < BADLOCKS_PATTERN_TYPES && test.value("detectFakes").isBool();
        if (!device || !shape) return false;
        decoded.badBlocksPattern = pattern;
        decoded.detectFakes = test.value("detectFakes").toBool();
    }
    if (raw && ((zero ? withSource : !withSource) || customized || enhanced))
        return false;
    const bool bootOptions = withSource || customized || enhanced;
    const bool sourceFree = formatOnly || uefiExfat || uefiMbrNtfs || dos || reactOs
        || ((grub2 || grub4dos) && !withSource) || standaloneSyslinux;
    if (sourceFree && bootOptions)
        return false;
    if ((grub2 || grub4dos) && (customized || enhanced))
        return false;
    if ((grub2NtfsUefi || grub4dosNtfsUefi) && !withSource)
        return false;
    if (syslinux && !standaloneSyslinux && (!withSource || customized || enhanced))
        return false;
    if (customized && (!device || !withSource))
        return false;
    if (enhanced)
    {
        const bool sourceRequest = device && withSource;
        if (!sourceRequest || !object.value("enhancedInstall").isBool())
            return false;
        decoded.enhancedInstall = object.value("enhancedInstall").toBool();
    }
    if (windowsDual && !decoded.enhancedInstall)
        return false;
    decoded.path = object.value("path").toString();
    decoded.label = object.value("label").toString();
    const bool usablePath = QDir::isAbsolutePath(decoded.path) && !decoded.path.contains(QChar::Null);
    const bool textLabel = object.value("label").isString() && !decoded.label.contains(QChar::Null);
    if (!usablePath || (!raw && !textLabel))
        return false;
    if (!unsignedValue(object.value("device"), decoded.device))
        return false;
    if (image && !unsignedValue(object.value("inode"), decoded.inode))
        return false;
    if (!unsignedValue(object.value("size"), decoded.size))
        return false;
    if (!raw && (!unsignedValue(object.value("cluster"), cluster) || cluster > std::numeric_limits<quint32>::max()))
        return false;
    if (!raw && (!unsignedValue(object.value("volume"), volume) || volume > std::numeric_limits<quint32>::max()))
        return false;
    decoded.clusterSize = quint32(cluster);
    decoded.volumeId = quint32(volume);
    decoded.isDevice = device;
    decoded.rawWrite = raw;
    decoded.zeroMode = fullZero ? ZERO_FULL : fastZero ? ZERO_FAST : ZERO_NONE;
    decoded.nonBootable = formatOnly;
    decoded.uefiNtfs = kolibriNtfsUefi || windowsDual || grub2NtfsUefi || grub4dosNtfsUefi || syslinuxNtfsUefi || uefiExfat ||
        (ntfs && !formatOnly && !withSource && !syslinux && !grub2 && !grub4dos);
    decoded.sfd = formatSfd16 || formatSfd32 || formatSfdNtfs || formatSfdExfat;
    decoded.mbr = toGoMbr || formatMbr16 || formatMbr32 || formatMbrNtfs || formatMbrExfat || uefiMbrNtfs || uefiMbrExfat || dos || reactOs || grub2 || grub4dos || windowsBios || kolibriOs;
    decoded.fileSystem = exfat ? FS_EXFAT : (ntfs ? FS_NTFS : (fat16 ? FS_FAT16 : FS_FAT32));
    if (grub2Ext2 || grub2Ext3)
        decoded.fileSystem = grub2Ext2 ? FS_EXT2 : FS_EXT3;
    if (nativeDataFormat) {
        const QJsonObject format = object.value("nativeFormat").toObject();
        const int filesystem = format.value("filesystem").toInt(-1);
        const bool udf = filesystem == FS_UDF;
        const bool ext = filesystem == FS_EXT2 || filesystem == FS_EXT3;
        const bool validFilesystem = format.value("filesystem").isDouble()
            && format.value("filesystem").toDouble() == filesystem && (udf || ext);
        if (!validFilesystem || format.size() != (udf ? 3 : 2)) return false;
        const QString layout = format.value("layout").toString();
        if (layout != QLatin1String("mbr") && layout != QLatin1String("gpt") && layout != QLatin1String("sfd")) return false;
        decoded.fileSystem = filesystem;
        decoded.sfd = layout == QLatin1String("sfd");
        decoded.mbr = layout == QLatin1String("mbr");
        if (udf) {
            const int revision = format.value("revision").toInt(-1);
            if (!format.value("revision").isDouble() || format.value("revision").toDouble() != revision
                || revision < 0 || revision > UINT16_MAX) return false;
            decoded.udfRevision = quint16(revision);
        }
    }
    if (fullFormat)
    {
        const QJsonValue value = object.value("fullFormat");
        if (!value.isBool() || !value.toBool() || raw || !SupportsFullFormat(decoded.fileSystem, decoded.size)) return false;
        decoded.quickFormat = false;
    }
    decoded.syslinux = syslinux;
    decoded.syslinuxSelection = standaloneSyslinux ? BT_SYSLINUX_V6 : BT_IMAGE;
    if (syslinux4)
        decoded.syslinuxSelection = BT_SYSLINUX_V4;
    decoded.freeDos = freeDos;
    decoded.msDos = msDos; decoded.reactOs = reactOs;
    if (msDos) {
        if (!object.value("dosImage").isString()) return false;
        decoded.dosImagePath = object.value("dosImage").toString();
    }
    decoded.grub2 = grub2;
    decoded.grub4dos = grub4dos;
    decoded.windowsBios = windowsBios;
    decoded.kolibriOs = kolibriOs;
    if (windowsBios || toGo || alternateSourceMbr)
        decoded.windowsRufusMbr = object.value("rufusMbr").toBool();
    if (toGo)
    {
        const QJsonObject selected = object.value("toGo").toObject();
        quint64 edition = 0;
        if (selected.size() != 4 || !selected.value("archive").isString()
            || !selected.value("uefiNtfs").isBool() || !selected.value("target").isDouble()
            || !unsignedValue(selected.value("edition"), edition) || edition == 0
            || edition > quint64(std::numeric_limits<int>::max())) return false;
        decoded.windowsToGo = true;
        decoded.windowsInstallPath = selected.value("archive").toString();
        decoded.windowsEdition = int(edition);
        decoded.windowsToGoTarget = selected.value("target").toInt(-1);
        if (selected.value("target").toDouble() != decoded.windowsToGoTarget) return false;
        decoded.uefiNtfs = selected.value("uefiNtfs").toBool();
    }
    if (syslinuxResources) {
        const QJsonObject resources = object.value("syslinuxResources").toObject();
        if (resources.size() != 3 || !resources.value("path").isString() || !resources.value("sha256").isString()
            || !unsignedValue(resources.value("size"),decoded.syslinuxResourcesSize)) return false;
        decoded.syslinuxResourcesPath = resources.value("path").toString();
        const QByteArray hash = resources.value("sha256").toString().toLatin1();
        decoded.syslinuxResourcesSha256 = QByteArray::fromHex(hash);
        if (decoded.syslinuxResourcesSha256.toHex() != hash) return false;
    }
    if (coreSource)
    {
        const QJsonObject core = object.value("grubCore").toObject();
        const bool textFields = core.value("path").isString() && core.value("sha256").isString();
        if (core.size() != 3 || !textFields || !unsignedValue(core.value("size"), decoded.grub2CoreSize))
            return false;
        decoded.grub2CorePath = core.value("path").toString();
        const QByteArray fingerprint = core.value("sha256").toString().toLatin1();
        decoded.grub2CoreSha256 = QByteArray::fromHex(fingerprint);
        const bool exactHex = decoded.grub2CoreSha256.toHex() == fingerprint;
        if (!exactHex || !ValidatePrepareGrub2Core(decoded))
            return false;
    }
    if (dos)
    {
        const QJsonObject dos = object.value("dos").toObject();
        quint64 codepage = 0;
        const bool localeFields = dos.value("keyboard").isString() && unsignedValue(dos.value("codepage"), codepage);
        if (dos.size() != 3 || !localeFields || !dos.value("rufusMbr").isBool())
            return false;
        if (codepage > std::numeric_limits<quint32>::max())
            return false;
        decoded.dosKeyboard = dos.value("keyboard").toString();
        decoded.dosCodepage = quint32(codepage);
        decoded.dosRufusMbr = dos.value("rufusMbr").toBool();
        if (!exactDosLocale(decoded))
            return false;
    }
    if ((ntfs || exfat) && volume != 0)
        return false; // Native formatters generate their own serial numbers.
    if ((grub2Ext2 || grub2Ext3) && (volume != 0 || cluster != RUFUS_EXT_DEFAULT_CLUSTER))
        return false;
    if (device)
    {
        if (!unsignedValue(object.value("sequence"), decoded.diskSequence) || decoded.diskSequence == 0)
            return false;
        decoded.objectPath = object.value("object").toString();
        decoded.serviceOwner = object.value("owner").toString();
        const bool nativeObject = decoded.objectPath.startsWith(QStringLiteral("/org/freedesktop/UDisks2/block_devices/"));
        const bool nativeOwner = decoded.serviceOwner.startsWith(QLatin1Char(':'));
        if (!nativeObject || !nativeOwner || decoded.device == 0)
            return false;
        if (!object.value("identity").isArray())
            return false;
        for (const QJsonValue& value : object.value("identity").toArray())
        {
            if (!value.isString())
                return false;
            decoded.identity.append(value.toString());
        }
        if (decoded.identity.isEmpty())
            return false;
        if (withSource)
        {
            const QJsonObject source = object.value("source").toObject();
            const bool rawOverride = raw && !compressed && source.contains("ignoreMarker");
            if (rawOverride && (!source.value("ignoreMarker").isBool() || !source.value("ignoreMarker").toBool()))
                return false;
            const bool preserveTimes = source.contains("preserveTimes");
            if (preserveTimes && (raw || !source.value("preserveTimes").isBool() || !source.value("preserveTimes").toBool())) return false;
            decoded.preserveTimestamps = preserveTimes;
            if (source.size() != (compressed ? 7 : rawOverride ? 7 : raw ? 6 : 8) + (preserveTimes ? 1 : 0))
                return false;
            decoded.imagePath = source.value("path").toString();
            const bool usableSource = QDir::isAbsolutePath(decoded.imagePath) && !decoded.imagePath.contains(QChar::Null);
            if (!usableSource)
                return false;
            if (!unsignedValue(source.value("device"), decoded.imageDevice)
                || !unsignedValue(source.value("inode"), decoded.imageInode)
                || !unsignedValue(source.value("size"), decoded.imageSize))
                return false;
            decoded.imageModified = source.value("modified").toString();
            decoded.imageChanged = source.value("changed").toString();
            if (decoded.imageModified.isEmpty() || decoded.imageChanged.isEmpty())
                return false;
            if (!raw)
            {
                if (!source.value("rockRidge").isBool() || !source.value("joliet").isBool())
                    return false;
                decoded.rockRidge = source.value("rockRidge").toBool();
                decoded.joliet = source.value("joliet").toBool();
            }
            else if (compressed)
            {
                const QJsonObject compression = source.value("compression").toObject();
                if (!compression.value("type").isDouble()
                    || !compression.value("sector").isDouble() || !compression.value("sha256").isString()
                    || !compression.value("ignoreMarker").isBool())
                    return false;
                decoded.imageCompression = compression.value("type").toInt(-1);
                const bool hasLayout = decoded.imageCompression == BLED_COMPRESSION_VTSI || decoded.imageCompression == IMG_COMPRESSION_FFU;
                const bool hasStore = compression.contains("store");
                const bool hasMinimum = compression.contains("minimum");
                if (compression.size() != (hasLayout ? 6 : 5) + int(hasStore) + int(hasMinimum))
                    return false;
                if (hasMinimum && (decoded.imageCompression != IMG_COMPRESSION_FFU
                    || !unsignedValue(compression.value("minimum"), decoded.ffuMinimumSize)
                    || decoded.ffuMinimumSize == 0)) return false;
                if (hasStore)
                {
                    const int store = compression.value("store").toInt(-1);
                    const bool validStore = compression.value("store").isDouble() && store > 0 && store <= 65535
                        && compression.value("store").toDouble() == double(store);
                    if (!validStore || decoded.imageCompression != IMG_COMPRESSION_FFU) return false;
                    decoded.ffuStore = quint32(store);
                }
                const int sector = compression.value("sector").toInt(-1);
                if (sector <= 0 || !unsignedValue(compression.value("decoded"), decoded.decodedImageSize))
                    return false;
                decoded.imageSectorSize = quint32(sector);
                const QString hash = compression.value("sha256").toString();
                decoded.decodedImageSha256 = QByteArray::fromHex(hash.toLatin1());
                if (hash != QString::fromLatin1(decoded.decodedImageSha256.toHex()))
                    return false;
                if (hasLayout)
                {
                    if (!compression.value("layoutSha256").isString())
                        return false;
                    const QString layout = compression.value("layoutSha256").toString();
                    decoded.imageLayoutSha256 = QByteArray::fromHex(layout.toLatin1());
                    if (layout != QString::fromLatin1(decoded.imageLayoutSha256.toHex()))
                        return false;
                }
                decoded.ignoreBootMarker = compression.value("ignoreMarker").toBool();
                if (!ValidatePrepareCompressedImage(decoded))
                    return false;
            }
            else
            {
                decoded.ignoreBootMarker = rawOverride;
                const bool wholeSectors = decoded.imageSize != 0 && decoded.imageSize % 512 == 0;
                if (!wholeSectors || decoded.imageSize > decoded.size)
                    return false;
            }
        }
    }
    if (zero && (decoded.device == 0 || decoded.size < 512 || decoded.size % 512 != 0
        || decoded.size > quint64(std::numeric_limits<qint64>::max()))) return false;
    const bool retainedSourceRequired = ((grub2 || grub4dos) && withSource) || windowsBios || kolibriOs || toGo;
    if (retainedSourceRequired && !validImageSource(decoded))
        return false;
    if (customized)
    {
        const QJsonObject windows = object.value("windows").toObject();
        quint64 flags = 0;
        if (!unsignedValue(windows.value("flags"), flags))
            return false;
        if (flags == 0 || flags > std::numeric_limits<int>::max())
            return false;
        if (!windows.value("username").isString())
            return false;
        decoded.windowsFlags = int(flags);
        const bool silent = decoded.windowsFlags & UNATTEND_SILENT_INSTALL;
        if (windows.size() != (silent ? 3 : 2) || windows.contains("silent") != silent)
            return false;
        if (silent && !decodeSilentInstall(windows.value("silent").toObject(), decoded))
            return false;
        decoded.windowsUsername = windows.value("username").toString();
        if (decoded.windowsUsername.contains(QChar::Null))
            return false;
    }
    if (object.contains("persistence")) {
        const QJsonObject persistence = object.value("persistence").toObject();
        if (persistence.size() != 2 || !unsignedValue(persistence.value("size"), decoded.persistenceSize)
            || decoded.persistenceSize == 0 || !persistence.value("ext").isDouble()) return false;
        decoded.persistenceExtVersion = persistence.value("ext").toInt(-1);
        if (persistence.value("ext").toDouble() != decoded.persistenceExtVersion) return false;
    }
    if (archive) {
        const QJsonObject value = object.value("archive").toObject();
        PrepareArchiveSource& source = decoded.archive;
        if (value.size() != 6 || !value.value("path").isString()
            || !value.value("modified").isString() || !value.value("changed").isString()) return false;
        source.path = value.value("path").toString();
        source.modified = value.value("modified").toString();
        source.changed = value.value("changed").toString();
        if (!unsignedValue(value.value("device"), source.device) || !unsignedValue(value.value("inode"), source.inode)
            || !unsignedValue(value.value("size"), source.size)) return false;
        if (!source.present()) return false;
    }
    if (!ValidatePrepareArchive(decoded)) return false;
    if (!ValidatePreparePersistence(decoded)) return false;
    if (isoEsp) {
        const QJsonValue value = object.value("isoEsp");
        if (!value.isBool() || !value.toBool()) return false;
        decoded.isoEsp = true;
        if (!ValidIsoEsp(decoded)) return false;
    }
    if (oldBios) {
        const QJsonValue value = object.value("oldBiosFixes");
        if (!value.isBool() || !value.toBool()) return false;
        decoded.oldBiosFixes = true;
        if (!ValidOldBiosFixes(decoded)) return false;
    }
    const bool appearanceFilesystem = decoded.fileSystem == FS_FAT16 || decoded.fileSystem == FS_FAT32
        || decoded.fileSystem == FS_NTFS || decoded.fileSystem == FS_EXFAT || decoded.fileSystem == FS_UDF;
    if (decoded.extendedLabel && (!device || raw || !appearanceFilesystem)) return false;
    if (!ValidatePrepareWindowsToGo(decoded)) return false;
    if (runtimeValidation) {
        const QJsonValue value = object.value("runtimeValidation");
        if (!device || !value.isBool() || !value.toBool()) return false;
        decoded.runtimeValidation = true;
        if (!ValidRuntimeValidation(decoded)) return false;
    }
    if (ntfsOptions) {
        const QJsonValue value = object.value("ntfsOptions"); const QJsonObject options = value.toObject();
        const bool format = device && decoded.fileSystem == FS_NTFS && !decoded.rawWrite && decoded.zeroMode == ZERO_NONE;
        if (!format || !value.isObject() || options.size() != 2 || !options.value("compression").isBool() || !options.value("indexing").isBool()) return false;
        decoded.ntfsCompression = options.value("compression").toBool(); decoded.fileIndexing = options.value("indexing").toBool();
        if (!decoded.ntfsCompression && !decoded.fileIndexing) return false;
    }
    if (udfIndexing) {
        const QJsonValue value = object.value("udfIndexing");
        const bool format = device && nativeDataFormat && decoded.fileSystem == FS_UDF && decoded.nonBootable;
        if (!format || !value.isBool() || !value.toBool() || ntfsOptions) return false;
        decoded.fileIndexing = true;
    }
    if ((nativeDataFormat || msDos || reactOs || kolibriOs || windowsFat16 || alternateSourceMbr)
        && EncodePrepareRequest(decoded) != object) return false;
    request = decoded;
    return true;
}

QJsonObject EncodeBadBlocksReport(const BadBlocksReport& report)
{
    return {{"event", "badblocks-report"}, {"round", report.round}, {"error", report.error},
        {"count", int(report.count)}, {"read", int(report.readErrors)}, {"write", int(report.writeErrors)},
        {"corrupt", int(report.corruptionErrors)}, {"written", QString::number(report.written)},
        {"readBytes", QString::number(report.read)}, {"threshold", report.threshold}};
}

bool DecodeBadBlocksReport(const QJsonObject& object, BadBlocksReport& report)
{
    if (object.size() != 10 || object.value("event") != "badblocks-report") return false;
    BadBlocksReport decoded;
    decoded.round = object.value("round").toInt(0);
    decoded.error = object.value("error").toInt(-1);
    const int count = object.value("count").toInt(-1), read = object.value("read").toInt(-1);
    const int write = object.value("write").toInt(-1), corrupt = object.value("corrupt").toInt(-1);
    const bool counters = count >= 0 && count <= BB_BAD_BLOCKS_THRESHOLD && read >= 0 && read <= count
        && write >= 0 && write <= count && corrupt >= 0 && corrupt <= count && read + write + corrupt == count;
    if (decoded.round <= 0 || decoded.error < 0 || !counters || !object.value("threshold").isBool()) return false;
    decoded.threshold = object.value("threshold").toBool();
    if (decoded.threshold != (count == BB_BAD_BLOCKS_THRESHOLD)) return false;
    if (!unsignedValue(object.value("written"), decoded.written) || !unsignedValue(object.value("readBytes"), decoded.read)) return false;
    decoded.count = quint32(count); decoded.readErrors = quint32(read);
    decoded.writeErrors = quint32(write); decoded.corruptionErrors = quint32(corrupt);
    report = decoded;
    return true;
}

QJsonObject EncodePrepareProgress(int operation, quint64 processed, quint64 total)
{
    return {{"event", "progress"}, {"operation", operation},
        {"processed", QString::number(processed)}, {"total", QString::number(total)}};
}

bool DecodePrepareProgress(const QJsonObject& object, int& operation, quint64& processed, quint64& total)
{
    if (object.size() != 4 || object.value("event") != "progress")
        return false;
    operation = object.value("operation").toInt(OP_MAX);
    const bool knownOperation = operation == OP_FILE_COPY || operation == OP_PATCH || operation == OP_BADBLOCKS
        || operation == OP_FORMAT || operation == OP_EXTRACT_ZIP;
    if (!knownOperation || !unsignedValue(object.value("processed"), processed))
        return false;
    return unsignedValue(object.value("total"), total) && processed <= total;
}

QJsonObject EncodeRawProgress(int phase, quint64 processed, quint64 total)
{
    return {{"event", "raw-progress"}, {"phase", phase},
        {"processed", QString::number(processed)}, {"total", QString::number(total)}};
}

bool DecodeRawProgress(const QJsonObject& object, int& phase, quint64& processed, quint64& total)
{
    if (object.size() != 4 || object.value("event") != "raw-progress")
        return false;
    phase = object.value("phase").toInt(-1);
    const bool knownPhase = phase >= RAW_WRITE_COPY && phase <= RAW_WRITE_VERIFY;
    if (!knownPhase || !unsignedValue(object.value("processed"), processed))
        return false;
    return unsignedValue(object.value("total"), total) && processed <= total;
}

bool DecodeRawOutcome(const QJsonObject& object, PrepareImageOutcome& outcome)
{
    if (object.size() != 7 || object.value("event") != "raw-result")
        return false;
    PrepareImageOutcome decoded;
    decoded.rawWrite = true;
    decoded.error = object.value("error").toInt(-1);
    decoded.syncError = object.value("syncError").toInt(-1);
    const int phase = object.value("phase").toInt(-1);
    const bool validErrors = decoded.error >= 0 && decoded.syncError >= 0;
    const bool validPhase = phase >= RAW_WRITE_PREPARE && phase <= RAW_WRITE_DONE;
    if (!validErrors || !validPhase)
        return false;
    quint64 written = 0, verified = 0;
    if (!unsignedValue(object.value("written"), written)
        || !unsignedValue(object.value("verified"), verified))
        return false;
    decoded.raw.bytes_written = written;
    decoded.raw.bytes_verified = verified;
    if (decoded.raw.bytes_verified > decoded.raw.bytes_written)
        return false;
    decoded.raw.phase = RAW_WRITE_PHASE(phase);
    decoded.raw.sync_error = decoded.syncError;
    if (!object.value("started").isBool())
        return false;
    decoded.started = object.value("started").toBool();
    if (decoded.raw.bytes_written != 0 && !decoded.started)
        return false;
    outcome = decoded;
    return true;
}

QJsonObject EncodePrepareOutcome(const PrepareImageOutcome& outcome)
{
    if (outcome.rawWrite)
        return {{"event", "raw-result"}, {"error", outcome.error}, {"syncError", outcome.raw.sync_error},
            {"started", outcome.started || outcome.raw.bytes_written != 0},
            {"phase", int(outcome.raw.phase)}, {"written", QString::number(outcome.raw.bytes_written)},
            {"verified", QString::number(outcome.raw.bytes_verified)}};
    QJsonObject object = {{"event", "result"}, {"error", outcome.error}, {"syncError", outcome.syncError}, {"cleanupError", outcome.cleanupError},
        {"started", outcome.started}, {"offset", QString::number(outcome.offset)},
        {"size", QString::number(outcome.size)}, {"filesystemBytes", QString::number(outcome.filesystemBytes)},
        {"copiedBytes", QString::number(outcome.copiedBytes)}};
    if (outcome.appearanceDone) object.insert("autorun", true);
    if (outcome.windowsToGoDone) object.insert("toGo", true);
    if (outcome.archiveDone || outcome.archiveBytes != 0)
        object.insert("archive", QJsonObject{{"done", outcome.archiveDone}, {"bytes", QString::number(outcome.archiveBytes)}});
    return object;
}

bool DecodePrepareOutcome(const QJsonObject& object, PrepareImageOutcome& outcome)
{
    PrepareImageOutcome decoded;
    const bool appearance = object.contains("autorun");
    const bool archive = object.contains("archive");
    const bool toGo = object.contains("toGo");
    if (object.size() != 9 + int(appearance) + int(archive) + int(toGo) || object.value("event") != "result")
        return false;
    if (appearance)
    {
        if (object.value("autorun") != QJsonValue(true)) return false;
        decoded.appearanceDone = true;
    }
    if (toGo)
    {
        if (object.value("toGo") != QJsonValue(true)) return false;
        decoded.windowsToGoDone = true;
    }
    if (archive) {
        const QJsonObject value = object.value("archive").toObject();
        if (value.size() != 2 || !value.value("done").isBool()
            || !unsignedValue(value.value("bytes"), decoded.archiveBytes)) return false;
        decoded.archiveDone = value.value("done").toBool();
    }
    decoded.error = object.value("error").toInt(-1);
    decoded.syncError = object.value("syncError").toInt(-1);
    decoded.cleanupError = object.value("cleanupError").toInt(-1);
    const bool validErrors = decoded.error >= 0 && decoded.syncError >= 0 && decoded.cleanupError >= 0;
    if (!validErrors || !object.value("started").isBool())
        return false;
    decoded.started = object.value("started").toBool();
    if (decoded.windowsToGoDone && (!decoded.started || decoded.error != 0
        || decoded.syncError != 0 || decoded.cleanupError != 0)) return false;
    if (!unsignedValue(object.value("offset"), decoded.offset))
        return false;
    if (!unsignedValue(object.value("size"), decoded.size))
        return false;
    if (!unsignedValue(object.value("filesystemBytes"), decoded.filesystemBytes))
        return false;
    if (!unsignedValue(object.value("copiedBytes"), decoded.copiedBytes))
        return false;
    outcome = decoded;
    return true;
}
QJsonObject EncodePrepareRequest(const PrepareImageRequest& request)
{
    if (request.jobPriority < MinimumJobPriority || request.jobPriority > MaximumJobPriority) return {};
    if (!request.exclusions.valid()) return {};
    const bool operation = request.deviceOperation != 0;
    if (operation && (request.deviceOperation < 1 || request.deviceOperation > 2 || !request.isDevice
        || !request.nonBootable || request.rawWrite || !request.imagePath.isEmpty()
        || request.archive.present() || request.badBlocksPattern >= 0 || request.espRecords.size() > 8)) return {};
    for (const QString& guid : request.espRecords) if (QUuid(guid).isNull() || QUuid(guid).toString() != guid) return {};
    QJsonObject object = encodePrepareContent(request);
    if (!object.isEmpty() && operation) object.insert("deviceOperation", QJsonObject{{"command", request.deviceOperation}, {"esp", QJsonArray::fromStringList(request.espRecords)}});
    if (!object.isEmpty() && (!request.lockDevice || !request.exclusions.empty())) {
        QJsonArray usb, disks;
        for (quint32 id : request.exclusions.usb) usb.append(QString::number(id));
        for (const QString& id : request.exclusions.disks) disks.append(id);
        object.insert("devicePolicy", QJsonObject{{"lock", request.lockDevice}, {"usb", usb}, {"disks", disks}});
    }
    if (!object.isEmpty() && request.jobPriority != DefaultJobPriority) object.insert("jobPriority", request.jobPriority);
    return object;
}

bool DecodePrepareRequest(const QJsonObject& object, PrepareImageRequest& request)
{
    QJsonObject content = object;
    int priority = DefaultJobPriority;
    if (content.contains("jobPriority")) {
        const auto value = content.take("jobPriority");
        if (!value.isDouble() || value.toDouble() < MinimumJobPriority || value.toDouble() > MaximumJobPriority
            || value.toDouble() != double(value.toInt())) return false;
        priority = value.toInt();
    }
    int operation = 0; QStringList espRecords;
    if (content.contains("deviceOperation")) {
        const QJsonValue value = content.take("deviceOperation");
        if (!value.isObject()) return false;
        const auto command = value.toObject();
        if (command.size() != 2 || !command.value("command").isDouble() || !command.value("esp").isArray()) return false;
        const double number = command.value("command").toDouble();
        if (number != 1 && number != 2) return false;
        operation = int(number);
        for (const auto guid : command.value("esp").toArray()) {
            if (!guid.isString()) return false;
            espRecords.append(guid.toString());
        }
    }
    bool lockDevice = true;
    DeviceExclusions exclusions;
    if (content.contains("devicePolicy")) {
        const QJsonValue policyValue = content.take("devicePolicy");
        if (!policyValue.isObject()) return false;
        const QJsonObject policy = policyValue.toObject();
        if (policy.size() != 3 || !policy.value("lock").isBool()
            || !policy.value("usb").isArray() || !policy.value("disks").isArray()) return false;
        lockDevice = policy.value("lock").toBool();
        for (const auto id : policy.value("usb").toArray()) {
            if (!id.isString()) return false;
            bool valid = false; const quint64 number = id.toString().toULongLong(&valid);
            if (!valid || number > UINT32_MAX || QString::number(number) != id.toString()) return false;
            exclusions.usb.append(quint32(number));
        }
        for (const auto id : policy.value("disks").toArray()) {
            if (!id.isString()) return false;
            exclusions.disks.append(id.toString());
        }
        if (!exclusions.valid()) return false;
    }
    if (!decodePrepareContent(content, request)) return false;
    request.deviceOperation = operation;
    request.espRecords = espRecords;
    request.lockDevice = lockDevice;
    request.exclusions = exclusions;
    request.jobPriority = priority;
    return !EncodePrepareRequest(request).isEmpty();
}

}
