/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "iso_file.h"
#include "image_source.h"

typedef struct {
	uint64_t reserved_bytes;
	unsigned parts;
} ISO_WIM_SPLIT_PLAN;

typedef struct {
	uint64_t bytes;
	uint64_t copied_blocks;
	unsigned parts;
} ISO_WIM_SPLIT_RESULT;

typedef int (*ISO_WIM_PART_READY)(void* context, int stage, const char* name);

#ifdef __cplusplus
extern "C" {
#endif

/* Read-only checks and a conservative output reservation. Ask upstream to plan
 * the split, aborting at its first BEGIN_PART before any output open/write.
 * Reserve the source size plus per-part overhead (at least Rufus's 2 MiB
 * headroom). This is a budget, not an exact output-size or allocation prediction.
 * Actual writes must fit it as well as FAT32's individual file-size limit. */
int PlanIsoWimSplit(ISO_FILE_READER* reader, ISO_WIM_SPLIT_PLAN* plan);

/* Caller owns the reader chain and a private destination directory. Writes
 * install.swm, install2.swm, ... using upstream wimlib and Rufus's 4094 MiB
 * target. Stage on the destination filesystem, then publish without replacing
 * existing names. Failure removes only this call's files; cleanup errors are
 * reported and the caller must discard the incomplete target. Returns errno.
 * Initialize wimlib before calling. On failure only copied_blocks is retained,
 * so the final progress update does not go backwards. Progress uses
 * Rufus's compressed-resource count, offset by previously copied ISO blocks. */
int SplitIsoWim(ISO_FILE_READER* reader, const IMAGE_FILE_SOURCE* source,
	const char* image, int parent, uint64_t copied_blocks, uint64_t total_blocks,
	ISO_WIM_SPLIT_RESULT* result, ISO_WIM_PART_READY part_ready, void* part_context);

#ifdef __cplusplus
}
#endif
