/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Read-only MS-VHDX adapter. File layout and GUIDs follow Microsoft's
 * MS-VHDX specification, sections 2.2, 2.5 and 2.6. Never replay a log into
 * the caller's source or resolve an unretained parent file. */
#include "compressed_vhdx.h"
#include "virtual_disk.h"
#include "rufus_reporting.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { VHDX_MIB = 1024 * 1024, VHDX_TABLE_BYTES = 64 * 1024,
	VHDX_HEADER_BYTES = 4096, VHDX_MAX_REGIONS = 2047,
	VHDX_EXTENT_MEMORY = 64 * 1024 * 1024 };

typedef struct { uint64_t offset, bytes; } VHDX_EXTENT;
typedef struct {
	VHDX_EXTENT regions[VHDX_MAX_REGIONS + 2];
	size_t region_count;
	VHDX_EXTENT bat, metadata;
	uint64_t disk_bytes, payload_blocks, bat_entries, chunk_ratio;
	uint32_t block_bytes, logical_sector, parent_bytes;
	uint64_t parent_offset;
	bool has_parent;
} VHDX_LAYOUT;

typedef struct {
	VIRTUAL_DISK_NODE* source;
	const VHDX_LAYOUT* layout;
	uint64_t first;
	size_t count;
	uint8_t bytes[VHDX_TABLE_BYTES];
} VHDX_BAT_READER;

/* On-disk GUID byte order, including the little-endian first three fields. */
static const uint8_t bat_guid[16] = {0x66,0x77,0xc2,0x2d,0x23,0xf6,0x00,0x42,0x9d,0x64,0x11,0x5e,0x9b,0xfd,0x4a,0x08};
static const uint8_t metadata_guid[16] = {0x06,0xa2,0x7c,0x8b,0x90,0x47,0x9a,0x4b,0xb8,0xfe,0x57,0x5f,0x05,0x0f,0x88,0x6e};
static const uint8_t item_guid[6][16] = {
	{0x37,0x67,0xa1,0xca,0x36,0xfa,0x43,0x4d,0xb3,0xb6,0x33,0xf0,0xaa,0x44,0xe7,0x6b},
	{0x24,0x42,0xa5,0x2f,0x1b,0xcd,0x76,0x48,0xb2,0x11,0x5d,0xbe,0xd8,0x3b,0xf4,0xb8},
	{0xab,0x12,0xca,0xbe,0xe6,0xb2,0x23,0x45,0x93,0xef,0xc3,0x09,0xe0,0x00,0xc7,0x46},
	{0x1d,0xbf,0x41,0x81,0x6f,0xa9,0x09,0x47,0xba,0x47,0xf2,0x33,0xa8,0xfa,0xab,0x5f},
	{0xc7,0x48,0xa3,0xcd,0x5d,0x44,0x71,0x44,0x9c,0xc9,0xe9,0x88,0x52,0x51,0xc5,0x56},
	{0x2d,0x5f,0xd3,0xa8,0x0b,0xb3,0x4d,0x45,0xab,0xf7,0xd3,0xd8,0x48,0x34,0xab,0x0c}
};

