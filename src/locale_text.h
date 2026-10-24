/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Original quoted-argument rules, including removal of the backslash before
 * an escaped quote. Mutates one logical line; preserves subsequent arguments. */
bool ReadLocaleQuoted(char* line, size_t capacity, size_t* position, size_t* start);
/* Bounded in-memory equivalent of get_loc_data_file's UTF-8 line collation and
 * escapes. Returns 1=line, 0=EOF, -1=invalid/too large. Position is a byte offset. */
int ReadLocaleLine(const char* data, size_t size, size_t* position, char* line, size_t capacity);
#ifdef __cplusplus
}
#endif
