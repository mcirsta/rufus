/* SPDX-License-Identifier: GPL-3.0-or-later */
/* MS-VHDX 2.3: validate the active circular log and replay into a private read
 * overlay. Never repair, truncate, extend or update a caller's source file. */
#include "virtual_disk.h"
#include "rufus_reporting.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

enum { LOG_SECTOR = 4096, LOG_MIB = 1048576, LOG_MAX_BYTES = 64 * LOG_MIB,
	LOG_CHAIN_MEMORY = 128 * LOG_MIB, LOG_MAX_PATCHES = 16384 };
static const uint64_t maximum_file_bytes = UINT64_C(256) << 40;
typedef struct { uint32_t bytes, tail, descriptors; uint64_t sequence, flushed, last; bool valid; } LOG_ENTRY;
typedef struct { uint64_t offset, bytes; uint32_t sector; uint8_t leading[8], trailing[4]; bool zero; } LOG_PATCH;
typedef struct { uint64_t offset, bytes, skip; uint32_t patch; } LOG_RANGE;
typedef struct {
	uint8_t* log;
	uint32_t bytes;
	uint64_t offset;
	LOG_PATCH* patches;
	LOG_RANGE* ranges;
	size_t count, range_count, range_capacity;
} LOG_REPLAY;

static uint32_t U32(const uint8_t* p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t U64(const uint8_t* p) { return U32(p) | (uint64_t)U32(p + 4) << 32; }

static void LogBytes(const LOG_REPLAY* replay, uint64_t offset, void* output, size_t bytes)
{
	uint8_t* result = output;
	while (bytes) {
		const uint32_t position = (uint32_t)(offset % replay->bytes);
		size_t count = replay->bytes - position;
		if (count > bytes) count = bytes;
		memcpy(result, replay->log + position, count);
		offset += count; result += count; bytes -= count;
	}
}

static int ValidateLogEntry(const LOG_REPLAY* replay, uint32_t position, const uint8_t guid[16],
	const uint32_t crc_table[256], uint64_t* budget, LOG_ENTRY* entry)
{
	uint8_t header[64]; LogBytes(replay, position, header, sizeof(header));
	if (memcmp(header, "loge", 4) != 0 || memcmp(header + 32, guid, 16) != 0) return 0;
	entry->bytes = U32(header + 8); entry->tail = U32(header + 12); entry->sequence = U64(header + 16);
	entry->descriptors = U32(header + 24); entry->flushed = U64(header + 48); entry->last = U64(header + 56);
	const bool aligned = entry->bytes >= LOG_SECTOR && entry->bytes <= replay->bytes && entry->bytes % LOG_SECTOR == 0
		&& entry->tail < replay->bytes && entry->tail % LOG_SECTOR == 0;
	if (!aligned || entry->sequence == 0 || U32(header + 28) != 0 || entry->flushed % LOG_MIB || entry->last % LOG_MIB
		|| entry->flushed > maximum_file_bytes || entry->last > maximum_file_bytes) return 0;
	const uint64_t descriptors = ((64 + (uint64_t)entry->descriptors * 32 + LOG_SECTOR - 1) / LOG_SECTOR) * LOG_SECTOR;
	if (descriptors > entry->bytes) return 0;
	if (*budget < entry->bytes) return E2BIG;
	*budget -= entry->bytes;
	uint32_t crc = UINT32_MAX;
	for (uint32_t i = 0; i < entry->bytes; i++) {
		if (i % LOG_SECTOR == 0 && IsIsoScanCancelled()) return ECANCELED;
		const uint8_t value = i >= 4 && i < 8 ? 0 : replay->log[((uint64_t)position + i) % replay->bytes];
		crc = crc_table[(crc ^ value) & 255] ^ (crc >> 8);
	}
	if (~crc != U32(header + 4)) return 0;
	uint64_t data_position = descriptors;
	for (uint32_t i = 0; i < entry->descriptors; i++) {
		uint8_t descriptor[32], sector[8], low[4];
		LogBytes(replay, (uint64_t)position + 64 + (uint64_t)i * 32, descriptor, sizeof(descriptor));
		const uint64_t target = U64(descriptor + 16);
		const bool data = memcmp(descriptor, "desc", 4) == 0, zero = memcmp(descriptor, "zero", 4) == 0;
		const uint64_t bytes = data ? LOG_SECTOR : U64(descriptor + 8);
		if ((!data && !zero) || target % LOG_SECTOR || bytes % LOG_SECTOR || U64(descriptor + 24) != entry->sequence
			|| target > maximum_file_bytes || bytes > maximum_file_bytes - target) return 0;
		/* Headers are not log-replay targets; region tables may be. */
		if (bytes != 0 && (target < 3 * 65536 || (target < replay->offset + replay->bytes && replay->offset < target + bytes))) return 0;
		if (zero && U32(descriptor + 4) != 0) return 0;
		if (data) {
			if (data_position > entry->bytes || LOG_SECTOR > entry->bytes - data_position) return 0;
			LogBytes(replay, (uint64_t)position + data_position, sector, sizeof(sector));
			LogBytes(replay, (uint64_t)position + data_position + LOG_SECTOR - 4, low, sizeof(low));
			if (memcmp(sector, "data", 4) != 0 || ((uint64_t)U32(sector + 4) << 32 | U32(low)) != entry->sequence) return 0;
			data_position += LOG_SECTOR;
		}
	}
	entry->valid = data_position == entry->bytes;
	return 0;
}

static int ApplyLogRange(LOG_REPLAY* replay, uint32_t patch)
{
	const LOG_PATCH* value = &replay->patches[patch];
	if (value->bytes == 0) return 0;
	const uint64_t end = value->offset + value->bytes;
	size_t first = 0, last = replay->range_count;
	while (first < last) {
		const size_t middle = first + (last - first) / 2;
		if (replay->ranges[middle].offset + replay->ranges[middle].bytes <= value->offset) first = middle + 1;
		else last = middle;
	}
	last = first;
	while (last < replay->range_count && replay->ranges[last].offset < end) last++;
	LOG_RANGE insert[3]; size_t count = 0;
	if (first < last && replay->ranges[first].offset < value->offset) {
		insert[count] = replay->ranges[first]; insert[count].bytes = value->offset - insert[count].offset; count++;
	}
	insert[count++] = (LOG_RANGE){value->offset, value->bytes, 0, patch};
	if (first < last && replay->ranges[last - 1].offset + replay->ranges[last - 1].bytes > end) {
		LOG_RANGE right = replay->ranges[last - 1];
		right.skip += end - right.offset; right.bytes -= end - right.offset; right.offset = end;
		insert[count++] = right;
	}
	const size_t needed = replay->range_count - (last - first) + count;
	if (needed > LOG_MAX_PATCHES * 2u + 1) return E2BIG;
	if (needed > replay->range_capacity) {
		size_t capacity = replay->range_capacity ? replay->range_capacity * 2 : 16;
		if (capacity < needed) capacity = needed;
		LOG_RANGE* resized = realloc(replay->ranges, capacity * sizeof(*resized));
		if (!resized) return ENOMEM;
		replay->ranges = resized; replay->range_capacity = capacity;
	}
	memmove(replay->ranges + first + count, replay->ranges + last, (replay->range_count - last) * sizeof(LOG_RANGE));
	memcpy(replay->ranges + first, insert, count * sizeof(LOG_RANGE)); replay->range_count = needed;
	return 0;
}

int ReplayVhdxLog(VIRTUAL_DISK_NODE* node, const uint8_t header[4096])
{
	uint8_t empty[16] = {0};
	if (memcmp(header + 48, empty, 16) == 0) return 0;
	if (header[64] != 0 || header[65] != 0) return ENOTSUP;
	const uint32_t bytes = U32(header + 68);
	if (bytes > LOG_MAX_BYTES || node->root->log_memory > LOG_CHAIN_MEMORY - bytes) return ENOTSUP;
	LOG_REPLAY* replay = calloc(1, sizeof(*replay));
	if (!replay) return ENOMEM;
	node->replay = replay; replay->bytes = bytes; replay->offset = U64(header + 72);
	replay->log = malloc(bytes);
	if (!replay->log) return ENOMEM;
	node->root->log_memory += bytes;
	int error = ReadVirtualDiskRawFile(node, replay->offset, replay->log, bytes);
	const uint32_t slots = bytes / LOG_SECTOR;
	LOG_ENTRY* entries = calloc(slots, sizeof(*entries));
	uint32_t* seen = calloc(slots, sizeof(*seen));
	if (!entries || !seen) error = ENOMEM;
	uint32_t table[256];
	for (unsigned i = 0; i < 256; i++) {
		uint32_t crc = i;
		for (unsigned j = 0; j < 8; j++) crc = (crc >> 1) ^ (0x82f63b78u & (0u - (crc & 1)));
		table[i] = crc;
	}
	uint64_t budget = (uint64_t)bytes * 8;
	for (uint32_t i = 0; error == 0 && i < slots; i++)
		error = ValidateLogEntry(replay, i * LOG_SECTOR, header + 48, table, &budget, entries + i);
	uint32_t tail = 0, best_start = 0, best_head = 0, best_count = 0, generation = 0;
	uint64_t best_sequence = 0;
	while (error == 0) {
		if (IsIsoScanCancelled()) { error = ECANCELED; break; }
		const uint32_t old_tail = tail;
		uint32_t current = tail, head = tail, count = 0;
		uint64_t sequence = 0, span = 0;
		generation++;
		while (entries[current / LOG_SECTOR].valid) {
			LOG_ENTRY* entry = entries + current / LOG_SECTOR;
			if ((count != 0 && (sequence == UINT64_MAX || entry->sequence != sequence + 1)) || entry->bytes > bytes - span) break;
			seen[current / LOG_SECTOR] = generation; count++; sequence = entry->sequence; head = current; span += entry->bytes;
			current = (uint32_t)(((uint64_t)current + entry->bytes) % bytes);
			if (span == bytes) break;
		}
		const bool valid = count != 0 && seen[entries[head / LOG_SECTOR].tail / LOG_SECTOR] == generation;
		if (valid && sequence > best_sequence) {
			best_sequence = sequence; best_start = tail; best_head = head; best_count = count;
		} else if (valid && sequence == best_sequence && head != best_head) error = EBADMSG;
		tail = !valid ? (tail + LOG_SECTOR) % bytes : current;
		if (tail <= old_tail) break;
	}
	if (error == 0 && best_count == 0) error = EBADMSG;
	if (error == 0 && (uint64_t)node->source.info.st_size < entries[best_head / LOG_SECTOR].flushed) error = EBADMSG;
	if (error == 0) {
		replay->patches = calloc(LOG_MAX_PATCHES, sizeof(LOG_PATCH));
		if (!replay->patches) error = ENOMEM;
	}
	uint32_t position = best_start;
	for (uint32_t i = 0; error == 0 && i < best_count; i++) {
		const LOG_ENTRY* entry = entries + position / LOG_SECTOR;
		uint64_t data_position = ((64 + (uint64_t)entry->descriptors * 32 + LOG_SECTOR - 1) / LOG_SECTOR) * LOG_SECTOR;
		for (uint32_t j = 0; error == 0 && j < entry->descriptors; j++) {
			if (replay->count == LOG_MAX_PATCHES) { error = E2BIG; break; }
			uint8_t descriptor[32]; LogBytes(replay, (uint64_t)position + 64 + (uint64_t)j * 32, descriptor, sizeof(descriptor));
			LOG_PATCH* patch = replay->patches + replay->count;
			patch->offset = U64(descriptor + 16); patch->zero = memcmp(descriptor, "zero", 4) == 0;
			patch->bytes = patch->zero ? U64(descriptor + 8) : LOG_SECTOR;
			if (!patch->zero) {
				patch->sector = (uint32_t)(((uint64_t)position + data_position) % bytes); data_position += LOG_SECTOR;
				memcpy(patch->leading, descriptor + 8, 8); memcpy(patch->trailing, descriptor + 4, 4);
			}
			error = ApplyLogRange(replay, (uint32_t)replay->count++);
			if (patch->bytes != 0 && patch->offset + patch->bytes > node->file_bytes) node->file_bytes = patch->offset + patch->bytes;
		}
		position = (uint32_t)(((uint64_t)position + entry->bytes) % bytes);
	}
	if (error == 0 && entries[best_head / LOG_SECTOR].last > node->file_bytes) node->file_bytes = entries[best_head / LOG_SECTOR].last;
	free(entries); free(seen);
	return error;
}

int ReadVhdxOverlay(VIRTUAL_DISK_NODE* node, uint64_t offset, void* data, size_t bytes)
{
	const LOG_REPLAY* replay = node->replay;
	uint8_t* output = data;
	while (bytes) {
		if (IsIsoScanCancelled()) return ECANCELED;
		size_t first = 0, last = replay->range_count;
		while (first < last) {
			const size_t middle = first + (last - first) / 2;
			if (replay->ranges[middle].offset + replay->ranges[middle].bytes <= offset) first = middle + 1;
			else last = middle;
		}
		size_t count = bytes;
		if (first < replay->range_count && replay->ranges[first].offset <= offset) {
			const LOG_RANGE* range = replay->ranges + first;
			const LOG_PATCH* patch = replay->patches + range->patch;
			if (range->offset + range->bytes - offset < count) count = (size_t)(range->offset + range->bytes - offset);
			if (patch->zero) memset(output, 0, count);
			else {
				uint8_t sector[LOG_SECTOR];
				LogBytes(replay, patch->sector, sector, sizeof(sector));
				memcpy(sector, patch->leading, 8); memcpy(sector + LOG_SECTOR - 4, patch->trailing, 4);
				const uint64_t within = range->skip + offset - range->offset;
				if (within > sizeof(sector) || count > sizeof(sector) - within) return EBADMSG;
				memcpy(output, sector + within, count);
			}
		} else {
			if (first < replay->range_count && replay->ranges[first].offset - offset < count) count = (size_t)(replay->ranges[first].offset - offset);
			const uint64_t stored = (uint64_t)node->source.info.st_size;
			if (offset >= stored) memset(output, 0, count);
			else {
				if (stored - offset < count) count = (size_t)(stored - offset);
				const int error = ReadVirtualDiskRawFile(node, offset, output, count);
				if (error != 0) return error;
			}
		}
		output += count; bytes -= count; offset += count;
	}
	return 0;
}

void CloseVhdxOverlay(VIRTUAL_DISK_NODE* node)
{
	LOG_REPLAY* replay = node->replay;
	if (!replay) return;
	free(replay->log); free(replay->patches); free(replay->ranges); free(replay); node->replay = NULL;
}
