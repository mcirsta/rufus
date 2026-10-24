// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "iso_file.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    const char* path;
    uint64_t size;
    bool directory;
    void* reader; // Borrowed only during the synchronous visitor.
} EMBEDDED_EFI_FILE;
// Native extension of Rufus embedded_efi.c's retained-view FAT adapter. No mount.
int WalkEmbeddedEfiFiles(iso9660_t* iso, udf_t* udf, const char* image,
    int (*visit)(const EMBEDDED_EFI_FILE*, void*), void* context);
int ReadEmbeddedEfiRange(const EMBEDDED_EFI_FILE* file, uint64_t offset, void* bytes, size_t size);
// Source tree wins, except Rufus's detected broken small x64 fallback.
int EmbeddedEfiPrecedence(iso9660_t* iso, udf_t* udf, const RUFUS_IMG_REPORT* report,
    const EMBEDDED_EFI_FILE* file, bool* copy);
#ifdef __cplusplus
}
#endif
