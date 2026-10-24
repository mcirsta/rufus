/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <stdint.h>

// Sectors to zap when clearing the MBR/GPT (must be >34).
#define MAX_SECTORS_TO_CLEAR (8 * 1048576LL / 512)

#ifdef __cplusplus
extern "C" {
#endif

/* Rufus placement arithmetic, independent of GUI controls and OS disk APIs.
 * legacy_bios means the MBR-only "Fixes for old BIOSes" option is active.
 * A zero cluster size retains the original 512-byte fallback. Zero return
 * denotes invalid/overflowing geometry or no room for a main partition. */
uint64_t GetPartitionStartOffset(uint32_t sector_size, uint32_t sectors_per_track,
	uint32_t cluster_size, bool legacy_bios);
uint64_t GetMainPartitionSize(uint64_t first_offset, uint64_t last_offset,
	uint32_t sector_size, uint32_t sectors_per_track, uint32_t cluster_size);

/* Extra partitions at the end of a Rufus disk are rounded up to tracks, then
 * placed backwards from last_offset (exclusive, after reserving GPT metadata).
 * Zero denotes invalid/overflowing geometry or no room for the partition. */
uint64_t GetTrailingPartitionSize(uint64_t requested_size,
	uint32_t sector_size, uint32_t sectors_per_track);
uint64_t GetTrailingPartitionOffset(uint64_t last_offset, uint64_t size,
	uint32_t sector_size, uint32_t sectors_per_track);

#ifdef __cplusplus
}
#endif
