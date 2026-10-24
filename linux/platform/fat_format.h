/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "format_command.h"

enum {
	FAT32_LABEL_LENGTH = 11,
	FAT32_DIAGNOSTIC_CAPACITY = FORMAT_DIAGNOSTIC_CAPACITY
};

typedef struct {
	uint64_t offset;
	uint64_t size;
	uint32_t cluster_size;
	uint32_t volume_id;
	char label[FAT32_LABEL_LENGTH + 1];
	bool full_format;
	uint32_t sector_size; /* Zero retains the 512-byte default. */
	uint32_t physical_sector_size; /* Zero retains the 512-byte default. */
} FAT_FORMAT_OPTIONS;

/* Both FAT variants use the same explicit request fields. */
typedef FAT_FORMAT_OPTIONS FAT32_FORMAT_OPTIONS;
typedef FAT_FORMAT_OPTIONS FAT16_FORMAT_OPTIONS;

typedef FORMAT_RESULT FAT32_FORMAT_RESULT;
typedef FORMAT_RESULT FAT16_FORMAT_RESULT;

#ifdef __cplusplus
extern "C" {
#endif

/* FAT16 regular files: same ownership, label, supervision
 * and flush contract as FormatFat32Image below. Clusters range from 512 to
 * 65536 bytes; geometry must stay within FAT16's cluster-count limits.
 * The root directory requests 512 entries, rounded by mkfs.fat for alignment.
 * Device formatting uses the separate partition entry point below. */
int CheckFat16FormatOptions(const FAT16_FORMAT_OPTIONS* options);
int CheckFat16ImageRequest(int fd, const char* mkfs, const FAT16_FORMAT_OPTIONS* options);
int CheckFat16Image(int fd, const FAT16_FORMAT_OPTIONS* options, FAT16_FORMAT_RESULT* result);
int FormatFat16Image(int fd, const char* mkfs, const FAT16_FORMAT_OPTIONS* options,
	FAT16_FORMAT_RESULT* result);

/* Run the formatter's existing geometry/label/descriptor checks without
 * starting a child or writing. Formatting repeats these checks. */
int CheckFat32ImageRequest(int fd, const char* mkfs, const FAT32_FORMAT_OPTIONS* options);
/* Geometry/label-only preflight, usable before the native partition exists. */
int CheckFat32FormatOptions(const FAT32_FORMAT_OPTIONS* options);
/* Read-only descriptor, bounds and formatted-volume validation. Reuses the
 * formatter's BPB/FAT metadata checks; does not flush, launch a tool or change fd. */
int CheckFat32Image(int fd, const FAT32_FORMAT_OPTIONS* options, FAT32_FORMAT_RESULT* result);
/* Same read-only FAT validation on an exclusively borrowed whole-disk block
 * descriptor. Check expected st_rdev, exact capacity, writable/buffered access
 * and requested logical/physical sectors. The FAT extent is at options.offset, not byte zero.
 * Caller owns authorization, disk sequence, partition checks and exclusivity. */
int CheckFat32Device(int fd, uint64_t device, uint64_t capacity,
	const FAT32_FORMAT_OPTIONS* options, FAT32_FORMAT_RESULT* result);

/* Same whole-disk read-only contract with FAT16 geometry and readback. */
int CheckFat16Device(int fd, uint64_t device, uint64_t capacity,
	const FAT16_FORMAT_OPTIONS* options, FAT16_FORMAT_RESULT* result);

/* Quick-format one region of an existing, exclusively borrowed REGULAR FILE.
 * The caller owns the O_RDWR descriptor, its offset and all media decisions.
 * No path is reopened in the parent; the child receives the retained descriptor.
 * Offset/size must align to the requested 512/1024/2048/4096-byte logical sectors;
 * physical sectors may be larger (512e). Use an explicit power-of-two cluster
 * size from the logical sector size through 65536 bytes. Label is already
 * normalized uppercase ASCII (at most 11 bytes); empty means no label.
 *
 * mkfs must be an absolute path to a trusted dosfstools mkfs.fat supporting
 * --offset and --mbr. Executed without a shell, with bounded diagnostics and
 * cancellation via IsRufusJobCancelled. A 120-second timeout stops the child;
 * cancellation/timeout sends TERM then KILL after 500 ms and always reaps it.
 * After any started process, flush the target even on failure/cancellation.
 * Returns 0 only after exit success, flush, size recheck and Rufus FAT-reader
 * validation of the requested geometry/label/ID. Native 4-KiB validation uses
 * bounded FAT/root metadata reads without changing shared libfat globals. Otherwise returns errno.
 * Failure after started=true leaves an incomplete region; there is no rollback.
 * The final filesystem may omit less than one 63-sector track at the end.
 * Odd sector counts use whole KiB blocks without additional track rounding;
 * mkfs.fat's partial-KiB handling can retain or omit the final sector, but the
 * validated filesystem must fit within the requested region.
 *
 * This does not create a partition table, erase old data, install a bootloader,
 * authorize a device or mount anything. Block devices are deliberately rejected.
 * Stock mkfs.fat reopens /proc/self/fd/N with O_EXCL; this conflicts with a
 * retained exclusive block descriptor. Device formatting needs a separate
 * ownership handoff, not simply relaxing the regular-file check here.
 * Serialize with other Rufus jobs using the existing libfat sector settings. */
int FormatFat32Image(int fd, const char* mkfs, const FAT32_FORMAT_OPTIONS* options,
	FAT32_FORMAT_RESULT* result);

/* Format an already validated, non-exclusive partition descriptor. Its expected
 * st_rdev and exact capacity must match. options.offset is the partition's disk
 * offset (FAT hidden sectors); reads/writes start at byte zero of this descriptor.
 * The caller must release its exclusive disk claim before this call: mkfs.fat
 * reopens the inherited /proc descriptor and acquires its own exclusive claim.
 * Reuses the image formatter's options, supervision, flush and bounded FAT readback.
 * The caller owns authorization, partition identity/extent checks and exclusive
 * reacquisition afterward. Neither function opens an arbitrary device pathname.
 * The same native FAT32 operation also accepts a whole-disk SFD descriptor with
 * offset zero and the exact disk capacity. That caller must first clear stale
 * metadata, refresh/verify the absence of kernel partitions, and revalidate the
 * whole volume after reacquiring the claim (PrepareDeviceTarget::Table::Sfd). */
int FormatFat32Partition(int fd, uint64_t device, const char* mkfs, const FAT32_FORMAT_OPTIONS* options,
	FAT32_FORMAT_RESULT* result);
/* Same partition ownership/handoff contract, with FAT16 geometry and readback. */
int FormatFat16Partition(int fd, uint64_t device, const char* mkfs, const FAT16_FORMAT_OPTIONS* options,
	FAT16_FORMAT_RESULT* result);

#ifdef __cplusplus
}
#endif
