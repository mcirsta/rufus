// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "format_command.h"
#include <QString>

namespace rufus
{
constexpr quint32 UdfDefaultCluster = 0x100; // Rufus's selectable "Default" sentinel.
struct UdfFormatOptions
{
    quint64 size = 0;
    QString label;
    quint16 revision = 0x0201;
    bool quickFormat = true;
    quint32 sectorSize = 512;
    quint32 physicalSectorSize = 512;
    quint32 blockSize = 0; // Zero selects the device logical sector size.
};

// Native random-access UDF with 512/1024/2048/4096-byte logical blocks.
// mkudffs requires UDF blocks to match the device logical sector size; a 512e
// physical sector does not change that size. Its block count is UINT32_MAX,
// so refuse larger volumes instead of accepting mkudffs's silent truncation.
// mkudffs 2.3 supports these
// non-VAT revisions: 1.02, 1.50, 2.00 and 2.01. Later metadata-partition
// revisions remain separate provider work; refuse them before target writes.
int CheckUdfFormatOptions(const UdfFormatOptions& options);
int CheckUdfFormatTools(const QString& mkfs, const UdfFormatOptions& options, QString& problem);
// Read back recognition, required anchors, main/reserve descriptor sequences,
// requested label/revision, physical extent, closed integrity and root ICB.
// These checks are bounded metadata verification, not a general UDF fsck.
int CheckUdfVolume(int fd, const UdfFormatOptions& options);
// Borrow a partition-sized O_RDWR descriptor at byte zero. No mounting,
// authorization, pathname reopen of the target or claim release. Full format
// reuses the existing bounded zero/write/readback engine before metadata.
int FormatUdfImage(int fd, const QString& mkfs, const UdfFormatOptions& options, FORMAT_RESULT& result);
int FormatUdfDevice(int fd, quint64 device, const QString& mkfs,
    const UdfFormatOptions& options, FORMAT_RESULT& result);
}
