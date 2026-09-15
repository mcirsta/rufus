/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "iso_scan.h"

#define ISO_FILE_CACHE_SIZE (16 * ISO_BLOCKSIZE)

typedef struct iso_file_reader {
	iso9660_t* iso;
	udf_dirent_t* udf_file;
	lsn_t lsn;
	uint64_t size;
	uint64_t cache_start;
	size_t cache_size;
	uint8_t cache[ISO_FILE_CACHE_SIZE];
} ISO_FILE_READER;

#ifdef __cplusplus
extern "C" {
#endif

/* Open a regular file in exactly one already selected ISO/UDF view, without
 * fallback. Borrows the filesystem handle and owns the UDF file entry. The
 * caller owns reader storage, serializes the handle and closes the reader before
 * closing that handle. Do not reopen an active reader without closing it first.
 * Failed opens leave a closed reader. Empty files are valid. ISO multi-extents
 * rely on libcdio's contiguous-extent validation; UDF extents may be fragmented.
 * OK/PARTIAL/ERROR/CANCELLED retain the existing scan-result meanings. */
int OpenIsoFileReader(ISO_FILE_READER* reader, iso9660_t* iso, udf_t* udf, const char* path);
void CloseIsoFileReader(ISO_FILE_READER* reader);

/* Exact random byte read, with a fixed 32 KiB cache and no per-read allocation.
 * Rejects ranges beyond file size before I/O; a zero read at EOF is valid.
 * Checks cancellation even for cache hits and between physical reads. On any
 * unsuccessful return discard the requested output (some bytes may be written).
 * Invalid ranges do not write output. A failed refill invalidates the cache.
 * Does not parse WIM/FAT, copy the whole file, snapshot concurrent file changes,
 * or interrupt an in-progress backend call. */
int ReadIsoFileRange(ISO_FILE_READER* reader, uint64_t offset, void* buffer, size_t count);

#ifdef __cplusplus
}
#endif
