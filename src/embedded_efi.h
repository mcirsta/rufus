/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "iso_scan.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Inspect an embedded FAT image through an already selected filesystem view.
 * Supply exactly one of iso/udf; no fallback between views occurs here.
 * The caller owns the handle and serializes its use and libfat's sector globals.
 * UDF access changes the handle's current file position.
 * OK means the directory search completed (found may be false); PARTIAL means
 * unreadable/unsupported image, ERROR means invalid arguments/storage failure,
 * CANCELLED means host interruption. found is cleared on every unsuccessful
 * return. This is filename/presence detection, not PE or Secure Boot validation. */
int InspectEfiImage(iso9660_t* iso, udf_t* udf, const char* path, bool* found);

#ifdef __cplusplus
}
#endif
