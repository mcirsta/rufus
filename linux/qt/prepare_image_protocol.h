// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "job_priority.h"
#include "platform/device_policy.h"
#include "badblocks_policy.h"
#include "platform/device_class.h"

#include "progress_policy.h"
#include "image_compression.h"
#include "platform/raw_write.h"
#include "platform/zero_write.h"
#include "format_policy.h"
#include "platform/windows_region_options.h"
#include <QJsonObject>
#include <QMetaType>
#include <QString>

namespace rufus
{
constexpr qsizetype PrepareMessageBytes = 4096;
constexpr quint64 MinimumPersistenceBytes = 256 * 1024 * 1024;
constexpr quint64 PersistenceUnitBytes = 1024 * 1024;
constexpr quint64 PrepareCoreSectorBytes = 512;
constexpr quint64 PrepareCoreMaximumBytes = 1024 * 1024 - PrepareCoreSectorBytes;

struct PrepareArchiveSource
{
    QString path;
    quint64 device = 0, inode = 0, size = 0;
    QString modified, changed;
    bool present() const
    {
        return !path.isEmpty() || device != 0 || inode != 0 || size != 0
            || !modified.isEmpty() || !changed.isEmpty();
    }
};

// A file uses filesystem device/inode; a native request uses block dev_t and
// the accepted UDisks identity plus the kernel disk sequence number.
struct PrepareImageRequest
{
    int deviceOperation = 0; // Bounded selected-device command, no formatter/source job.
    QStringList espRecords;
    bool lockDevice = true; // Session Alt+, override of the kernel exclusive claim only.
    DeviceExclusions exclusions; // Original numbered settings, frozen before authorization.
    int jobPriority = DefaultJobPriority; // Original default ABOVE_NORMAL; validated optional scheduling preference.
    PrepareArchiveSource archive; // Ctrl+SELECT supplement; file-based native creation only.
    QString path;
    quint64 device = 0;
    quint64 inode = 0;
    quint64 size = 0;
    quint32 logicalSectorSize = 512; // Frozen native geometry, rechecked before writes.
    quint32 physicalSectorSize = 512;
    quint32 clusterSize = 512;
    quint32 volumeId = 0;
    QString label;
    bool isDevice = false;
    DeviceClass deviceClass = DeviceClass::Usb;
    bool allowFixedUsb = false; // Explicit job opt-in; never inferred from device facts.
    quint64 persistenceSize = 0;
    int persistenceExtVersion = 3; // Explicit UseExtVersion preference, default ext3.
    quint16 udfRevision = 0x0201; // Explicit UseUdfVersion; zero preference means the default 2.01.
    bool rawWrite = false;
    bool quickFormat = true;
    bool extendedLabel = false; // Optional autorun metadata; absent in legacy requests.
    bool ntfsCompression = false; // Session Alt+N; effective for clusters no larger than 4096.
    bool fileIndexing = false; // Saved Alt+Q; NTFS root policy or optional UDF hint.
    bool runtimeValidation = false; // ISO copy: checksum list and bundled EFI validation chain.
    bool oldBiosFixes = false; // Explicit MBR/BIOS layout choice, frozen before authorization.
    bool isoEsp = false; // GPT main ESP for a freshly validated small regular-EFI ISO.
    QString displayLabel; // Original user text, before filesystem label normalization.
    int badBlocksPattern = -1; // -1 disables; 0..4 are the original Rufus choices.
    bool detectFakes = true;
    int zeroMode = ZERO_NONE; // Whole-image DD; no formatting or customization options.
    bool nonBootable = false; // Explicit complete format job; no source or boot options.
    bool uefiNtfs = false; // Standalone loader, or the extra partition required by image policy.
    bool sfd = false; // Non-bootable whole-device FAT16/FAT32/NTFS/exFAT; mutually exclusive with MBR.
    bool mbr = false; // Non-bootable, UEFI:NTFS, FreeDOS, GRUB2 or Windows; Syslinux already implies MBR.
    // Bootable GPT/NTFS without an image is standalone UEFI:NTFS preparation.
    // It reuses the same embedded payload/layout as an NTFS ISO job.
    int fileSystem = FS_FAT32;
    bool syslinux = false; // MBR/FAT16/FAT32/NTFS; standalone v4 remains FAT-only.
    int syslinuxSelection = BT_IMAGE; // BT_SYSLINUX_V4/V6 explicitly select a source-free 4.07/6.04-pre1 job.
    bool msDos = false;
    bool reactOs = false; // Source-free MBR/FAT16/FAT32 boot records, no OS payload.
    bool kolibriOs = false; // Source-backed original Kolibri PBR route on MBR/FAT32 or NTFS.
    QString dosImagePath; // Owned fixed Microsoft resource; privately staged before authorization.
    bool freeDos = false; // Source-free MBR/FAT16 or FAT32; requires explicit DOS choices.
    bool grub2 = false; // MBR GRUB2; optional retained ISO source, otherwise a source-free boot-record job.
    bool grub4dos = false; // Standalone MBR/FAT16/FAT32/NTFS with frozen loader, or BIOS/FAT32 ISO with its own grldr.
    bool windowsBios = false; // Source-backed Windows MBR/FAT16/FAT32/NTFS, with optional enhanced dual mode.
    bool windowsToGo = false;
    int windowsToGoTarget = TT_UEFI; // Explicit firmware choice; BIOS requires MBR.
    bool windowsRufusMbr = true; // Frozen Alt+A choice; false selects original Windows 7 code.
    // Frozen boot file: required filesystem grldr for standalone GRUB4DOS,
    // or optional core for GRUB2 / the opted-in Windows NTFS/Alt+E SBR
    // route. Fresh source policy must independently select that SBR. Bytes are staged
    // privately by PrepareImageProcess; JSON carries only bounded metadata.
    // Hash fixes byte identity, not signature authenticity or module compatibility.
    QString grub2CorePath;
    quint64 grub2CoreSize = 0;
    QByteArray grub2CoreSha256;
    bool hasGrub2Core() const
    {
        return !grub2CorePath.isEmpty() || grub2CoreSize != 0 || !grub2CoreSha256.isEmpty();
    }
    QString syslinuxResourcesPath;
    quint64 syslinuxResourcesSize = 0;
    QByteArray syslinuxResourcesSha256;
    bool hasSyslinuxResources() const { return !syslinuxResourcesPath.isEmpty() || syslinuxResourcesSize != 0 || !syslinuxResourcesSha256.isEmpty(); }
    QString dosKeyboard;
    quint32 dosCodepage = 0; // Final code page from the chooser; never inferred by the helper.
    bool dosRufusMbr = true; // False selects the original Windows 7 fallback.
    bool enhancedInstall = false; // Win32 Alt+E policy; not Ctrl+Alt+E expert mode.
    quint64 diskSequence = 0;
    QString objectPath;
    QString serviceOwner;
    QStringList identity;
    QString imagePath;
    quint64 imageDevice = 0;
    quint64 imageInode = 0;
    quint64 imageSize = 0;
    QString imageModified;
    QString imageChanged;
    // Compressed DD retains stored source identity separately from decoded geometry.
    int imageCompression = BLED_COMPRESSION_NONE;
    quint64 decodedImageSize = 0;
    quint32 imageSectorSize = 0;
    QByteArray decodedImageSha256;
    QByteArray imageLayoutSha256; // Frozen VTSI extent map or FFU container identity.
    quint32 ffuStore = 0; // Explicit one-based store; legacy zero permits a single store only.
    quint64 ffuMinimumSize = 0; // Authenticated optimized FFU fit bound; decoded size stays captured.
    bool ignoreBootMarker = false; // Explicit frozen choice; never inferred.
    bool hasCompressedImage() const
    {
        return ffuStore != 0 || ffuMinimumSize != 0 || imageCompression != BLED_COMPRESSION_NONE || decodedImageSize != 0
            || imageSectorSize != 0 || !decodedImageSha256.isEmpty() || !imageLayoutSha256.isEmpty();
    }
    bool rockRidge = true;
    bool joliet = true;
    bool preserveTimestamps = false;
    bool sizeChecks = true; // Explicit MS-DOS 64-KiB override only; physical bounds stay mandatory.
    int windowsFlags = 0;
    QString windowsUsername;
    QString windowsInstallPath; // Image-internal path, never a host file to open.
    int windowsEdition = 0; // Selected native WIM index.
    WindowsRegionOptions windowsRegion;
    bool windowsSilentConfirmed = false; // All three original acknowledgements.

