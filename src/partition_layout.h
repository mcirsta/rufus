/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <stdint.h>

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

#ifdef __cplusplus
}
#endif
