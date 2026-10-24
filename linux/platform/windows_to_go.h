// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "windows_apply.h"
#include "windows_bcd.h"
#include "windows_install_options.h"
#include "partition_image.h"
#include "iso_analyze.h"
#include "iso_file.h"
#include "windows_image.h"
#include <QStringList>

namespace rufus
{
class PrepareIsoSource;
struct PrepareImageRequest;

// Retain the complete ordinary WIM/ESD or embedded ISO/UDF reader chain through
// apply. The outer source stays owned by the caller. No pathname reopen for
// source bytes, target access, authorization or host-setting inference.
class WindowsToGoImage
{
public:
    WindowsToGoImage() = default;
    ~WindowsToGoImage();
    WindowsToGoImage(const WindowsToGoImage&) = delete;
    WindowsToGoImage& operator=(const WindowsToGoImage&) = delete;
    int open(const PrepareImageRequest& request, const PrepareIsoSource& source, bool deployment = true);
    void close();
    WIMStruct* handle() const { return wim_; }
    WindowsApplyPlan plan;
    WindowsInstallOptions editions;
    int architecture = ARCH_UNKNOWN, wimError = 0;
    bool bios = false, uefi = false;
    QStringList archives;
    QString archive;
    RUFUS_IMG_REPORT report = {};

private:
    WIMStruct* wim_ = nullptr;
    ISO_ANALYSIS_SOURCE view_ = {};
    ISO_FILE_READER reader_ = {};
};

// After the helper creates and validates its table, bind BCD to the actual disk
// and partition IDs. Borrow the claimed disk; preserve flags/position. The
// helper supplies the signature it generated for MBR and keeps the table fixed
// through apply/boot setup. GPT's Windows/ESP partitions are numbers 3/1.
int ReadWindowsToGoBootIdentity(int disk, const PrepareImageRequest& request,
    quint32 signature, WindowsBcdOptions& options);

// Initial SELECT report: image 1 metadata and EFI architecture, as on Windows.
// Read the borrowed ordinary WIM/ESD through its descriptor, never as an ISO/DD
// image. This reports eligibility, not deployment fit or a selected edition.
int ScanWindowsStandaloneImage(const IMAGE_FILE_SOURCE& source, const char* path,
    ISO_ANALYSIS& analysis, WINDOWS_IMAGE_ANALYSIS& windows);
}
