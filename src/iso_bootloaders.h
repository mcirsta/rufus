/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "iso_scan.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Content stages extracted from ExtractISO. The caller supplies the filesystem
 * name options and initialized scan data. Reads retain Rufus's UDF preference.
 * Return ISO_SCAN_OK, ERROR (path/filesystem storage), CANCELLED, or PARTIAL
 * (a content read failed). Unknown version banners are successful inspections.
 * Partial findings remain in the report; cancellation is checked between reads.
 * No GUI, media writes, installer selection, or Secure Boot checks occur here. */
int ScanSyslinuxBootloaders(const char* image, iso_extension_mask_t mask,
	const ISO_SCAN_STATE* state, const uint16_t embedded_version[2]);
int ScanGrubAndEfiBootloaders(const char* image, iso_extension_mask_t mask,
	RUFUS_IMG_REPORT* report);

#ifdef __cplusplus
}
#endif
