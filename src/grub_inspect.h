/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "image_report.h"
#include "strarray.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Inspect an accessible byte span, without modifying it. Strings must terminate
 * within the span; pass byte_count + 1 for ReadISOFileToBuffer[Ext] results.
 * Retains Rufus's 32-byte search margin and last usable version per buffer.
 * Logs the detected version with source, but only fills an empty report version.
 * The initialized report supplies the GRUB path flags and terminated ISO label
 * for Rufus's build-specific lookup suffixes. This does not establish bootability. */
void GetGrubVersion(const char* buf, size_t buf_size, const char* source,
	RUFUS_IMG_REPORT* report);

/* BootCheckThread's BIOS/MBR image lookup gate. A missing detection does not
 * become an embedded-version detection. Syslinux precedence does not suppress
 * this check on Windows; it is applied later when choosing boot records.
 * embedded_version is a terminated version string. No I/O or compatibility
 * certification is performed; unreadable cache/downloads still fall back. */
bool NeedsGrub2CoreLookup(const RUFUS_IMG_REPORT* report, bool mbr, bool bios,
	const char* embedded_version);

/* Next core.img server lookup uses the full detected name, then successively
 * shorter prefixes at dots/dashes, preserving BootCheckThread's order. Start
 * with sizeof(report->grub2_version), then pass the returned length to advance.
 * Zero means exhausted or missing/unterminated detection. Does not modify the
 * report, select a core or claim shortened names identify compatible modules. */
size_t GetGrub2DownloadVersionLength(const RUFUS_IMG_REPORT* report, size_t before);

/* Add discovered filesystem names to an initialized caller-owned string array,
 * preserving order and case-sensitive uniqueness. Names are copied. Returns
 * false on allocation failure, retaining any names collected before failure.
 * Unknown or unterminated markers add nothing. */
bool GetGrubFs(const char* buf, size_t buf_size, StrArray* filesystems);

/* Rufus report bits: FAT=1, exFAT=2, NTFS=4; names compare without case. */
uint8_t GetGrubFsMask(const StrArray* filesystems);

#ifdef __cplusplus
}
#endif
