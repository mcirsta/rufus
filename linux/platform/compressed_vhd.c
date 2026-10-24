/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "compressed_vhd.h"
#include "virtual_disk.h"
#include "rufus_reporting.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { VHD_SECTOR = 512, VHD_HEADER = 1024, VHD_TRANSFER = 64 * 1024,
	VHD_BAT_BATCH = 64 * 1024, VHD_EXTENT_MEMORY = 64 * 1024 * 1024 };

typedef struct {
	uint64_t disk_size, header_offset, table_offset, table_bytes, block_size, bitmap_bytes, footer_offset;
	uint32_t blocks;
	bool dynamic, parent;
	uint64_t locator_offsets[8], locator_bytes[8];
} VHD_LAYOUT;

typedef struct {
	VIRTUAL_DISK_NODE* source;
	const VHD_LAYOUT* layout;
	uint64_t first;
	uint32_t count;
	uint8_t bytes[VHD_BAT_BATCH];
} VHD_BAT_READER;

static uint32_t Be32(const uint8_t* value)
{
	return (uint32_t)value[0] << 24 | (uint32_t)value[1] << 16
		| (uint32_t)value[2] << 8 | value[3];
}

static uint64_t Be64(const uint8_t* value)
{
	return (uint64_t)Be32(value) << 32 | Be32(value + 4);
}

static bool HasChecksum(const uint8_t* bytes, size_t size, size_t checksum)
{
	uint32_t sum = 0;
	for (size_t i = 0; i < size; i++)
		if (i < checksum || i >= checksum + 4)
			sum += bytes[i];
	return ~sum == Be32(bytes + checksum);
}

static int ReadVhdBytes(VIRTUAL_DISK_NODE* source, uint64_t offset, uint8_t* bytes, size_t size)
{
	return ReadVirtualDiskFile(source, offset, bytes, size);
}

static int ParseVhdLayout(VIRTUAL_DISK_NODE* source, uint64_t maximum, VHD_LAYOUT* layout)
{
	const uint64_t file_size = (uint64_t)source->file_bytes;
	if (file_size < VHD_SECTOR)
		return EBADMSG;
	uint8_t footer[VHD_SECTOR], header[VHD_HEADER], backup[VHD_SECTOR];
	int error = ReadVhdBytes(source, file_size - VHD_SECTOR, footer, sizeof(footer));
	if (error != 0)
		return error;
	if (memcmp(footer, "conectix", 8) != 0 || !HasChecksum(footer, sizeof(footer), 64)
		|| Be32(footer + 12) != 0x00010000 || (Be32(footer + 8) & ~3u) != 0
		|| (Be32(footer + 8) & 2u) == 0 || footer[84] != 0)
		return EBADMSG;
	layout->disk_size = Be64(footer + 48);
	layout->footer_offset = file_size - VHD_SECTOR;
	if (layout->disk_size == 0 || layout->disk_size % VHD_SECTOR != 0)
		return EBADMSG;
	if (layout->disk_size > maximum)
		return EFBIG;
	memcpy(source->identifier, footer + 68, 16);
	const uint32_t kind = Be32(footer + 60);
	if (kind == 2) {
		if (Be64(footer + 16) != UINT64_MAX || layout->disk_size != layout->footer_offset)
			return EBADMSG;
		return 0;
	}
	if (kind != 3 && kind != 4)
		return EBADMSG;
	layout->parent = kind == 4;
	layout->dynamic = true;
	layout->header_offset = Be64(footer + 16);
	if (layout->header_offset % VHD_SECTOR != 0 || layout->header_offset < VHD_SECTOR
		|| layout->header_offset > layout->footer_offset
		|| VHD_HEADER > layout->footer_offset - layout->header_offset)
		return EBADMSG;
	error = ReadVhdBytes(source, 0, backup, sizeof(backup));
	if (error != 0)
		return error;
	if (memcmp(footer, backup, sizeof(footer)) != 0)
		return EBADMSG;
	error = ReadVhdBytes(source, layout->header_offset, header, sizeof(header));
	if (error != 0)
		return error;
	if (memcmp(header, "cxsparse", 8) != 0 || !HasChecksum(header, sizeof(header), 36)
		|| Be64(header + 8) != UINT64_MAX || Be32(header + 24) != 0x00010000)
		return EBADMSG;
	layout->table_offset = Be64(header + 16);
	layout->blocks = Be32(header + 28);
	layout->block_size = Be32(header + 32);
	if (layout->block_size < VHD_SECTOR || layout->block_size % VHD_SECTOR != 0
		|| (layout->block_size & (layout->block_size - 1)) != 0
		|| layout->blocks == 0 || layout->blocks < 1 + (layout->disk_size - 1) / layout->block_size)
		return EBADMSG;
	/* The bitmap has one bit per 512-byte sector and is sector padded. */
	const uint64_t bitmap_bits = layout->block_size / VHD_SECTOR;
	layout->bitmap_bytes = ((bitmap_bits + 4095) / 4096) * VHD_SECTOR;
	layout->table_bytes = (uint64_t)layout->blocks * 4;
	const uint64_t table_end = layout->table_offset + layout->table_bytes;
	if (layout->table_offset % VHD_SECTOR != 0 || layout->table_offset < VHD_SECTOR
		|| table_end < layout->table_offset || table_end > layout->footer_offset
		|| (layout->table_offset < layout->header_offset + VHD_HEADER
			&& table_end > layout->header_offset))
		return EBADMSG;
	if (layout->parent) {
		for (unsigned i = 0; i < 8; i++) {
			const uint8_t* locator = header + 576 + i * 24;
			const uint64_t offset = Be64(locator + 16), bytes = Be32(locator + 8);
			const uint64_t space = (uint64_t)Be32(locator + 4) * VHD_SECTOR;
			if (Be32(locator) == 0) continue;
			if (bytes == 0 || bytes > 65536 || space < bytes || Be32(locator + 12) != 0
				|| offset % VHD_SECTOR != 0 || offset < VHD_SECTOR
				|| offset > layout->footer_offset || space > layout->footer_offset - offset
				|| (offset < table_end && layout->table_offset < offset + space)
				|| (offset < layout->header_offset + VHD_HEADER && layout->header_offset < offset + space)) return EBADMSG;
			for (unsigned j = 0; j < i; j++)
				if (layout->locator_bytes[j] && offset < layout->locator_offsets[j] + layout->locator_bytes[j]
					&& layout->locator_offsets[j] < offset + space) return EBADMSG;
			layout->locator_offsets[i] = offset;
			layout->locator_bytes[i] = space;
		}
	}
	return 0;
}

