/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "iso_analyze.h"

typedef struct {
	uint64_t files;
	uint64_t directories;
	uint64_t skipped;
	uint64_t bytes_written;
	char path[ISO_SCAN_PATH_MAX]; /* Current/failed image entry; empty on success. */
} ISO_EXTRACT_RESULT;

#ifdef __cplusplus
extern "C" {
#endif

/* Extract ordinary files/directories into a caller-owned, private, empty
 * directory. Never opens a device, mounts, formats, or overwrites an entry.
 * Uses Rufus's UDF-first/Rock Ridge/Joliet selection and shared tree/readers.
 * Preflight rejects unsupported links/special entries and deep/incomplete trees.
 * Root ldlinux.sys is reserved for the native installer, as on Windows.
 *
 * Returns 0 or errno (ECANCELED/ESTALE included). Keeps the source descriptor
 * and checks its identity before/during/after copying. Writes stay relative to
 * the retained target directory with no symlink traversal. Files/directories
 * are flushed; a failed current file is removed, previously completed entries
 * remain. On error, treat the whole output as incomplete and use a fresh target.
 * Fixed memory for file data; no complete-image or all-files manifest buffer.
 *
 * This is file extraction, not a bootable-media operation: timestamps, symlink
 * policy, FAT naming, WIM splitting, payload replacement and config patching
 * remain separate steps. No installer compatibility or boot claim is implied. */
int ExtractIsoTree(const char* image, const char* directory,
	const ISO_ANALYSIS_OPTIONS* options, ISO_EXTRACT_RESULT* result);

#ifdef __cplusplus
}
#endif
