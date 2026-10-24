/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "compressed_source.h"

/* Internal provider; source validation and publication belong to the caller. */
int ReadBzip2ImageSource(const IMAGE_FILE_SOURCE* source, uint64_t maximum_output,
	compressed_data_t consumer, void* context, COMPRESSED_IMAGE_RESULT* completed);

/* ZIP method 12 uses the original BZIP2 stream inside a bounded entry. */
int ReadZipBzip2Source(const IMAGE_FILE_SOURCE* source, uint64_t offset, uint64_t size,
	uint64_t maximum_output, compressed_data_t consumer, void* context,
	COMPRESSED_IMAGE_RESULT* completed, uint32_t* crc);
