/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "compressed_source.h"
#include "hash_core.h"
#include "raw_write.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	struct stat source_info;
	COMPRESSED_IMAGE_RESULT image;
	uint64_t target_bytes;
	unsigned sector_size;
	bool ignore_boot_marker;
	uint8_t decoded_sha256[SHA256_HASHSIZE];
	uint8_t layout_sha256[SHA256_HASHSIZE]; /* VTSI map or FFU authenticated container identity. */
} COMPRESSED_WRITE_PLAN;

/* Round nonzero decoded bytes to an explicit power-of-two sector (512..65536).
 * Require the entire padded output to fit signed 64-bit file offsets. Clears
 * target_bytes on error. This computes geometry, not capacity or eligibility. */
int CompressedImageWriteSize(uint64_t decoded_bytes, unsigned sector_size, uint64_t* target_bytes);

/* Read and validate gzip, XZ, BZIP2, LZMA-Alone, Unix-compress/LZW, ZIP, Zstd or VTSI with the native retained-source
 * provider, freezing its retained identity,
 * exact decoded size, prefix/members/tail and SHA256, then apply the shared strict
 * boot-marker/explicit-force policy and sector rounding. No target access.
 * VTSI freezes an ordered segment-map digest; FFU freezes its authenticated
 * metadata/payload hash table. VTSI rejects extents that do
 * not align to the chosen target sector size.
 * maximum_output is a caller-selected byte bound, not a footer/partition hint.
 * Return 0 or errno; any failure/cancellation clears the plan. A plan is not a
 * source snapshot, authorization or proof that the decoded image will boot. */
int PrepareCompressedWrite(const IMAGE_FILE_SOURCE* source, const char* source_path,
	uint64_t maximum_output, unsigned sector_size, bool ignore_boot_marker, COMPRESSED_WRITE_PLAN* plan);

/* Regular-file qualification/write foundation. Borrow existing ordinary source
 * and O_RDWR buffered regular target descriptors exclusively until return. Never
 * open/create/truncate/close a target or accept a block device. Preserve offsets,
 * file length. Dense formats preserve target bytes beyond plan.target_bytes. Reject source aliases.
 *
 * Recompute the entire frozen plan before the first target write. Dense formats
 * decode into bounded sector-aware batches and zero-pad the final sector. VTSI
 * writes only its listed extents, leaving every gap and tail byte untouched;
 * FFU resolves beginning/end-relative runs at the actual target capacity and
 * preserves gaps, relocating validated GPT metadata for a larger disk without
 * resizing partitions. Its physical counters cover only final mapped bytes.
 * Partial FFU validation ranges must all match the existing target before any
 * write. They are not reapplied to changed data during readback. Validated FFU
 * containers follow Windows's FFU admission without a required MBR marker.
 * VTSI logical SHA256 covers zero-filled gaps while its separate map SHA256 fixes
 * the extents. Check every write, fsync and read back all written bytes. Recheck
 * source and target identity/size/flags before success. No expanded temporary
 * image. All native hash users share once-only CPU dispatch initialization.
 *
 * result follows RAW_WRITE_PHASE and counts actual physical I/O bytes, including
 * padding for dense formats but only final extent data for VTSI/FFU.
 * Failed/cancelled copies still fsync all written bytes; preserve the primary
 * error and expose a separate sync_error. No rollback. DONE follows successful
 * validation, writes, sync and readback; the caller still owns descriptor close,
 * helper/worker join and GUI READY. Readback may use cache, not prove persistence.
 */
int WriteCompressedImageToFile(const IMAGE_FILE_SOURCE* source, const char* source_path,
	const COMPRESSED_WRITE_PLAN* plan, int target_fd, RAW_WRITE_RESULT* result);

/* Same engine for a block descriptor retained by an already authorized job.
 * Require exact dev_t/capacity and the plan's kernel logical sector size before
 * writing and after sync/readback. Reject sources on that device. This neither
 * opens/acquires a claim nor checks mounts/holders/whole-disk ancestry; the native
 * authorized owner supplies those protections and retains the claim to close. */
int WriteCompressedImageToDevice(const IMAGE_FILE_SOURCE* source, const char* source_path,
	const COMPRESSED_WRITE_PLAN* plan, int target_fd, uint64_t device, uint64_t capacity, RAW_WRITE_RESULT* result);

#ifdef __cplusplus
}
#endif
