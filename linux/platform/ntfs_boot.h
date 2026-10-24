// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "ntfs_format.h"

namespace rufus
{
struct NtfsBootResult
{
    quint64 bytesWritten = 0;
    int syncError = 0;
};

// Apply Rufus's original standard NTFS PBR to an already-formatted partition.
// This executable BIOS payload requires 512-byte logical sectors; native 4-KiB
// data/UEFI formatting is independent of this installer. Refuse before writing.
// The borrowed descriptor starts at the partition, not the whole disk; a block
// caller retains its exclusive whole-disk claim and verifies the partition
// identity. No formatter, mount, privilege request or backup-sector rewrite.
// Flush partial writes on failure/cancellation and read back the original
// 4,136-byte NTFS boot record, leaving its backup sector unchanged.
int InstallNtfsBootRecord(int fd, quint64 device, const NtfsFormatOptions& options,
    NtfsBootResult& result);
}
