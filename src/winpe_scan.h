/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "iso_scan.h"

#define WINPE_OPTIONS_MAX_SIZE UINT32_C(65536)

#ifdef __cplusplus
extern "C" {
#endif

/* Inspect ANSI/UTF-8 SIF text. buffer has size bytes plus one writable byte;
 * it is modified while parsing/logging. One trailing NUL is accepted; embedded
 * NULs/UTF-16 and oversized/empty input return PARTIAL with unknown options.
 * Retains the first nonempty OsLoadOptions value and Rufus's /minint substring
 * rule. This does not parse WIM metadata or validate a Windows installation. */
int InspectWinPeOptions(char* buffer, size_t size, RUFUS_IMG_REPORT* report);

/* Select the original i386/amd64/minint priority and read the recorded path,
 * using the caller's ISO name options and the existing UDF-first lookup.
 * File size is limited before allocation. Cancellation is checked around I/O
 * and parsing; cancelled options are cleared. No GUI/privileged operations. */
int ScanWinPeOptions(const char* image, iso_extension_mask_t mask, RUFUS_IMG_REPORT* report);

#ifdef __cplusplus
}
#endif
