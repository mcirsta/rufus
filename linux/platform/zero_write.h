/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "raw_write.h"

enum { ZERO_NONE, ZERO_FULL, ZERO_FAST };
enum { ZERO_COMPARE_BYTES = 32 * 1024 * 1024 };

#ifdef __cplusplus
extern "C" {
#endif
/* Windows WriteDrive zeroing semantics: FULL writes zeros everywhere; FAST
 * preserves uniform zero/FF 32-MiB regions and otherwise writes FF, backing
 * off comparison for the next 15 regions. No discard or secure erase.
 * Both modes flush and verify every resulting byte. Physical counters exclude
 * skipped regions; progress covers the complete capacity. Memory is bounded.
 * Caller retains the exclusive claim and owns authorization and cleanup. */
int ZeroDeviceData(int fd, uint64_t device, uint64_t capacity, int mode, RAW_WRITE_RESULT* result);
/* Same operation on an existing regular file, for native qualification. */
int ZeroFileData(int fd, int mode, RAW_WRITE_RESULT* result);
/* Clear and verify a FAT format extent. Device descriptors name the partition
 * and require offset zero. Regular files may contain a bounded volume extent.
 * Emit OP_FORMAT progress through 90%, reserving metadata creation for mkfs. */
int ZeroFormatExtent(int fd, uint64_t device, uint64_t offset, uint64_t bytes, RAW_WRITE_RESULT* result);
#ifdef __cplusplus
}
#endif
