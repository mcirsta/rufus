/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stdbool.h>
#include <sys/stat.h>

typedef struct {
	int fd;
	struct stat info;
	bool is_wim;
	bool has_boot_marker;
	struct virtual_disk_node* virtual_disk; /* Borrowed optional retained parent/replay graph. */
	unsigned ffu_store; /* Explicit one-based store; zero permits only a single-store FFU. */
} IMAGE_FILE_SOURCE;

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize fd to -1; close before reusing. Opens one ordinary file, with
 * nonblocking/CLOEXEC flags, and reads up to its first 512 bytes without moving
 * the offset. Symlinks are resolved once at open. WIM and boot-marker facts are
 * independent; neither establishes image validity or permission to write it.
 * Returns 0 or an errno value (including ECANCELED/ESTALE); failure closes and
 * clears the output. Uses the host's IsIsoScanCancelled() query. */
int OpenImageFileSource(const char* path, IMAGE_FILE_SOURCE* source);

/* Check the retained descriptor AND the current pathname against the initial
 * device/inode/size/mtime/ctime. Call before publishing any dependent result.
 * Returns 0, ESTALE on mismatch, ECANCELED, or the failed stat's errno.
 * Does not close the source. This detects changes; it is not a file snapshot. */
int CheckImageFileSource(const IMAGE_FILE_SOURCE* source, const char* path);
void CloseImageFileSource(IMAGE_FILE_SOURCE* source);

#ifdef __cplusplus
}
#endif
