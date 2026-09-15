/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#ifndef DO_NOT_WANT_COMPATIBILITY
#define DO_NOT_WANT_COMPATIBILITY
#endif
#include <cdio/iso9660.h>

#define ISO_READ_MAX_SIZE UINT32_C(1073741824)

#ifdef __cplusplus
extern "C" {
#endif

/* Read a file using Rufus's UDF-first lookup and the caller's ISO name options.
 * Returns its byte length and a malloc-owned buffer with an extra trailing NUL.
 * The caller must free the buffer. Empty files, directories, files over 1 GiB
 * and errors return 0 with *buf == NULL. A failed UDF file lookup does not fall
 * back to the ISO view. Logging preferences and cancellation belong to the
 * caller; an individual library read is synchronous. */
uint32_t ReadISOFileToBufferExt(const char* iso, const char* iso_file, uint8_t** buf,
	iso_extension_mask_t extension_mask);

/* Same ownership and lookup rules, with a caller limit checked against the
 * file length before allocation/I/O. max_size must be between 1 and 1 GiB. */
uint32_t ReadISOFileToBufferLimited(const char* iso, const char* iso_file, uint8_t** buf,
	iso_extension_mask_t extension_mask, uint32_t max_size);

#ifdef __cplusplus
}
#endif
