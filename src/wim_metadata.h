/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "image_report.h"
#include <wimlib.h>

#define WIM_VERSION_MAJOR    0x01
#define WIM_VERSION_MINOR    0x02
#define WIM_VERSION_BUILD    0x04
#define WIM_VERSION_REVISION 0x08
#define WIM_VERSION_REQUIRED 0x07
#define WIM_VERSION_ALL      0x0f

typedef struct {
	uint32_t version;
	uint32_t image_count;
	uint32_t boot_index;
	uint16_t part_number;
	uint16_t total_parts;
} RUFUS_WIM_INFO;

typedef struct {
	RUFUS_WIM_INFO archive;
	winver_t raw_version;
	winver_t win_version;
	uint8_t valid_fields;
	uint8_t invalid_fields;
} RUFUS_WIM_METADATA;

#ifdef __cplusplus
extern "C" {
#endif

/* Rufus's reporting policy from wue.c, including its existing build > 20000
 * Windows 11 rule. This does not identify editions or validate bootability. */
void NormalizeWindowsVersion(winver_t* version);

/* These queries borrow an open wimlib handle. They return wimlib error codes
 * and clear the output on failure. Opening, initialization, handle ownership
 * and cancellation belong to the caller. Archive version is not OS version. */
int ReadWimArchiveInfo(WIMStruct* wim, RUFUS_WIM_INFO* info);

/* Image indexes are 1-based; automatic reporting uses image 1. Missing fields
 * have neither bit set; invalid/non-decimal/out-of-range fields set invalid.
 * Decimal text is bounded to 32 characters and uint16_t, matching winver_t.
 * win_version is normalized only when MAJOR/MINOR/BUILD are valid; otherwise
 * it stays zero. A missing SPBUILD stays unknown even though its value is zero.
 * Successful metadata access (return 0) does not imply a known OS version. */
int ReadWimMetadata(WIMStruct* wim, int image, RUFUS_WIM_METADATA* metadata);

#ifdef __cplusplus
}
#endif
