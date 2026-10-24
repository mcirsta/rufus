/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "compressed_source.h"

/* Read the logical disk from a fixed or dynamic VHD. Differencing disks need
 * their parent chain and are deliberately rejected. */
int ReadVhdImageSource(const IMAGE_FILE_SOURCE* source, uint64_t maximum_output,
	compressed_data_t consumer, void* context, COMPRESSED_IMAGE_RESULT* completed);
