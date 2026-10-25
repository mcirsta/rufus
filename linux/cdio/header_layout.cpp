// SPDX-License-Identifier: GPL-3.0-or-later
#include "header_layout.h"
#include <cstdio>

#define CDIO_LAYOUT_VALUE(name, value) value,
#define CDIO_LAYOUT_NAME(name, value) #name,

int main()
{
    const size_t cppLayout[] = { CDIO_HEADER_LAYOUT_CASES(CDIO_LAYOUT_VALUE) };
    const char* names[] = { CDIO_HEADER_LAYOUT_CASES(CDIO_LAYOUT_NAME) };
    size_t readerLayout[CDIO_HEADER_LAYOUT_COUNT] = {};
    size_t publicLayout[CDIO_HEADER_LAYOUT_COUNT] = {};
    if (CdioPrivateHeaderLayout(readerLayout, CDIO_HEADER_LAYOUT_COUNT) != CDIO_HEADER_LAYOUT_COUNT
        || CdioPublicHeaderLayout(publicLayout, CDIO_HEADER_LAYOUT_COUNT) != CDIO_HEADER_LAYOUT_COUNT)
        return 1;
    for (size_t index = 0; index < CDIO_HEADER_LAYOUT_COUNT; index++) {
        if (readerLayout[index] != publicLayout[index] || readerLayout[index] != cppLayout[index]) {
            std::fprintf(stderr, "%s: reader C=%zu, public C=%zu, public C++=%zu\n",
                names[index], readerLayout[index], publicLayout[index], cppLayout[index]);
            return 1;
        }
    }
    std::printf("libcdio: %u private C/public C/public C++ sizes and offsets match.\n",
        unsigned(CDIO_HEADER_LAYOUT_COUNT));
    return 0;
}
