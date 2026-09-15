/*
 * Rufus: The Reliable USB Formatting Utility
 * Partition placement arithmetic extracted from drive.c
 * Copyright © 2011-2026 Pete Batard <pete@akeo.ie>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "partition_layout.h"

uint64_t GetPartitionStartOffset(uint32_t sector_size, uint32_t sectors_per_track,
	uint32_t cluster_size, bool legacy_bios)
{
	uint64_t track = (uint64_t)sector_size * sectors_per_track;
	uint64_t offset;

	if (track == 0)
		return 0;
	if (!legacy_bios)
		return 1024 * 1024;
	if (cluster_size == 0)
		cluster_size = 512;
	// Old BIOS mode starts on a track rounded up to a cluster boundary.
	// Double that space because GRUB2 no longer fits in the usual 31.5 KiB.
	if (track > UINT64_MAX - (cluster_size - 1))
		return 0;
	offset = (track + cluster_size - 1) / cluster_size * cluster_size;
	return offset > UINT64_MAX / 2 ? 0 : offset * 2;
}

uint64_t GetMainPartitionSize(uint64_t first_offset, uint64_t last_offset,
	uint32_t sector_size, uint32_t sectors_per_track, uint32_t cluster_size)
{
	uint64_t track = (uint64_t)sector_size * sectors_per_track;
	uint64_t size;

	if (track == 0 || last_offset <= first_offset)
		return 0;
	if (cluster_size == 0)
		cluster_size = 512;
	// Keep Rufus's order: round the available space down to tracks, then to
	// clusters when the cluster size is a multiple of the logical sector size.
	size = (last_offset - first_offset) / track * track;
	if (cluster_size % sector_size == 0)
		size = size / cluster_size * cluster_size;
	return size;
}
