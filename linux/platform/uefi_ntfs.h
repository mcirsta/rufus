/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "partition_image.h"
#include "raw_write.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Write the caller's retained, trusted bundled res/uefi/uefi-ntfs.img to an
 * existing GPT file or authorized block descriptor with main/payload partitions
 * and optional intervening persistence. The source
 * size must match the plan. Native requests provide expected dev_t/capacity;
 * the caller keeps its whole-disk claim, mount/use checks and disk identity.
 * Verify the stored GPT before writing and after copy/flush/readback. Reuse
 * the raw writer's cancellation and partial-write reporting; no rollback.
 * Preserve the main partition, GPT, gaps and padding beyond the payload.
 * Caller owns source and target exclusively until return. No opening devices,
 * resource installation, formatting, mounting or authorization here. */
int WriteUefiNtfsImage(const IMAGE_FILE_SOURCE* source, const char* source_path,
	int target_fd, const GPT_IMAGE_OPTIONS* options, RAW_WRITE_RESULT* result);

/* Same retained-source, bounded-copy and revalidation contract for the MBR
 * standalone layout. Preserve every table entry, data, gaps and payload padding.
 * No device/helper route is enabled by this low-level writer alone. */
int WriteMbrUefiNtfsImage(const IMAGE_FILE_SOURCE* source, const char* source_path,
	int target_fd, const MBR_IMAGE_OPTIONS* options, RAW_WRITE_RESULT* result);

#ifdef __cplusplus
}
#endif
