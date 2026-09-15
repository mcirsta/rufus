/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stdbool.h>
#include <stdint.h>

enum {
	FAT32_LABEL_LENGTH = 11,
	FAT32_DIAGNOSTIC_CAPACITY = 4096
};

typedef struct {
	uint64_t offset;
	uint64_t size;
	uint32_t cluster_size;
	uint32_t volume_id;
	char label[FAT32_LABEL_LENGTH + 1];
} FAT32_FORMAT_OPTIONS;

typedef struct {
	bool started;
	int exit_code;
	int signal;
	int sync_error;
	uint64_t filesystem_bytes;
	char diagnostics[FAT32_DIAGNOSTIC_CAPACITY];
} FAT32_FORMAT_RESULT;

#ifdef __cplusplus
extern "C" {
#endif

/* Run the formatter's existing geometry/label/descriptor checks without
 * starting a child or writing. Formatting repeats these checks. */
int CheckFat32ImageRequest(int fd, const char* mkfs, const FAT32_FORMAT_OPTIONS* options);
/* Geometry/label-only preflight, usable before the native partition exists. */
int CheckFat32FormatOptions(const FAT32_FORMAT_OPTIONS* options);

/* Quick-format one region of an existing, exclusively borrowed REGULAR FILE.
 * The caller owns the O_RDWR descriptor, its offset and all media decisions.
 * No path is reopened in the parent; the child receives the retained descriptor.
 * Offset/size must be multiples of 512, with 512-byte logical sectors and an
 * explicit power-of-two cluster size from 512 through 32768. Label is already
 * normalized uppercase ASCII (at most 11 bytes); empty means no label.
 *
 * mkfs must be an absolute path to a trusted dosfstools mkfs.fat supporting
 * --offset and --mbr. Executed without a shell, with bounded diagnostics and
 * cancellation via IsRufusJobCancelled. A 120-second timeout stops the child;
 * cancellation/timeout sends TERM then KILL after 500 ms and always reaps it.
 * After any started process, flush the target even on failure/cancellation.
 * Returns 0 only after exit success, flush, size recheck and Rufus FAT-reader
 * validation of the requested geometry/label/ID. Otherwise returns errno.
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
 * Reuses the image formatter's options, supervision, flush and libfat readback.
 * The caller owns authorization, partition identity/extent checks and exclusive
 * reacquisition afterward. Neither function opens an arbitrary device pathname. */
int FormatFat32Partition(int fd, uint64_t device, const char* mkfs, const FAT32_FORMAT_OPTIONS* options,
	FAT32_FORMAT_RESULT* result);

#ifdef __cplusplus
}
#endif
