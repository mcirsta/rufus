// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QString>
#include <QByteArray>
#include <cstdint>
#include "msdos_image.h"

namespace rufus
{
struct FreeDosCopyResult
{
    unsigned files = 0;
    quint64 bytesWritten = 0;
    int syncError = 0;
    QString path; // Current/failed file, empty on success.
};

// Copy the 27 original resources into an exclusively borrowed, empty directory
// on the caller's private kernel FAT mount. Retain root ownership and position.
// Require real FAT attributes: COMMAND.COM/KERNEL.SYS get hidden+system; locale
// support files remain ordinary files. The same shared manifest drives Windows.
//
// Returns errno; success includes payload/attribute readback and file/directory
// fsync. On error, completed/partial output remains, is flushed, and must be
// treated as incomplete. syncError preserves a flush error after a prior error.
// No source path, overwrite, mount, privilege request, locale generation, boot
// installation or READY notification. Caller owns final filesystem/device sync,
// unmount and helper exit. Emits the original five discrete copy milestones;
// the caller owns the preceding sixth slot and job progress/completion state.
int CopyFreeDosFiles(int root, FreeDosCopyResult& result);
int CopyMsDosFiles(int root, const QVector<MsDosFile>& files, FreeDosCopyResult& result);
int WriteDosStartupFiles(int root, const char* keyboard, uint32_t codepage, bool freedos, FreeDosCopyResult& result);

// Reuse the bounded exclusive file writer/readback and file/directory fsync for
// a retained standalone grldr. Require an empty private FAT or NTFS directory;
// caller has verified the mount/device/provider (including NTFS-3G FUSE).
// No DOS attributes or startup files. Caller owns sync/cleanup and READY.
int CopyGrub4dosLoader(int root, const QByteArray& loader, FreeDosCopyResult& result, bool ntfsCompression = false);

// Publish CONFIG.SYS (when needed) then AUTOEXEC.BAT after payload copying on
// the same privately owned FAT directory. Uses the shared Rufus DOS selection,
// fallback rules and writers; keyboard/codepage are explicit DOS inputs, not
// XKB names or a Linux locale. NULL/unknown inputs use the shared fallback.
// Neither startup name may already exist, including the skipped US/437 CONFIG.
// Reuses exclusive creation, sync and readback; retains partial files on error.
// result describes this operation only. No extra progress slot or host query.
int WriteFreeDosStartupFiles(int root, const char* keyboard, uint32_t codepage, FreeDosCopyResult& result);
}
