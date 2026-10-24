/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "image_source.h"
#include <stdint.h>

typedef enum {
	RAW_WRITE_PREPARE,
	RAW_WRITE_COPY,
	RAW_WRITE_SYNC,
	RAW_WRITE_VERIFY,
	RAW_WRITE_DONE
} RAW_WRITE_PHASE;

typedef struct {
	RAW_WRITE_PHASE phase;
	uint64_t bytes_written;
	uint64_t bytes_verified;
	int sync_error;
} RAW_WRITE_RESULT;

#ifdef __cplusplus
extern "C" {
#endif

/* Copy the whole retained source to offset zero, fsync, then compare every
 * written byte with the source. Returns 0 only after verification and final
 * source checks; otherwise an errno value (ECANCELED, ESTALE, EBADMSG included).
 * result is initialized on entry and reports partial work and any flush error.
 * On copy failure/cancellation, written bytes are still flushed; no rollback.
 *
 * This entry point accepts existing REGULAR FILE targets only. They must
 * be opened O_RDWR without O_APPEND/O_DIRECT and already be large enough. It
 * neither creates/truncates files nor opens devices; the target tail is untouched.
 * Both descriptors and their offsets remain owned by the caller. Keep them
 * exclusively borrowed, including result, until return. Memory use is 2 MiB.
 * Cancellation is cooperative; fsync completes even after cancellation.
 *
 * The caller approves raw-image compatibility and owns source/target selection.
 * Use WriteRawImageToDevice for an authorized block-device descriptor.
 * Read-back may use the OS cache: this is byte verification, not a boot test
 * or proof of a device's physical persistence. */
int WriteRawImage(const IMAGE_FILE_SOURCE* source, const char* source_path,
	int target_fd, RAW_WRITE_RESULT* result);

/* Same regular-file copy/flush/verify operation, bounded to an explicit target
 * extent. Require nonzero size and source bytes <= size; preserve all bytes
 * before offset and after the copied source, including unused extent padding.
 * The caller validates partition metadata and retains exclusive file ownership.
 * No block-device support in this entry point. */
int WriteRawImageToExtent(const IMAGE_FILE_SOURCE* source, const char* source_path,
	int target_fd, uint64_t offset, uint64_t size, RAW_WRITE_RESULT* result);

/* Kernel validation shared by native authorization and the write boundary.
 * Checks block identity, capacity, writable/buffered access and sector alignment.
 * It does not authorize, acquire exclusivity or open anything. Returns errno. */
int CheckRawBlockTarget(int fd, uint64_t device, uint64_t capacity, uint64_t image_size);

/* Same copy/flush/verify loop for a block descriptor already acquired with
 * exclusive access through native authorization. Rechecks the expected dev_t,
 * capacity and kernel flags before writing and before reporting success.
 * Caller owns confirmation, mounts/holders, source-on-target checks and lifetime.
 * The Qt application supplies descriptors through native UDisks authorization. */
int WriteRawImageToDevice(const IMAGE_FILE_SOURCE* source, const char* source_path,
	int target_fd, uint64_t device, uint64_t capacity, RAW_WRITE_RESULT* result);

/* Same authorized device operation, bounded to a nonempty sector-aligned extent.
 * Offset/size must fit the expected capacity and contain the complete source.
 * Retain whole-device exclusivity; preserve all bytes outside the payload. */
int WriteRawImageToDeviceExtent(const IMAGE_FILE_SOURCE* source, const char* source_path,
	int target_fd, uint64_t device, uint64_t capacity, uint64_t offset, uint64_t size, RAW_WRITE_RESULT* result);

#ifdef __cplusplus
}
#endif