    bool isStandaloneUefiNtfs() const
    {
        return uefiNtfs && !syslinux && !grub2 && !grub4dos && !windowsBios && !kolibriOs && !windowsToGo;
    }

    bool isStandaloneSyslinux() const
    {
        const bool standalone = syslinuxSelection == BT_SYSLINUX_V4 || syslinuxSelection == BT_SYSLINUX_V6;
        return syslinux && standalone;
    }

    bool isDos() const { return freeDos || msDos; }
    int dosBootType() const { return msDos ? BT_MSDOS : BT_FREEDOS; }

    bool isSourceFree() const
    {
        return zeroMode != ZERO_NONE || nonBootable || isStandaloneUefiNtfs() || isStandaloneSyslinux() || isDos() || reactOs
            || ((grub2 || grub4dos) && imagePath.isEmpty());
    }

    bool usesUefiNtfs() const
    {
        if (windowsToGo) return mbr && uefiNtfs;
        const bool biosLoaderAllowsUefi = (!syslinux && !grub2 && !grub4dos && !windowsBios && !kolibriOs) || uefiNtfs;
        return !nonBootable && biosLoaderAllowsUefi
            && (fileSystem == FS_NTFS || (fileSystem == FS_EXFAT && uefiNtfs));
    }

    bool hasWindowsInstallSettings() const
    {
        const bool locales = !windowsRegion.inputLocale.isEmpty() || !windowsRegion.systemLocale.isEmpty()
            || !windowsRegion.userLocale.isEmpty();
        const bool display = !windowsRegion.uiLanguage.isEmpty() || !windowsRegion.uiLanguageFallback.isEmpty()
            || !windowsRegion.timezone.isEmpty();
        return locales || display || !windowsInstallPath.isEmpty() || windowsEdition != 0 || windowsSilentConfirmed;
    }
};

bool ValidFormatGeometry(const PrepareImageRequest& request);

enum BadBlocksDecision { BadBlocksAbort, BadBlocksRetry, BadBlocksIgnore };
struct BadBlocksReport
{
    int round = 0, error = 0;
    quint32 count = 0, readErrors = 0, writeErrors = 0, corruptionErrors = 0;
    quint64 written = 0, read = 0;
    bool threshold = false;
};
QJsonObject EncodeBadBlocksReport(const BadBlocksReport& report);
bool DecodeBadBlocksReport(const QJsonObject& object, BadBlocksReport& report);

struct PrepareImageOutcome
{
    quint64 persistenceSize = 0;
    int persistenceExtVersion = 3; // Explicit UseExtVersion preference, default ext3.
    bool rawWrite = false;
    RAW_WRITE_RESULT raw = {};
    int error = 0;
    int syncError = 0;
    int cleanupError = 0;
    bool started = false;
    bool appearanceDone = false;
    quint64 offset = 0;
    quint64 size = 0;
    quint64 filesystemBytes = 0;
    quint64 copiedBytes = 0;
    quint64 archiveBytes = 0;
    bool archiveDone = false;
    bool windowsToGoDone = false;
};

// Integers travel as decimal strings, preserving all 64 bits on Qt 6.2 too.
bool ValidatePreparePersistence(const PrepareImageRequest& request);
bool ValidatePrepareGrub2Core(const PrepareImageRequest& request);
bool ValidatePrepareCompressedImage(const PrepareImageRequest& request);
bool ValidatePrepareArchive(const PrepareImageRequest& request);
bool ValidatePrepareWindowsToGo(const PrepareImageRequest& request);
// Physical raw progress/counters include final-sector padding for compressed DD.
quint64 PrepareRawWriteBytes(const PrepareImageRequest& request);
QJsonObject EncodePrepareRequest(const PrepareImageRequest& request);
bool DecodePrepareRequest(const QJsonObject& object, PrepareImageRequest& request);
// Operation-local counters, in the units supplied by Rufus (ISO blocks for copy).
QJsonObject EncodePrepareProgress(int operation, quint64 processed, quint64 total);
bool DecodePrepareProgress(const QJsonObject& object, int& operation, quint64& processed, quint64& total);
QJsonObject EncodeRawProgress(int phase, quint64 processed, quint64 total);
bool DecodeRawProgress(const QJsonObject& object, int& phase, quint64& processed, quint64& total);
bool DecodeRawOutcome(const QJsonObject& object, PrepareImageOutcome& outcome);
QJsonObject EncodePrepareOutcome(const PrepareImageOutcome& outcome);
bool DecodePrepareOutcome(const QJsonObject& object, PrepareImageOutcome& outcome);
}

Q_DECLARE_METATYPE(rufus::PrepareImageOutcome)

Q_DECLARE_METATYPE(rufus::BadBlocksReport)
