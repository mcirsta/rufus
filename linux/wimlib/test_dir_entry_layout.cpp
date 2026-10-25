// SPDX-License-Identifier: GPL-3.0-or-later
extern "C" {
#include "native_input.h"
size_t WimDirEntryCSize(void);
size_t WimDirEntryCAlignment(void);
size_t WimDirEntryCStreamOffset(void);
struct wimlib_dir_entry* WimDirEntryFixture(void);
}

#include <cstdio>
#include <cstdlib>
#include <type_traits>

int main()
{
    static_assert(std::is_standard_layout<wimlib_dir_entry>::value);
    const bool matching_layout = WimDirEntryCSize() == sizeof(wimlib_dir_entry) &&
        WimDirEntryCAlignment() == alignof(wimlib_dir_entry) &&
        WimDirEntryCStreamOffset() == sizeof(wimlib_dir_entry);
    if (!matching_layout)
    {
        std::fputs("wimlib directory-entry C/C++ prefix layout differs\n", stderr);
        return 1;
    }
    wimlib_dir_entry* entry = WimDirEntryFixture();
    if (entry == nullptr)
        return 2;
    const bool matching_prefix = entry->num_named_streams == 2 &&
        entry->reserved[3] == UINT64_C(0xaabbccdd11223344);
    const wimlib_stream_entry* streams = WimDirEntryStreams(entry);
    bool matching_streams = true;
    for (size_t index = 0; index < 3; ++index)
        matching_streams = matching_streams &&
            streams[index].resource.uncompressed_size == 17 + index;
    std::free(entry);
    if (!matching_prefix || !matching_streams)
    {
        std::fputs("wimlib directory-entry stream bridge changed data\n", stderr);
        return 3;
    }
    return 0;
}