static int BatEntry(VHD_BAT_READER* reader, uint64_t index, uint64_t* block_offset)
{
	const VHD_LAYOUT* layout = reader->layout;
	if (index < reader->first || index - reader->first >= reader->count) {
		reader->first = index;
		const uint64_t remaining = layout->blocks - index;
		reader->count = remaining < VHD_BAT_BATCH / 4
			? (uint32_t)remaining : VHD_BAT_BATCH / 4;
		const int error = ReadVhdBytes(reader->source, layout->table_offset + index * 4,
			reader->bytes, reader->count * 4);
		if (error != 0)
			return error;
	}
	const uint32_t sector = Be32(reader->bytes + (index - reader->first) * 4);
	if (sector == UINT32_MAX) {
		*block_offset = UINT64_MAX;
		return 0;
	}
	*block_offset = (uint64_t)sector * VHD_SECTOR;
	const uint64_t end = *block_offset + layout->bitmap_bytes + layout->block_size;
	if (end < *block_offset || end > layout->footer_offset
		|| *block_offset < VHD_SECTOR
		|| (*block_offset < layout->table_offset + layout->table_bytes
			&& end > layout->table_offset)
		|| (*block_offset < layout->header_offset + VHD_HEADER
			&& end > layout->header_offset))
		return EBADMSG;
	for (unsigned i = 0; i < 8; i++)
		if (layout->locator_bytes[i] != 0 && *block_offset < layout->locator_offsets[i] + layout->locator_bytes[i]
			&& layout->locator_offsets[i] < end) return EBADMSG;
	return 0;
}

static int CompareOffset(const void* left, const void* right)
{
	const uint64_t a = *(const uint64_t*)left, b = *(const uint64_t*)right;
	return a < b ? -1 : a > b ? 1 : 0;
}

static int ValidateVhdBat(VHD_BAT_READER* reader, uint64_t needed)
{
	if (needed > VHD_EXTENT_MEMORY / sizeof(uint64_t))
		return ENOTSUP;
	uint64_t* offsets = malloc((size_t)needed * sizeof(uint64_t));
	if (offsets == NULL)
		return ENOMEM;
	size_t allocated = 0;
	int error = 0;
	for (uint64_t i = 0; i < needed; i++) {
		uint64_t block;
		error = BatEntry(reader, i, &block);
		if (error != 0)
			break;
		if (block != UINT64_MAX)
			offsets[allocated++] = block;
	}
	if (error == 0) {
		qsort(offsets, allocated, sizeof(uint64_t), CompareOffset);
		const uint64_t span = reader->layout->bitmap_bytes + reader->layout->block_size;
		for (size_t i = 1; i < allocated; i++)
			if (offsets[i] - offsets[i - 1] < span) {
				error = EBADMSG;
				break;
			}
	}
	free(offsets);
	return error;
}


typedef struct {
	VHD_LAYOUT layout;
	VHD_BAT_READER bat;
	uint8_t* bitmap;
	uint64_t bitmap_block;
} VHD_NODE;

