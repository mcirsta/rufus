// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "image_source.h"
#include "wim_metadata.h"
#include <QtGlobal>

namespace rufus
{
struct WindowsApplyPlan
{
    RUFUS_WIM_METADATA metadata = {};
    quint64 bytes = 0, files = 0, directories = 0, hardLinks = 0;
    quint32 efsClusterLimit = 0; // Zero when encrypted streams impose no hole alignment.
    int wimError = 0;
};

struct WindowsApplyResult
{
    WindowsApplyPlan plan;
    quint64 completedBytes = 0, totalBytes = 0;
    bool started = false;
    int wimError = 0, syncError = 0;
};

// Inspect one native WIM index without target access. The open WIM may use our
// retained ordinary-file or ISO/UDF reader. Check required resources, count
// each hardlinked inode once, validate encrypted streams and reject device files
// before writes. Encrypted sparse holes can restrict the target cluster size.
// Return errno; preserve the native reader error separately. Initialize wimlib
// for UTF-8 before use and serialize this handle through the complete call.
int ReadWindowsApplyPlan(WIMStruct* wim, int image, WindowsApplyPlan& plan);

// Apply to a freshly formatted, unmounted NTFS volume through a borrowed R/W
// descriptor. The helper owns acquisition, source/backing protection and layout
// checks; this function performs no discovery, mounting or authorization.
// source/path must be the retained outer WIM or ISO backing this handle. Keep
// its complete reader chain alive. Recheck source identity around native work
// and in progress callbacks; never reopen its original pathname for reading.
// The offline NTFS backend preserves ACLs, streams, links, attributes and
// reparse points. Partial failure/cancellation has no rollback. Flush even on
// failure and retain sync errors; full READY belongs to the caller after all
// boot setup, filesystem cleanup and helper exit have succeeded.
int ApplyWindowsImage(WIMStruct* wim, int image, const IMAGE_FILE_SOURCE& source,
    const char* sourcePath, int target, quint64 device, quint64 size,
    WindowsApplyResult& result);
}
