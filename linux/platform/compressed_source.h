/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "image_source.h"
#include "image_compression.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	int compression_type;
	uint64_t source_bytes, decoded_bytes, members, trailing_bytes;
	size_t prefix_size;
	uint8_t prefix[COMPRESSED_BOOT_SECTOR_BYTES];
	bool has_boot_marker;
	uint8_t layout_sha256[32]; /* FFU authenticated metadata/payload identity. */
	unsigned ffu_store, ffu_stores;
	uint64_t ffu_minimum_bytes; /* Optimized FFU target bound; logical decode retains captured geometry. */
} COMPRESSED_IMAGE_RESULT;

/* Optional consumer of provisional decompressed bytes, in order. Return 0 or
 * a positive errno; do not throw or retain the buffer. A later checksum/source
 * failure invalidates the operation, even after data has been delivered. */
typedef int (*compressed_data_t)(void* context, const uint8_t* bytes, size_t size, uint64_t offset);

/* Read-only image-source foundation. Implements .gz, .xz, .bz2, .lzma, .Z, .zst, .vtsi, .ffu, .vhd, .vhdx and
 * ZIP stored/deflate/BZIP2/LZMA/XZ entries; unknown ZIP methods and other
 * original associations return ENOTSUP pending their providers. Initial Unix-
 * compress magic inside .gz dispatches to LZW as on Windows, retaining filename
 * association 3. Parent files are opened read-only; no target or source is written.
 * Uses a retained ordinary source and pread, preserving ownership and offset.
 *
 * Validate all contiguous actual gzip members, including CRC/ISIZE, count actual output
 * in 64 bits and retain only its first sector. Original gzip trailing-garbage
 * handling is preserved and reported; it is not interpreted as image data.
 * XZ reuses the original decoder and host BCJ set with its 64-MiB dictionary
 * limit. Validate none/CRC32/CRC64/SHA256 checks; reserved checks are unsupported.
 * As on Windows, stop after the first XZ stream and report remaining bytes.
 * BZIP2 reuses the original Huffman/BWT/output decoder, validates each block and
 * combined CRC (including empty members), joins contiguous streams as on Windows
 * and reports a non-BZ tail. Obsolete randomized blocks remain unsupported.
 * LZMA-Alone reuses the original range/model/dictionary decoder and its property
 * range and 1-GiB dictionary cap. Stop at the known size or unknown-size EOS,
 * report remaining bytes, and reject known-size mismatches and invalid history.
 * LZMA has no container checksum. Unix-compress (.Z) reuses the original LZW
 * dictionary/width/CLEAR decoder. Native nine-bit resets stay at nine bits to
 * prevent the original wrapper's silent output loss at dictionary saturation.
 * It consumes to EOF without a size, checksum or end marker; incomplete final
 * code bits are ignored. A truncated stream can be a valid shorter stream.
 * All providers count actual output in 64 bits.
 * ZIP retains the original first-central-entry selection (including a leading
 * directory); it does not search by name or marker. Bound ordinary/ZIP64 records
 * and selected compressed data, validate every selected entry's CRC, and retain
 * actual decoded output despite a declared-length mismatch for compressed methods.
 * Stored sizes must agree. ZIP-LZMA's short properties header is translated to
 * the original decoder's EOS or declared-size form according to ZIP flag bit 1;
 * the ZIP CRC is checked.
 * ZIP trailing_bytes includes all archive bytes after actual entry consumption,
 * including other files/directory/comment and ignored compressed tails.
 * Encryption, split volumes and unknown ZIP methods return ENOTSUP.
 * Zstd reuses the bundled original decoder with its 128-MiB window limit. It
 * validates checked frames, consumes all concatenated and skippable frames,
 * counts each completed frame (including skippable frames) as a member, and
 * rejects a truncated frame or non-Zstd tail.
 * VTSI validates the original Ventoy footer and segment checksums, version,
 * bounds and ordered sparse layout. It reconstructs a logical image with zero
 * gaps for analysis, using the original sequential data-area interpretation.
 * The native VTSI writer uses the validated segment map to preserve target
 * gaps; this logical source stream is used for analysis and frozen hashing.
 * VHD validates the legacy footer, dynamic header, BAT and sector bitmaps, then
 * streams fixed, dynamic or differencing logical disk bytes through a retained
 * parent chain with UUID, timestamp and geometry checks.
 * VHDX validates redundant CRC-32C roots, regions, metadata and BAT extents,
 * then streams fixed/dynamic/differencing payloads with LSB sector bitmaps.
 * Pending version-0 logs replay into a bounded private overlay before metadata
 * reads. Parents use validated relative paths and retained read-only descriptors;
 * source files and headers are never repaired or modified.
 * FFU reconstructs the manifest's captured disk, resolves repeated/overwritten
 * and end-relative blocks, zeroes omitted blocks and validates every SHA256
 * chunk before output. Single-store full V1/V2 and V1-compressed none, plain
 * XPRESS, LZNT1 and Huffman-XPRESS are supported, including grouped buffers
 * up to 16 MiB and bounded multi-block/end-relative runs. Catalog signatures
 * follow the desktop FFU policy. Logical decoding retains captured geometry;
 * optimized resizing belongs to target deployment/helper finalization. V2 multiple stores require
 * an explicit one-based source.ffu_store; all metadata/payloads are authenticated,
 * including unselected stores. Require 512/4096-sector captured sizes and
 * block/hash chunks up to 16 MiB. Partial updates are deployed against their
 * validated base through the target-aware writer, not this logical zero-gap stream.
 * maximum_output is an explicit byte bound (1..INT64_MAX), not a size hint.
 * Per-call input/output buffers and decoder allocation budget are bounded.
 * Return 0 or errno; errors/cancellation clear result. Publish only after source
 * identity has been rechecked. Bytes delivered to a consumer remain provisional.
 * Does not establish bootability, media geometry or permission to write anything.
 */
int ReadCompressedImageSource(const IMAGE_FILE_SOURCE* source, const char* path,
	uint64_t maximum_output, compressed_data_t consumer, void* context, COMPRESSED_IMAGE_RESULT* result);

#ifdef __cplusplus
}
#endif
