/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "compressed_source.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Multi-entry supplement extraction shares the DD reader's bounded ZIP64
 * parsing and codecs. The archive borrows source; Next selects one entry for
 * Read, in central-directory order. ENOENT means a complete directory. */
typedef struct zip_archive ZIP_ARCHIVE;
typedef struct {
	char name[4096];
	size_t name_bytes;
	uint64_t size;
	uint32_t attributes;
	uint16_t flags;
	bool directory;
} ZIP_ARCHIVE_ENTRY;
int OpenZipArchive(const IMAGE_FILE_SOURCE* source, ZIP_ARCHIVE** archive);
int NextZipArchiveEntry(ZIP_ARCHIVE* archive, ZIP_ARCHIVE_ENTRY* entry);
int ReadZipArchiveEntry(ZIP_ARCHIVE* archive, uint64_t maximum_output,
	compressed_data_t consumer, void* context, COMPRESSED_IMAGE_RESULT* completed);
void CloseZipArchive(ZIP_ARCHIVE* archive);

/* Internal providers; retained-source validation and publication belong to the caller. */
int ReadZipImageSource(const IMAGE_FILE_SOURCE* source, uint64_t maximum_output,
	compressed_data_t consumer, void* context, COMPRESSED_IMAGE_RESULT* completed);

/* Reuse the bounded zlib owner and output loop with a raw deflate entry slice. */
int ReadZipDeflateSource(const IMAGE_FILE_SOURCE* source, uint64_t offset, uint64_t size,
	uint64_t maximum_output, compressed_data_t consumer, void* context,
	COMPRESSED_IMAGE_RESULT* completed, uint32_t* crc);

#ifdef __cplusplus
}
#endif
