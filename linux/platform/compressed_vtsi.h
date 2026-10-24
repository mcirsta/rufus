/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "compressed_source.h"

/* Reconstruct the logical disk image from Ventoy's sparse segment layout. */
int ReadVtsiImageSource(const IMAGE_FILE_SOURCE* source, uint64_t maximum_output,
	compressed_data_t consumer, void* context, COMPRESSED_IMAGE_RESULT* completed);

/* Visit validated, ordered segments. source_offset follows the original
 * decoder's consecutive data-area interpretation, not data_offset metadata.
 * The callback must not retain source data or mutate the source descriptor. */
typedef int (*vtsi_segment_t)(void* context, uint64_t disk_offset, uint64_t source_offset,
	uint64_t bytes);
int VisitVtsiImageSegments(const IMAGE_FILE_SOURCE* source, const char* path, uint64_t maximum_output,
	vtsi_segment_t visitor, void* context, uint64_t* disk_size, uint64_t* data_bytes);
