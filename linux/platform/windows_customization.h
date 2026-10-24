// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "unattend.h"
#include <QStringList>

namespace rufus
{
struct PrepareImageRequest;
constexpr int WindowsOemFlags = UNATTEND_NO_ONLINE_ACCOUNT | UNATTEND_SET_USER |
    UNATTEND_NO_DATA_COLLECTION | UNATTEND_DISABLE_BITLOCKER | UNATTEND_QOL_ENHANCEMENTS |
    UNATTEND_APPLY_SKUSIPOLICY | UNATTEND_FORCE_S_MODE;
constexpr int WindowsCustomizationFlags = WindowsOemFlags | UNATTEND_SECUREBOOT_TPM_MINRAM |
    UNATTEND_USE_MS2023_BOOTLOADERS;
constexpr int WindowsToGoCustomizationFlags = UNATTEND_OFFLINE_INTERNAL_DRIVES |
    UNATTEND_NO_ONLINE_ACCOUNT | UNATTEND_SET_USER | UNATTEND_NO_DATA_COLLECTION |
    UNATTEND_QOL_ENHANCEMENTS | UNATTEND_USE_MS2023_BOOTLOADERS | UNATTEND_FORCE_S_MODE;

int CheckWindowsCustomizationSettings(int flags, const QString& username);
// Also validate the explicit silent-install fields and acknowledgement. The
// caller must still re-read archive/edition eligibility before target writes.
int CheckWindowsCustomizationSettings(const PrepareImageRequest& request);
int CheckWindowsCustomizationImage(const RUFUS_IMG_REPORT& report, int flags);
int CheckWindowsToGoCustomizationSettings(int flags, const QString& username);

// Apply accepted To Go options to the stable private NTFS mount. Write the
// shared answer to Windows/Panther without replacing an existing answer. The
// selected offline-internal-drives policy edits this image's SYSTEM hive using
// Select/Default and preserves its inode, ACLs, named streams and hard links.
// CA2023 boot-file selection is handled by StageWindowsToGoBootFiles, not here.
// Selected S-mode is applied natively to Control/CI/Policy/SkuPolicyRequired in
// the same offline SYSTEM hive, using this image's next-boot control set.
// Caller revalidates source/image eligibility and owns final sync/unmount/join.
// Partial hive writes have no rollback; return failure and still clean up.
int ApplyWindowsToGoCustomization(const char* directory, int arch, int flags,
    const QString& username);

// After successful ISO extraction into the helper's private, stable FAT/NTFS
// mount (case insensitive), apply Rufus's selected installation customizations.
// arch/build come from the helper's own image analysis. Initialize wimlib with
// ASSUME_UTF8 | DEFAULT_CASE_INSENSITIVE before any use. This function does not
// mount, authorize or execute commands. Never follow links in target paths.
// Existing OEM answers are preserved. Upgrade files retain Rufus's backups;
// WIM failures do not promise rollback. Always flush/unmount, even on failure.
int ApplyWindowsCustomization(const char* directory, int arch, uint32_t build,
    int flags, const QString& username, QStringList* modified = nullptr);

// Request adapter: Setup language and arch/build come from fresh source analysis,
// never the client. Own the UTF-8 fields for the shared writer's complete call.
int ApplyWindowsCustomization(const char* directory, int arch, uint32_t build,
    const PrepareImageRequest& request, const QString& setupLanguage, QStringList* modified = nullptr);

// Native backend entry for fully prepared options, including silent installs.
// The caller must first validate image eligibility and selected archive/edition
// against the retained source, read the Setup language, and obtain the original
// three acknowledgements. This checks input values, not those prerequisites.
// Strings are borrowed for this call; username is mutable as in WriteUnattendXml.
// DUPLICATE_LOCALE here only instructs the shared writer to emit the supplied
// explicit fields. No host settings are read.
int ApplyWindowsCustomization(const char* directory, uint32_t build, RUFUS_UNATTEND_OPTIONS& options,
    QStringList* modified = nullptr);
}
