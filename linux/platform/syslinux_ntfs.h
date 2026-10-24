// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "syslinux_resources.h"
#include "ntfs_format.h"
#include "image_source.h"
#include "iso_analyze.h"
#include "format_policy.h"

namespace rufus
{
struct NativeSyslinuxNtfsRequest
{
    int partition = -1;
    int directory = -1;
    quint64 device = 0;
    quint64 diskSequence = 0;
    NtfsFormatOptions format;
    quint64 volumeId = 0;
    QString mountPath;
    QString installer;
    int bootType = BT_SYSLINUX_V6;
    const IMAGE_FILE_SOURCE* image = nullptr;
    const char* imagePath = nullptr;
    const ISO_ANALYSIS_SOURCE* source = nullptr;
    const ISO_ANALYSIS* analysis = nullptr;
    bool rockRidge = true;
    const SyslinuxResources* resources = nullptr;
};

// BT_SYSLINUX_V6 requires a freshly formatted empty root and all four source
// fields null. BT_IMAGE requires checked shared extraction on fresh NTFS, with
// root ldlinux.sys absent. Borrow the retained image and its analysis/view with
// the same Rock Ridge setting as extraction. Reuse the FAT source policy; any
// existing root companion must match the pinned replacement. The caller owns
// extraction and copied-file verification; this is not an existing-volume API.
// Borrow the validated partition and directory descriptors. The caller owns the
// mount/claim lifecycle, authorization, device-use checks and disk layout. Use a
// dedicated child-subreaper worker, the qualified extlinux executable, and a
// caller-controlled absolute mount path. The mount owns the device claim here.
// Neither paths nor provider selection may come directly from helper requests.
// No mount, privilege acquisition or driver switching here.
//
// Success verifies identity, geometry, raw mapped files, the pinned v6 payload,
// reset ADV, companion, Windows file attributes, original BPB and backup VBR.
// Protect ldlinux.sys (read-only/hidden/system), keep ldlinux.c32 unprotected,
// and preserve other flags. Attribute errors fail instead of using POSIX modes.
// Sync the filesystem and device even after a started child fails.
// Preserve borrowed flags/positions;
// serialize the original patcher globals with other jobs. No rollback on error.
// The caller must still unmount, reacquire/check the whole disk and finish its
// final sync before READY. The helper owns that lifecycle; GUI eligibility is
// separate from this adapter's contract.
int InstallNativeSyslinuxNtfs(const NativeSyslinuxNtfsRequest& request,
    FORMAT_RESULT& result, QString& problem);
}
