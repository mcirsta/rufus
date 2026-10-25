// SPDX-License-Identifier: GPL-3.0-or-later
#include "native_input.h"

#include <stdlib.h>

int IsIsoScanCancelled(void)
{
    return 0;
}

size_t WimDirEntryCSize(void)
{
    return sizeof(struct wimlib_dir_entry);
}

size_t WimDirEntryCAlignment(void)
{
    return _Alignof(struct wimlib_dir_entry);
}

size_t WimDirEntryCStreamOffset(void)
{
    return offsetof(struct wimlib_dir_entry, streams);
}

struct wimlib_dir_entry* WimDirEntryFixture(void)
{
    const size_t stream_count = 3;
    struct wimlib_dir_entry* entry = calloc(1, sizeof(*entry) +
        stream_count * sizeof(struct wimlib_stream_entry));
    if (entry == NULL)
        return NULL;
    entry->num_named_streams = stream_count - 1;
    entry->reserved[3] = UINT64_C(0xaabbccdd11223344);
    for (size_t index = 0; index < stream_count; ++index)
        entry->streams[index].resource.uncompressed_size = 17 + index;
    return entry;
}
