/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#ifndef DO_NOT_WANT_COMPATIBILITY
#define DO_NOT_WANT_COMPATIBILITY
#endif
#include <cdio/iso9660.h>
#include <cdio/udf.h>
#include "image_report.h"
#include "strarray.h"

/* Retain Rufus's current image-path limit until traversal and extraction can
 * both support longer paths. An overlong path fails instead of being truncated. */
#define ISO_SCAN_PATH_MAX 260

typedef struct {
	RUFUS_IMG_REPORT* report;
	StrArray* config_path;
	StrArray* isolinux_path;
	uint64_t total_blocks;
	uint64_t extra_blocks;
	uint64_t files;
	bool has_ldlinux_c32;
	bool enable_rockridge;
	uint8_t joliet_level;
} ISO_SCAN_STATE;

#ifdef __cplusplus
extern "C" {
#endif

/* Host-side synchronous cancellation query, like the existing reporting entry
 * points. Cancellation needs no function pointer or GUI handle.
 * The host must make this safe for its worker/cancellation mechanism. */
int IsIsoScanCancelled(void);

enum ISO_TREE_KIND { ISO_TREE_FILE, ISO_TREE_DIRECTORY, ISO_TREE_DIRECTORY_END, ISO_TREE_DEEP };

/* Entry strings and library metadata are borrowed for the duration of visit.
 * DIRECTORY precedes children; DIRECTORY_END follows them. DEEP preserves
 * Rufus's Rock Ridge parent-link short circuit. No file data is read here. */
typedef struct {
	enum ISO_TREE_KIND kind;
	const char* parent;
	const char* name;
	const char* path;
	const iso9660_stat_t* iso;
	udf_dirent_t* udf;
} ISO_TREE_ENTRY;

/* One synchronous C visitor separates scan accounting from native extraction.
 * Return scan-result codes, or -1 for the original deep-directory short circuit.
 * This is internal file processing; reporting/cancellation keep their existing
 * named host functions and no GUI object participates in the traversal. */
typedef struct {
	int (*visit)(const ISO_TREE_ENTRY* entry, void* context);
	void* context;
	bool enable_rockridge;
	uint8_t joliet_level;
} ISO_TREE_WALK;

int WalkIsoFiles(iso9660_t* iso, const char* path, const ISO_TREE_WALK* walk);
/* Consumes the iterator on every return, as ScanUdfFiles does. */
int WalkUdfFiles(udf_dirent_t* directory, const char* path, const ISO_TREE_WALK* walk);

/* Scan an already opened ISO tree using initialized report/path arrays. The
 * caller owns them and the ISO handle, including cleanup after error/cancel.
 * Returns 0, a positive error/cancellation code, or -1 for Rufus's deep-directory
 * short circuit. has_deep_directories also marks a partial tree when a parent
 * absorbs that short circuit. File content and final image policy are not read. */
int ScanIsoFiles(iso9660_t* iso, const char* path, ISO_SCAN_STATE* state);

/* Same accounting for UDF. Consumes and frees the directory iterator on every
 * return, including error/cancel; the caller still owns the UDF image handle.
 * An unreadable directory is an error, never a complete or silently skipped tree. */
int ScanUdfFiles(udf_dirent_t* directory, const char* path, ISO_SCAN_STATE* state);

#ifdef __cplusplus
}
#endif
