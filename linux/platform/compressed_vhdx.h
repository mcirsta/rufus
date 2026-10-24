/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "compressed_source.h"

int ReadVhdxImageSource(const IMAGE_FILE_SOURCE* source, uint64_t maximum_output,
	compressed_data_t consumer, void* context, COMPRESSED_IMAGE_RESULT* completed);
