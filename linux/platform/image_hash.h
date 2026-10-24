/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "image_source.h"
#include "hash_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	unsigned count;
	uint64_t size;
	uint8_t digest[HASH_MAX][MAX_HASHSIZE];
} IMAGE_HASH_RESULT;

/* Read one retained ordinary file through three 64-KiB buffers, one pread reader
 * and one worker per requested algorithm. All are joined and buffers released
 * before returning, including setup failure and cancellation. Reporting and
 * cancellation queries stay on the calling thread. The descriptor and
 * pathname must still match its original identity before AND after reading.
 * Hashes are MD5, SHA-1, SHA-256, plus SHA-512 only when extra is true, as on
 * Windows. Each digest occupies its algorithm's HASHSIZE bytes; unused bytes
 * stay zero. No partial result is published on error or cancellation.
 *
 * Returns 0 or an errno value. Does not open/close the caller's descriptor or
 * change its offset. Uses the existing synchronous job cancellation query and
 * progress reporting (OP_NOOP_WITH_TASKBAR, MSG_271). No target or privilege.
 * File change detection is not a snapshot or protection against hostile writes.
 */
int HashImageFileSource(const IMAGE_FILE_SOURCE* source, const char* path,
	bool extra, IMAGE_HASH_RESULT* result);

#ifdef __cplusplus
}
#endif
