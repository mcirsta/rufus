// SPDX-License-Identifier: GPL-3.0-or-later
#include "format_options.h"
#include "qt/prepare_image_protocol.h"
#include <cerrno>
#include "fat_format.h"
#include "ntfs_format.h"
#include "exfat_format.h"
#include "ext_format.h"
#include "udf_format.h"
#include "partition_image.h"
#include <QVector>
#include <cstring>

namespace rufus
{
namespace
{
constexpr quint32 sectorsPerTrack = 63;
constexpr quint64 minimumExtDiskBytes = 256 * 1024 * 1024; // Win32 MIN_EXT_SIZE.
}

MBR_BOOT_CODE FormatMbrBootCode(const PrepareImageRequest& request)
{
    // Requests currently select one validated provider. This supplies its facts
    // to the common decision; it does not admit mixed image reports. Final WinPE
    // masquerading still follows the freshly checked shared setup plan.
    RUFUS_IMG_REPORT report = {};
    RUFUS_BOOT_OPTIONS options = {BT_NON_BOOTABLE, request.fileSystem, TT_BIOS,
        false, request.enhancedInstall, false, false};
    if (request.windowsBios || request.windowsToGo)
    {
        options.boot_type = BT_IMAGE;
        options.use_rufus_mbr = request.windowsRufusMbr;
        report.has_bootmgr = 1;
        if (request.windowsToGo) options.target_type = request.windowsToGoTarget;
    }
    else if (request.isDos())
    {
        options.boot_type = request.dosBootType();
        options.use_rufus_mbr = request.dosRufusMbr;
    }
    else if (request.reactOs)
        options.boot_type = BT_REACTOS;
    else if (request.kolibriOs)
    {
        options.boot_type = BT_IMAGE;
        report.has_kolibrios = 1;
    }
    else if (request.grub2)
    {
        options.boot_type = request.imagePath.isEmpty() ? BT_GRUB2 : BT_IMAGE;
        report.has_grub2 = 1;
    }
    else if (request.grub4dos)
    {
        options.boot_type = request.imagePath.isEmpty() ? BT_GRUB4DOS : BT_IMAGE;
        report.has_grub4dos = 1;
    }
    else if (request.syslinux)
    {
        options.boot_type = request.syslinuxSelection;
        report.has_syslinux = 1;
    }
    switch (GetMasterBootRecord(&report, &options))
    {
    case RUFUS_MBR_REACTOS: return MBR_BOOT_REACTOS;
    case RUFUS_MBR_RUFUS: return MBR_BOOT_RUFUS;
    case RUFUS_MBR_WIN7: return MBR_BOOT_WIN7;
    case RUFUS_MBR_GRUB2: return MBR_BOOT_GRUB2;
    case RUFUS_MBR_GRUB4DOS: return MBR_BOOT_GRUB4DOS;
    case RUFUS_MBR_SYSLINUX: return MBR_BOOT_SYSLINUX;
    case RUFUS_MBR_KOLIBRIOS: return MBR_BOOT_KOLIBRIOS;
    default: return MBR_BOOT_ZERO;
    }
}

quint32 FormatLayoutClusterSize(const PrepareImageRequest& request)
{
    const bool defaultGeometry = request.fileSystem == FS_UDF || request.fileSystem == FS_EXT2 || request.fileSystem == FS_EXT3;
    return defaultGeometry ? request.logicalSectorSize : request.clusterSize;
}

int PlanFormatPartition(const PrepareImageRequest& request, GPT_IMAGE_RESULT& partition)
{
    partition = {};
    if (!ValidFormatGeometry(request)) return ENOTSUP;
    if (request.sfd)
    {
        if (request.persistenceSize != 0)
            return EINVAL;
        const SFD_IMAGE_OPTIONS layout = {request.size, request.logicalSectorSize, 0, request.physicalSectorSize};
        SFD_IMAGE_RESULT result = {};
        const int error = PlanSfdImageLayout(&layout, &result);
        partition.offset = result.offset;
        partition.size = result.size;
        return error;
    }
    const quint64 bootBytes = request.usesUefiNtfs() ? quint64(RUFUS_UEFI_NTFS_BYTES) : 0;
    if (request.mbr || request.syslinux)
    {
        const MBR_IMAGE_OPTIONS layout = {request.size, request.logicalSectorSize, sectorsPerTrack,
            request.clusterSize, 0, 0, request.fileSystem, FormatMbrBootCode(request), bootBytes, request.persistenceSize, request.oldBiosFixes, false, false, request.physicalSectorSize};
        MBR_IMAGE_RESULT result = {};
        const int error = PlanMbrImageLayout(&layout, &result);
        partition.offset = result.offset;
        partition.size = result.size;
        partition.uefi_ntfs_offset = result.uefi_ntfs_offset;
        partition.uefi_ntfs_size = result.uefi_ntfs_size;
        partition.persistence_offset = result.persistence_offset;
        partition.persistence_size = result.persistence_size;
        return error;
    }
    const GPT_IMAGE_OPTIONS layout = {request.size, request.logicalSectorSize, sectorsPerTrack,
        FormatLayoutClusterSize(request), 0, bootBytes, request.persistenceSize, request.isoEsp, request.windowsToGo, request.physicalSectorSize};
    return PlanGptImageLayout(&layout, &partition);
}

quint32 FormatFatClusterSize(const PrepareImageRequest& request, const GPT_IMAGE_RESULT& partition)
{
    if (!request.isoEsp) return request.clusterSize;
    return request.fileSystem == FS_FAT16 ? GetFat16ClusterSizes(partition.size, request.logicalSectorSize).Default
        : GetLargeFat32DefaultClusterSize(partition.size);
}

int CheckFormatSettings(const PrepareImageRequest& request, const GPT_IMAGE_RESULT& partition)
{
    if (!ValidFormatGeometry(request)) return ENOTSUP;
    if (request.persistenceSize != 0)
    {
        const bool version = request.persistenceExtVersion >= 2 && request.persistenceExtVersion <= 4;
        if (!version || partition.persistence_size < request.persistenceSize)
            return EINVAL;
        const ExtFormatOptions persistence = {partition.persistence_size,
            FS_EXT2 + request.persistenceExtVersion - 2, QStringLiteral("persistence"), true, request.quickFormat, request.logicalSectorSize, request.physicalSectorSize};
        const int error = CheckExtFormatOptions(persistence);
        if (error != 0) return error;
    }
    if (request.fileSystem == FS_EXT2 || request.fileSystem == FS_EXT3)
    {
        if (request.size < minimumExtDiskBytes) return ENOTSUP;
        const bool dataFormat = request.nonBootable && request.isSourceFree();
        const bool grubFormat = request.grub2 && request.mbr;
        if ((!dataFormat && !grubFormat) || request.clusterSize != RUFUS_EXT_DEFAULT_CLUSTER || request.volumeId != 0)
            return ENOTSUP;
        const ExtFormatOptions format = {partition.size, request.fileSystem, request.label, false, request.quickFormat, request.logicalSectorSize, request.physicalSectorSize};
        return CheckExtFormatOptions(format);
    }
    if (request.fileSystem == FS_UDF) {
        if (!request.nonBootable || !request.isSourceFree() || request.clusterSize != UdfDefaultCluster || request.volumeId != 0) return ENOTSUP;
        return CheckUdfFormatOptions({partition.size, request.label, request.udfRevision, request.quickFormat, request.logicalSectorSize, request.physicalSectorSize});
    }
    if (request.fileSystem == FS_NTFS)
    {
        const NtfsFormatOptions format = {partition.size, partition.offset, request.clusterSize, request.label, request.quickFormat, request.ntfsCompression, request.fileIndexing, request.logicalSectorSize, request.physicalSectorSize};
        return CheckNtfsFormatOptions(format);
    }
    if (request.fileSystem == FS_EXFAT)
    {
        const ExfatFormatOptions format = {partition.size, request.clusterSize, request.label, partition.offset, request.quickFormat, request.logicalSectorSize, request.physicalSectorSize};
        return CheckExfatFormatOptions(format);
    }
    if (request.fileSystem != FS_FAT16 && request.fileSystem != FS_FAT32)
        return ENOTSUP;
    const QByteArray label = request.label.toUtf8();
    if (label.size() > FAT32_LABEL_LENGTH || label.contains('\0'))
        return EINVAL;
    const quint32 cluster = FormatFatClusterSize(request, partition);
    if (request.msDos && request.sizeChecks && cluster >= 65536) return EINVAL;
    FAT_FORMAT_OPTIONS format = {partition.offset, partition.size, cluster, request.volumeId, {0}, false, 0, 0};
    format.sector_size = request.logicalSectorSize;
    format.physical_sector_size = request.physicalSectorSize;
    memcpy(format.label, label.constData(), size_t(label.size()));
    return request.fileSystem == FS_FAT16 ? CheckFat16FormatOptions(&format) : CheckFat32FormatOptions(&format);
}

QString NormalizeFormatLabel(const QString& text, quint64 diskSize, bool ntfs)
{
    // Preserve Windows UTF-16 semantics, including two units for a non-BMP
    // character. Do not reinterpret QString's mutable QChar storage.
    QVector<uint16_t> source(text.size() + 1);
    memcpy(source.data(), text.utf16(), size_t(text.size()) * sizeof(uint16_t));
    source.last() = 0;
    uint16_t normalized[VOLUME_LABEL_CAPACITY];
    NormalizeVolumeLabel(source.constData(), normalized, !ntfs, diskSize);
    QString result;
    result.reserve(VOLUME_LABEL_CAPACITY - 1);
    for (size_t i = 0; normalized[i] != 0; ++i)
        result.append(QChar(normalized[i]));
    // FAT stores the label padded with spaces. Win32 refreshes the effective
    // label after formatting; use that spelling for Linux preflight/config
    // patches too. Leave invalid leading-space labels for validation to reject.
    if (!ntfs && !result.startsWith(QLatin1Char(' ')))
        while (result.endsWith(QLatin1Char(' ')))
            result.chop(1);
    return result;
}

RUFUS_CLUSTER_SIZES NativeClusterSizes(quint64 diskSize, bool ntfs, bool syslinux)
{
    return NativeFileSystemClusterSizes(diskSize, ntfs ? FS_NTFS : FS_FAT32, syslinux);
}

RUFUS_CLUSTER_SIZES NativeFileSystemClusterSizes(quint64 diskSize, int fileSystem, bool syslinux,
    int bootType, PartitionScheme scheme, bool imageUefiNtfs, MBR_BOOT_CODE windowsBoot, bool oldBiosFixes,
    bool windowsToGo, int windowsTarget, bool sizeChecks, quint32 logicalSectorSize, quint32 physicalSectorSize)
{
    const quint32 sectorBytes = logicalSectorSize;
    if (windowsToGo)
    {
        const bool schemeSupported = scheme == PartitionScheme::Mbr || scheme == PartitionScheme::Gpt;
        if (!schemeSupported || fileSystem != FS_NTFS || bootType != BT_IMAGE || syslinux) return {};
        RUFUS_CLUSTER_SIZES sizes = GetNtfsClusterSizes(diskSize, sectorBytes);
        quint32 candidates = sizes.Allowed;
        while (candidates != 0)
        {
            const quint32 cluster = candidates & (0u - candidates);
            candidates &= ~cluster;
            PrepareImageRequest request;
            request.size = diskSize;
            request.logicalSectorSize = logicalSectorSize;
            request.physicalSectorSize = physicalSectorSize;
            request.clusterSize = cluster;
            request.fileSystem = FS_NTFS;
            request.windowsToGo = true;
            request.windowsToGoTarget = windowsTarget;
            request.mbr = scheme == PartitionScheme::Mbr;
            request.uefiNtfs = request.mbr && imageUefiNtfs;
            request.oldBiosFixes = oldBiosFixes;
            GPT_IMAGE_RESULT partition = {};
            int error = PlanFormatPartition(request, partition);
            if (error == 0) error = CheckFormatSettings(request, partition);
            if (error != 0) sizes.Allowed &= ~cluster;
        }
        if (bootType == BT_MSDOS && sizeChecks) sizes.Allowed &= 65535;
        if (!(sizes.Default & sizes.Allowed)) sizes.Default = sizes.Allowed & (0u - sizes.Allowed);
        return sizes;
    }
    const bool nonBootable = bootType == BT_NON_BOOTABLE;
    const bool standalone = bootType == BT_UEFI_NTFS;
    const bool freeDos = bootType == BT_FREEDOS || bootType == BT_MSDOS || bootType == BT_REACTOS;
    const bool grub2 = bootType == BT_GRUB2;
    const bool grub4dos = bootType == BT_GRUB4DOS;
    const bool standaloneSyslinux = bootType == BT_SYSLINUX_V4 || bootType == BT_SYSLINUX_V6;
    const bool sourceFree = nonBootable || standalone || freeDos || grub2 || grub4dos || standaloneSyslinux;
    if (!sourceFree && bootType != BT_IMAGE)
        return {};
    const bool fat = fileSystem == FS_FAT16 || fileSystem == FS_FAT32;
    const bool syslinuxNtfs = (bootType == BT_IMAGE || bootType == BT_SYSLINUX_V6) && fileSystem == FS_NTFS;
    const bool syslinuxFileSystem = fat || syslinuxNtfs;
    const bool syslinuxBootType = bootType == BT_IMAGE || standaloneSyslinux;
    if (syslinux && (!syslinuxFileSystem || !syslinuxBootType))
        return {};
    const bool ntfsSyslinuxImage = syslinux && bootType == BT_IMAGE && fileSystem == FS_NTFS;
    const bool mbr = scheme == PartitionScheme::Mbr;
    const bool windowsImage = windowsBoot == MBR_BOOT_RUFUS || windowsBoot == MBR_BOOT_WIN7;
    if (windowsBoot != MBR_BOOT_ZERO && !windowsImage)
        return {};
    const bool windowsFilesystem = fileSystem == FS_NTFS || fileSystem == FS_FAT16 || fileSystem == FS_FAT32;
    const bool windowsLayout = bootType == BT_IMAGE && mbr && !syslinux && windowsFilesystem;
    if (windowsImage && !windowsLayout)
        return {};
    if (windowsImage && imageUefiNtfs && fileSystem != FS_NTFS)
        return {};
    const bool grub2ImageFileSystem = fat || fileSystem == FS_NTFS;
    const bool grub2Image = bootType == BT_IMAGE && mbr && !syslinux && !windowsImage && grub2ImageFileSystem;
    const bool ntfsGrub2Image = grub2Image && fileSystem == FS_NTFS;
    if (imageUefiNtfs && !ntfsSyslinuxImage && !ntfsGrub2Image && !windowsImage)
        return {};
    const bool sfd = scheme == PartitionScheme::Sfd;
    const bool ext = fileSystem == FS_EXT2 || fileSystem == FS_EXT3;
    if (ext && !grub2 && !nonBootable)
        return {};
    if (fileSystem == FS_UDF && !nonBootable) return {};
    const bool grub2FileSystem = fat || fileSystem == FS_NTFS || ext;
    if (freeDos && (!fat || !mbr))
        return {};
    const bool grub4dosFileSystem = fat || fileSystem == FS_NTFS;
    if (grub4dos && (!grub4dosFileSystem || !mbr))
        return {};
    if (grub2 && (!grub2FileSystem || !mbr))
        return {};
    if (standaloneSyslinux && (!syslinuxFileSystem || !mbr))
        return {};
    if (standaloneSyslinux && !syslinux)
        return {};
    const bool supportedScheme = mbr || sfd || scheme == PartitionScheme::Gpt;
    // Image eligibility is checked at START; this function checks geometry.
    const bool unsupportedBootLayout = sfd && !nonBootable;
    if (!supportedScheme || unsupportedBootLayout)
        return {};
    RUFUS_CLUSTER_SIZES sizes;
    switch (fileSystem)
    {
    case FS_FAT16: sizes = GetFat16ClusterSizes(diskSize, sectorBytes); break;
    case FS_FAT32: sizes = GetFat32ClusterSizes(diskSize, sectorBytes); break;
    case FS_NTFS: sizes = GetNtfsClusterSizes(diskSize, sectorBytes); break;
    case FS_EXFAT: sizes = GetExfatClusterSizes(diskSize, sectorBytes); break;
    case FS_EXT2:
    case FS_EXT3: sizes = {RUFUS_EXT_DEFAULT_CLUSTER, RUFUS_EXT_DEFAULT_CLUSTER}; break;
    case FS_UDF: sizes = {UdfDefaultCluster, UdfDefaultCluster}; break;
    default: return {};
    }
    quint32 candidates = sizes.Allowed;
    while (candidates != 0)
    {
        const quint32 cluster = candidates & (0u - candidates);
        candidates &= ~cluster;
        PrepareImageRequest request;
        request.size = diskSize;
        request.logicalSectorSize = logicalSectorSize;
        request.physicalSectorSize = physicalSectorSize;
        request.oldBiosFixes = oldBiosFixes;
        request.clusterSize = cluster;
        request.fileSystem = fileSystem;
        request.syslinux = syslinux;
        request.syslinuxSelection = standaloneSyslinux ? bootType : BT_IMAGE;
        request.nonBootable = nonBootable;
        request.uefiNtfs = standalone || imageUefiNtfs;
        request.freeDos = bootType == BT_FREEDOS;
        request.msDos = bootType == BT_MSDOS;
        request.sizeChecks = sizeChecks;
        request.reactOs = bootType == BT_REACTOS;
        request.grub2 = grub2 || grub2Image;
        request.grub4dos = grub4dos;
        request.windowsBios = windowsImage;
        request.windowsRufusMbr = !windowsImage || windowsBoot == MBR_BOOT_RUFUS;
        request.mbr = mbr && !syslinux; // Syslinux requests already imply MBR.
        request.sfd = sfd;
        GPT_IMAGE_RESULT partition = {};
        int error = PlanFormatPartition(request, partition);
        if (error == 0)
            error = CheckFormatSettings(request, partition);
        if (error != 0)
            sizes.Allowed &= ~cluster;
    }
    if (bootType == BT_MSDOS && sizeChecks) sizes.Allowed &= 65535;
    if ((sizes.Default & sizes.Allowed) == 0)
        sizes.Default = sizes.Allowed & (0u - sizes.Allowed);
    return sizes;
}
}
