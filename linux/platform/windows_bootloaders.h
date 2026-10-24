// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <wimlib.h>
#include <QByteArray>
#include <QStringList>
#include "windows_bcd.h"

namespace rufus
{
// Borrow the selected Setup image before any WIM update. Copy Rufus's CA 2023
// payloads into the helper's private, case-insensitive FAT/NTFS mount. Return
// errno values. Each replacement is atomic; the complete set is not. The
// caller retains responsibility for final flushing/unmounting on every exit.
int ApplyWindowsCa2023Bootloaders(WIMStruct* wim, int image, const char* directory, QStringList* modified = nullptr);

// Install bytes validated by fresh ReadWindowsEfiRepair preflight, after normal
// image copying. Create EFI/BOOT in the stable private case-insensitive mount;
// refuse links/non-files, replace atomically and check file sync. Return errno.
// Caller owns final filesystem/device sync and mount cleanup even on failure.
int InstallWindowsEfiRepair(const QByteArray& payload, const char* directory);

struct WindowsToGoBootOptions
{
    WindowsBcdOptions bcd;
    int architecture = 0;
    bool bios = false, uefi = true, ca2023 = false;
};

struct WindowsBootFilesResult
{
    quint64 files = 0, bytes = 0;
    QString path;
};

// Build the complete boot-file tree in an empty private staging directory,
// borrowing the applied Windows volume's stable private mount. This permits
// unmounting NTFS before mounting an ESP in the same authorized helper. Stage
// the image's PCAT/EFI, fonts/resources, generated BCD and architecture-specific
// removable EFI entry. Reject links/special nodes and crossing source mounts.
// Failure can leave a partial private stage; never install it after failure.
int StageWindowsToGoBootFiles(int windowsDirectory, int stageDirectory,
    const WindowsToGoBootOptions& options, WindowsBootFilesResult& result);

// Copy a successful private stage to the helper's checked system filesystem.
// File replacement is atomic and synced; the complete tree has no rollback.
// Caller owns destination eligibility, DOS attributes, final filesystem/device
// sync, private-mount cleanup and helper exit before READY.
int InstallWindowsToGoBootFiles(int stageDirectory, int destination,
    WindowsBootFilesResult& result);
// Protect the newly installed BIOS manager/Boot directory/BCD and EFI BCD.
// Support the checked NTFS kernel/FUSE and FAT private mounts, preserve unrelated
// DOS flags, verify readback and sync. Caller still owns final cleanup/join.
int SetWindowsToGoBootAttributes(int directory, bool bios, bool uefi);
}
