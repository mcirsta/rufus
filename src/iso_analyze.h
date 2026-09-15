/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "iso_scan.h"

enum ISO_ANALYSIS_PENDING {
	ISO_PENDING_DEEP_DIRECTORIES = 1 << 1,
	ISO_PENDING_CONTENT = 1 << 2,
	ISO_PENDING_EMBEDDED_EFI = 1 << 3,
	ISO_PENDING_WINDOWS = 1 << 4,
	ISO_PENDING_SECURE_BOOT = 1 << 5,
	ISO_PENDING_TRUNCATED = 1 << 6
};

enum ISO_FILESYSTEM {
	ISO_FILESYSTEM_NONE,
	ISO_FILESYSTEM_ISO9660,
	ISO_FILESYSTEM_UDF
};

typedef struct {
	bool enable_rockridge;
	bool enable_joliet;
	uint16_t embedded_sl_version[2];
} ISO_ANALYSIS_OPTIONS;

typedef struct {
	RUFUS_IMG_REPORT report;
	uint64_t total_blocks;
	uint64_t extra_blocks;
	uint64_t files;
	uint32_t pending;
	enum ISO_FILESYSTEM filesystem;
	bool tree_complete;
	bool volume_size_known;
} ISO_ANALYSIS;

/* Owns exactly the selected filesystem. A companion ISO used for UDF volume
 * sizing is closed before return. Initialize empty; close before reuse. */
typedef struct {
	iso9660_t* iso;
	udf_t* udf;
} ISO_ANALYSIS_SOURCE;

#ifdef __cplusplus
extern "C" {
#endif

/* Original label trimming, PVD/file-size difference and projected size (+1%).
 * report is non-NULL and its label is terminated.
 * Negative image_size or volume_blocks means unavailable: leave mismatch alone.
 * Returns false if size arithmetic cannot be represented, with projection zero.
 * This only finalizes a complete tree; it does not include extra_blocks (Rufus
 * adds the firmware symlink allowance later, when it selects a filesystem). */
bool FinalizeIsoScan(RUFUS_IMG_REPORT* report, uint64_t total_blocks,
	int64_t image_size, int64_t volume_blocks);

/* One read-only UDF/ISO9660 metadata operation. Host validates a regular file and
 * supplies its current size; it owns logging and IsIsoScanCancelled(). No global
 * report, callbacks, GUI, stat adapter, or privileged work is needed.
 *
 * The result owns no allocations and is always reset, including after error or
 * cancellation. Returns ISO_SCAN_OK for completed image metadata, PARTIAL with
 * pending reasons, ERROR, or CANCELLED. UDF is preferred; only a failed UDF open
 * tries ISO9660. An unreadable UDF root/tree fails, keeping content and tree
 * reads on the same filesystem. A companion ISO PVD, if present, supplies size
 * checks only. volume_size_known is false without one (mismatch stays zero). Deep
 * trees retain partial file facts but no projected size or bootloader selection.
 *
 * Complete means ISO metadata only: raw/hybrid-image detection, final media
 * policy, native installer compatibility and actual bootability are separate
 * stages. Partial reports are diagnostics, never permission to write media.
 * Individual libcdio reads are synchronous; cancellation is between operations.
 */
int AnalyzeIsoImage(const char* image, uint64_t image_size,
	const ISO_ANALYSIS_OPTIONS* options, ISO_ANALYSIS* analysis);

/* Same scan, retaining its selected handle for subsequent content queries.
 * The caller closes source after all readers borrowing it have been closed.
 * ERROR/CANCELLED leave source closed. No reader callbacks or WIM dependency. */
int AnalyzeIsoImageWithSource(const char* image, uint64_t image_size,
	const ISO_ANALYSIS_OPTIONS* options, ISO_ANALYSIS* analysis, ISO_ANALYSIS_SOURCE* source);
void CloseIsoAnalysisSource(ISO_ANALYSIS_SOURCE* source);

#ifdef __cplusplus
}
#endif
