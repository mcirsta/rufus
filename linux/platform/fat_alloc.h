/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stdlib.h>

/* Selected only by the native libfat build. Preserve its aligned-buffer
 * contract without requiring x86 intrinsics on Linux architectures. */
static inline void* RufusFatAlignedAlloc(size_t size, size_t alignment)
{
	void* buffer = NULL;
	if (posix_memalign(&buffer, alignment, size) != 0)
		return NULL;
	return buffer;
}

#define _mm_malloc RufusFatAlignedAlloc
#define _mm_free free
