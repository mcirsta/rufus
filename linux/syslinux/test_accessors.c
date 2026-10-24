/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <string.h>
#include "syslxint.h"
int main(void)
{
    unsigned char bytes[32];
    const uint64_t values[] = {0,UINT64_MAX,UINT64_C(0x0123456789abcdef),UINT64_C(0xfedcba9876543210)};
    for (unsigned offset = 1; offset < 8; ++offset) for (unsigned i = 0; i < sizeof(values)/sizeof(values[0]); ++i) {
        memset(bytes,0x55,sizeof(bytes));
        set_64((uint64_t*)(bytes+offset),values[i]);
        if (get_64((const uint64_t*)(bytes+offset)) != values[i]) return 1;
        for (unsigned j = 0; j < 8; ++j) if (bytes[offset+j] != ((values[i]>>(8*j))&255)) return 2;
        if (get_32((const uint32_t*)(bytes+offset)) != (uint32_t)values[i]) return 3;
        if (get_16((const uint16_t*)(bytes+offset)) != (uint16_t)values[i]) return 4;
        if (bytes[offset-1] != 0x55 || bytes[offset+8] != 0x55) return 5;
    }
    return 0;
}
