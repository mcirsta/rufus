/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "iso_analyze.h"
#include "wim_metadata.h"

enum WINDOWS_IMAGE_STATE {
	WINDOWS_IMAGE_UNCHECKED,
	WINDOWS_IMAGE_KNOWN,
	WINDOWS_IMAGE_UNKNOWN,
	WINDOWS_IMAGE_FAILED
};

typedef struct {
	RUFUS_WIM_METADATA metadata;
	enum WINDOWS_IMAGE_STATE state;
	int wim_error;
} WINDOWS_IMAGE_ANALYSIS;

#ifdef __cplusplus
extern "C" {
#endif

/* Complete Windows install metadata using the scan's selected, borrowed handle.
 * The host initializes wimlib before workers. First recorded install path and
 * WIM image 1 match Rufus's automatic reporting; never try a later candidate
 * after a failed/unknown first one. No install image leaves Windows checks as-is.
 * Returns overall ISO_SCAN_* and updates pending/report fields. An unreadable or
 * unknown WIM retains partial tree facts; cancellation clears both outputs. */
int ScanWindowsImage(const ISO_ANALYSIS_SOURCE* source, ISO_ANALYSIS* analysis,
	WINDOWS_IMAGE_ANALYSIS* windows);

#ifdef __cplusplus
}
#endif