static int ReadVhdNode(VIRTUAL_DISK_NODE* node, uint64_t offset, void* data, size_t bytes)
{
	VHD_NODE* state = node->layout;
	const VHD_LAYOUT* layout = &state->layout;
	if (offset > node->disk_bytes || bytes > node->disk_bytes - offset) return EBADMSG;
	if (!layout->dynamic) return ReadVhdBytes(node, offset, data, bytes);
	uint8_t* output = data;
	while (bytes != 0) {
		if (IsIsoScanCancelled()) return ECANCELED;
		const uint64_t index = offset / layout->block_size, within = offset % layout->block_size;
		uint64_t physical;
		int error = BatEntry(&state->bat, index, &physical);
		if (error != 0) return error;
		if (physical != UINT64_MAX && state->bitmap_block != index) {
			error = ReadVhdBytes(node, physical, state->bitmap, (size_t)layout->bitmap_bytes);
			if (error != 0) return error;
			state->bitmap_block = index;
		}
		const uint64_t sector = within / VHD_SECTOR;
		const bool present = physical != UINT64_MAX && (state->bitmap[sector / 8] & (0x80u >> (sector % 8))) != 0;
		size_t count = VHD_SECTOR - (size_t)(within % VHD_SECTOR);
		/* Join adjacent sectors with identical ownership into one batched read. */
		while (count < bytes && count < VHD_TRANSFER && within + count < layout->block_size) {
			const uint64_t next = (within + count) / VHD_SECTOR;
			const bool owned = physical != UINT64_MAX && (state->bitmap[next / 8] & (0x80u >> (next % 8))) != 0;
			if (owned != present) break;
			count += VHD_SECTOR;
		}
		if (count > bytes) count = bytes;
		if (present) error = ReadVhdBytes(node, physical + layout->bitmap_bytes + within, output, count);
		else if (node->parent) error = node->parent->read(node->parent, offset, output, count);
		else memset(output, 0, count);
		if (error != 0) return error;
		output += count; offset += count; bytes -= count;
	}
	return 0;
}

static void ReleaseVhdNode(VIRTUAL_DISK_NODE* node)
{
	VHD_NODE* state = node->layout;
	if (state) free(state->bitmap);
	free(state);
}

static int VhdParent(VIRTUAL_DISK_NODE* node, VHD_LAYOUT* layout)
{
	uint8_t header[VHD_HEADER];
	int error = ReadVhdBytes(node, layout->header_offset, header, sizeof(header));
	if (error != 0) return error;
	char path[4096];
	/* Windows relative Unicode locator, followed by the original parent name.
	 * Do not reinterpret drive letters or UNC roots as Linux mount locations. */
	for (unsigned pass = 0; pass < 2; pass++) for (unsigned i = 0; i < 8; i++) {
		const uint8_t* locator = header + 576 + i * 24;
		if (memcmp(locator, pass == 0 ? "W2ru" : "W2ku", 4) != 0) continue;
		const size_t bytes = Be32(locator + 8);
		uint8_t* text = malloc(bytes);
		if (!text) return ENOMEM;
		error = ReadVhdBytes(node, layout->locator_offsets[i], text, bytes);
		if (error == 0) error = VirtualDiskUtf16(text, bytes, true, true, path);
		free(text);
		if (error != 0) return error;
		error = OpenVirtualDiskParent(node, path, header + 40, Be32(header + 56), true);
		if (error != ENOENT && error != ENOTDIR && error != ENOTSUP) return error;
	}
	/* Explicit per-child relocation also covers older platform locators without
	 * inventing a Windows code page or interpreting a drive letter. */
	error = OpenVirtualDiskParent(node, "@parent:", header + 40, Be32(header + 56), true);
	if (error != ENOTSUP) return error;
	error = VirtualDiskUtf16(header + 64, 512, false, true, path);
	return error != 0 ? error : OpenVirtualDiskParent(node, path, header + 40, Be32(header + 56), true);
}

int InitializeVhdNode(VIRTUAL_DISK_NODE* node, uint64_t maximum)
{
	VHD_NODE* state = calloc(1, sizeof(*state));
	if (!state) return ENOMEM;
	node->layout = state; node->release = ReleaseVhdNode; node->read = ReadVhdNode;
	int error = ParseVhdLayout(node, maximum, &state->layout);
	node->disk_bytes = state->layout.disk_size; node->logical_sector = VHD_SECTOR;
	state->bat.source = node; state->bat.layout = &state->layout;
	state->bitmap_block = UINT64_MAX;
	if (error == 0 && state->layout.dynamic) {
		error = ValidateVhdBat(&state->bat, 1 + (node->disk_bytes - 1) / state->layout.block_size);
		if (error == 0) {
			state->bitmap = malloc((size_t)state->layout.bitmap_bytes);
			if (!state->bitmap) error = ENOMEM;
		}
	}
	if (error == 0 && state->layout.parent) error = VhdParent(node, &state->layout);
	return error;
}

int ReadVhdImageSource(const IMAGE_FILE_SOURCE* source, uint64_t maximum_output,
	compressed_data_t consumer, void* context, COMPRESSED_IMAGE_RESULT* completed)
{
	return StreamVirtualDiskSource(source, IMG_COMPRESSION_VHD, maximum_output, consumer, context, completed);
}
