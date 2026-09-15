/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "wim_metadata.h"

typedef struct {
	RUFUS_WIM_METADATA metadata;
	int wim_error;
} WIM_FILE_ANALYSIS;

#ifdef __cplusplus
extern "C" {
#endif

/* Borrow an already-open ordinary file. wimlib acquires its own descriptor via
 * /proc/self/fd, preserving the caller's offset and selected inode. Keep the
 * caller's descriptor open until wimlib_free(); no original-path reopening or
 * ownership transfer. Upstream still requires a resolvable filename and returns
 * NO_FILENAME for an already-unlinked file. Returns a wimlib code and clears the
 * output on failure. The host owns initialization, cancellation and final
 * source-change checks. */
int OpenWimFileDescriptor(int fd, WIMStruct** wim);

/* Read-only metadata for an ordinary native file; image is 1-based. The host
 * initializes wimlib once before workers and cleans it up after they finish.
 * Cancellation uses the existing direct IsIsoScanCancelled() host query, before
 * and after wimlib I/O; it cannot interrupt an in-progress wimlib call.
 * Returns ISO_SCAN_*; PARTIAL retains archive facts and unknown field masks.
 * ERROR/CANCELLED clear metadata; ERROR retains the wimlib error code. */
int ScanWimFile(const char* path, int image, WIM_FILE_ANALYSIS* analysis);

#ifdef __cplusplus
}
#endif
