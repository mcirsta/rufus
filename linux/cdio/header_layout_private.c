/* SPDX-License-Identifier: GPL-3.0-or-later */
#define CDIO_NATIVE_READER 1
#include "header_layout.h"
#include <string.h>

#define CDIO_LAYOUT_VALUE(name, value) value,
size_t CdioPrivateHeaderLayout(size_t* values, size_t capacity)
{
    static const size_t native_layout[] = { CDIO_HEADER_LAYOUT_CASES(CDIO_LAYOUT_VALUE) };
    if (capacity < CDIO_HEADER_LAYOUT_COUNT)
        return 0;
    memcpy(values, native_layout, sizeof(native_layout));
    return CDIO_HEADER_LAYOUT_COUNT;
}
