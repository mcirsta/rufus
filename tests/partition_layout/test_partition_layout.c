/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "partition_layout.h"
#include <stdio.h>

static unsigned checks, failures;
#define CHECK(value) do { checks++; if (!(value)) { failures++; fprintf(stderr, "Failed line %d\n", __LINE__); } } while (0)

int main(void)
{
	const uint32_t sectors[] = { 512, 1024, 4096 };
	const uint32_t tracks[] = { 1, 32, 63 };
	const uint32_t clusters[] = { 0, 1, 512, 1024, 4096, 32768, 65536 };
	const uint64_t disks[] = { 64ULL << 20, 513ULL << 20, 32ULL << 30, 2ULL << 40 };
	uint64_t track, cluster, first, last, size, expected;

	for (size_t s = 0; s < sizeof(sectors) / sizeof(sectors[0]); s++)
	for (size_t t = 0; t < sizeof(tracks) / sizeof(tracks[0]); t++)
	for (size_t c = 0; c < sizeof(clusters) / sizeof(clusters[0]); c++)
	for (size_t d = 0; d < sizeof(disks) / sizeof(disks[0]); d++)
	for (int legacy = 0; legacy < 2; legacy++) {
		track = (uint64_t)sectors[s] * tracks[t];
		cluster = clusters[c] ? clusters[c] : 512;
		// Independent transcription of the two original drive.c blocks.
		expected = legacy ? ((track + cluster - 1) / cluster) * cluster * 2 : 1024 * 1024;
		first = GetPartitionStartOffset(sectors[s], tracks[t], clusters[c], legacy != 0);
		CHECK(first == expected);
		for (int gpt = 0; gpt < 2; gpt++) {
			last = disks[d] - (gpt ? 33ULL * sectors[s] : 0);
			// Include room already reserved for an extra partition at the end.
			for (int extra = 0; extra < 2; extra++) {
				uint64_t end = last - extra * (16ULL << 20);
				size = ((end - first) / track) * track;
				if (cluster % sectors[s] == 0)
					size = (size / cluster) * cluster;
				CHECK(GetMainPartitionSize(first, end, sectors[s], tracks[t], clusters[c]) == size);
			}
		}
	}
	CHECK(GetPartitionStartOffset(0, 63, 512, false) == 0);
	CHECK(GetPartitionStartOffset(512, 0, 512, false) == 0);
	CHECK(GetPartitionStartOffset(UINT32_MAX, UINT32_MAX, UINT32_MAX, true) == 0);
	CHECK(GetMainPartitionSize(100, 100, 512, 63, 512) == 0);
	CHECK(GetMainPartitionSize(101, 100, 512, 63, 512) == 0);
	CHECK(GetMainPartitionSize(0, 100, 512, 63, 512) == 0);
	CHECK(GetMainPartitionSize(0, UINT64_MAX, 0, 63, 512) == 0);
	printf("Partition layout: %u checks, %u failures\n", checks, failures);
	return failures ? 1 : 0;
}
