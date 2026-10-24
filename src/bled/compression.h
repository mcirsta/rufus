/* Bled compression identifiers shared by Windows and native adapters.
 * Copyright © 2014-2026 Pete Batard <pete@akeo.ie>
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

typedef enum {
	BLED_COMPRESSION_NONE = 0,
	BLED_COMPRESSION_ZIP,		// .zip
	BLED_COMPRESSION_LZW,		// .Z
	BLED_COMPRESSION_GZIP,		// .gz
	BLED_COMPRESSION_LZMA,		// .lzma
	BLED_COMPRESSION_BZIP2,		// .bz2
	BLED_COMPRESSION_XZ,		// .xz
	BLED_COMPRESSION_7ZIP,		// .7z
	BLED_COMPRESSION_VTSI,		// .vtsi
	BLED_COMPRESSION_ZSTD,		// .zst
	BLED_COMPRESSION_MAX
} bled_compression_type;
