/* Rufus compressed-image selection rules, shared with Windows vhd.c.
 * Copyright © 2013-2026 Pete Batard <pete@akeo.ie>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include "bled/compression.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define IMG_COMPRESSION_FFU     (BLED_COMPRESSION_MAX)
#define IMG_COMPRESSION_VHD     (BLED_COMPRESSION_MAX + 1)
#define IMG_COMPRESSION_VHDX    (BLED_COMPRESSION_MAX + 2)

#define COMPRESSED_BOOT_SECTOR_BYTES 512

/* Extensions are ASCII identifiers. Avoid depending on host locale. */
static inline unsigned char CompressionExtensionLower(unsigned char byte)
{
    return byte >= 'A' && byte <= 'Z' ? (unsigned char)(byte + 'a' - 'A') : byte;
}

static inline bool CompressionExtensionEqual(const char* first, const char* second)
{
    if (first == NULL || second == NULL)
        return false;
    while (*first && *second) {
        if (CompressionExtensionLower((unsigned char)*first++) != CompressionExtensionLower((unsigned char)*second++))
            return false;
    }
    return *first == *second;
}

/* Original filename policy, including the reserved .7z identifier's absence
 * and FFU/VHD/VHDX association values. A candidate does not prove validity. */
static inline int GetImageCompressionType(const char* path)
{
typedef struct {
	const char* ext;
	uint8_t type;
} comp_assoc;

static comp_assoc file_assoc[] = {
	{ ".zip", BLED_COMPRESSION_ZIP },
	{ ".Z", BLED_COMPRESSION_LZW },
	{ ".gz", BLED_COMPRESSION_GZIP },
	{ ".lzma", BLED_COMPRESSION_LZMA },
	{ ".bz2", BLED_COMPRESSION_BZIP2 },
	{ ".bzip2", BLED_COMPRESSION_BZIP2 },
	{ ".xz", BLED_COMPRESSION_XZ },
	{ ".vtsi", BLED_COMPRESSION_VTSI },
	{ ".zst", BLED_COMPRESSION_ZSTD },
	{ ".ffu", BLED_COMPRESSION_MAX },
	{ ".vhd", BLED_COMPRESSION_MAX + 1 },
	{ ".vhdx", BLED_COMPRESSION_MAX + 2 },
};


    const char* ext = NULL;
    const size_t length = path == NULL ? 0 : strlen(path);
    if (length > 4)
        for (ext = path + length - 1; *ext != '.' && ext != path; ext--);
    for (size_t i = 0; i < sizeof(file_assoc) / sizeof(file_assoc[0]); i++) {
        if (CompressionExtensionEqual(ext, file_assoc[i].ext))
            return file_assoc[i].type;
    }
    return BLED_COMPRESSION_NONE;
}

/* Original compressed-prefix rule: 0 absent/short, 1 marker, 2 explicit force.
 * This is a selection fact; it establishes neither bootability nor data validity. */
static inline int GetCompressedBootMarker(const void* data, size_t size, bool ignore)
{
    if (data == NULL || size != COMPRESSED_BOOT_SECTOR_BYTES)
        return 0;
    const uint8_t* bytes = (const uint8_t*)data;
    if (bytes[510] == 0x55 && bytes[511] == 0xaa)
        return 1;
    return ignore ? 2 : 0;
}
