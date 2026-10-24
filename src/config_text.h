/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <wchar.h>

#define CONFIG_TEXT_LINE_CAPACITY 1024
#define CONFIG_TEXT_MAX_REPLACEMENTS 4

/* The ordered label passes, followed by the optional Red Hat passes. */
#define ISO_CONFIG_LABEL_PASSES 7
#define ISO_CONFIG_MAX_PATCHES (2 * ISO_CONFIG_LABEL_PASSES)
#define CONFIG_VOLUME_PATH_CAPACITY 260 /* Original MAX_PATH-sized FreeNAS buffers. */
typedef struct {
	const char* token;
	const char* source;
	const char* replacement;
} CONFIG_TEXT_PATCH;

#ifdef __cplusplus
extern "C" {
#endif

/* Labels passed here already have spaces escaped. image_path is the original
 * user-selected path, not a retained-descriptor alias. Returned pointers borrow
 * those labels or refer to literals; no allocation or file I/O. */
size_t GetIsoConfigPatches(const char* iso_label, const char* usb_label,
	bool rh8_derivative, const char* image_path, CONFIG_TEXT_PATCH patches[ISO_CONFIG_MAX_PATCHES]);

/* FreeNAS's set directive uses literal label spaces, unlike kernel options.
 * Returned pointers borrow the caller's buffers. Labels come from the bounded
 * image report; formatting retains the original MAX_PATH limit. */
CONFIG_TEXT_PATCH GetFreeNasConfigPatch(const char* iso_label, const char* usb_label,
	char source[CONFIG_VOLUME_PATH_CAPACITY], char replacement[CONFIG_VOLUME_PATH_CAPACITY]);

/* Original parser.c character expansion. Caller frees the returned string. */
char* replace_char(const char* src, const char c, const char* rep);

/* Section insertion's original prefix match after leading spaces/tabs.
 * Strings are terminated. Caller writes the original line then the inserted
 * data for every match; encoding, chunking and file operations stay native. */
bool IsConfigSection(const wchar_t* line, const wchar_t* section);

/* Matching loop from parser.c's replace_in_token_data. Skip spaces/tabs, match
 * the token case-insensitively at the start, require a space/tab after it, then
 * find up to four case-sensitive, non-overlapping source substrings in its data.
 * All arguments are non-NULL and strings are terminated. Offsets identify the
 * fragments before/between/after matches; matches are replaced with NULs in line.
 * Returns the match count. Zero leaves line unchanged. Encoding, chunk boundaries,
 * replacement text and file I/O belong to the caller. */
size_t SplitConfigToken(wchar_t* line, const wchar_t* token, const wchar_t* source,
	size_t offsets[CONFIG_TEXT_MAX_REPLACEMENTS + 1]);

#ifdef __cplusplus
}
#endif
