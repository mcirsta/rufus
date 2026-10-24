// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "format_command.h"
#include "format_policy.h"
#include <QString>

namespace rufus
{
// Win32 reserves the final byte of the 16-byte volume name for a terminator.
QString NormalizeExtFormatLabel(const QString& text);

struct ExtFormatOptions
{
    quint64 size = 0; // Partition-sized file, with I/O starting at byte zero.
    int fileSystem = FS_EXT2;
    QString label; // UTF-8, at most 16 bytes in the ext superblock.
    bool persistenceConf = false; // Seed and verify the Debian / union file.
    bool quickFormat = true;
    quint32 sectorSize = 512;
    quint32 physicalSectorSize = 512;
};

// Borrow an O_RDWR descriptor of exact size; keep its flags and position.
// The trusted absolute mke2fs path receives only fd 3, no shell.
// Reuse formatter cancellation, bounded output, child reaping and sync even
// after a failed command. Check the resulting native superblock before success,
// including e2fsprogs's exact small-final-group reduction. A 4-KiB logical
// sector requires at least a 4-KiB filesystem block for Linux mounting. Optional Debian
// persistence.conf is populated from a private seed and independently read back.
// The device form requires the caller to verify and reacquire its disk claim.
// Neither form mounts a volume.
int CheckExtFormatOptions(const ExtFormatOptions& options);
int FormatExtImage(int fd, const QString& mke2fs, const ExtFormatOptions& options, FORMAT_RESULT& result);
int FormatExtDevice(int fd, quint64 device, const QString& mke2fs,
    const ExtFormatOptions& options, FORMAT_RESULT& result);
}
