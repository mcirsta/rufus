/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Original Windows Rufus resource identity. The DLL is read as data only. */
#define SYMBOL_SERVER_USER_AGENT "Microsoft-Symbol-Server/10.0.22621.755"
#define DISKCOPY_URL "https://msdl.microsoft.com/download/symbols/diskcopy.dll/54505118173000/diskcopy.dll"
#define DISKCOPY_HASH "95fc0786f5bc0a6db5c0604b31ac18fbed0502a2c6858e5fb02a647983ae03c7"
#define DISKCOPY_SIZE 0x16ee00
#define DISKCOPY_IMAGE_OFFSET 0x66d8
#define DISKCOPY_IMAGE_SIZE 0x168000

typedef struct {
    const char* fat_name;
    const char* name;
    bool root;
} MSDOS_FILE;

#ifdef __cplusplus
extern "C" {
#endif
const MSDOS_FILE* GetMsDosFile(size_t index);
/* Original WinME real-mode patches. Other selected files are unchanged. */
bool PatchMsDosFile(const char* name, uint8_t* bytes, size_t size);
#ifdef __cplusplus
}
#endif
