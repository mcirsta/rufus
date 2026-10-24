/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "config_text.h"

#define CONFIG_PATCH_MAX_BYTES (8 * 1024 * 1024)

#ifdef __cplusplus
extern "C" {
#endif

/* ISO-config form of Rufus's replace_in_token_data(..., TRUE). The caller owns
 * a private directory and retains parent throughout this call; name is one
 * basename. Parameters are UTF-8. Accept UTF-8 (with/without BOM) and UTF-16LE
 * with BOM, preserving encoding. Malformed text and NUL/EOF control markers are
 * rejected, not silently lost. Input/output are bounded to 8 MiB for these
 * whole-file text edits. Match in Rufus's 1023 UTF-16-unit chunks, at most four
 * replacements per chunk. Changed files use LF; unchanged files retain every
 * original byte and inode. Does not choose tokens or image-specific rules.
 *
 * Returns 0 or errno. Writes/flushing/close/check failures before atomic rename
 * preserve the original. modified becomes true on rename, even if the following
 * directory flush fails. Existing links/special files are refused; temp files
 * are unique, in the same directory, and cleaned on failure. IsIsoScanCancelled
 * supplies cancellation. No privileges, GUI or QCoreApplication are required. */
int ReplaceConfigTokenAt(int parent, const char* name, const char* token,
	const char* source, const char* replacement, bool* modified);

/* Apply the supplied passes in order, preserving each pass's chunking and
 * encoding round trip, but commit the final file once. Empty sources and equal
 * source/replacement pairs are no-ops, as in Rufus. A later failure preserves
 * the original file instead of leaving only some passes applied. */
int ReplaceConfigTokensAt(int parent, const char* name, const CONFIG_TEXT_PATCH* patches,
	size_t count, bool* modified);

/* Run exactly the same transformations without writes. Used on bounded source
 * config data during image preflight, before any partitioning/formatting.
 * output_size receives the final size on success, for filesystem-limit checks. */
int CheckConfigText(const void* bytes, size_t size, const CONFIG_TEXT_PATCH* patches, size_t count,
	size_t* output_size);

/* The ISO variant prepends Rufus's first-matching Ubuntu/Debian persistence
 * pass, then applies the normal label/workaround passes in one atomic edit. */
int ReplaceIsoConfigTokensAt(int parent, const char* name, const CONFIG_TEXT_PATCH* patches,
    size_t count, bool persistence, bool grub, bool* modified);
int CheckIsoConfigText(const void* bytes, size_t size, const CONFIG_TEXT_PATCH* patches,
    size_t count, bool persistence, bool grub, size_t* output_size);

#ifdef __cplusplus
}

#include <QByteArray>
/* Buffer form of insert_section_data(..., FALSE). Shares decoding and the
 * original section matcher. Preserve BOM/encoding, 1023 UTF-16-unit chunks,
 * repeated prefix matches, bare CR and missing final newline; output uses CRLF.
 * Accept ASCII without BOM or valid UTF-8/UTF-16LE with BOM. Non-ASCII without
 * BOM requires a known code page and is refused. Return ENOENT for no matching
 * section, errno for invalid/oversized/cancelled input; output is empty on error.
 * Does not perform I/O or establish that the configuration is bootable. */
int InsertConfigSectionText(const QByteArray& bytes, const char* section, const char* data, QByteArray& output);
#endif
