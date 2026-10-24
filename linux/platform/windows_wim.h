/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <wimlib.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Open the extracted boot.wim in the helper's private target tree. Like Rufus,
 * choose image 2 when present, otherwise image 1. Return wimlib error codes;
 * failed opens clear both outputs. This does not open the source ISO.
 * The caller owns the returned WIM and must call wimlib_free(). A cancellation
 * callback is registered for subsequent extraction and update operations.
 *
 * The caller must initialize wimlib with ASSUME_UTF8 and
 * DEFAULT_CASE_INSENSITIVE before any library use, serialize access, and keep
 * the private target path stable. Symlinks, non-files and hardlinks are refused.
 */
int OpenWindowsSetupWim(const char* path, WIMStruct** wim, int* image);

/* Apply the caller's native wimlib commands, then commit with Rufus's RECOMPRESS
 * flag and automatic compression threading, plus FSYNC. Zero commands do not
 * rewrite the file. Commands and their source files must remain valid and
 * unchanged until return. Replaces the WIM's progress callback for cancellation.
 *
 * After this call, free the WIM even on failure; do not retry that handle.
 * Append/rebuild and recovery are upstream wimlib's responsibility. Errors do
 * not promise rollback after an I/O failure or power loss. Success may already
 * have committed when cancellation arrives, so no late cancellation is returned.
 * The enclosing job still owns final flushing/unmounting and authorization.
 */
int UpdateWindowsSetupWim(WIMStruct* wim, int image,
    const struct wimlib_update_command* commands, size_t count);

#ifdef __cplusplus
}
#endif
