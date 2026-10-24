// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "syslinux_resources.h"
#include "fat_format.h"
#include "image_source.h"
#include "iso_analyze.h"
#include "partition_image.h"
#include <QString>

namespace rufus
{
// Fixed installed paths shared by GUI preflight and the authorized helper.
// Availability is not authorization or a substitute for the helper's recheck.
// Unsupported selections return nullptr; image jobs use the qualified v6 provider.
const char* NativeSyslinuxInstaller(int bootType, int fileSystem = FS_FAT32);
int CheckNativeSyslinuxTools(QString& problem, int bootType = BT_IMAGE, int fileSystem = FS_FAT32);

struct NativeSyslinuxRequest
{
    int target = -1;
    FAT_FORMAT_OPTIONS format = {};
    int fileSystem = FS_FAT32;
    // BT_SYSLINUX_V4/V6 select Rufus's embedded 4.07/6.04-pre1 explicitly.
    // Standalone requests must leave all four image/source fields null.
    int bootType = BT_IMAGE;
    const IMAGE_FILE_SOURCE* image = nullptr;
    const char* imagePath = nullptr;
    const ISO_ANALYSIS_SOURCE* source = nullptr;
    const ISO_ANALYSIS* analysis = nullptr;
    bool rockRidge = true;
    const SyslinuxResources* resources = nullptr;
    QString installer;
    QString toolsDirectory;
    QString temporaryRoot;
    // Null keeps regular-file-only operation. Native mode requires the exact
    // prepared MBR layout and kernel disk sequence captured by the caller.
    const MBR_IMAGE_OPTIONS* deviceLayout = nullptr;
    quint64 diskSequence = 0;
};

// Install on an exclusively borrowed, newly formatted FAT16/FAT32
// target. BT_IMAGE requires the checked shared extractor's output (root
// ldlinux.sys absent) and analysis/source belonging to the retained image, using
// the same Rock Ridge option. Caller owns extraction and copied-file checks.
// BT_SYSLINUX_V4/V6 require a freshly formatted empty root (optional volume label
// followed by the end marker). They install only the loader (plus companion for
// v6), with no configuration or OS.
// Native mode borrows the whole-disk O_EXCL descriptor after successful private
// unmount and claim reacquisition. Keep this claim throughout the installer and
// readback; the native installer/mtools reopen it without another exclusive claim.
// No arbitrary existing FAT volume, mounted target or concurrent job. Caller owns
// authorization, device-use/source-on-target checks and the complete job lifetime.
//
// Run in a dedicated child-subreaper worker with no other child reaper. installer
// is the matching trusted CP12a (v6) or CP20e (v4) executable; toolsDirectory
// contains trusted mcopy/mattrib. A mismatched provider fails readback after writes.
// temporaryRoot is an absolute, caller-controlled directory. Nothing is searched
// in the user's PATH and no privileges are acquired here.
//
// Returns errno and a diagnostic. A started command means the image may have been
// changed even on failure; no rollback. Success includes whole-group exit, sync,
// source/target rechecks, exact boot-file/VBR verification and private-dir removal.
// It does not certify all copied files or bootability, install an MBR, or enable
// BIOS creation in Qt. Preserve fd position/flags; serialize the Rufus FAT/patcher
// globals with other jobs, as the existing formatter does.
int InstallNativeSyslinux(const NativeSyslinuxRequest& request,
    FORMAT_RESULT& result, QString& problem);
}
