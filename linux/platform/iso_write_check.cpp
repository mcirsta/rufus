#include "efi_architecture.h"
// SPDX-License-Identifier: GPL-3.0-or-later
#include "iso_write_check.h"
#include "ntfs_format.h"
#include "media_integrity.h"
#include "format_options.h"
#include "fat_format.h"
#include "format_policy.h"
#include "prepare_source.h"
#include "iso_file.h"
#include "image_file.h"
#include "embedded_efi_files.h"
#include "pe_arch.h"
#include "windows_customization.h"
#include "syslinux_source.h"
#include "syslinux_native.h"
#include "rufus_reporting.h"
#include "wim_read_file.h"
#include "config_patch.h"
#include "winpe_scan.h"
#include "winpe_setup.h"
#include "legacy_iso.h"

extern "C" {
#include "native_input.h"
}

#include <QFile>
#include <QtEndian>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

namespace rufus
{
namespace
{
constexpr quint32 maxPeHeaderBytes = 1024 * 1024;
constexpr quint32 dosHeaderBytes = 64;
constexpr quint32 peOffsetField = 60;
constexpr quint32 peCoffBytes = 24;
constexpr quint32 optionalSizeField = 20;
constexpr quint32 subsystemField = 68;
constexpr quint32 pe64OptionalBytes = 112;
constexpr quint16 pe32PlusMagic = 0x20b;
constexpr quint16 efiApplication = 10;

struct IsoView
{
    ISO_ANALYSIS_SOURCE source = {};
    ~IsoView() { CloseIsoAnalysisSource(&source); }
};

struct IsoReader
{
    ISO_FILE_READER file = {};
    ~IsoReader() { CloseIsoFileReader(&file); }
};

struct WinPeFile
{
    QByteArray wanted, source;
    const char* destination;
    QByteArray existing = {};
};

enum WinPeFileIndex { WinPeDetector, WinPeCopiedSif, WinPeLoader, WinPeOptionsSif, WinPeFileCount };

struct WinPeEntries
{
    WinPeFile files[WinPeFileCount];
    QByteArray directories[3];
    bool seenDirectory[3] = {};
    int error = 0;
};

int visitWinPeEntry(const ISO_TREE_ENTRY* entry, void* data)
{
    WinPeEntries& entries = *static_cast<WinPeEntries*>(data);
    if (entry->kind == ISO_TREE_DIRECTORY_END)
        return ISO_SCAN_OK;
    for (WinPeFile& file : entries.files)
    {
        const bool rootCollision = entry->parent[0] == 0 && file.destination != nullptr
            && strcasecmp(entry->name, file.destination) == 0;
        if (rootCollision)
        {
            const bool supportedRoot = &file == &entries.files[WinPeDetector]
                || &file == &entries.files[WinPeCopiedSif] || &file == &entries.files[WinPeLoader];
            if (!supportedRoot || !file.existing.isEmpty())
                entries.error = EEXIST;
            else if (entry->kind != ISO_TREE_FILE)
                entries.error = EINVAL;
            else
                file.existing = entry->path;
        }
        if (!file.wanted.isEmpty() && strcasecmp(entry->path, file.wanted.constData()) == 0)
        {
            if (entry->kind != ISO_TREE_FILE)
                entries.error = EINVAL;
            else if (!file.source.isEmpty())
                entries.error = EEXIST;
            else
                file.source = entry->path; // Preserve the selected view's spelling.
        }
    }
    for (size_t i = 0; i < sizeof(entries.directories) / sizeof(entries.directories[0]); ++i)
    {
        if (strcasecmp(entry->path, entries.directories[i].constData()) != 0)
            continue;
        if (entry->kind != ISO_TREE_DIRECTORY)
            entries.error = ENOTDIR;
        else if (entries.seenDirectory[i])
            entries.error = EEXIST;
        else
            entries.seenDirectory[i] = true;
    }
    return entries.error == 0 ? ISO_SCAN_OK : ISO_SCAN_ERROR;
}

int readWinPeFile(const ISO_ANALYSIS_SOURCE& source, const QByteArray& path, quint64 limit,
    QByteArray& bytes)
{
    if (path.isEmpty())
        return ENOENT;
    IsoReader reader;
    const int opened = OpenIsoFileReader(&reader.file, source.iso, source.udf, path.constData());
    if (opened != ISO_SCAN_OK)
        return opened == ISO_SCAN_CANCELLED ? ECANCELED : EIO;
    if (reader.file.size == 0)
        return EBADMSG;
    if (reader.file.size > limit)
        return EFBIG;
    bytes.resize(int(reader.file.size));
    const int read = ReadIsoFileRange(&reader.file, 0, bytes.data(), size_t(bytes.size()));
    if (read != ISO_SCAN_OK)
        return read == ISO_SCAN_CANCELLED ? ECANCELED : EIO;
    return 0;
}

int checkWinPeFile(const ISO_ANALYSIS_SOURCE& source, const WinPeFile& file, WinPeFileIndex index,
    const WINPE_SETUP_PLAN& plan, bool usesMinint, quint64& generatedBytes)
{
    const quint64 limit = index == WinPeOptionsSif ? WINPE_OPTIONS_MAX_SIZE : CONFIG_PATCH_MAX_BYTES;
    QByteArray bytes, existing;
    int error = readWinPeFile(source, file.source, limit, bytes);
    if (error != 0)
        return error;
    constexpr qsizetype excludedLoaderTail = 32; // Shared PatchWinPeLoader scan tail.
    if (index == WinPeLoader && bytes.size() <= excludedLoaderTail)
        return EBADMSG;
    if (!file.existing.isEmpty())
    {
        error = readWinPeFile(source, file.existing, limit, existing);
        if (error != 0)
            return error;
        if (index == WinPeCopiedSif)
            bytes.swap(existing); // SetupWinPE edits the existing root input itself.
    }
    if (index == WinPeCopiedSif)
    {
        QByteArray edited;
        error = InsertConfigSectionText(bytes, WINPE_SETUP_SECTION, WINPE_SETUP_SOURCE_DEVICE, edited);
        if (error == 0)
            bytes.swap(edited);
    }
    else if (index == WinPeLoader)
    {
        const bool patch = plan.action == WINPE_PATCH_LOADER;
        if (patch && !PatchWinPeLoader(bytes.data(), uint32_t(bytes.size()), plan.index, usesMinint))
            error = EBADMSG;
        if (error == 0 && !file.existing.isEmpty())
        {
            if (patch && !PatchWinPeLoader(existing.data(), uint32_t(existing.size()), plan.index, usesMinint))
                error = EBADMSG;
            // Match preparation's one original pass per input, including scan skips.
            if (error == 0 && bytes != existing)
                error = EEXIST;
            if (error == 0)
                bytes.swap(existing);
        }
    }
    else if (index == WinPeOptionsSif)
    {
        RUFUS_IMG_REPORT inspected = {};
        const size_t size = size_t(bytes.size());
        bytes.append('\0');
        const int inspectedStatus = InspectWinPeOptions(bytes.data(), size, &inspected);
        const bool matchingOptions = inspectedStatus == ISO_SCAN_OK && inspected.winpe_options_known
            && bool(inspected.uses_minint) == usesMinint;
        if (!matchingOptions)
            error = ENOTSUP;
    }
    const bool editsExistingLoader = index == WinPeLoader && plan.action == WINPE_PATCH_LOADER;
    const bool stagesOutput = file.destination != nullptr
        && (file.existing.isEmpty() || index == WinPeCopiedSif || editsExistingLoader);
    if (error == 0 && stagesOutput)
        // Atomic root replacement needs the full edited file alongside the original.
        generatedBytes += quint64(bytes.size()); // Three bounded outputs; no overflow.
    return error;
}

int checkWinPeSource(const PrepareImageRequest& request, const ISO_ANALYSIS_SOURCE& source,
    IsoWriteCheck& result, quint64& generatedBytes, bool allowUnselectedCore)
{
    const ISO_ANALYSIS& analysis = result.analysis;
    const RUFUS_IMG_REPORT& report = analysis.report;
    const quint32 allowedPending = ISO_PENDING_WINDOWS | ISO_PENDING_SECURE_BOOT | ISO_PENDING_EMBEDDED_EFI;
    const bool incomplete = !analysis.tree_complete || (analysis.pending & ~allowedPending) != 0;
    const WINPE_SETUP_PLAN plan = GetWinPeSetupPlan(report.winpe, report.uses_minint);
    const RUFUS_FS_OPTIONS selection = {BT_IMAGE, TT_BIOS, true, false, request.enhancedInstall};
    bool forcedFat = false;
    const quint32 filesystems = GetAllowedFileSystems(&report, &selection, &forcedFat);
    const RUFUS_PARTITION_OPTIONS partitions = {BT_IMAGE, request.fileSystem, TT_BIOS,
        false, false, false, request.enhancedInstall, false, 0};
    result.extraPartitions = GetExtraPartitions(&report, &partitions);
    const bool fat = request.fileSystem == FS_FAT16 || request.fileSystem == FS_FAT32;
    const bool fatIncompatible = fat && !IsFat32Compatible(&report, request.enhancedInstall);
    const bool sbrFilesystem = request.fileSystem == FS_NTFS || fat;
    const bool reactosPrimary = request.syslinux && HAS_REACTOS(report) && !HAS_SYSLINUX(report);
    const bool coreLookup = allowUnselectedCore && sbrFilesystem
        && (reactosPrimary || (request.windowsBios && request.enhancedInstall && !request.usesUefiNtfs()));
    const bool allowGrub2Sbr = request.hasGrub2Core() || coreLookup;
    const RUFUS_BOOT_OPTIONS boot = {BT_IMAGE, request.fileSystem, TT_BIOS, false,
        request.enhancedInstall, false, false};
    const bool syslinux = GetBootInstallType(&report, &boot) == RUFUS_BOOT_INSTALL_SYSLINUX;
    const int secondary = GetSecondaryBootType(BT_IMAGE, &report, false);
    const bool selectedPrimary = syslinux ? request.syslinux
        : request.windowsBios || request.kolibriOs || (secondary == BT_GRUB2 && request.grub2)
            || (secondary == BT_GRUB4DOS && request.grub4dos);
    const bool selectedSecondary = secondary == BT_IMAGE || request.grub2 || request.grub4dos
        || (request.windowsBios && request.enhancedInstall
            && (allowGrub2Sbr || secondary == BT_GRUB4DOS))
        || (reactosPrimary && ((secondary == BT_GRUB2 && allowGrub2Sbr) || secondary == BT_GRUB4DOS));
    const bool supportedLayout = HAS_WINPE(report) && report.winpe_options_known
        && plan.action != WINPE_UNSUPPORTED_LAYOUT;
    const quint8 requestedPartitions = request.usesUefiNtfs() ? XP_UEFI_NTFS : 0;
    if (incomplete)
        result.problem = QStringLiteral("Image analysis is incomplete; legacy WinPE preparation cannot proceed.");
    else if (!report.is_iso || report.disable_iso)
        result.problem = QStringLiteral("Rufus image policy does not allow ISO mode for this image.");
    else if (!supportedLayout || !selectedPrimary || !selectedSecondary)
        result.problem = QStringLiteral("Legacy WinPE requires a complete setup layout, readable options and its original selected boot loader.");
    else if (HAS_KOLIBRIOS(report) && !syslinux && request.fileSystem == FS_FAT16)
        result.problem = QStringLiteral("The original KolibriOS partition boot record does not support FAT16.");
    else if (!(filesystems & FS_MASK(request.fileSystem)) || fatIncompatible
        || result.extraPartitions != requestedPartitions)
        result.problem = QStringLiteral("This legacy image requires a filesystem or partition layout that is not available yet.");
    if (!result.problem.isEmpty())
        return ENOTSUP;

    // The original options scan prefers i386; root preparation prefers amd64.
    // Validate both selected SIFs when they differ, without changing that policy.
    const char* optionsDirectory = (report.winpe & WINPE_I386) == WINPE_I386 ? "i386"
        : ((report.winpe & WINPE_AMD64) == WINPE_AMD64 ? "amd64" : "minint");
    WinPeEntries entries = {};
    entries.directories[0] = QByteArray("/") + plan.loader_directory;
    entries.directories[1] = QByteArray("/") + plan.setup_directory;
    entries.directories[2] = QByteArray("/") + optionsDirectory;
    entries.files[WinPeDetector] = {entries.directories[0] + "/ntdetect.com", {}, "ntdetect.com"};
    entries.files[WinPeCopiedSif] = {plan.copy_sif ? entries.directories[1] + "/txtsetup.sif" : QByteArray(), {},
        plan.copy_sif ? "TXTSETUP.SIF" : nullptr};
    entries.files[WinPeLoader] = {entries.directories[0] + "/setupldr.bin", {}, "BOOTMGR"};
    entries.files[WinPeOptionsSif] = {entries.directories[2] + "/txtsetup.sif", {}, nullptr};
    const ISO_TREE_WALK walk = {visitWinPeEntry, &entries, request.rockRidge,
        uint8_t(source.iso != nullptr ? iso9660_ifs_get_joliet_level(source.iso) : 0)};
    const int status = source.udf != nullptr ? WalkUdfFiles(udf_get_root(source.udf, true, 0), "", &walk)
        : WalkIsoFiles(source.iso, "", &walk);
    int error = entries.error;
    if (error == 0 && status != ISO_SCAN_OK)
        error = status == ISO_SCAN_CANCELLED ? ECANCELED : EIO;
    generatedBytes = 0;
    for (size_t i = 0; i < sizeof(entries.files) / sizeof(entries.files[0]) && error == 0; ++i)
    {
        const WinPeFile& file = entries.files[i];
        if (file.wanted.isEmpty())
            continue;
        error = checkWinPeFile(source, file, WinPeFileIndex(i), plan, report.uses_minint, generatedBytes);
    }
    if (error != 0)
        result.problem = QStringLiteral("The legacy setup files cannot be prepared: %1 (%2).")
            .arg(QString::fromLocal8Bit(strerror(error))).arg(error);
    return error;
}

int checkSettings(const PrepareImageRequest& request, IsoWriteCheck& result)
{
    const bool explicitBiosImage = request.grub2 || request.grub4dos || request.windowsBios || request.kolibriOs
        || (request.syslinux && request.hasGrub2Core());
    if (explicitBiosImage && EncodePrepareRequest(request).isEmpty())
    {
        result.problem = request.windowsBios ? QStringLiteral("Invalid Windows BIOS preparation settings.")
            : QStringLiteral("Invalid GRUB image preparation settings.");
        return EINVAL;
    }
    const bool supportedFilesystem = request.fileSystem == FS_FAT16 ||
        request.fileSystem == FS_FAT32 || request.fileSystem == FS_NTFS;
    if (!supportedFilesystem)
    {
        result.problem = QStringLiteral("This filesystem does not yet support native ISO creation.");
        return ENOTSUP;
    }
    if (request.syslinux)
    {
        const bool windowsSettings = request.windowsFlags != 0 || !request.windowsUsername.isEmpty()
            || request.hasWindowsInstallSettings() || request.enhancedInstall;
        if (windowsSettings)
        {
            result.problem = QStringLiteral("The native Syslinux path does not support Windows customization.");
            return EINVAL;
        }
    }
    int error = PlanFormatPartition(request, result.partition);
    if (error != 0)
    {
        result.problem = QStringLiteral("The selected size or cluster size is unsupported by the partition writer.");
        return error;
    }
    error = CheckFormatSettings(request, result.partition);
    if (request.fileSystem == FS_NTFS && request.volumeId != 0)
        error = EINVAL;
    if (error != 0)
    {
        if (request.fileSystem == FS_NTFS)
            result.problem = QStringLiteral("Invalid NTFS format settings; the formatter must choose the volume serial number.");
        else
        {
            const int fatBits = request.fileSystem == FS_FAT16 ? 16 : 32;
            const QByteArray label = request.label.toUtf8();
            const bool invalidLabel = label.size() > FAT32_LABEL_LENGTH || label.contains('\0');
            result.problem = invalidLabel
                ? QStringLiteral("The FAT%1 volume label must contain at most 11 ASCII characters.").arg(fatBits)
                : QStringLiteral("The volume label, size or cluster size is unsupported by the FAT%1 formatter.").arg(fatBits);
        }
    }
    return error;
}

int checkWindowsOptions(const PrepareImageRequest& request, IsoWriteCheck& result)
{
    const int error = CheckWindowsCustomizationSettings(request);
    if (error != 0)
    {
        result.problem = QStringLiteral("The Windows customization settings are invalid or not yet supported on Linux.");
        return error;
    }
    if (CheckWindowsCustomizationImage(result.analysis.report, request.windowsFlags) != 0)
        result.problem = QStringLiteral("The selected Windows customization is not available for this image, or it already has a Panther answer file.");
    return result.problem.isEmpty() ? 0 : ENOTSUP;
}

int checkSplitInstallWim(const PrepareImageRequest& request, IsoWriteCheck& result, bool windowsInstaller)
{
    const RUFUS_IMG_REPORT& report = result.analysis.report;
    const int fatBits = request.fileSystem == FS_FAT16 ? 16 : 32;
    // Call only after shared policy accepts the single-large-install.wim case.
    result.splitInstallWim = request.fileSystem != FS_NTFS && report.has_4GB_file != 0;
    if (result.splitInstallWim)
    {
        const bool singleArchive = report.wininst_index == 1;
        const QString path = QString::fromUtf8(report.wininst_path[0]);
        const bool conventional = path.compare(QStringLiteral("?:/sources/install.wim"), Qt::CaseInsensitive) == 0;
        if (!windowsInstaller || !singleArchive || !conventional)
        {
            result.problem = QStringLiteral("FAT%1 splitting requires one conventional sources/install.wim Windows installation archive.").arg(fatBits);
            return ENOTSUP;
        }
    }
    return 0;
}

bool isKnownWindowsInstaller(const RUFUS_IMG_REPORT& report,
    const WINDOWS_IMAGE_ANALYSIS& windows, bool allowOlderInstallers)
{
    const bool modernWindows = report.win_version.major == 10 || report.win_version.major == 11;
    const bool windows7 = report.win_version.major == 7 && report.win_version.minor == 0;
    const bool windows8 = report.win_version.major == 8 && report.win_version.minor <= 1;
    const bool supportedVersion = modernWindows || (allowOlderInstallers && (windows7 || windows8));
    return windows.state == WINDOWS_IMAGE_KNOWN && supportedVersion && HAS_WININST(report);
}

int checkReport(const PrepareImageRequest& request, IsoWriteCheck& result)
{
    const ISO_ANALYSIS& analysis = result.analysis;
    const RUFUS_IMG_REPORT& report = analysis.report;
    const quint32 allowedPending = ISO_PENDING_WINDOWS | ISO_PENDING_SECURE_BOOT | ISO_PENDING_EMBEDDED_EFI;
    const bool incomplete = !analysis.tree_complete || (analysis.pending & ~allowedPending) != 0;
    const bool isoDisabled = !report.is_iso || report.disable_iso;
    const bool windowsInstaller = HAS_WININST(report) && HAS_BOOTMGR(report);
    const int filesystem = request.fileSystem;
    const int fatBits = filesystem == FS_FAT16 ? 16 : 32;
    const RUFUS_FS_OPTIONS selection = {BT_IMAGE, TT_UEFI, true, false, request.enhancedInstall};
    bool forcedFat = false;
    const quint32 allowed = GetAllowedFileSystems(&report, &selection, &forcedFat);
    const bool filesystemAllowed = (allowed & FS_MASK(filesystem)) != 0;
    const bool fatIncompatible = request.fileSystem != FS_NTFS && !IsFat32Compatible(&report, request.enhancedInstall);
    const RUFUS_PARTITION_OPTIONS partitions = {BT_IMAGE, filesystem, TT_UEFI,
        true, false, false, request.enhancedInstall, false, 0};
    result.extraPartitions = GetExtraPartitions(&report, &partitions);
    const quint8 supportedPartitions = request.fileSystem == FS_NTFS ? XP_UEFI_NTFS : 0;
    if (incomplete)
        result.problem = QStringLiteral("Image analysis is incomplete; the ISO tree cannot be prepared.");
    else if (isoDisabled)
        result.problem = QStringLiteral("Rufus image policy does not allow ISO mode for this image.");
    else if (fatIncompatible)
        result.problem = QStringLiteral("This image is incompatible with the FAT%1 preparation path.").arg(fatBits);
    else if (!filesystemAllowed)
        result.problem = QStringLiteral("Rufus image policy does not allow the selected filesystem.");
    else if (!HAS_REGULAR_EFI(report) && !HAS_WIN7_EFI(report) && !(report.has_efi & 0x8000))
        result.problem = QStringLiteral("ISO mode requires a regular fallback EFI application in the image tree.");
    else if (result.extraPartitions != supportedPartitions)
        result.problem = QStringLiteral("This image needs a partition layout that is not available yet.");
    if (!result.problem.isEmpty())
        return ENOTSUP;
    const int splitError = checkSplitInstallWim(request, result, windowsInstaller);
    if (splitError != 0)
        return splitError;
    return checkWindowsOptions(request, result);
}

int checkBiosReport(const PrepareImageRequest& request, IsoWriteCheck& result, bool allowUnselectedCore)
{
    const ISO_ANALYSIS& analysis = result.analysis;
    const RUFUS_IMG_REPORT& report = analysis.report;
    const quint32 allowedPending = ISO_PENDING_WINDOWS | ISO_PENDING_SECURE_BOOT | ISO_PENDING_EMBEDDED_EFI;
    const bool incomplete = !analysis.tree_complete || (analysis.pending & ~allowedPending) != 0;
    const bool windows = HAS_WINDOWS(report) || HAS_WININST(report) || HAS_WINPE(report) || report.uses_minint;
    const int secondaryBoot = GetSecondaryBootType(BT_IMAGE, &report, false);
    const RUFUS_BOOT_OPTIONS boot = {BT_IMAGE, request.fileSystem, TT_BIOS, false,
        request.enhancedInstall, false, false};
    const bool syslinux = GetBootInstallType(&report, &boot) == RUFUS_BOOT_INSTALL_SYSLINUX;
    const bool coreLookup = allowUnselectedCore && request.windowsBios && request.enhancedInstall
        && !request.usesUefiNtfs();
    const bool windowsLoader = IsWindowsBiosImage(analysis, result.windows, true, false,
        request.enhancedInstall, request.hasGrub2Core() || coreLookup, secondaryBoot == BT_GRUB4DOS);
    const bool linuxLoader = request.grub4dos ? secondaryBoot == BT_GRUB4DOS
        : request.grub2 ? secondaryBoot == BT_GRUB2 : syslinux;
    const bool reactosPrimary = request.syslinux && HAS_REACTOS(report) && !HAS_SYSLINUX(report);
    const bool syslinuxSecondary = secondaryBoot == BT_IMAGE || (reactosPrimary
        && ((secondaryBoot == BT_GRUB2 && (request.hasGrub2Core() || allowUnselectedCore))
            || secondaryBoot == BT_GRUB4DOS));
    const bool kolibriLoader = HAS_KOLIBRIOS(report) && !syslinux && secondaryBoot == BT_IMAGE
        && !(HAS_WINDOWS(report) && request.enhancedInstall);
    const bool expectedLoader = request.windowsBios ? windowsLoader : request.kolibriOs ? kolibriLoader
        : linuxLoader && (!request.syslinux || syslinuxSecondary);
    const RUFUS_FS_OPTIONS selection = {BT_IMAGE, TT_BIOS, true, false, request.enhancedInstall};
    bool forcedFat = false;
    const quint32 filesystems = GetAllowedFileSystems(&report, &selection, &forcedFat);
    const RUFUS_PARTITION_OPTIONS partitions = {BT_IMAGE, request.fileSystem, TT_BIOS,
        false, false, false, request.enhancedInstall, false, 0};
    result.extraPartitions = GetExtraPartitions(&report, &partitions);
    const bool allowSplit = request.windowsBios && request.enhancedInstall;
    const bool fatIncompatible = request.fileSystem != FS_NTFS && !IsFat32Compatible(&report, allowSplit);
    const quint8 requestedPartitions = request.usesUefiNtfs() ? XP_UEFI_NTFS : 0;
    if (incomplete)
        result.problem = QStringLiteral("Image analysis is incomplete; BIOS preparation cannot proceed.");
    else if (!report.is_iso || report.disable_iso)
        result.problem = QStringLiteral("Rufus image policy does not allow ISO mode for this image.");
    else if (!expectedLoader)
    {
        if (request.windowsBios)
            result.problem = QStringLiteral("Windows BIOS preparation requires BOOTMGR and the image's original Windows PBR dispatch.");
        else
            result.problem = request.grub4dos ? QStringLiteral("This MBR path requires an image with its own GRUB4DOS loader.")
                : request.grub2 ? QStringLiteral("This MBR path requires a supported Linux GRUB2 image.")
                : QStringLiteral("This MBR path requires a supported Linux Syslinux image.");
    }
    else if (HAS_KOLIBRIOS(report) && !syslinux && request.fileSystem == FS_FAT16)
        result.problem = QStringLiteral("The original KolibriOS partition boot record does not support FAT16.");
    else if (!(filesystems & FS_MASK(request.fileSystem)) || fatIncompatible)
        result.problem = QStringLiteral("Rufus image policy does not allow the selected filesystem for this image.");
    else if (result.extraPartitions != requestedPartitions)
        result.problem = QStringLiteral("The requested partition layout does not match Rufus's image policy.");
    if (!result.problem.isEmpty())
        return ENOTSUP;
    if (windows)
    {
        const int splitError = checkSplitInstallWim(request, result, windowsLoader);
        if (splitError != 0)
            return splitError;
        return checkWindowsOptions(request, result);
    }
    return 0;
}

int efiHeaderSize(const unsigned char* dos, quint64 fileSize, quint64& headerBytes)
{
    if (fileSize < dosHeaderBytes)
        return EBADMSG;
    const quint32 peOffset = qFromLittleEndian<quint32>(dos + peOffsetField);
    headerBytes = quint64(peOffset) + peCoffBytes + subsystemField + sizeof(quint16);
    const bool headerFits = peOffset >= dosHeaderBytes && headerBytes <= fileSize;
    if (!headerFits)
        return EBADMSG;
    return headerBytes > maxPeHeaderBytes ? ENOTSUP : 0;
}

int checkEfiApplication(const char* header, size_t headerBytes, quint64 fileSize, ArchType architecture)
{
    if (architecture == ARCH_UNKNOWN || NativeEfiArchitecture(header, headerBytes) != architecture)
        return ENOTSUP;
    const quint32 peOffset = qFromLittleEndian<quint32>(header + peOffsetField);
    const char* pe = header + peOffset;
    const quint16 optionalBytes = qFromLittleEndian<quint16>(pe + optionalSizeField);
    const bool wide = architecture != ARCH_X86_32 && architecture != ARCH_ARM_32;
    const quint16 minimumOptionalBytes = wide ? pe64OptionalBytes : 96;
    const bool optionalHeaderFits = optionalBytes >= minimumOptionalBytes
        && quint64(peOffset) + peCoffBytes + optionalBytes <= fileSize;
    const bool format = qFromLittleEndian<quint16>(pe + peCoffBytes) == (wide ? pe32PlusMagic : 0x10b);
    const bool application = qFromLittleEndian<quint16>(pe + peCoffBytes + subsystemField) == efiApplication;
    return optionalHeaderFits && format && application ? 0 : EBADMSG;
}

struct EmbeddedFallbackCheck { const ISO_ANALYSIS_SOURCE* source; IsoWriteCheck* result; bool found = false; };
int checkEmbeddedFallback(const EMBEDDED_EFI_FILE* file, void* opaque)
{
    auto& check = *static_cast<EmbeddedFallbackCheck*>(opaque);
    if (file->directory) return 0;
    bool copy = false;
    int error = EmbeddedEfiPrecedence(check.source->iso,check.source->udf,&check.result->analysis.report,file,&copy);
    if (error || !copy) return error;
    ArchType architecture = ARCH_UNKNOWN;
    for (int i = 1; i < ARCH_MAX; ++i) {
        const QByteArray expected = QByteArray("/efi/boot/boot") + efi_archname[i] + ".efi";
        if (strcasecmp(file->path,expected.constData()) == 0) architecture = ArchType(i);
    }
    if (architecture == ARCH_UNKNOWN) return 0;
    unsigned char dos[dosHeaderBytes];
    if (file->size < sizeof(dos)) return EBADMSG;
    error = ReadEmbeddedEfiRange(file,0,dos,sizeof(dos));
    quint64 size = 0;
    if (!error) error = efiHeaderSize(dos,file->size,size);
    QByteArray header;
    if (!error) { header.resize(qsizetype(size)); error = ReadEmbeddedEfiRange(file,0,header.data(),size_t(header.size())); }
    if (!error) error = checkEfiApplication(header.constData(),size_t(header.size()),file->size,architecture);
    if (!error) { check.found = true; if (check.result->efiPath.isEmpty()) check.result->efiPath = QString::fromUtf8(file->path); }
    else check.result->problem = QStringLiteral("Invalid embedded EFI fallback: %1.").arg(QString::fromUtf8(file->path));
    return error;
}

int checkFallbackEfi(const ISO_ANALYSIS_SOURCE& source, IsoWriteCheck& result)
{
    // Validate every source-offered fallback, independent of the host CPU.
    bool found = false;
    bool embeddedAvailable = false;
    if ((result.analysis.report.has_efi & 0xc000) && HAS_EFI_IMG(result.analysis.report)) {
        IsoReader image;
        const int status = OpenIsoFileReader(&image.file,source.iso,source.udf,result.analysis.report.efi_img_path);
        if (status == ISO_SCAN_CANCELLED) return ECANCELED;
        embeddedAvailable = status == ISO_SCAN_OK;
    }
    for (const efi_boot_entry_t& entry : result.analysis.report.efi_boot_entry)
    {
        const QString path = QString::fromUtf8(entry.path);
        ArchType architecture = ARCH_UNKNOWN;
        for (int candidate = 1; candidate < ARCH_MAX; ++candidate) {
            const QString fallback = QStringLiteral("/efi/boot/boot%1.efi").arg(QString::fromLatin1(efi_archname[candidate]));
            if (path.compare(fallback, Qt::CaseInsensitive) == 0) architecture = ArchType(candidate);
        }
        if (entry.type != EBT_MAIN || architecture == ARCH_UNKNOWN) continue;
        const bool brokenX64 = (result.analysis.report.has_efi & 0x4000) && embeddedAvailable && architecture == ARCH_X86_64;
        if (brokenX64) continue;
        if (!found) result.efiPath = path;
        found = true;
        result.problem = QStringLiteral("The fallback loader is not a readable matching EFI application: %1.").arg(path);
        IsoReader reader;
        int status = OpenIsoFileReader(&reader.file, source.iso, source.udf, entry.path);
        if (status != ISO_SCAN_OK) return status == ISO_SCAN_CANCELLED ? ECANCELED : EIO;
        if (reader.file.size < dosHeaderBytes) return EBADMSG;
        unsigned char dos[dosHeaderBytes];
        status = ReadIsoFileRange(&reader.file, 0, dos, sizeof(dos));
        if (status != ISO_SCAN_OK) return status == ISO_SCAN_CANCELLED ? ECANCELED : EIO;
        quint64 headerBytes = 0;
        const int headerError = efiHeaderSize(dos, reader.file.size, headerBytes);
        if (headerError != 0) return headerError;
        QByteArray header(qsizetype(headerBytes), '\0');
        status = ReadIsoFileRange(&reader.file, 0, header.data(), size_t(header.size()));
        if (status != ISO_SCAN_OK) return status == ISO_SCAN_CANCELLED ? ECANCELED : EIO;
        const int payloadError = checkEfiApplication(header.constData(), size_t(header.size()), reader.file.size, architecture);
        if (payloadError != 0) return payloadError;
    }
    if (embeddedAvailable) {
        EmbeddedFallbackCheck check{&source,&result};
        const int error = WalkEmbeddedEfiFiles(source.iso,source.udf,result.analysis.report.efi_img_path,checkEmbeddedFallback,&check);
        if (error) return error;
        found |= check.found;
    }
    if (!found)
    {
        result.problem = QStringLiteral("The image needs a supported removable-media EFI fallback application.");
        return ENOTSUP;
    }
    result.problem.clear();
    return 0;
}
}

int ReadWindowsEfiRepair(const ISO_ANALYSIS_SOURCE& source, const RUFUS_IMG_REPORT& report,
    QByteArray& payload, QString& problem)
{
    payload.clear();
    problem.clear();
    if (!HAS_WIN7_EFI(report))
    {
        problem = QStringLiteral("This image does not require Windows EFI boot repair.");
        return ENOTSUP;
    }
    const char* archive = report.wininst_path[0];
    const bool terminated = memchr(archive, '\0', sizeof(report.wininst_path[0])) != nullptr;
    if (!terminated || strncmp(archive, "?:/", 3) != 0)
    {
        problem = QStringLiteral("The first Windows installation archive has an invalid source path.");
        return EINVAL;
    }
    if (IsIsoScanCancelled())
        return ECANCELED;
    IsoReader reader;
    const int status = OpenIsoFileReader(&reader.file, source.iso, source.udf, archive + 2);
    problem = QStringLiteral("Cannot read the Windows EFI repair payload from %1, image 1.")
        .arg(QString::fromUtf8(archive + 2));
    if (status != ISO_SCAN_OK)
        return status == ISO_SCAN_CANCELLED ? ECANCELED : EIO;
    WIMStruct* wim = nullptr;
    int error = OpenWimFromIsoReader(&reader.file, 0, &wim);
    uint8_t* bytes = nullptr;
    size_t size = 0;
    // FormatThread selects this exact file from image 1 of the first archive,
    // regardless of the archive's boot index or reported Windows version.
    if (error == 0)
        error = ReadWimFile(wim, 1, "Windows/Boot/EFI/bootmgfw.efi", &bytes, &size);
    wimlib_free(wim); // Release borrowed resources before closing the ISO reader.
    if (IsIsoScanCancelled() || error == WIMLIB_ERR_ABORTED_BY_PROGRESS)
    {
        free(bytes);
        return ECANCELED;
    }
    if (error != 0)
    {
        problem += QStringLiteral(" %1").arg(QString::fromUtf8(
            wimlib_get_error_string(static_cast<wimlib_error_code>(error))));
        free(bytes);
        return EIO;
    }
    quint64 headerBytes = 0;
    error = efiHeaderSize(bytes, size, headerBytes);
    if (error == 0)
        error = checkEfiApplication(reinterpret_cast<const char*>(bytes), size_t(headerBytes), size, ARCH_X86_64);
    if (error == 0)
    {
        payload = QByteArray(reinterpret_cast<const char*>(bytes), qsizetype(size));
        problem.clear();
    }
    else
        problem = QStringLiteral("Windows/Boot/EFI/bootmgfw.efi in the first installation image is not a readable x64 EFI application.");
    free(bytes);
    return error;
}

bool IsLegacyWinPeImage(const ISO_ANALYSIS& analysis, bool allowDualBios, bool allowGrub2Sbr, bool allowGrub4dosSbr)
{
    const RUFUS_IMG_REPORT& report = analysis.report;
    const quint32 allowedPending = ISO_PENDING_WINDOWS | ISO_PENDING_SECURE_BOOT | ISO_PENDING_EMBEDDED_EFI;
    const bool completeTree = analysis.tree_complete && (analysis.pending & ~allowedPending) == 0;
    // Keep a BIOS root BOOTMGR's report flag. Fresh preflight checks the
    // root/directory loader contract through the shared transformation.
    // Alt+E makes the original Windows branch install a PBR rather than
    // Syslinux. Extraction still processes its configs before SetupWinPE.
    const RUFUS_BOOT_OPTIONS boot = {BT_IMAGE, FS_FAT32, TT_BIOS, false, allowDualBios, false, false};
    const bool syslinuxPbr = HAS_SYSLINUX(report)
        && GetBootInstallType(&report, &boot) == RUFUS_BOOT_INSTALL_PBR;
    const bool incompatibleSyslinux = HAS_SYSLINUX(report) && !syslinuxPbr;
    const WINPE_SETUP_PLAN plan = GetWinPeSetupPlan(report.winpe, report.uses_minint);
    const int secondaryBoot = GetSecondaryBootType(BT_IMAGE, &report, false);
    const bool selectedGrub2Sbr = allowGrub2Sbr && allowDualBios && secondaryBoot == BT_GRUB2;
    const bool selectedGrub4dosSbr = allowGrub4dosSbr && allowDualBios && secondaryBoot == BT_GRUB4DOS;
    const bool selectedGrubSbr = selectedGrub2Sbr || selectedGrub4dosSbr;
    const bool grubNeedsSbr = HAS_GRUB(report) && secondaryBoot != BT_IMAGE && !selectedGrubSbr;
    const bool incompatibleReactos = HAS_REACTOS(report)
        && GetBootInstallType(&report, &boot) != RUFUS_BOOT_INSTALL_PBR;
    const bool mixedLoader = incompatibleSyslinux || grubNeedsSbr || incompatibleReactos
        || (HAS_KOLIBRIOS(report) && !allowDualBios);
    // With a complete setup family, /minint and partial minint files select
    // the original keep-loader plan. Without /minint that plan refuses them.
    const bool supportedLayout = HAS_WINPE(report) && report.winpe_options_known
        && plan.action != WINPE_UNSUPPORTED_LAYOUT;
    return completeTree && report.is_iso && !report.disable_iso && supportedLayout && !mixedLoader;
}

bool IsWindowsBiosImage(const ISO_ANALYSIS& analysis, const WINDOWS_IMAGE_ANALYSIS& windows,
    bool allowOlderInstallers, bool allowLegacyWinPe, bool allowDualBios, bool allowGrub2Sbr, bool allowGrub4dosSbr)
{
    if (allowLegacyWinPe && IsLegacyWinPeImage(analysis, allowDualBios, allowGrub2Sbr, allowGrub4dosSbr))
        return true;
    const RUFUS_IMG_REPORT& report = analysis.report;
    const quint32 allowedPending = ISO_PENDING_WINDOWS | ISO_PENDING_SECURE_BOOT | ISO_PENDING_EMBEDDED_EFI;
    const bool completeTree = analysis.tree_complete && (analysis.pending & ~allowedPending) == 0;
    // Ordinary creation copies BOOTMGR/PE sources without interpreting an
    // install archive. Vista, WIM PE and missing/unknown version metadata do
    // not change the original boot installation dispatch.
    const bool knownInstaller = (allowOlderInstallers || isKnownWindowsInstaller(report, windows, false))
        && HAS_BOOTMGR_BIOS(report);
    const bool legacyLoader = HAS_WINPE(report) || report.uses_minint;
    const RUFUS_BOOT_OPTIONS boot = {BT_IMAGE, FS_FAT32, TT_BIOS, false, allowDualBios, false, false};
    const bool windowsPbr = GetBootInstallType(&report, &boot) == RUFUS_BOOT_INSTALL_PBR;
    const int secondary = GetSecondaryBootType(BT_IMAGE, &report, false);
    const bool selectedSecondary = secondary == BT_IMAGE || (allowDualBios
        && ((secondary == BT_GRUB2 && allowGrub2Sbr) || (secondary == BT_GRUB4DOS && allowGrub4dosSbr)));
    const bool mixedLoader = !windowsPbr || !selectedSecondary || (HAS_KOLIBRIOS(report) && !allowDualBios);
    return completeTree && report.is_iso && !report.disable_iso && knownInstaller && !legacyLoader && !mixedLoader;
}

int CheckWindowsSilentInstallSource(const IsoWriteCheck& checked, QString& problem)
{
    problem.clear();
    const bool eligible = GetWindowsUnattendFlags(&checked.analysis.report, false) & UNATTEND_SILENT_INSTALL;
    if (!eligible)
    {
        problem = QStringLiteral("Silent installation is not available for this image.");
        return ENOTSUP;
    }
    // The shared answer selects /IMAGE/INDEX without naming an archive.
    const bool singleArchive = checked.analysis.report.wininst_index == 1;
    const QString archive = checked.windowsInstallPath.toLower();
    const bool standardArchive = archive == QStringLiteral("/sources/install.wim")
        || archive == QStringLiteral("/sources/install.esd");
    if (!singleArchive || !standardArchive)
    {
        problem = QStringLiteral("Silent installation requires one unambiguous sources/install.wim or install.esd archive. Ordinary creation remains available.");
        return ENOTSUP;
    }
    if (checked.windowsInstallError != 0 || checked.windowsInstall.editions.isEmpty())
    {
        problem = QStringLiteral("Windows editions could not be read from this image.");
        return EIO;
    }
    if (!IsWindowsLanguageTag(checked.windowsInstall.uiLanguage))
    {
        problem = QStringLiteral("The Windows Setup language is invalid for silent installation.");
        return EINVAL;
    }
    return 0;
}

int CheckWindowsInstallSelection(const PrepareImageRequest& request, const IsoWriteCheck& checked, QString& problem)
{
    if (!(request.windowsFlags & UNATTEND_SILENT_INSTALL))
        return 0;
    const int error = CheckWindowsSilentInstallSource(checked, problem);
    if (error != 0)
        return error;
    if (request.windowsInstallPath != checked.windowsInstallPath)
    {
        problem = QStringLiteral("The selected Windows install archive no longer matches this image. Select it again.");
        return ESTALE;
    }
    for (const WindowsEdition& edition : checked.windowsInstall.editions)
    {
        if (edition.image == request.windowsEdition)
            return 0;
    }
    problem = QStringLiteral("The selected Windows edition no longer exists in this archive. Select it again.");
    return ESTALE;
}

static int checkIsoWriteImage(const PrepareImageRequest& request, uid_t caller, IsoWriteCheck& result,
    ISO_ANALYSIS_SOURCE* retainedView, bool requireLegacyWinPe, bool allowLegacyWinPe,
    bool allowUnselectedCore)
{
    result = {};
    if (retainedView != nullptr && (retainedView->iso != nullptr || retainedView->udf != nullptr))
        return EINVAL;
    int error = checkSettings(request, result);
    if (error != 0)
        return error;
    PrepareIsoSource retained(request);
    error = retained.open(caller);
    if (error != 0)
    {
        result.problem = QStringLiteral("The selected image changed or cannot be retained. Select it again.");
        return error;
    }
    IMAGE_FILE_SOURCE signature;
    error = OpenImageFileSource(retained.retainedPath(), &signature);
    if (error != 0)
    {
        result.problem = QStringLiteral("Cannot read the selected image signature.");
        return error;
    }
    const bool bootMarker = signature.has_boot_marker;
    CloseImageFileSource(&signature);
    IsoView view;
    const ISO_ANALYSIS_OPTIONS analysis = {request.rockRidge, request.joliet, {uint16_t(request.syslinux ? 0x407 : 0), uint16_t(request.syslinux ? nativeSyslinuxVersion : 0)}};
    const int status = AnalyzeIsoImageWithSource(retained.retainedPath(), request.imageSize,
        &analysis, &result.analysis, &view.source);
    const bool analyzed = status == ISO_SCAN_OK || status == ISO_SCAN_PARTIAL;
    if (!analyzed)
    {
        result.problem = QStringLiteral("Cannot analyze this image as UDF or ISO9660.");
        return status == ISO_SCAN_CANCELLED ? ECANCELED : EIO;
    }
    const bool bios = request.syslinux || request.grub2 || request.grub4dos || request.windowsBios || request.kolibriOs;
    const bool legacyWinPe = requireLegacyWinPe || (allowLegacyWinPe && bios && HAS_WINPE(result.analysis.report));
    if ((request.windowsBios || request.syslinux) && request.hasGrub2Core())
    {
        const RUFUS_IMG_REPORT& report = result.analysis.report;
        const int secondaryBoot = GetSecondaryBootType(BT_IMAGE, &report, false);
        const bool reactosCore = HAS_REACTOS(report) && !HAS_SYSLINUX(report) && secondaryBoot == BT_GRUB2;
        const bool matching = request.syslinux ? reactosCore
            : secondaryBoot == BT_GRUB2 || secondaryBoot == BT_GRUB4DOS;
        if (!matching)
        {
            result.problem = QStringLiteral("The selected GRUB2 core does not match this image's secondary boot record.");
            return ENOTSUP;
        }
    }
    if (legacyWinPe)
    {
        const bool ntfs = request.fileSystem == FS_NTFS;
        const bool fat = request.fileSystem == FS_FAT16 || request.fileSystem == FS_FAT32;
        const bool supportedBios = bios && (request.mbr || request.syslinux) && (ntfs || fat);
        const bool customized = request.windowsFlags != 0 || !request.windowsUsername.isEmpty()
            || request.hasWindowsInstallSettings();
        if (!supportedBios || customized)
        {
            result.problem = QStringLiteral("Legacy WinPE requires MBR/BIOS with an eligible FAT or NTFS filesystem, without Windows customization.");
            return ENOTSUP;
        }
    }
    const int windowsStatus = ScanWindowsImage(&view.source, &result.analysis, &result.windows);
    if (legacyWinPe && result.windows.state == WINDOWS_IMAGE_UNCHECKED)
        result.windows.state = WINDOWS_IMAGE_UNKNOWN; // No modern installer version is inferred.
    if (windowsStatus == ISO_SCAN_CANCELLED)
    {
        result.problem = QStringLiteral("Image analysis was cancelled.");
        return ECANCELED;
    }
    // Include the separate ESP question for eligible non-hybrid/persistent ISOs.
    result.analysis.report.is_bootable_img = bootMarker ? 1 : 0;
    ApplyIsoPolicy(&result.analysis.report);
    if (request.persistenceSize != 0 && !HAS_PERSISTENCE(result.analysis.report)) {
        result.problem = QStringLiteral("This image is not eligible for Linux persistence.");
        return ENOTSUP;
    }
    result.choices = GetFinalImageWriteChoices(&result.analysis.report, request.persistenceSize, !bios, request.fileSystem != FS_NTFS);
    const bool espFat = request.fileSystem == FS_FAT16 || request.fileSystem == FS_FAT32;
    if (request.isoEsp && (request.mbr || request.sfd || request.rawWrite || bios
        || !CanWriteIsoToEsp(&result.analysis.report, true, espFat))) {
        result.problem = QStringLiteral("ISO → ESP requires a GPT/FAT image with regular EFI files and less than 1 GiB of projected content.");
        return ENOTSUP;
    }
    quint64 legacyBytes = 0;
    error = legacyWinPe ? checkWinPeSource(request, view.source, result, legacyBytes, allowUnselectedCore)
        : (bios ? checkBiosReport(request, result, allowUnselectedCore) : checkReport(request, result));
    if (error == 0 && legacyWinPe)
        error = checkSplitInstallWim(request, result, HAS_WININST(result.analysis.report)
            && HAS_BOOTMGR(result.analysis.report));
    const bool reactosSyslinux = request.syslinux && HAS_REACTOS(result.analysis.report);
    quint64 legacyIsoBytes = 0;
    if (error == 0)
        error = CheckLegacyIsoFiles(view.source, result.analysis.report, reactosSyslinux,
            result.problem, request.rockRidge, &legacyIsoBytes);
    RUFUS_IMG_REPORT syslinuxReport = result.analysis.report;
    if (reactosSyslinux && !HAS_SYSLINUX(syslinuxReport))
    {
        syslinuxReport.has_syslinux = true;
        syslinuxReport.sl_version = 0x407; // Original embedded ReactOS installer pair.
    }
    if (error == 0 && request.syslinux)
    {
        if (reactosSyslinux && !HAS_SYSLINUX(result.analysis.report))
            error = CheckNativeSyslinuxSelection(syslinuxReport, request.fileSystem, request.logicalSectorSize,
                result.partition.offset, result.problem);
        else
            error = CheckNativeSyslinuxSource(view.source, result.analysis, request.rockRidge,
                request.fileSystem, request.logicalSectorSize, result.partition.offset, result.problem);
    }
    if (error == 0 && request.syslinux && !request.isStandaloneSyslinux()) {
        error = ReadEmbeddedSyslinuxResources(syslinuxReport,result.syslinuxResources);
        if (!error) error = ReadSyslinuxCompanion(view.source,syslinuxReport,result.syslinuxResources);
        if (!error && request.hasSyslinuxResources()) {
            QByteArray bytes; SyslinuxResources selected;
            error = LoadPrepareSyslinuxResources(request,caller,bytes);
            if (!error) error = DecodeSyslinuxResources(bytes,selected);
            if (!error) error = ValidateSyslinuxResources(syslinuxReport,selected);
            if (!error && selected.module != result.syslinuxResources.module) error = ESTALE;
            if (!error) result.syslinuxResources = std::move(selected);
        }
    }
    const bool windowsFatDual = request.windowsBios && (request.fileSystem == FS_FAT16 || request.fileSystem == FS_FAT32)
        && request.enhancedInstall && IS_EFI_BOOTABLE(result.analysis.report);
    const bool fat = request.fileSystem == FS_FAT16 || request.fileSystem == FS_FAT32;
    const bool grub4dosFatDual = request.grub4dos && fat
        && IS_EFI_BOOTABLE(result.analysis.report);
    const bool needsEfi = !bios || request.usesUefiNtfs() || windowsFatDual || grub4dosFatDual;
    if (error == 0 && needsEfi)
    {
        if (HAS_WIN7_EFI(result.analysis.report))
        {
            error = ReadWindowsEfiRepair(view.source, result.analysis.report,
                result.windowsEfiRepair, result.problem);
            if (error == 0)
                result.efiPath = QStringLiteral("/EFI/BOOT/BOOTX64.EFI");
        }
        else
            error = checkFallbackEfi(view.source, result);
    }
    if (error != 0)
        return error;
    const QByteArray label = request.label.toUtf8();
    const QByteArray path = QFile::encodeName(request.imagePath);
    const ISO_BOOT_RESOURCES bootResources = {{reinterpret_cast<const uint8_t*>(result.syslinuxResources.oldModules[0].constData()),reinterpret_cast<const uint8_t*>(result.syslinuxResources.oldModules[1].constData())},
        {size_t(result.syslinuxResources.oldModules[0].size()),size_t(result.syslinuxResources.oldModules[1].size())}};
    const ISO_EXTRACT_OPTIONS options = {analysis, label.constData(), path.constData(), result.splitInstallWim, request.persistenceSize, nullptr, request.preserveTimestamps, request.fileSystem == FS_NTFS && request.ntfsCompression && request.clusterSize <= NtfsCompressionClusterBytes, request.fileSystem == FS_NTFS, &bootResources, nullptr};
    const uint64_t fileLimit = request.fileSystem == FS_NTFS ? 0 : UINT32_MAX;
    error = CheckIsoTree(retained.retainedPath(), &options, fileLimit, &result.tree);
    if (error == 0 && request.runtimeValidation) {
        const bool biosTarget = request.windowsBios || request.syslinux || request.grub2 || request.grub4dos || request.kolibriOs;
        if (!CanUseRuntimeUefiValidation(&result.analysis.report, BT_IMAGE, request.windowsToGo,
            biosTarget, request.enhancedInstall)) {
            result.problem = QStringLiteral("Runtime UEFI media validation is unavailable for this image and firmware choice.");
            return ENOTSUP;
        }
    }
    if (error == 0) {
        quint64 reserve = 0;
        error = CheckMediaIntegritySource(view.source, result.analysis, request.runtimeValidation,
            result.tree.split_parts, request.rockRidge, reserve, result.problem);
        if (error == 0 && reserve > UINT64_MAX - result.tree.required_bytes) error = EOVERFLOW;
        if (error == 0) result.tree.required_bytes += reserve;
        if (error != 0) return error;
    }
    if (error == 0)
    {
        const quint64 repairBytes = quint64(result.windowsEfiRepair.size()) + legacyBytes;
        const bool sizeFits = repairBytes <= UINT64_MAX - result.tree.required_bytes;
        if (!sizeFits)
            error = EOVERFLOW;
        else
            result.tree.required_bytes += repairBytes;
        if (error == 0 && legacyIsoBytes > UINT64_MAX - result.tree.required_bytes)
            error = EOVERFLOW;
        if (error == 0)
            result.tree.required_bytes += legacyIsoBytes;
    }
    if (error != 0)
        result.problem = QStringLiteral("The image tree or boot configuration cannot be extracted: %1 (%2).")
            .arg(QString::fromUtf8(result.tree.path), QString::fromLocal8Bit(strerror(error)));
    else if (result.tree.required_bytes > result.partition.size)
    {
        error = ENOSPC;
        result.problem = result.splitInstallWim
            ? QStringLiteral("The image files and required WIM split space exceed the selected partition.")
            : QStringLiteral("The image files are larger than the selected partition.");
    }
    const bool offersSilentInstall = result.windows.state == WINDOWS_IMAGE_KNOWN
        && (GetWindowsUnattendFlags(&result.analysis.report, false) & UNATTEND_SILENT_INSTALL);
    if (error == 0 && offersSilentInstall)
    {
        // ScanWindowsImage has already validated this first archive's path and
        // version. Retain that identity; never substitute another candidate.
        const char* installPath = result.analysis.report.wininst_path[0] + 2;
        result.windowsInstallPath = QString::fromUtf8(installPath);
        result.windowsInstallError = ReadWindowsInstallOptions(view.source, installPath, result.windowsInstall);
        const bool cancelled = IsRufusJobCancelled()
            || result.windowsInstallError == WIMLIB_ERR_ABORTED_BY_PROGRESS;
        if (cancelled)
        {
            result.windowsInstall = {};
            error = ECANCELED;
            result.problem = QStringLiteral("Image analysis was cancelled.");
        }
        else if (result.windowsInstallError != 0)
            uprintf("Cannot read Windows installation choices: %s. Ordinary creation remains available.",
                wimlib_get_error_string(static_cast<wimlib_error_code>(result.windowsInstallError)));
        else
            uprintf("Windows installation choices: %d edition(s), Setup language %s",
                int(result.windowsInstall.editions.size()), result.windowsInstall.uiLanguage.toUtf8().constData());
    }
    if (error == 0)
    {
        error = retained.check();
        if (error != 0)
            result.problem = QStringLiteral("The image changed during preflight. Select it again.");
    }
    if (error == 0)
        error = CheckWindowsInstallSelection(request, result, result.problem);
    if (error == 0 && retainedView != nullptr)
    {
        *retainedView = view.source;
        view.source = {};
    }
    if (error != 0)
        result.windowsEfiRepair.clear();
    else
        result.legacyWinPe = legacyWinPe;
    return error;
}

int CheckIsoWriteImage(const PrepareImageRequest& request, uid_t caller, IsoWriteCheck& result,
    ISO_ANALYSIS_SOURCE* retainedView, bool allowLegacyWinPe)
{
    return checkIsoWriteImage(request, caller, result, retainedView, false, allowLegacyWinPe, false);
}

int CheckLegacyWinPeImage(const PrepareImageRequest& request, uid_t caller, IsoWriteCheck& result,
    ISO_ANALYSIS_SOURCE* retainedView)
{
    return checkIsoWriteImage(request, caller, result, retainedView, true, true, false);
}

int CheckIsoWriteSource(const PrepareImageRequest& request, uid_t caller, IsoWriteCheck& result,
    ISO_ANALYSIS_SOURCE* retainedView, bool allowLegacyWinPe)
{
    return checkIsoWriteImage(request, caller, result, retainedView, false, allowLegacyWinPe, true);
}
}
