/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "compressed_source.h"

/* Internal provider; source validation and publication belong to the caller. */
int ReadLzmaImageSource(const IMAGE_FILE_SOURCE* source, uint64_t maximum_output,
	compressed_data_t consumer, void* context, COMPRESSED_IMAGE_RESULT* completed);

/* ZIP method 14 supplies ZIP-LZMA properties before a bounded LZMA1 stream. */
int ReadZipLzmaSource(const IMAGE_FILE_SOURCE* source, uint64_t offset, uint64_t size,
	uint64_t declared, bool has_eos, uint64_t maximum_output, compressed_data_t consumer, void* context,
	COMPRESSED_IMAGE_RESULT* completed, uint32_t* crc);
