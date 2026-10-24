/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Packed Syslinux fields can occur at any byte alignment. Verify actual bytes,
 * neighboring guards and reads under UBSan, not just paired getter/setter output. */
#include <stddef.h>
#include "syslinux/libinstaller/syslxint.h"
#include <stdio.h>

int main(void)
{
    static const unsigned char expected[] = { 0x10, 0x32, 0x54, 0x76, 0x98, 0xba, 0xdc, 0xfe };
    unsigned char buffer[32];
    for (size_t offset = 8; offset < 16; offset++) {
        for (size_t size = 2; size <= 8; size *= 2) {
            memset(buffer, 0x55, sizeof(buffer));
            void* field = buffer + offset;
            if (size == 2)
                set_16(field, UINT16_C(0x3210));
            else if (size == 4)
                set_32(field, UINT32_C(0x76543210));
            else
                set_64(field, UINT64_C(0xfedcba9876543210));
            if (memcmp(field, expected, size) != 0)
                return 1;
            for (size_t i = 0; i < sizeof(buffer); i++) {
                const int outside = i < offset || i >= offset + size;
                if (outside && buffer[i] != 0x55)
                    return 2;
            }
            memcpy(field, expected, sizeof(expected));
            if (get_16(field) != UINT16_C(0x3210))
                return 3;
            if (get_32(field) != UINT32_C(0x76543210))
                return 4;
            if (get_64(field) != UINT64_C(0xfedcba9876543210))
                return 5;
        }
    }
    puts("24 packed-field cases passed at all byte alignments.");
    return 0;
}
