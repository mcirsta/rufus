// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "format_command.h"
#include "format_policy.h"
#include <QString>

namespace rufus
{
struct ExfatFormatOptions
{
    quint64 size = 0;
    quint32 clusterSize = 0;
    QString label; // At most 11 UTF-16 units; preserve case and valid Unicode.
    quint64 partitionOffset = 0; // Expected sysfs/boot offset; never an I/O offset.
    bool quickFormat = true;
    quint32 sectorSize = 512;
    quint32 physicalSectorSize = 512;
};

// Quick-format an exclusively borrowed, partition-sized regular file, starting
// at byte zero. Exact capacity, O_RDWR buffered access, requested 512/1024/2048/4096-byte
// logical/physical sector geometry and
// partitionOffset=0. No file subranges, mounting, authorization or GUI integration.
// Geometry preflight assumes 1-MiB alignment (passed explicitly), one FAT,
// an unpacked bitmap and the native standard upcase table, as in exfatprogs
// 1.4.3. Reject metadata that cannot fit and oversized cluster/FAT counts.
// mkfs is a trusted absolute exfatprogs path supporting -C, -K and -P none
// (qualified with 1.4.3). Unsupported tools fail; no weaker-option retry.
// libblkid 2.39+ is required at runtime for boot-checksum validation. Earlier
// libraries remain build-compatible but formatting returns ENOTSUP before writes.
// Reuse child supervision/cancellation and sync after every started command.
// Native readback checks all metadata during mkfs; our final check uses libblkid
// plus requested geometry and matching primary/backup boot regions. It is not
// a full filesystem checker. Failure after started=true leaves partial media.
// The formatter generates its own serial number; preserve caller fd/position.
int CheckExfatFormatOptions(const ExfatFormatOptions& options);
// Read-only, bounded provider preflight, before a complete job changes the GPT.
int CheckExfatFormatTools(const QString& mkfs, QString& problem);
// Preserve Rufus's whole-disk default, filtering choices against the actual
// partition-sized file. Pure geometry only: does not check installed providers.
RUFUS_CLUSTER_SIZES NativeExfatClusterSizes(quint64 diskSize, quint64 volumeSize, quint32 sectorSize = 512, quint32 physicalSectorSize = 512);
int CheckExfatImageRequest(int fd, const QString& mkfs, const ExfatFormatOptions& options);
int FormatExfatImage(int fd, const QString& mkfs, const ExfatFormatOptions& options, FORMAT_RESULT& result);
// Recheck metadata on a retained, validated volume descriptor. Read-only; the
// caller holds the disk claim and has already validated geometry/identity.
int CheckExfatVolume(int fd, const ExfatFormatOptions& options);
// Authorized caller supplies a retained volume fd and expected dev_t. Only
// Requested 512/1024/2048/4096-byte logical sectors and 1-MiB-aligned starts. Recheck
// identity, capacity, read-only state and sysfs start before/after formatting.
// Whole-device SFD requires zero offset and absence of a sysfs partition marker;
// the caller clears metadata and removes kernel children before handoff.
// mkfs takes O_EXCL itself: caller releases the disk claim, then reacquires and
// verifies disk identity/use/table even on failure. No force option or prompts.
int FormatExfatDevice(int fd, quint64 device, const QString& mkfs,
    const ExfatFormatOptions& options, FORMAT_RESULT& result, bool wholeDevice = false);
}
