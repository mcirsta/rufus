/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t offset_sectors, sectors, attributes;
    uint8_t id[16], type[16], name[72];
    unsigned fields;
    bool use_all_space, rewrite;
} FFU_MANIFEST_PARTITION;

typedef struct {
    bool enabled;
    uint8_t id[16];
    uint64_t usable_offset, usable_bytes;
    uint32_t maximum_partitions;
    unsigned fields, selected_partition;
    size_t count;
    FFU_MANIFEST_PARTITION* partitions;
} FFU_OPTIMIZATION;

/* Parse the richer Windows optimized manifest only when FullFlash declares it.
 * Store/partition order is significant. Names use the UTF-16LE Name_U value
 * when present, otherwise UTF-8 Name. No host locale or settings are inferred.
 * The caller owns count zero-initialized stores and frees each on every exit. */
int ParseFfuOptimizedManifest(const char* manifest, unsigned count, FFU_OPTIMIZATION* stores);
void FreeFfuOptimization(FFU_OPTIMIZATION* store);
bool SameFfuPartitionName(const uint8_t first[72], const uint8_t second[72]);

#ifdef __cplusplus
}
#endif
