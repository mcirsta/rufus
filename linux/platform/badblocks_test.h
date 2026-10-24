/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "badblocks_policy.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { BADBLOCK_READ_ERROR, BADBLOCK_WRITE_ERROR, BADBLOCK_CORRUPTION_ERROR };
typedef struct { uint64_t block; unsigned type; } BADBLOCK_ERROR;
typedef struct {
    uint32_t count, read_errors, write_errors, corruption_errors;
    uint64_t bytes_written, bytes_read;
    int sync_error;
    bool threshold_reached;
    BADBLOCK_ERROR errors[BB_BAD_BLOCKS_THRESHOLD];
} BADBLOCK_RESULT;
/* Error callbacks are bounded to 256 unique blocks, as in Windows Rufus.
 * The borrowed callback context is used only on this calling thread. */
typedef void (*badblock_error_t)(void* context, uint64_t block, unsigned type);

/* Destructive whole-device test. The caller retains an exclusive whole-disk
 * claim and owns source/target protection, authorization and cleanup. Native
 * data I/O uses a separate aligned O_DIRECT descriptor; each write phase is
 * flushed before readback. No verification through the page cache. Pattern
 * index is 0..4 (1 pass, 2 passes, SLC, MLC, TLC); fake detection defaults to
 * first-pass block-number tags. A nonzero count is a report, not an I/O errno:
 * the caller must present Abort/Retry/Ignore before creating any media. */
int TestBadBlocksDevice(int fd, uint64_t device, uint64_t capacity, int pattern,
    bool detect_fakes, badblock_error_t callback, void* context, BADBLOCK_RESULT* result);
/* Identical algorithm on an existing ordinary file, for disposable fixtures.
 * seed chooses only the tag location; patterns and block IDs remain exact. */
int TestBadBlocksFile(int fd, int pattern, bool detect_fakes, uint32_t seed,
    badblock_error_t callback, void* context, BADBLOCK_RESULT* result);
/* Final test metadata cleanup, including after cancellation. It preserves
 * descriptor flags/position, verifies the fixed Rufus ranges and rereads the
 * kernel partition table. Device zero is the ordinary-file fixture adapter. */
int ClearBadBlocksHeaders(int fd, uint64_t device, uint64_t capacity, int* sync_error);
#ifdef __cplusplus
}
#endif
