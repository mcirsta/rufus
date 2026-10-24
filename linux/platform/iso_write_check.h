// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "iso_extract.h"
#include "syslinux_resources.h"
#include "partition_image.h"
#include "qt/prepare_image_protocol.h"
#include "image_policy.h"
#include "windows_image.h"
#include "windows_install_options.h"
#include <sys/types.h>
#include <QStringList>

namespace rufus
{
struct IsoWriteCheck
{
    ISO_ANALYSIS analysis = {};
    WINDOWS_IMAGE_ANALYSIS windows = {};
    QString windowsInstallPath;
    WindowsInstallOptions windowsInstall;
    int windowsInstallError = 0; // Optional edition read: wimlib error, not errno.
    QStringList windowsArchives;
    quint64 windowsApplyBytes = 0;
    int existingPartitions = 0; // GUI preflight warning count; separate from the new layout.
    quint8 extraPartitions = 0;
    bool splitInstallWim = false; // Freshly derived from policy and source, not a client decision.
    bool legacyWinPe = false; // BIOS setup family validated for post-extraction preparation.
    IMAGE_WRITE_CHOICES choices = {};
    GPT_IMAGE_RESULT partition = {};
    ISO_EXTRACT_RESULT tree = {};
    quint64 archiveBytes = 0, archiveFiles = 0;
    QString efiPath;
    SyslinuxResources syslinuxResources;
    QByteArray windowsEfiRepair; // Owned, source-checked payload; never supplied by the client.
    QString problem;
};

// Read-only preflight for GPT/FAT16/FAT32 or GPT/NTFS + UEFI:NTFS,
// or explicit MBR/FAT16/FAT32/NTFS with the qualified native Syslinux pair.
// GRUB2 ISO requests also get read-only BIOS/MBR checks using shared SBR policy,
// filesystem/partition policy and extraction checks, without requiring an EFI
// application. This does not validate core/module compatibility or enable the
// helper write route. NTFS BIOS images explicitly reserve UEFI:NTFS when shared
// image policy requires it. Windows BIOS PBR requests require BOOTMGR or a
// qualified legacy setup family. Ordinary creation does not require an install
// archive or a recognized Windows version. FAT16/FAT32/NTFS follow the original
// filesystem and primary/secondary loader dispatch. UEFI/dual paths validate the fallback EFI
// application. The original HAS_WIN7_EFI source-content case reads its image-1
// repair payload instead, retaining owned bytes across copying/WIM splitting.
// Qualified native Windows BIOS/EFI paths are exposed in Qt; this function
// itself performs no writes.
// CapturePrepareSource must first capture the caller's image identity. The
// request supplies the proposed target size, cluster size and normalized label;
// this function never opens the target, launches a process or authorizes writes.
// caller is the image owner's UID: getuid() in the GUI, the validated PKEXEC_UID
// in the authorized helper. Never substitute the helper's root identity.
// Initialize wimlib before workers and run under RufusJob's normal
// serialization/cancellation guard.
//
// Checks the retained source, shared ISO policy, selected tree/config support,
// filesystem limits, optional Windows install metadata and fallback EFI headers.
// Eligible Windows images also return owned edition/Setup-language data from
// the same source view and first install archive used by the version scan.
// An edition-read error is recorded separately and does not prevent ordinary
// creation. Cancellation/source changes still fail preflight. No edition is chosen.
// A silent request must name the sole conventional install.wim/esd archive and
// an index found in this fresh metadata; it must supply explicit regional values
// and confirmation of all three warnings. Other archive layouts remain available
// for ordinary creation. Setup language always comes from this source read.
// Enhanced installation follows Win32's separate Alt+E preference. For FAT32,
// the shared single-large-WIM rule and a sole conventional install.wim select
// splitting; the helper must use this fresh decision, never a client split flag.
// tree.required_bytes includes a conservative WIM split reservation checked
// against the partition and any additional EFI repair bytes. Extraction
// rechecks actual free space after mounting.
// Success establishes these prerequisites, not bootability, Secure Boot trust,
// full allocation fit, installed native-tool availability or permission to write.
// Analysis/paths remain diagnostic on failure. The caller must test supported
// payload behavior and revalidate the accepted source/settings/device before execution.
// Optional selected view must start empty. On success the caller owns/closes
// it, allowing the installer to reuse preflight's source without another scan.
// The backend may explicitly allow legacy WinPE: fresh BIOS source facts select
// the setup checks below, with no fallback on failure. UEFI-only jobs never run
// SetupWinPE. Default callers retain the legacy opt-in.
int CheckIsoWriteImage(const PrepareImageRequest& request, uid_t caller, IsoWriteCheck& result,
    ISO_ANALYSIS_SOURCE* retainedView = nullptr, bool allowLegacyWinPe = false);

// Read-only source preflight before the GRUB version lookup. Apply the same
// retained-source, contents, extraction and target-layout checks as above, but
// permit an unselected core for an Alt+E Windows/WinPE or ReactOS GRUB2 SBR.
// No core version is inferred or payload accepted here. Before writing, the
// helper must still call CheckIsoWriteImage with the frozen core request.
int CheckIsoWriteSource(const PrepareImageRequest& request, uid_t caller, IsoWriteCheck& result,
    ISO_ANALYSIS_SOURCE* retainedView = nullptr, bool allowLegacyWinPe = false);

// Read-only legacy WinPE preflight using the same retained-source, analysis,
// extraction and partition checks. Supports original eligible MBR/BIOS
// FAT16/FAT32/NTFS settings with a complete setup family, known options and no
// modern customization. BOOTMGR EFI, optional install archives and mixed loaders
// retain their original primary/secondary dispatch. Checks unique bounded inputs, retains an existing root
// detector even with different content, requires loader equality after one
// shared patch pass per input (unmodified equality for keep-loader), and edits
// the selected root SIF's own content. Ordinary prepatched/partial inputs can
// match; neither equality nor detector retention authenticates an executable.
// A complete i386/AMD64 family with partial minint files follows the shared
// keep-loader plan when /minint is known; partial minint alone is insufficient.
// Syslinux takes primary installation precedence unless Alt+E selects Windows
// PBR installation. Shared GRUB SBR precedence is checked independently; a core
// request must match that selection. Ordinary extraction patches/creates configs
// before WinPE preparation. Optional EFI repair remains source checked.
// tree.required_bytes reserves full prepared/edit stages. Unknown Windows
// version remains unknown. Source ownership/job guard/retained-view rules above
// apply. CheckIsoWriteImage can explicitly opt in to the same legacy checks;
// Qt's selection predicate remains separate. No client legacy masks are accepted.
int CheckLegacyWinPeImage(const PrepareImageRequest& request, uid_t caller, IsoWriteCheck& result,
    ISO_ANALYSIS_SOURCE* retainedView = nullptr);

// Read-only Windows 7-style EFI repair lookup, using the original HAS_WIN7_EFI
// source-content trigger (not a version guess). Return errno; success supplies
// owned x64 EFI application bytes from Windows/Boot/EFI/bootmgfw.efi in image 1
// of the first install archive. No substitute file/image/archive is attempted.
// Borrow the retained ISO/UDF view under the normal initialized/serialized WIM
// job guard; caller owns source-change checks. ReadWimFile's 16-MiB bound applies.
// Payload is cleared on failure. This neither installs the fallback file nor
// enables additional GUI/backend routes; those require separate qualification.
int ReadWindowsEfiRepair(const ISO_ANALYSIS_SOURCE& source, const RUFUS_IMG_REPORT& report,
    QByteArray& payload, QString& problem);

// Shared by the input dialog, controller and fresh helper preflight. These check
// already-read metadata; the preflight functions above revalidate the source itself.
// Callers must opt in to ordinary BOOTMGR sources of any version; this predicate alone
// does not validate a selected filesystem/target or EFI application.
// Legacy selection shares the setup plan and report checks with preflight;
// actual file contents, settings and target limits are still checked at START.
// A BIOS root BOOTMGR remains a candidate with its report flag preserved;
// preflight must compare it with the selected directory SETUPLDR before writes.
// Alt+E candidate checks account for Syslinux primary suppression and selected
// GRUB SBRs. Filesystem eligibility, retained files and payloads still require
// fresh preflight; no version or loader authenticity is inferred.
bool IsLegacyWinPeImage(const ISO_ANALYSIS& analysis, bool allowDualBios = false,
    bool allowGrub2Sbr = false, bool allowGrub4dosSbr = false);
bool IsWindowsBiosImage(const ISO_ANALYSIS& analysis, const WINDOWS_IMAGE_ANALYSIS& windows,
    bool allowOlderInstallers = false, bool allowLegacyWinPe = false, bool allowDualBios = false,
    bool allowGrub2Sbr = false, bool allowGrub4dosSbr = false);
int CheckWindowsSilentInstallSource(const IsoWriteCheck& checked, QString& problem);
int CheckWindowsInstallSelection(const PrepareImageRequest& request, const IsoWriteCheck& checked, QString& problem);
}
