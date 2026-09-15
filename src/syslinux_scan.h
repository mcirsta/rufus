/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "image_report.h"
#include "strarray.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* All report/array pointers are non-NULL; paths and suffixes are terminated.
 * Reset only the report's Syslinux fields and choose a config from an initialized
 * path array. Preserves Rufus's shortest-path/latest-tie and openSUSE preferences.
 * Returns false for no configs or when none fits cfg_path; has_syslinux separates
 * those cases. Paths are copied, never silently truncated or replaced by filler. */
bool SelectSyslinuxConfig(RUFUS_IMG_REPORT* report, const StrArray* configs);

/* Apply Rufus's loader-conflict policy to one successfully read image file.
 * Version/ext come from GetSyslinuxVersion; zero means an unknown banner, not
 * a failed file read. Pass NULL as selected_source for the first candidate and
 * retain the returned source for the next call. Source paths are borrowed until
 * SelectSyslinuxVersion finishes; the detected suffix is copied into the report.
 * Call only after successful config selection. No file reads occur here. */
const char* SelectSyslinuxCandidate(RUFUS_IMG_REPORT* report, const char* selected_source,
	const char* source, uint16_t version, const char* ext);

/* After successful config selection and candidate processing, populate the
 * existing sl_version/string/ext installation fields, preserving
 * the detected fields. Unknown detection uses the supplied v4/v6 numeric fallback
 * selected by ldlinux.c32 presence, with Rufus's empty fallback suffix. Zero in
 * a fallback slot means unavailable: presence survives but no version is selected.
 * These are payload-selection rules, not a native installer capability check. */
void SelectSyslinuxVersion(RUFUS_IMG_REPORT* report, const char* source,
	bool has_ldlinux_c32, const uint16_t embedded_version[2]);

#ifdef __cplusplus
}
#endif
