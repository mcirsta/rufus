/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <wimlib.h>

#define WIM_READ_FILE_MAX_SIZE (16u * 1024u * 1024u)

#ifdef __cplusplus
extern "C" {
#endif

/* Extract one regular unnamed stream, at most 16 MiB, using a private temporary
 * directory. Return a wimlib error code; success gives malloc-owned bytes/size.
 * Empty files succeed with NULL/0; errors and cancellation clear both outputs.
 * Directories, reparse points, encrypted files and named streams are unsupported.
 * The WIM must be exclusively borrowed, without an existing progress callback.
 * This function installs a cancellation callback and removes it before returning.
 * No resource references escape; free the WIM before its borrowed ISO/UDF reader.
 * Path matching follows the host's wimlib initialization flags. Library internal
 * allocations and solid-block decompression are not bounded by this file limit. */
int ReadWimFile(WIMStruct* wim, int image, const char* path, uint8_t** data, size_t* size);

#ifdef __cplusplus
}
#endif