static uint32_t Le32(const uint8_t* p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t Le64(const uint8_t* p)
{
	return Le32(p) | (uint64_t)Le32(p + 4) << 32;
}

static bool VhdxChecksum(const uint8_t* data, size_t bytes)
{
	/* CRC-32C Castagnoli, with the checksum field zero for this calculation.
	 * Only the small headers/tables use it; payload I/O stays batched. */
	uint32_t crc = UINT32_MAX;
	for (size_t i = 0; i < bytes; i++) {
		crc ^= i >= 4 && i < 8 ? 0 : data[i];
		for (unsigned bit = 0; bit < 8; bit++)
			crc = (crc >> 1) ^ (0x82f63b78u & (0u - (crc & 1)));
	}
	return ~crc == Le32(data + 4);
}

static int ReadVhdxBytes(VIRTUAL_DISK_NODE* source, uint64_t offset, void* buffer, size_t bytes)
{
	return ReadVirtualDiskFile(source, offset, buffer, bytes);
}

static int CompareVhdxExtent(const void* a, const void* b)
{
	const VHDX_EXTENT* left = a;
	const VHDX_EXTENT* right = b;
	return left->offset < right->offset ? -1 : left->offset > right->offset ? 1 : 0;
}

static int CheckVhdxExtents(VHDX_EXTENT* extents, size_t count)
{
	qsort(extents, count, sizeof(*extents), CompareVhdxExtent);
	for (size_t i = 1; i < count; i++)
		if (extents[i].offset - extents[i - 1].offset < extents[i - 1].bytes)
			return EBADMSG;
	return 0;
}

static int VhdxHeader(VIRTUAL_DISK_NODE* source, VHDX_LAYOUT* layout)
{
	uint8_t signature[8], headers[2][VHDX_HEADER_BYTES];
	int error = ReadVhdxBytes(source, 0, signature, sizeof(signature));
	if (error != 0)
		return error;
	if (memcmp(signature, "vhdxfile", sizeof(signature)) != 0)
		return EBADMSG;
	bool valid[2];
	for (unsigned i = 0; i < 2; i++) {
		error = ReadVhdxBytes(source, (i + 1) * VHDX_TABLE_BYTES, headers[i], sizeof(headers[i]));
		if (error != 0)
			return error;
		valid[i] = memcmp(headers[i], "head", 4) == 0 && VhdxChecksum(headers[i], sizeof(headers[i]));
	}
	if (!valid[0] && !valid[1])
		return EBADMSG;
	const uint64_t first_sequence = Le64(headers[0] + 8), second_sequence = Le64(headers[1] + 8);
	if (valid[0] && valid[1] && first_sequence == second_sequence
		&& memcmp(headers[0], headers[1], sizeof(headers[0])) != 0)
		return EBADMSG;
	const uint8_t* header = headers[!valid[0] || (valid[1] && second_sequence > first_sequence)];
	if (header[66] != 1 || header[67] != 0)
		return ENOTSUP;
	memcpy(source->identifier, header + 32, 16);
	VHDX_EXTENT log = {Le64(header + 72), Le32(header + 68)};
	const uint64_t file_bytes = (uint64_t)source->file_bytes;
	if (log.offset < VHDX_MIB || log.offset % VHDX_MIB != 0 || log.bytes < VHDX_MIB
		|| log.bytes % VHDX_MIB != 0 || log.offset > file_bytes || log.bytes > file_bytes - log.offset)
		return EBADMSG;
	layout->regions[layout->region_count++] = (VHDX_EXTENT){0, VHDX_MIB};
	layout->regions[layout->region_count++] = log;
	return ReplayVhdxLog(source, header);
}

static int VhdxRegions(VIRTUAL_DISK_NODE* source, VHDX_LAYOUT* layout, uint8_t* table)
{
	int error = ReadVhdxBytes(source, 3 * VHDX_TABLE_BYTES, table, VHDX_TABLE_BYTES);
	if (error != 0)
		return error;
	const bool primary_valid = memcmp(table, "regi", 4) == 0 && VhdxChecksum(table, VHDX_TABLE_BYTES);
	uint8_t* backup = malloc(VHDX_TABLE_BYTES);
	if (backup == NULL)
		return ENOMEM;
	error = ReadVhdxBytes(source, 4 * VHDX_TABLE_BYTES, backup, VHDX_TABLE_BYTES);
	if (error == 0) {
		const bool backup_valid = memcmp(backup, "regi", 4) == 0 && VhdxChecksum(backup, VHDX_TABLE_BYTES);
		if ((!primary_valid && !backup_valid)
			|| (primary_valid && backup_valid && memcmp(table, backup, VHDX_TABLE_BYTES) != 0))
			error = EBADMSG;
		else if (!primary_valid)
			memcpy(table, backup, VHDX_TABLE_BYTES);
	}
	free(backup);
	if (error != 0)
		return error;
	const uint32_t count = Le32(table + 8);
	if (count > VHDX_MAX_REGIONS)
		return EBADMSG;
	const uint64_t file_bytes = (uint64_t)source->file_bytes;
	for (uint32_t i = 0; i < count; i++) {
		const uint8_t* entry = table + 16 + 32 * i;
		VHDX_EXTENT region = {Le64(entry + 16), Le32(entry + 24)};
		if (region.offset < VHDX_MIB || region.offset % VHDX_MIB != 0
			|| region.bytes == 0 || region.bytes % VHDX_MIB != 0
			|| region.offset > file_bytes || region.bytes > file_bytes - region.offset)
			return EBADMSG;
		for (uint32_t j = 0; j < i; j++)
			if (memcmp(entry, table + 16 + 32 * j, 16) == 0)
				return EBADMSG;
		const bool bat = memcmp(entry, bat_guid, 16) == 0;
		const bool metadata = memcmp(entry, metadata_guid, 16) == 0;
		if (bat)
			layout->bat = region;
		else if (metadata)
			layout->metadata = region;
		else if ((Le32(entry + 28) & 1) != 0)
			return ENOTSUP;
		layout->regions[layout->region_count++] = region;
	}
	if (layout->bat.bytes == 0 || layout->metadata.bytes == 0)
		return EBADMSG;
	return CheckVhdxExtents(layout->regions, layout->region_count);
}

static int VhdxMetadata(VIRTUAL_DISK_NODE* source, uint64_t maximum, VHDX_LAYOUT* layout, uint8_t* table)
{
	int error = ReadVhdxBytes(source, layout->metadata.offset, table, VHDX_TABLE_BYTES);
	if (error != 0)
		return error;
	if (memcmp(table, "metadata", 8) != 0)
		return EBADMSG;
	const unsigned count = (unsigned)table[10] | (unsigned)table[11] << 8;
	if (count > 2047)
		return EBADMSG;
	unsigned found = 0;
	for (unsigned i = 0; i < count; i++) {
		const uint8_t* entry = table + 32 + 32 * i;
		const uint32_t offset = Le32(entry + 16), bytes = Le32(entry + 20), flags = Le32(entry + 24);
		if (bytes > VHDX_MIB || (bytes == 0 ? offset != 0
			: offset < VHDX_TABLE_BYTES || offset > layout->metadata.bytes || bytes > layout->metadata.bytes - offset))
			return EBADMSG;
		for (unsigned j = 0; j < i; j++) {
			const uint8_t* previous = table + 32 + 32 * j;
			const uint64_t start = Le32(previous + 16), span = Le32(previous + 20);
			const bool overlap = bytes != 0 && span != 0 && offset < start + span && start < (uint64_t)offset + bytes;
			if (memcmp(entry, previous, 16) == 0 || overlap)
				return EBADMSG;
		}
		unsigned kind = 0;
		while (kind < 6 && memcmp(entry, item_guid[kind], 16) != 0)
			kind++;
		if (kind == 6) {
			if ((flags & 4) != 0)
				return ENOTSUP;
			continue;
		}
		if (kind == 5) {
			if (flags != 4 || bytes < 20 || bytes > 65536) return EBADMSG;
			layout->parent_offset = layout->metadata.offset + offset; layout->parent_bytes = bytes;
			found |= 32; continue;
		}
		const unsigned expected_bytes[] = {8, 8, 16, 4, 4};
		const unsigned expected_flags = kind == 0 ? 4 : 6;
		if (bytes != expected_bytes[kind] || flags != expected_flags)
			return EBADMSG;
		uint8_t value[16];
		error = ReadVhdxBytes(source, layout->metadata.offset + offset, value, bytes);
		if (error != 0)
			return error;
		if (kind == 0) {
			layout->block_bytes = Le32(value);
			layout->has_parent = (Le32(value + 4) & 2u) != 0;
			if ((Le32(value + 4) & ~3u) != 0)
				return ENOTSUP;
		} else if (kind == 1)
			layout->disk_bytes = Le64(value);
		else if (kind == 3 || kind == 4) {
			const uint32_t sector = Le32(value);
			if (sector != 512 && sector != 4096)
				return EBADMSG;
			if (kind == 3)
				layout->logical_sector = sector;
		}
		found |= 1u << kind;
	}
	if ((found & 31) != 31 || layout->has_parent != ((found & 32) != 0) || layout->block_bytes < VHDX_MIB || layout->block_bytes > 256u * VHDX_MIB
		|| (layout->block_bytes & (layout->block_bytes - 1)) != 0
		|| layout->disk_bytes == 0 || layout->disk_bytes > (UINT64_C(64) << 40)
		|| layout->disk_bytes % layout->logical_sector != 0)
		return EBADMSG;
	if (layout->disk_bytes > maximum)
		return EFBIG;
	layout->payload_blocks = 1 + (layout->disk_bytes - 1) / layout->block_bytes;
	layout->chunk_ratio = (UINT64_C(1) << 23) * layout->logical_sector / layout->block_bytes;
	layout->bat_entries = layout->has_parent
		? (1 + (layout->payload_blocks - 1) / layout->chunk_ratio) * (layout->chunk_ratio + 1)
		: layout->payload_blocks + (layout->payload_blocks - 1) / layout->chunk_ratio;
	if (layout->bat_entries > layout->bat.bytes / 8)
		return EBADMSG;
	return 0;
}

static int VhdxBatEntry(VHDX_BAT_READER* reader, uint64_t index, uint64_t* value)
{
	if (index < reader->first || index - reader->first >= reader->count) {
		reader->first = index;
		const uint64_t remaining = reader->layout->bat_entries - index;
		reader->count = remaining < VHDX_TABLE_BYTES / 8 ? (size_t)remaining : VHDX_TABLE_BYTES / 8;
		const int error = ReadVhdxBytes(reader->source, reader->layout->bat.offset + index * 8,
			reader->bytes, reader->count * 8);
		if (error != 0)
			return error;
	}
	*value = Le64(reader->bytes + (index - reader->first) * 8);
	return 0;
}

static int ValidateVhdxBat(VHDX_BAT_READER* reader)
{
	const VHDX_LAYOUT* layout = reader->layout;
	const uint64_t capacity = layout->bat_entries + layout->region_count;
	if (capacity > VHDX_EXTENT_MEMORY / sizeof(VHDX_EXTENT))
		return ENOTSUP;
	VHDX_EXTENT* extents = malloc((size_t)capacity * sizeof(*extents));
	if (extents == NULL)
		return ENOMEM;
	memcpy(extents, layout->regions, layout->region_count * sizeof(*extents));
	size_t count = layout->region_count;
	const uint64_t file_bytes = (uint64_t)reader->source->file_bytes;
	int error = 0;
	for (uint64_t i = 0; i < layout->bat_entries; i++) {
		uint64_t value;
		error = VhdxBatEntry(reader, i, &value);
		if (error != 0)
			break;
		const unsigned state = value & 7;
		const bool bitmap = (i + 1) % (layout->chunk_ratio + 1) == 0;
		if ((value & 0xffff8) != 0 || (bitmap ? state != 0 && state != 6 : state > 3 && state != 6 && !(state == 7 && layout->has_parent))) {
			error = EBADMSG;
			break;
		}
		if (!bitmap && state == 7) {
			const uint64_t bitmap_index = (i / (layout->chunk_ratio + 1)) * (layout->chunk_ratio + 1) + layout->chunk_ratio;
			uint64_t bitmap_value;
			error = VhdxBatEntry(reader, bitmap_index, &bitmap_value);
			if (error != 0) break;
			if ((bitmap_value & 7) != 6) { error = EBADMSG; break; }
		}
		if (state != 6 && state != 7) continue;
		VHDX_EXTENT extent = {value & ~UINT64_C(0xfffff), bitmap ? VHDX_MIB : layout->block_bytes};
		if (extent.offset == 0 || extent.offset > file_bytes || extent.bytes > file_bytes - extent.offset) {
			error = EBADMSG;
			break;
		}
		extents[count++] = extent;
	}
	if (error == 0)
		error = CheckVhdxExtents(extents, count);
	free(extents);
	return error;
}


typedef struct { VHDX_LAYOUT layout; VHDX_BAT_READER bat; } VHDX_NODE;

static int ReadVhdxNode(VIRTUAL_DISK_NODE* node, uint64_t offset, void* data, size_t bytes)
{
	VHDX_NODE* state = node->layout;
	const VHDX_LAYOUT* layout = &state->layout;
	if (offset > node->disk_bytes || bytes > node->disk_bytes - offset) return EBADMSG;
	uint8_t* output = data;
	while (bytes != 0) {
		if (IsIsoScanCancelled()) return ECANCELED;
		const uint64_t block = offset / layout->block_bytes, within = offset % layout->block_bytes;
		uint64_t value;
		int error = VhdxBatEntry(&state->bat, block + block / layout->chunk_ratio, &value);
		if (error != 0) return error;
		const unsigned block_state = value & 7;
		size_t count = (size_t)(layout->block_bytes - within);
		if (count > bytes) count = bytes;
		if (count > VHDX_TABLE_BYTES) count = VHDX_TABLE_BYTES;
		if (block_state == 6) error = ReadVhdxBytes(node, (value & ~UINT64_C(0xfffff)) + within, output, count);
		else if (block_state == 0 && node->parent) error = node->parent->read(node->parent, offset, output, count);
		else if (block_state == 7) {
			uint64_t bitmap_value;
			const uint64_t bitmap_index = (block / layout->chunk_ratio) * (layout->chunk_ratio + 1) + layout->chunk_ratio;
			error = VhdxBatEntry(&state->bat, bitmap_index, &bitmap_value);
			if (error != 0) return error;
			const uint64_t first_bit = (offset / layout->logical_sector) % (UINT64_C(1) << 23);
			const size_t sectors = (size_t)((offset % layout->logical_sector + count + layout->logical_sector - 1) / layout->logical_sector);
			uint8_t bitmap[18];
			const size_t bitmap_bytes = (size_t)((first_bit % 8 + sectors + 7) / 8);
			if (bitmap_bytes > sizeof(bitmap) || !node->parent) return EBADMSG;
			error = ReadVhdxBytes(node, (bitmap_value & ~UINT64_C(0xfffff)) + first_bit / 8, bitmap, bitmap_bytes);
			for (size_t done = 0; error == 0 && done < count;) {
				const size_t bit = (size_t)(first_bit % 8) + (size_t)((offset % layout->logical_sector + done) / layout->logical_sector);
				const bool owned = (bitmap[bit / 8] & (1u << (bit % 8))) != 0;
				size_t run = layout->logical_sector - (size_t)((offset + done) % layout->logical_sector);
				while (done + run < count) {
					const size_t next = (size_t)(first_bit % 8) + (size_t)((offset % layout->logical_sector + done + run) / layout->logical_sector);
					if (((bitmap[next / 8] & (1u << (next % 8))) != 0) != owned) break;
					run += layout->logical_sector;
				}
				if (run > count - done) run = count - done;
				error = owned ? ReadVhdxBytes(node, (value & ~UINT64_C(0xfffff)) + within + done, output + done, run)
					: node->parent->read(node->parent, offset + done, output + done, run);
				done += run;
			}
		} else memset(output, 0, count); /* deterministic permitted values in states 1..3 */
		if (error != 0) return error;
		output += count; offset += count; bytes -= count;
	}
	return 0;
}

static int VhdxParent(VIRTUAL_DISK_NODE* node, VHDX_LAYOUT* layout)
{
	static const uint8_t locator_type[16] = {0xb7,0xef,0x4a,0xb0,0x9e,0xd1,0x81,0x4a,0xb7,0x89,0x25,0xb8,0xe9,0x44,0x59,0x13};
	uint8_t* data = malloc(layout->parent_bytes);
	if (!data) return ENOMEM;
	int error = ReadVhdxBytes(node, layout->parent_offset, data, layout->parent_bytes);
	unsigned count = 0;
	if (error == 0) {
		count = data[18] | (unsigned)data[19] << 8;
		if (memcmp(data, locator_type, 16) != 0) error = ENOTSUP;
		else if (data[16] != 0 || data[17] != 0 || count == 0 || count > 64 || 20 + count * 12 > layout->parent_bytes) error = EBADMSG;
	}
	char linkage[4096] = {0}, linkage2[4096] = {0};
	char paths[3][4096] = {{0}};
	char keys[64][64];
	for (unsigned i = 0; error == 0 && i < count; i++) {
		const uint8_t* entry = data + 20 + i * 12;
		const uint32_t offsets[2] = {Le32(entry), Le32(entry + 4)};
		const unsigned sizes[2] = {entry[8] | (unsigned)entry[9] << 8, entry[10] | (unsigned)entry[11] << 8};
		char key[4096], value[4096];
		for (unsigned j = 0; error == 0 && j < 2; j++) {
			if (offsets[j] < 20 + count * 12 || offsets[j] > layout->parent_bytes || sizes[j] > layout->parent_bytes - offsets[j]) error = EBADMSG;
			else error = VirtualDiskUtf16(data + offsets[j], sizes[j], true, false, j == 0 ? key : value);
		}
		if (error != 0) break;
		if (strlen(key) >= sizeof(keys[0])) { error = ENOTSUP; break; }
		for (unsigned j = 0; j < i; j++) if (strcmp(keys[j], key) == 0) error = EBADMSG;
		strcpy(keys[i], key);
		if (strcmp(key, "parent_linkage") == 0) strcpy(linkage, value);
		else if (strcmp(key, "relative_path") == 0) strcpy(paths[0], value);
		else if (strcmp(key, "volume_path") == 0) strcpy(paths[1], value);
		else if (strcmp(key, "absolute_win32_path") == 0) strcpy(paths[2], value);
		else if (strcmp(key, "parent_linkage2") == 0) strcpy(linkage2, value);
	}
	free(data);
	uint8_t id[16], alternate[16];
	if (error == 0) error = ParseVirtualDiskGuid(linkage, id);
	if (error == 0 && *linkage2) error = ParseVirtualDiskGuid(linkage2, alternate);
	if (error == 0) {
		error = ENOTSUP;
		for (unsigned i = 0; i < 3; i++) {
			if (!*paths[i]) continue;
			error = OpenVirtualDiskParent(node, paths[i], id, 0, false);
			if (error == ESTALE && *linkage2) error = OpenVirtualDiskParent(node, paths[i], alternate, 0, false);
			if (error != ENOENT && error != ENOTDIR && error != ENOTSUP) break;
		}
		if (error == ENOENT || error == ENOTDIR || error == ENOTSUP) {
			int mapped = OpenVirtualDiskParent(node, "@parent:", id, 0, false);
			if (mapped == ESTALE && *linkage2) mapped = OpenVirtualDiskParent(node, "@parent:", alternate, 0, false);
			if (mapped != ENOTSUP) error = mapped;
		}
	}
	return error;
}

static void ReleaseVhdxNode(VIRTUAL_DISK_NODE* node) { free(node->layout); }

int InitializeVhdxNode(VIRTUAL_DISK_NODE* node, uint64_t maximum)
{
	VHDX_NODE* state = calloc(1, sizeof(*state));
	uint8_t* buffer = malloc(VHDX_TABLE_BYTES);
	if (!state || !buffer) { free(state); free(buffer); return ENOMEM; }
	node->layout = state; node->release = ReleaseVhdxNode; node->read = ReadVhdxNode;
	int error = VhdxHeader(node, &state->layout);
	if (error == 0) error = VhdxRegions(node, &state->layout, buffer);
	if (error == 0) error = VhdxMetadata(node, maximum, &state->layout, buffer);
	node->disk_bytes = state->layout.disk_bytes; node->logical_sector = state->layout.logical_sector;
	state->bat.source = node; state->bat.layout = &state->layout;
	if (error == 0) error = ValidateVhdxBat(&state->bat);
	if (error == 0 && state->layout.has_parent) error = VhdxParent(node, &state->layout);
	free(buffer);
	return error;
}

int ReadVhdxImageSource(const IMAGE_FILE_SOURCE* source, uint64_t maximum_output,
	compressed_data_t consumer, void* context, COMPRESSED_IMAGE_RESULT* completed)
{
	return StreamVirtualDiskSource(source, IMG_COMPRESSION_VHDX, maximum_output, consumer, context, completed);
}
