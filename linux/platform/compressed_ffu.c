/* SPDX-License-Identifier: GPL-3.0-or-later */
/* FFU reconstruction and target-aware full deployment. Layout follows Microsoft's Windows Manufacturing
 * Documentation (2019), FFU image format; plain XPRESS follows MS-XCA 2.4.
 * Resolve end-relative blocks at the manifest's captured disk size. Partition
 * resizing/portable FFU deployment is not implicit. V2 stores require an explicit
 * selection, and every store's payload is authenticated before deployment. */
#include "compressed_ffu.h"
#include "ffu_codec.h"
#include "ffu_catalog.h"
#include "ffu_manifest.h"
#include "hash_native.h"
#include "rufus_reporting.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <zlib.h>

enum { FFU_MAX_BLOCK = 16 * 1024 * 1024,
	FFU_TABLE_BUDGET = 64 * 1024 * 1024, FFU_TRANSFER = 65536,
	FFU_MAX_GROUP = 16 * 1024 * 1024 };

typedef struct { uint64_t offset; uint32_t size, decoded_bytes, skip; } FFU_PAYLOAD;
typedef struct { uint8_t *packed, *decoded; uint64_t offset; } FFU_BUFFER;
typedef struct { uint64_t position; uint32_t payload, order, method, bytes, skip, run_start; } FFU_MAP;
typedef struct {
	uint64_t image_offset, store_offset, data_offset, disk_bytes;
	uint32_t block_bytes, sector_bytes, algorithm, descriptors, table_bytes, hash_bytes, buffer_bytes;
	uint32_t validation_count, validation_bytes;
	uint8_t* validation;
	bool partial;
	uint64_t hash_offset;
	uint8_t* hashes;
	FFU_PAYLOAD* payloads;
	FFU_MAP* map;
	size_t payload_count, map_count;
	unsigned store_count, store_index;
	uint8_t* device_path;
	size_t path_bytes;
	uint64_t descriptor_offset, v2_payload_bytes;
	bool compressed;
	bool has_catalog;
	uint8_t catalog_sha256[32];
	FFU_OPTIMIZATION optimization;
} FFU_LAYOUT;

typedef struct {
	FFU_LAYOUT* stores;
	unsigned count;
	uint64_t data_offset;
} FFU_CONTAINER;

static uint32_t Le32(const uint8_t* p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint16_t Le16(const uint8_t* p)
{
	return (uint16_t)p[0] | (uint16_t)p[1] << 8;
}

static uint64_t Le64(const uint8_t* p)
{
	return Le32(p) | (uint64_t)Le32(p + 4) << 32;
}

static uint64_t AlignFfu(uint64_t value, uint32_t alignment)
{
	return value + (alignment - value % alignment) % alignment;
}

int PlanFfuCapture(uint64_t disk_bytes, uint64_t catalog_bytes, uint64_t manifest_bytes,
	FFU_CAPTURE_GEOMETRY* geometry)
{
	if (geometry == NULL) return EINVAL;
	memset(geometry, 0, sizeof(*geometry));
	if (disk_bytes < 1024 || disk_bytes > INT64_MAX || catalog_bytes > UINT32_MAX) return ENOTSUP;
	if (manifest_bytes > FFU_MAX_MANIFEST_BYTES) return ENOTSUP;
	uint32_t chunk = FFU_CAPTURE_DEFAULT_CHUNK_BYTES;
	while (chunk > disk_bytes && chunk > 1024) chunk /= 2;
	for (; chunk <= FFU_MAX_BLOCK; chunk *= 2) {
		const uint64_t blocks = (disk_bytes + chunk - 1) / chunk;
		const bool map_fits = blocks <= FFU_TABLE_BUDGET / sizeof(FFU_MAP);
		const bool payloads_fit = blocks <= FFU_TABLE_BUDGET / sizeof(FFU_PAYLOAD);
		const bool descriptors_fit = blocks <= FFU_TABLE_BUDGET / 20;
		if (!map_fits || !payloads_fit || !descriptors_fit) continue;
		const uint64_t store_bytes = AlignFfu(252 + blocks * 20, chunk);
		/* Keep chunk selection independent of the actual manifest. The caller
		 * can reserve a maximum-sized manifest before acquiring the source,
		 * and its file bound also covers a smaller manifest's geometry. */
		const uint64_t maximum_metadata = AlignFfu(24 + FFU_MAX_MANIFEST_BYTES, chunk) + store_bytes;
		const uint64_t maximum_hashes = (blocks + maximum_metadata / chunk) * 32;
		if (maximum_metadata > FFU_TABLE_BUDGET || maximum_hashes > FFU_TABLE_BUDGET) continue;
		const uint64_t image_bytes = AlignFfu(24 + manifest_bytes, chunk);
		const uint64_t metadata = image_bytes + store_bytes;
		const uint64_t hashes = (blocks + metadata / chunk) * 32;
		const uint64_t security = AlignFfu(32 + catalog_bytes + hashes, chunk);
		/* XPRESS literal flags and its terminal word bound every capture chunk. */
		const uint64_t packed_block = FfuXpressCapacity(chunk);
		const uint64_t data = blocks * packed_block;
		if (security > INT64_MAX - metadata || data > INT64_MAX - security - metadata) return EOVERFLOW;
		*geometry = (FFU_CAPTURE_GEOMETRY){chunk, (uint32_t)blocks, image_bytes, metadata, hashes, security, security + metadata + data};
		return 0;
	}
	return ENOTSUP;
}

static int ReadFfuBytes(const IMAGE_FILE_SOURCE* source, uint64_t offset, void* buffer, size_t size)
{
	const uint64_t file_bytes = (uint64_t)source->info.st_size;
	if (offset > file_bytes || size > file_bytes - offset)
		return EBADMSG;
	uint8_t* bytes = buffer;
	while (size != 0) {
		if (IsRufusJobCancelled())
			return ECANCELED;
		const size_t batch = size < FFU_TRANSFER ? size : FFU_TRANSFER;
		const ssize_t count = pread(source->fd, bytes, batch, (off_t)offset);
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			return count == 0 ? EIO : errno;
		offset += (uint64_t)count;
		bytes += count;
		size -= (size_t)count;
	}
	return 0;
}

static char* TrimFfu(char* line)
{
	while (*line == ' ' || *line == '\t' || *line == '\r')
		line++;
	size_t length = strlen(line);
	while (length != 0 && (line[length - 1] == ' ' || line[length - 1] == '\t' || line[length - 1] == '\r'))
		line[--length] = 0;
	return line;
}

static int FfuNumber(const char* value, uint64_t* result)
{
	if (*value < '0' || *value > '9')
		return EBADMSG;
	char* end;
	errno = 0;
	const unsigned long long number = strtoull(value, &end, 10);
	if (errno != 0 || *end != 0 || number == 0 || number > INT64_MAX)
		return EBADMSG;
	*result = number;
	return 0;
}

static int FfuManifest(char* manifest, FFU_CONTAINER* container)
{
	unsigned count = 0;
	for (const char* line = manifest; line != NULL; ) {
		while (*line == ' ' || *line == '\t' || *line == '\r') line++;
		if (strncasecmp(line, "[Store]", 7) == 0) {
			const char* end = line + 7;
			while (*end == ' ' || *end == '\t' || *end == '\r') end++;
			if (*end == '\n' || *end == 0) count++;
		}
		line = strchr(line, '\n');
		if (line != NULL) line++;
	}
	if (count == 0 || count > UINT16_MAX) return ENOTSUP;
	container->stores = calloc(count, sizeof(*container->stores));
	if (container->stores == NULL) return ENOMEM;
	container->count = count;
	FFU_OPTIMIZATION* optimized = calloc(count, sizeof(*optimized));
	if (optimized == NULL) return ENOMEM;
	const int manifest_error = ParseFfuOptimizedManifest(manifest, count, optimized);
	for (unsigned i = 0; i < count; i++) container->stores[i].optimization = optimized[i];
	free(optimized);
	if (manifest_error != 0) return manifest_error;
	FFU_LAYOUT* store = NULL;
	unsigned index = 0;
	char* state;
	for (char* line = strtok_r(manifest, "\n", &state); line != NULL; line = strtok_r(NULL, "\n", &state)) {
		line = TrimFfu(line);
		if (*line == '[') {
			store = strcasecmp(line, "[Store]") == 0 ? &container->stores[index++] : NULL;
			continue;
		}
		if (store == NULL || *line == ';' || *line == '#') continue;
		char* separator = strchr(line, '=');
		if (separator == NULL) continue;
		*separator = 0;
		const char* key = TrimFfu(line);
		uint64_t number;
		const bool sectors = strcasecmp(key, "MinSectorCount") == 0;
		const bool sector_size = strcasecmp(key, "SectorSize") == 0;
		if (!sectors && !sector_size) continue;
		if ((sectors && store->disk_bytes != 0) || (sector_size && store->sector_bytes != 0)) return EBADMSG;
		const int error = FfuNumber(TrimFfu(separator + 1), &number);
		if (error != 0) return error;
		if (sectors) store->disk_bytes = number;
		else {
			if (number != 512 && number != 4096) return EBADMSG;
			store->sector_bytes = (uint32_t)number;
		}
	}
	for (unsigned i = 0; i < count; i++) {
		store = &container->stores[i];
		if (store->sector_bytes == 0 || store->disk_bytes == 0 || store->disk_bytes > INT64_MAX / store->sector_bytes)
			return EBADMSG;
		store->disk_bytes *= store->sector_bytes;
		store->store_index = i + 1;
		store->store_count = count;
	}
	return 0;
}

static int FfuStoreHeader(const IMAGE_FILE_SOURCE* source, FFU_LAYOUT* layout, uint64_t* next);

static int FfuRoots(const IMAGE_FILE_SOURCE* source, FFU_CONTAINER* container)
{
	FFU_LAYOUT common = {0};
	FFU_LAYOUT* layout = &common;
	uint8_t security[32], image[28];
	int error = ReadFfuBytes(source, 0, security, sizeof(security));
	if (error != 0)
		return error;
	if (Le32(security) != 32 || memcmp(security + 4, "SignedImage ", 12) != 0)
		return EBADMSG;
	const uint32_t chunk_kib = Le32(security + 16);
	if (chunk_kib == 0 || chunk_kib > FFU_MAX_BLOCK / 1024 || (chunk_kib & (chunk_kib - 1)) != 0)
		return ENOTSUP;
	const uint32_t chunk_bytes = chunk_kib * 1024;
	if (Le32(security + 20) != 0x800c)
		return ENOTSUP;
	const uint32_t catalog_bytes = Le32(security + 24);
	if (catalog_bytes > FFU_MAX_CATALOG_BYTES) return ENOTSUP;
	if (catalog_bytes != 0) {
		uint8_t* catalog = malloc(catalog_bytes);
		if (catalog == NULL) return ENOMEM;
		FFU_CATALOG_RESULT result;
		error = ReadFfuBytes(source, 32, catalog, catalog_bytes);
		if (error == 0) error = VerifyFfuCatalog(catalog, catalog_bytes, &result);
		free(catalog);
		if (error != 0) return error;
		layout->has_catalog = true;
		memcpy(layout->catalog_sha256, result.sha256, 32);
	}
	layout->hash_offset = 32 + (uint64_t)catalog_bytes;
	layout->hash_bytes = Le32(security + 28);
	if (layout->hash_bytes == 0 || layout->hash_bytes % 32 != 0)
		return EBADMSG;
	if (layout->hash_bytes > FFU_TABLE_BUDGET)
		return ENOTSUP;
	layout->image_offset = AlignFfu(layout->hash_offset + layout->hash_bytes, chunk_bytes);
	error = ReadFfuBytes(source, layout->image_offset, image, sizeof(image));
	if (error != 0)
		return error;
	const uint32_t image_bytes = Le32(image), manifest_bytes = Le32(image + 16);
	if ((image_bytes != 24 && image_bytes != 28) || memcmp(image + 4, "ImageFlash  ", 12) != 0
		|| Le32(image + 20) != chunk_kib || manifest_bytes == 0)
		return EBADMSG;
	if (manifest_bytes > FFU_MAX_MANIFEST_BYTES)
		return ENOTSUP;
	char* manifest = malloc((size_t)manifest_bytes + 1);
	if (manifest == NULL)
		return ENOMEM;
	error = ReadFfuBytes(source, layout->image_offset + image_bytes, manifest, manifest_bytes);
	if (error == 0) {
		manifest[manifest_bytes] = 0;
		if (memchr(manifest, 0, manifest_bytes) != NULL)
			error = EBADMSG;
		else
			error = FfuManifest(manifest, container);
	}
	free(manifest);
	if (error != 0)
		return error;
	uint64_t position = layout->image_offset + image_bytes + manifest_bytes;
	const uint32_t target_infos = image_bytes == 28 ? Le32(image + 24) : 0;
	if (target_infos > 1024)
		return ENOTSUP;
	for (uint32_t i = 0; i < target_infos; i++) {
		uint8_t lengths[28];
		error = ReadFfuBytes(source, position, lengths, sizeof(lengths));
		if (error != 0)
			return error;
		position += sizeof(lengths);
		for (unsigned j = 0; j < 7; j++) {
			const uint32_t length = Le32(lengths + j * 4);
			if (length > FFU_MAX_MANIFEST_BYTES)
				return ENOTSUP;
			position += length;
		}
	}
	position = AlignFfu(position, chunk_bytes);
	for (unsigned i = 0; i < container->count; i++) {
		FFU_LAYOUT* store = &container->stores[i];
		store->image_offset = layout->image_offset;
		store->hash_offset = layout->hash_offset;
		store->hash_bytes = layout->hash_bytes;
		store->has_catalog = layout->has_catalog;
		memcpy(store->catalog_sha256, layout->catalog_sha256, 32);
		store->block_bytes = chunk_bytes;
		store->store_offset = position;
		error = FfuStoreHeader(source, store, &position);
		if (error != 0) return error;
		if (position - layout->image_offset > FFU_TABLE_BUDGET) return ENOTSUP;
	}
	container->data_offset = position;
	return 0;
}

static int FfuStoreHeader(const IMAGE_FILE_SOURCE* source, FFU_LAYOUT* layout, uint64_t* next)
{
	uint8_t store[262];
	bool* compressed = &layout->compressed;
	uint64_t* v2_payload_bytes = &layout->v2_payload_bytes;
	int error = ReadFfuBytes(source, layout->store_offset, store, 248);
	if (error != 0)
		return error;
	const unsigned major = Le16(store + 4), full_major = Le16(store + 8);
	*compressed = major == 1 && full_major == 3;
	const bool v2 = major == 2 && full_major == 2;
	const bool v1 = major == 1 && full_major == 2;
	if ((!v1 && !v2 && !*compressed) || Le16(store + 6) != 0 || Le16(store + 10) != 0
		|| Le32(store) > 1)
		return ENOTSUP;
	if (!v2 && layout->store_count != 1) return ENOTSUP;
	layout->partial = Le32(store) == 1;
	layout->validation_count = Le32(store + 216);
	layout->validation_bytes = Le32(store + 220);
	const bool validation_shape = layout->validation_count <= layout->validation_bytes / 12
		&& (layout->validation_count != 0 || layout->validation_bytes == 0);
	if (!validation_shape) return EBADMSG;
	if (!layout->partial && layout->validation_bytes != 0) return EBADMSG;
	if (layout->validation_bytes > FFU_TABLE_BUDGET) return ENOTSUP;
	const uint32_t block_bytes = Le32(store + 204);
	if (layout->block_bytes != block_bytes || layout->disk_bytes < block_bytes)
		return ENOTSUP;
	layout->descriptors = Le32(store + 208);
	layout->table_bytes = Le32(store + 212);
	if (layout->descriptors == 0 || layout->descriptors > layout->table_bytes / (*compressed ? 20 : 16))
		return EBADMSG;
	if (layout->table_bytes > FFU_TABLE_BUDGET)
		return ENOTSUP;
	for (unsigned i = 224; i < 248; i += 8) {
		const uint32_t index = Le32(store + i), count = Le32(store + i + 4);
		if (index > layout->descriptors || count > layout->descriptors - index)
			return EBADMSG;
	}
	uint64_t position = layout->store_offset + 248;
	if (*compressed) {
		error = ReadFfuBytes(source, position, store, 4);
		if (error != 0)
			return error;
		layout->algorithm = Le32(store);
		if (layout->algorithm > 4)
			return ENOTSUP;
		position += 4;
	} else if (v2) {
		error = ReadFfuBytes(source, position, store, 14);
		if (error != 0)
			return error;
		if (Le16(store) != layout->store_count || Le16(store + 2) != layout->store_index)
			return ENOTSUP;
		*v2_payload_bytes = Le64(store + 4);
		layout->path_bytes = (size_t)Le16(store + 12) * 2;
		position += 14;
		if (layout->path_bytes != 0) {
			layout->device_path = malloc(layout->path_bytes);
			if (layout->device_path == NULL) return ENOMEM;
			error = ReadFfuBytes(source, position, layout->device_path, layout->path_bytes);
			if (error != 0) return error;
		}
		position += layout->path_bytes;
	}
	if (layout->validation_bytes != 0) {
		layout->validation = malloc(layout->validation_bytes);
		if (layout->validation == NULL) return ENOMEM;
		error = ReadFfuBytes(source, position, layout->validation, layout->validation_bytes);
		if (error != 0) return error;
		size_t cursor = 0;
		for (uint32_t i = 0; i < layout->validation_count; i++) {
			if (IsRufusJobCancelled()) return ECANCELED;
			if (cursor > layout->validation_bytes || layout->validation_bytes - cursor < 12) return EBADMSG;
			const uint8_t* entry = layout->validation + cursor;
			const uint64_t offset = (uint64_t)Le32(entry) * layout->sector_bytes + Le32(entry + 4);
			const uint32_t bytes = Le32(entry + 8);
			cursor += 12;
			const bool fits_table = bytes <= layout->validation_bytes - cursor;
			const bool fits_disk = offset <= layout->disk_bytes && bytes <= layout->disk_bytes - offset;
			if (!fits_table || !fits_disk) return EBADMSG;
			cursor += bytes;
		}
		if (cursor != layout->validation_bytes) return EBADMSG;
	}
	position += layout->validation_bytes;
	layout->descriptor_offset = position;
	*next = AlignFfu(position + layout->table_bytes, layout->block_bytes);
	return 0;
}

static int CompareFfuMap(const void* a, const void* b)
{
	const FFU_MAP* left = a;
	const FFU_MAP* right = b;
	if (left->position != right->position)
		return left->position < right->position ? -1 : 1;
	return left->order < right->order ? -1 : left->order > right->order ? 1 : 0;
}

static int FfuDescriptors(const IMAGE_FILE_SOURCE* source, FFU_LAYOUT* layout,
	uint64_t table_offset, bool compressed, uint64_t v2_payload_bytes, uint64_t* next)
{
	uint8_t* table = malloc(layout->table_bytes);
	if (table == NULL)
		return ENOMEM;
	int error = ReadFfuBytes(source, table_offset, table, layout->table_bytes);
	size_t position = 0, payloads = 0, maps = 0;
	const size_t entry_bytes = compressed ? 12 : 8;
	/* First pass validates variable-length entries and counts bounded allocations. */
	for (uint32_t i = 0; error == 0 && i < layout->descriptors; i++) {
		if (position > layout->table_bytes || entry_bytes > layout->table_bytes - position) {
			error = EBADMSG;
			break;
		}
		const uint32_t locations = Le32(table + position), blocks = Le32(table + position + 4);
		position += entry_bytes;
		if (locations == 0 || blocks == 0 || locations > (layout->table_bytes - position) / 8) {
			error = EBADMSG;
			break;
		}
		if (compressed && blocks > FFU_MAX_GROUP / layout->block_bytes) {
			error = ENOTSUP;
			break;
		}
		const uint64_t count = (uint64_t)locations * blocks;
		if (count > FFU_TABLE_BUDGET / sizeof(FFU_MAP) || blocks > FFU_TABLE_BUDGET / sizeof(FFU_PAYLOAD)
			|| maps + count > FFU_TABLE_BUDGET / sizeof(FFU_MAP)
			|| payloads + blocks > FFU_TABLE_BUDGET / sizeof(FFU_PAYLOAD)) {
			error = ENOTSUP;
			break;
		}
		maps += (size_t)count;
		payloads += blocks;
		position += (size_t)locations * 8;
	}
	if (error == 0 && position != layout->table_bytes)
		error = EBADMSG;
	if (error == 0) {
		layout->payloads = malloc(payloads * sizeof(*layout->payloads));
		layout->map = malloc(maps * sizeof(*layout->map));
		if (layout->payloads == NULL || layout->map == NULL)
			error = ENOMEM;
	}
	position = 0;
	uint64_t data = layout->data_offset;
	const uint64_t disk_blocks = layout->disk_bytes / layout->block_bytes;
	for (uint32_t i = 0; error == 0 && i < layout->descriptors; i++) {
		const uint32_t locations = Le32(table + position), blocks = Le32(table + position + 4);
		const uint32_t stored = compressed ? Le32(table + position + 8) : layout->block_bytes;
		position += entry_bytes;
		const uint32_t decoded_bytes = compressed ? blocks * layout->block_bytes : layout->block_bytes;
		if (stored == 0 || stored > 2 * decoded_bytes
			|| (layout->algorithm == 0 && stored != decoded_bytes)) {
			error = EBADMSG;
			break;
		}
		if (decoded_bytes > layout->buffer_bytes) layout->buffer_bytes = decoded_bytes;
		const uint32_t first_payload = (uint32_t)layout->payload_count;
		for (uint32_t j = 0; j < blocks; j++) {
			layout->payloads[layout->payload_count++] = (FFU_PAYLOAD){data, stored, decoded_bytes, compressed ? j * layout->block_bytes : 0};
			if (!compressed) data += stored;
		}
		if (compressed) data += stored;
		for (uint32_t j = 0; error == 0 && j < locations; j++) {
			const uint32_t method = Le32(table + position), block = Le32(table + position + 4);
			position += 8;
			if (method != 0 && method != 2) {
				error = ENOTSUP;
				break;
			}
			if (block >= disk_blocks) { error = EBADMSG; break; }
			// End-relative indices identify the first block of the forward run.
			const uint64_t start = method == 0 ? block : disk_blocks - 1 - block;
			if (blocks > disk_blocks - start) { error = EBADMSG; break; }
			for (uint32_t k = 0; k < blocks; k++) {
				const FFU_MAP mapping = {method == 0 ? start + k : block - k, first_payload + k,
					(uint32_t)layout->map_count, method, layout->block_bytes, 0, block};
				layout->map[layout->map_count++] = mapping;
			}
		}
	}
	free(table);
	if (error != 0)
		return error;
	if (data > (uint64_t)source->info.st_size || (v2_payload_bytes != 0 && data - layout->data_offset != v2_payload_bytes))
		return EBADMSG;
	if (layout->store_count > 1 && v2_payload_bytes == 0) return EBADMSG;
	*next = data;

	return 0;
}

static int ResolveFfuMap(FFU_LAYOUT* layout, uint64_t capacity)
{
	if (!layout->optimization.enabled && capacity < layout->disk_bytes) return ENOSPC;
	if (capacity > INT64_MAX || capacity % layout->sector_bytes != 0) return EINVAL;
	for (size_t i = 0; i < layout->map_count; i++) {
		FFU_MAP* entry = &layout->map[i];
		const uint64_t block_offset = entry->position * layout->block_bytes;
		if (entry->method == 0) {
			if (block_offset > capacity || entry->bytes > capacity - block_offset) return ENOSPC;
		} else if (block_offset > capacity || layout->block_bytes > capacity - block_offset) return ENOSPC;
		entry->position = entry->method == 0 ? entry->position * layout->block_bytes
			: capacity - (entry->position + 1) * layout->block_bytes;
	}
	qsort(layout->map, layout->map_count, sizeof(*layout->map), CompareFfuMap);
	size_t count = 0;
	for (size_t i = 0; i < layout->map_count; i++) {
		FFU_MAP entry = layout->map[i];
		if (i + 1 < layout->map_count && entry.position == layout->map[i + 1].position)
			continue;
		if (count != 0) {
			FFU_MAP* previous = &layout->map[count - 1];
			const uint64_t end = previous->position + previous->bytes;
			if (entry.position < end) {
				if (previous->order > entry.order) {
					const uint32_t skip = (uint32_t)(end - entry.position);
					if (skip >= entry.bytes) continue;
					entry.position += skip; entry.skip += skip; entry.bytes -= skip;
				} else {
					previous->bytes = (uint32_t)(entry.position - previous->position);
					if (previous->bytes == 0) count--;
				}
			}
		}
		layout->map[count++] = entry;
	}
	layout->map_count = count;
	return 0;
}


static int FfuPayload(const IMAGE_FILE_SOURCE* source, const FFU_LAYOUT* layout, size_t index,
	FFU_BUFFER* buffer, const uint8_t** bytes)
{
	const FFU_PAYLOAD* payload = &layout->payloads[index];
	if (buffer->offset != payload->offset) {
		buffer->offset = UINT64_MAX;
		int error = ReadFfuBytes(source, payload->offset, buffer->packed, payload->size);
		if (error != 0) return error;
		if (layout->algorithm == 0) memcpy(buffer->decoded, buffer->packed, payload->decoded_bytes);
		else if (layout->algorithm == 2)
			error = DecodeFfuLznt1(buffer->packed, payload->size, buffer->decoded, payload->decoded_bytes);
		else if (layout->algorithm == 4)
			error = DecodeFfuHuffman(buffer->packed, payload->size, buffer->decoded, payload->decoded_bytes);
		else error = DecodeFfuXpress(buffer->packed, payload->size, buffer->decoded, payload->decoded_bytes);
		if (error != 0) return error;
		buffer->offset = payload->offset;
	}
	*bytes = buffer->decoded + payload->skip;
	return 0;
}

static int CheckFfuHash(const FFU_LAYOUT* layout, uint64_t index, const uint8_t* bytes)
{
	HASH_CONTEXT hash;
	hash_init[HASH_SHA256](&hash);
	hash_write[HASH_SHA256](&hash, bytes, layout->block_bytes);
	hash_final[HASH_SHA256](&hash);
	return memcmp(hash.buf, layout->hashes + index * 32, 32) == 0 ? 0 : EBADMSG;
}

static void FreeFfu(FFU_LAYOUT* layout, FFU_BUFFER* buffer)
{
	free(buffer->packed); free(buffer->decoded);
	free(layout->map); free(layout->payloads); free(layout->validation); free(layout->hashes); free(layout->device_path);
	FreeFfuOptimization(&layout->optimization);
	memset(layout, 0, sizeof(*layout)); memset(buffer, 0, sizeof(*buffer));
}

static void FreeFfuContainer(FFU_CONTAINER* container)
{
	for (unsigned i = 0; i < container->count; i++) {
		FFU_BUFFER buffer = {0};
		FreeFfu(&container->stores[i], &buffer);
	}
	free(container->stores);
	memset(container, 0, sizeof(*container));
}

static int SelectFfuStore(const IMAGE_FILE_SOURCE* source, const FFU_CONTAINER* container, unsigned* index)
{
	if (source->ffu_store == 0 && container->count != 1) return EDESTADDRREQ;
	const unsigned selected = source->ffu_store == 0 ? 1 : source->ffu_store;
	if (selected > container->count) return EINVAL;
	*index = selected - 1;
	return 0;
}

int FfuRequiresBase(const IMAGE_FILE_SOURCE* source, bool* required)
{
	if (source == NULL || required == NULL) return EINVAL;
	*required = false;
	FFU_CONTAINER container = {0};
	unsigned index = 0;
	int error = FfuRoots(source, &container);
	if (error == 0) error = SelectFfuStore(source, &container, &index);
	if (error == 0) *required = container.stores[index].validation_count != 0;
	FreeFfuContainer(&container);
	return error;
}

static int LoadFfuContainer(const IMAGE_FILE_SOURCE* source, FFU_CONTAINER* container, FFU_BUFFER* buffer)
{
	int error = FfuRoots(source, container);
	uint64_t position = container->data_offset, payloads = 0, maps = 0, validations = 0;
	uint32_t buffer_bytes = 0;
	for (unsigned i = 0; error == 0 && i < container->count; i++) {
		FFU_LAYOUT* layout = &container->stores[i];
		layout->data_offset = position;
		error = FfuDescriptors(source, layout, layout->descriptor_offset, layout->compressed, layout->v2_payload_bytes, &position);
		payloads += layout->payload_count; maps += layout->map_count; validations += layout->validation_bytes;
		if (payloads > FFU_TABLE_BUDGET / sizeof(FFU_PAYLOAD) || maps > FFU_TABLE_BUDGET / sizeof(FFU_MAP)
			|| validations > FFU_TABLE_BUDGET) error = ENOTSUP;
		if (layout->buffer_bytes > buffer_bytes) buffer_bytes = layout->buffer_bytes;
	}
	if (error == 0) {
		FFU_LAYOUT* first = &container->stores[0];
		const uint64_t metadata = (container->data_offset - first->image_offset) / first->block_bytes;
		if (position != (uint64_t)source->info.st_size || metadata + payloads != first->hash_bytes / 32) error = EBADMSG;
		if (error == 0) {
			first->hashes = malloc(first->hash_bytes);
			buffer->packed = malloc(2 * buffer_bytes);
			buffer->decoded = malloc(buffer_bytes);
			buffer->offset = UINT64_MAX;
			if (first->hashes == NULL || buffer->packed == NULL || buffer->decoded == NULL) error = ENOMEM;
		}
		if (error == 0) error = InitializeNativeHashes();
		if (error == 0) error = ReadFfuBytes(source, first->hash_offset, first->hashes, first->hash_bytes);
		for (uint64_t offset = first->image_offset; error == 0 && offset < container->data_offset; offset += first->block_bytes) {
			error = ReadFfuBytes(source, offset, buffer->decoded, first->block_bytes);
			if (error == 0) error = CheckFfuHash(first, (offset - first->image_offset) / first->block_bytes, buffer->decoded);
		}
	}
	return error;
}

int ReadFfuStoreList(const IMAGE_FILE_SOURCE* source, ffu_store_t consumer, void* context)
{
	if (source == NULL || consumer == NULL) return EINVAL;
	FFU_CONTAINER container = {0}; FFU_BUFFER buffer = {0};
	int error = LoadFfuContainer(source, &container, &buffer);
	for (unsigned i = 0; error == 0 && i < container.count; i++) {
		const FFU_LAYOUT* store = &container.stores[i];
		error = consumer(context, store->store_index, store->disk_bytes, store->sector_bytes, store->device_path, store->path_bytes);
		if (error < 0) error = EIO;
	}
	FreeFfuContainer(&container);
	free(buffer.packed); free(buffer.decoded);
	return error;
}

static int LoadFfu(const IMAGE_FILE_SOURCE* source, uint64_t maximum_output,
	FFU_LAYOUT* loaded, FFU_BUFFER* buffers, uint8_t digest[32])
{
	FFU_CONTAINER container = {0}; FFU_BUFFER buffer = {0};
	int error = LoadFfuContainer(source, &container, &buffer);
	unsigned selected = 0;
	if (error == 0) error = SelectFfuStore(source, &container, &selected);
	if (error == 0 && container.stores[selected].disk_bytes > maximum_output) error = EFBIG;
	uint64_t hash_index = 0;
	if (error == 0) hash_index = (container.data_offset - container.stores[0].image_offset) / container.stores[0].block_bytes;
	/* Authenticate every store, including unselected payloads, before exposing output. */
	for (unsigned i = 0; error == 0 && i < container.count; i++) {
		FFU_LAYOUT* layout = &container.stores[i];
		for (size_t j = 0; error == 0 && j < layout->payload_count; j++) {
			const uint8_t* bytes;
			error = FfuPayload(source, layout, j, &buffer, &bytes);
			if (error == 0) error = CheckFfuHash(&container.stores[0], hash_index++, bytes);
		}
	}
	if (error == 0) {
		FFU_LAYOUT* first = &container.stores[0];
		HASH_CONTEXT hash;
		hash_init[HASH_SHA256](&hash);
		if (first->has_catalog) {
			static const uint8_t domain[] = "Rufus FFU catalog";
			hash_write[HASH_SHA256](&hash, domain, sizeof(domain));
			hash_write[HASH_SHA256](&hash, first->catalog_sha256, 32);
		}
		hash_write[HASH_SHA256](&hash, first->hashes, first->hash_bytes);
		if (container.count > 1) {
			const uint8_t index[2] = {(uint8_t)(selected + 1), (uint8_t)((selected + 1) >> 8)};
			hash_write[HASH_SHA256](&hash, index, sizeof(index));
		}
		hash_final[HASH_SHA256](&hash);
		memcpy(digest, hash.buf, 32);
		if (selected != 0) {
			container.stores[selected].hashes = first->hashes;
			first->hashes = NULL;
		}
		*loaded = container.stores[selected];
		memset(&container.stores[selected], 0, sizeof(*loaded));
		*buffers = buffer;
		memset(&buffer, 0, sizeof(buffer));
	}
	FreeFfuContainer(&container);
	free(buffer.packed); free(buffer.decoded);
	return error;
}

static int ConsumeFfuLogical(compressed_data_t consumer, void* context, COMPRESSED_IMAGE_RESULT* completed,
	const uint8_t* data, uint64_t bytes)
{
	while (bytes != 0) {
		if (IsRufusJobCancelled()) return ECANCELED;
		const size_t count = bytes < FFU_TRANSFER ? (size_t)bytes : FFU_TRANSFER;
		int error = consumer != NULL ? consumer(context, data, count, completed->decoded_bytes) : 0;
		if (error != 0) return error < 0 ? EIO : error;
		const size_t room = sizeof(completed->prefix) - completed->prefix_size;
		const size_t copy = count < room ? count : room;
		memcpy(completed->prefix + completed->prefix_size, data, copy);
		completed->prefix_size += copy; completed->decoded_bytes += count;
		data += count; bytes -= count;
	}
	return 0;
}

static int ConsumeFfuZeros(compressed_data_t consumer, void* context, COMPRESSED_IMAGE_RESULT* completed,
	FFU_BUFFER* buffer, uint64_t end, uint32_t block_bytes)
{
	if (completed->decoded_bytes > end) return EBADMSG;
	memset(buffer->decoded, 0, block_bytes); buffer->offset = UINT64_MAX;
	while (completed->decoded_bytes < end) {
		const uint64_t remaining = end - completed->decoded_bytes;
		const uint64_t count = remaining < block_bytes ? remaining : block_bytes;
		const int error = ConsumeFfuLogical(consumer, context, completed, buffer->decoded, count);
		if (error != 0) return error;
	}
	return 0;
}

static int InspectFfuOptimization(const IMAGE_FILE_SOURCE* source, const FFU_LAYOUT* layout,
	FFU_BUFFER* buffer, uint64_t* minimum);

int ReadFfuImageSource(const IMAGE_FILE_SOURCE* source, uint64_t maximum_output,
	compressed_data_t consumer, void* context, COMPRESSED_IMAGE_RESULT* completed)
{
	FFU_LAYOUT layout = {0}; FFU_BUFFER buffer = {0};
	int error = LoadFfu(source, maximum_output, &layout, &buffer, completed->layout_sha256);
	if (error == 0) {
		completed->ffu_store = layout.store_index;
		completed->ffu_stores = layout.store_count;
		if (layout.optimization.enabled) error = InspectFfuOptimization(source, &layout, &buffer, &completed->ffu_minimum_bytes);
		if (error == 0) error = ResolveFfuMap(&layout, layout.disk_bytes);
	}
	for (size_t i = 0; error == 0 && i < layout.map_count; i++) {
		const FFU_MAP* entry = &layout.map[i];
		if (completed->decoded_bytes < entry->position)
			error = ConsumeFfuZeros(consumer, context, completed, &buffer, entry->position, layout.block_bytes);
		if (error == 0 && completed->decoded_bytes != entry->position) error = EBADMSG;
		const uint8_t* data;
		if (error == 0) error = FfuPayload(source, &layout, entry->payload, &buffer, &data);
		if (error == 0) error = ConsumeFfuLogical(consumer, context, completed, data + entry->skip, entry->bytes);
	}
	if (error == 0 && completed->decoded_bytes < layout.disk_bytes)
		error = ConsumeFfuZeros(consumer, context, completed, &buffer, layout.disk_bytes, layout.block_bytes);
	FreeFfu(&layout, &buffer);
	if (error == 0) completed->members = 1;
	return error;
}

/* GPT relocation changes only disk geometry. Partition starts, lengths, names
 * and filesystem contents remain captured values for an ordinary full FFU. */
typedef struct { uint64_t offset, bytes; const uint8_t* data; } FFU_PATCH;
typedef struct {
	uint8_t primary[4096], backup[4096], mbr[4096];
	uint8_t* table;
	FFU_PATCH patch[5];
	unsigned count;
	uint32_t shift_start;
	int64_t shift_blocks;
	FFU_DEPLOYMENT_INFO deployment;
} FFU_GPT;

static void PutFfu32(uint8_t* p, uint32_t value)
{
	for (unsigned i = 0; i < 4; i++) p[i] = (uint8_t)(value >> (8 * i));
}

static void PutFfu64(uint8_t* p, uint64_t value)
{
	for (unsigned i = 0; i < 8; i++) p[i] = (uint8_t)(value >> (8 * i));
}

static int ReadFfuLogical(const IMAGE_FILE_SOURCE* source, const FFU_LAYOUT* layout,
	FFU_BUFFER* buffer, uint64_t offset, uint8_t* data, size_t bytes)
{
	if (offset > layout->disk_bytes || bytes > layout->disk_bytes - offset) return EBADMSG;
	memset(data, 0, bytes);
	/* The original table is still in descriptor order, including overwritten
	 * temporary GPTs. Only the few GPT ranges use this bounded scan. */
	for (size_t i = 0; i < layout->map_count; i++) {
		const FFU_MAP* entry = &layout->map[i];
		const uint64_t start = entry->method == 0 ? entry->position * layout->block_bytes
			: layout->disk_bytes - (entry->position + 1) * layout->block_bytes;
		const uint64_t begin = start > offset ? start : offset;
		const uint64_t end = start + layout->block_bytes < offset + bytes
			? start + layout->block_bytes : offset + bytes;
		if (begin >= end) continue;
		const uint8_t* payload;
		int error = FfuPayload(source, layout, entry->payload, buffer, &payload);
		if (error != 0) return error;
		memcpy(data + (begin - offset), payload + (begin - start), (size_t)(end - begin));
	}
	return 0;
}

static int CheckFfuGptHeader(uint8_t* header, unsigned sector)
{
	if (memcmp(header, "EFI PART", 8) != 0 || Le32(header + 8) != 0x10000 || Le32(header + 20) != 0)
		return EBADMSG;
	const uint32_t bytes = Le32(header + 12), expected = Le32(header + 16);
	if (bytes < 92 || bytes > sector) return EBADMSG;
	PutFfu32(header + 16, 0);
	const uint32_t actual = (uint32_t)crc32(0, header, bytes);
	PutFfu32(header + 16, expected);
	return actual == expected ? 0 : EBADMSG;
}

static void UpdateFfuGptCrc(uint8_t* header)
{
	PutFfu32(header + 16, 0);
	PutFfu32(header + 16, (uint32_t)crc32(0, header, Le32(header + 12)));
}

typedef struct { uint8_t id[16]; uint32_t index; } FFU_PARTITION_REF;

static int CompareFfuPartitionId(const void* first, const void* second)
{
	return memcmp(((const FFU_PARTITION_REF*)first)->id, ((const FFU_PARTITION_REF*)second)->id, 16);
}

static int AddFfuDelta(uint64_t base, int64_t delta, uint64_t* result)
{
	if (base > INT64_MAX) return EOVERFLOW;
	if (delta < 0) {
		const uint64_t magnitude = (uint64_t)(-(delta + 1)) + 1;
		if (base < magnitude) return ENOSPC;
		*result = base - magnitude;
	} else {
		if (base > (uint64_t)INT64_MAX - (uint64_t)delta) return EOVERFLOW;
		*result = base + (uint64_t)delta;
	}
	return 0;
}

static int CheckOptimizedFfuPartitions(const FFU_LAYOUT* layout, FFU_GPT* gpt)
{
	const FFU_OPTIMIZATION* manifest = &layout->optimization;
	const uint32_t entries = Le32(gpt->primary + 80), width = Le32(gpt->primary + 84);
	const uint64_t first = Le64(gpt->primary + 40), last = Le64(gpt->primary + 48);
	const bool same_store = memcmp(manifest->id, gpt->primary + 56, 16) == 0
		&& manifest->maximum_partitions == entries
		&& manifest->usable_offset == first * layout->sector_bytes
		&& manifest->usable_bytes == (last - first + 1) * layout->sector_bytes;
	if (!same_store) return EBADMSG;
	FFU_PARTITION_REF* references = malloc((size_t)entries * sizeof(*references));
	if (references == NULL) return ENOMEM;
	size_t count = 0;
	static const uint8_t empty[16] = {0};
	for (uint32_t index = 0; index < entries; index++) {
		const uint8_t* entry = gpt->table + (size_t)index * width;
		if (memcmp(entry, empty, 16) == 0) continue;
		memcpy(references[count].id, entry + 16, 16);
		references[count++].index = index;
	}
	qsort(references, count, sizeof(*references), CompareFfuPartitionId);
	int error = count == manifest->count ? 0 : EBADMSG;
	for (size_t index = 1; error == 0 && index < count; index++)
		if (memcmp(references[index - 1].id, references[index].id, 16) == 0) error = EBADMSG;
	uint64_t previous_end = first;
	for (size_t index = 0; error == 0 && index < manifest->count; index++) {
		const FFU_MANIFEST_PARTITION* part = &manifest->partitions[index];
		FFU_PARTITION_REF key = {0}; memcpy(key.id, part->id, 16);
		const FFU_PARTITION_REF* reference = bsearch(&key, references, count, sizeof(*references), CompareFfuPartitionId);
		if (reference == NULL) { error = EBADMSG; break; }
		const uint8_t* entry = gpt->table + (size_t)reference->index * width;
		const uint64_t begin = Le64(entry + 32), end = Le64(entry + 40);
		const bool same_partition = begin == part->offset_sectors && end - begin + 1 == part->sectors
			&& memcmp(entry, part->type, 16) == 0 && Le64(entry + 48) == part->attributes
			&& SameFfuPartitionName(entry + 56, part->name);
		if (!same_partition || begin < previous_end) { error = EBADMSG; break; }
		previous_end = end + 1;
		if (index + 1 == manifest->selected_partition) gpt->deployment.partition = reference->index + 1;
	}
	free(references);
	return error;
}

static int PrepareOptimizedFfu(const IMAGE_FILE_SOURCE* source, const FFU_LAYOUT* layout,
	FFU_BUFFER* buffer, uint64_t capacity, FFU_GPT* gpt)
{
	const uint64_t block = FFU_CAPTURE_DEFAULT_CHUNK_BYTES, following_alignment = 1024 * 1024;
	if (layout->block_bytes != block || layout->partial) return ENOTSUP;
	int error = CheckOptimizedFfuPartitions(layout, gpt);
	if (error != 0) return error;
	const FFU_OPTIMIZATION* manifest = &layout->optimization;
	const unsigned selected = manifest->selected_partition - 1, sector = layout->sector_bytes;
	const FFU_MANIFEST_PARTITION* part = &manifest->partitions[selected];
	const uint64_t start = part->offset_sectors * sector, captured_end = start + part->sectors * sector;
	uint64_t highest = 0;
	bool beginning = false;
	for (size_t index = 0; index < layout->map_count; index++) {
		const FFU_MAP* entry = &layout->map[index];
		if (entry->method != 0) continue;
		if (!beginning || entry->position > highest) highest = entry->position;
		beginning = true;
	}
	if (!beginning || highest > UINT32_MAX) return EBADMSG;
	const int64_t baseline = (int64_t)highest + 1;
	int64_t shift = (int64_t)(capacity / block) - baseline;
	if (shift < INT32_MIN || shift > INT32_MAX) return EOVERFLOW;
	uint64_t boundary, minimum_boundary;
	int64_t minimum_shift;
	if (selected + 1 == manifest->count) {
		gpt->shift_start = (uint32_t)highest;
		boundary = manifest->usable_offset + manifest->usable_bytes;
		if (highest < captured_end / block) return EBADMSG;
		const int64_t room = (int64_t)captured_end - (int64_t)boundary;
		minimum_shift = room / (int64_t)block; /* ceil for this nonpositive difference */
		minimum_boundary = captured_end;
	} else {
		boundary = manifest->partitions[selected + 1].offset_sectors * sector;
		if (boundary % block != 0 || boundary / block > UINT32_MAX) return ENOTSUP;
		gpt->shift_start = (uint32_t)(boundary / block);
		if (captured_end > INT64_MAX - following_alignment) return EOVERFLOW;
		minimum_boundary = AlignFfu(captured_end, (uint32_t)following_alignment);
		minimum_shift = ((int64_t)minimum_boundary - (int64_t)boundary) / (int64_t)block;
		uint64_t proposed;
		error = AddFfuDelta(boundary, shift * (int64_t)block, &proposed);
		if (error != 0) return error;
		const uint64_t aligned = proposed / following_alignment * following_alignment;
		shift = ((int64_t)aligned - (int64_t)boundary) / (int64_t)block;
	}
	const int64_t minimum_blocks = baseline + minimum_shift;
	if (minimum_blocks <= 0) return EBADMSG;
	gpt->deployment.minimum_bytes = (uint64_t)minimum_blocks * block;
	if (capacity < gpt->deployment.minimum_bytes) return ENOSPC;
	if (shift < INT32_MIN || shift > INT32_MAX) return EOVERFLOW;
	const int64_t delta = shift * (int64_t)block;
	uint64_t new_boundary, usable_end;
	error = AddFfuDelta(boundary, delta, &new_boundary);
	if (error == 0) error = AddFfuDelta(manifest->usable_offset + manifest->usable_bytes, delta, &usable_end);
	if (error != 0) return error;
	if (new_boundary < minimum_boundary) return ENOSPC;
	const uint64_t table_bytes = (uint64_t)Le32(gpt->primary + 80) * Le32(gpt->primary + 84);
	const uint64_t table_sectors = (table_bytes + sector - 1) / sector;
	const uint64_t total_sectors = capacity / sector;
	if (total_sectors <= table_sectors + 1) return ENOSPC;
	const uint64_t backup_table = total_sectors - table_sectors - 1;
	if (usable_end <= manifest->usable_offset || usable_end > backup_table * sector) return EBADMSG;
	const uint32_t width = Le32(gpt->primary + 84), entries = Le32(gpt->primary + 80);
	for (uint32_t index = 0; index < entries; index++) {
		uint8_t* entry = gpt->table + (size_t)index * width;
		static const uint8_t empty[16] = {0};
		if (memcmp(entry, empty, 16) == 0) continue;
		if (index + 1 == gpt->deployment.partition) PutFfu64(entry + 40, new_boundary / sector - 1);
		else if (Le64(entry + 32) * sector >= boundary) {
			uint64_t begin, end;
			error = AddFfuDelta(Le64(entry + 32) * sector, delta, &begin);
			if (error == 0) error = AddFfuDelta((Le64(entry + 40) + 1) * sector, delta, &end);
			if (error != 0) return error;
			PutFfu64(entry + 32, begin / sector);
			PutFfu64(entry + 40, end / sector - 1);
		}
	}
	uint8_t boot[4096];
	error = ReadFfuLogical(source, layout, buffer, start, boot, sector);
	if (error != 0) return error;
	const uint64_t filesystem_sectors = Le64(boot + 40);
	const bool ordinary_cluster = boot[13] != 0 && boot[13] <= 128 && (boot[13] & (boot[13] - 1)) == 0;
	const bool large_cluster = boot[13] > 128 && 256 - boot[13] < 26
		&& (UINT64_C(1) << (256 - boot[13])) >= sector;
	const bool ntfs = memcmp(boot + 3, "NTFS    ", 8) == 0 && Le16(boot + 11) == sector
		&& boot[510] == 0x55 && boot[511] == 0xaa && (ordinary_cluster || large_cluster);
	if (!ntfs) return ENOTSUP;
	if (filesystem_sectors == 0 || filesystem_sectors >= part->sectors) return EBADMSG;
	gpt->shift_blocks = shift;
	gpt->deployment.optimized = true;
	gpt->deployment.partition_offset = start;
	gpt->deployment.partition_bytes = new_boundary - start;
	gpt->deployment.filesystem_bytes = (filesystem_sectors + 1) * sector;
	memcpy(gpt->deployment.disk_id, manifest->id, 16);
	PutFfu64(gpt->primary + 48, usable_end / sector - 1);
	PutFfu64(gpt->backup + 48, usable_end / sector - 1);
	const uint32_t crc = (uint32_t)crc32(0, gpt->table, (uInt)table_bytes);
	PutFfu32(gpt->primary + 88, crc); PutFfu32(gpt->backup + 88, crc);
	return 0;
}

static int PrepareFfuGpt(const IMAGE_FILE_SOURCE* source, const FFU_LAYOUT* layout,
	FFU_BUFFER* buffer, uint64_t capacity, FFU_GPT* gpt)
{
	/* A partial payload describes changes to the validated existing disk. Its
	 * MinSectorCount is a lower bound, not a complete captured GPT geometry. */
	const bool optimized = layout->optimization.enabled;
	if (!optimized && (capacity == layout->disk_bytes || layout->partial)) return 0;
	const unsigned sector = layout->sector_bytes;
	const uint64_t old_sectors = layout->disk_bytes / sector, new_sectors = capacity / sector;
	int error = ReadFfuLogical(source, layout, buffer, sector, gpt->primary, sector);
	if (error != 0) return error;
	if (memcmp(gpt->primary, "EFI PART", 8) != 0) return optimized ? EBADMSG : 0;
	error = CheckFfuGptHeader(gpt->primary, sector);
	if (error != 0) return error;
	error = ReadFfuLogical(source, layout, buffer, layout->disk_bytes - sector, gpt->backup, sector);
	if (error == 0) error = CheckFfuGptHeader(gpt->backup, sector);
	if (error != 0) return error;
	const uint64_t first = Le64(gpt->primary + 40), last = Le64(gpt->primary + 48);
	const uint64_t table_lba = Le64(gpt->primary + 72), backup_lba = Le64(gpt->backup + 72);
	const uint32_t entries = Le32(gpt->primary + 80), entry_bytes = Le32(gpt->primary + 84);
	const bool geometry = Le64(gpt->primary + 24) == 1 && Le64(gpt->primary + 32) == old_sectors - 1
		&& Le64(gpt->backup + 24) == old_sectors - 1 && Le64(gpt->backup + 32) == 1
		&& first > 1 && last >= first && last < old_sectors - 1;
	const bool same_table = memcmp(gpt->primary + 40, gpt->backup + 40, 32) == 0
		&& memcmp(gpt->primary + 80, gpt->backup + 80, 12) == 0;
	if (!geometry || !same_table) return EBADMSG;
	const uint64_t table_bytes = (uint64_t)entries * entry_bytes;
	if (entries == 0 || entry_bytes < 128 || entry_bytes % 128 != 0 || table_bytes > FFU_GPT_TABLE_BUDGET)
		return ENOTSUP;
	const size_t bytes = (size_t)table_bytes;
	const uint64_t table_sectors = (bytes + sector - 1) / sector;
	const bool ranges = table_lba >= 2 && table_lba < first && table_sectors <= first - table_lba
		&& backup_lba > last && backup_lba < old_sectors - 1 && table_sectors <= old_sectors - 1 - backup_lba;
	if (!ranges) return EBADMSG;
	const size_t padded = (size_t)table_sectors * sector;
	gpt->table = malloc(padded);
	uint8_t* backup = malloc(padded);
	if (gpt->table == NULL || backup == NULL) { free(backup); return ENOMEM; }
	error = ReadFfuLogical(source, layout, buffer, table_lba * sector, gpt->table, padded);
	if (error == 0) error = ReadFfuLogical(source, layout, buffer, backup_lba * sector, backup, padded);
	if (error == 0 && (memcmp(gpt->table, backup, bytes) != 0
		|| (uint32_t)crc32(0, gpt->table, (uInt)bytes) != Le32(gpt->primary + 88))) error = EBADMSG;
	free(backup);
	if (error != 0) return error;
	for (uint32_t i = 0; i < entries; i++) {
		const uint8_t* entry = gpt->table + (size_t)i * entry_bytes;
		static const uint8_t empty[16] = {0};
		if (memcmp(entry, empty, 16) == 0) continue;
		const uint64_t begin = Le64(entry + 32), end = Le64(entry + 40);
		if (begin < first || end < begin || end > last) return EBADMSG;
	}
	if (optimized) {
		error = PrepareOptimizedFfu(source, layout, buffer, capacity, gpt);
		if (error != 0) return error;
	}
	const uint64_t new_backup_lba = optimized ? new_sectors - 1 - table_sectors
		: backup_lba + new_sectors - old_sectors;
	PutFfu64(gpt->primary + 32, new_sectors - 1);
	PutFfu64(gpt->backup + 24, new_sectors - 1);
	if (!optimized) {
		PutFfu64(gpt->primary + 48, last + new_sectors - old_sectors);
		PutFfu64(gpt->backup + 48, last + new_sectors - old_sectors);
	}
	PutFfu64(gpt->backup + 72, new_backup_lba);
	UpdateFfuGptCrc(gpt->primary); UpdateFfuGptCrc(gpt->backup);
	error = ReadFfuLogical(source, layout, buffer, 0, gpt->mbr, sector);
	if (error != 0) return error;
	for (unsigned i = 0; i < 4; i++) {
		uint8_t* entry = gpt->mbr + 446 + i * 16;
		if (entry[4] == 0xee && Le32(entry + 8) == 1) {
			PutFfu32(entry + 12, new_sectors - 1 > UINT32_MAX ? UINT32_MAX : (uint32_t)(new_sectors - 1));
			gpt->patch[gpt->count++] = (FFU_PATCH){0, sector, gpt->mbr};
			break;
		}
	}
	gpt->patch[gpt->count++] = (FFU_PATCH){sector, sector, gpt->primary};
	if (optimized) gpt->patch[gpt->count++] = (FFU_PATCH){table_lba * sector, padded, gpt->table};
	gpt->patch[gpt->count++] = (FFU_PATCH){new_backup_lba * sector, padded, gpt->table};
	gpt->patch[gpt->count++] = (FFU_PATCH){capacity - sector, sector, gpt->backup};
	return 0;
}

static int InspectFfuOptimization(const IMAGE_FILE_SOURCE* source, const FFU_LAYOUT* layout,
	FFU_BUFFER* buffer, uint64_t* minimum)
{
	FFU_GPT gpt = {0};
	const int error = PrepareFfuGpt(source, layout, buffer, layout->disk_bytes, &gpt);
	if (error == 0) *minimum = gpt.deployment.minimum_bytes;
	free(gpt.table);
	return error;
}

int PlanFfuDeployment(const IMAGE_FILE_SOURCE* source, uint64_t capacity, unsigned sector,
	const uint8_t expected_layout[32], FFU_DEPLOYMENT_INFO* result, ffu_partition_t consumer, void* context)
{
	if (source == NULL || expected_layout == NULL || result == NULL) return EINVAL;
	memset(result, 0, sizeof(*result));
	FFU_LAYOUT layout = {0}; FFU_BUFFER buffer = {0}; FFU_GPT gpt = {0};
	uint8_t digest[32];
	int error = LoadFfu(source, INT64_MAX, &layout, &buffer, digest);
	if (error == 0 && memcmp(digest, expected_layout, 32) != 0) error = ESTALE;
	if (error == 0 && (sector != layout.sector_bytes || capacity > INT64_MAX || capacity % sector)) error = EINVAL;
	if (error == 0 && !layout.optimization.enabled && capacity < layout.disk_bytes) error = ENOSPC;
	if (error == 0) error = PrepareFfuGpt(source, &layout, &buffer, capacity, &gpt);
	if (error == 0 && gpt.deployment.optimized && consumer != NULL) {
		const uint32_t count = Le32(gpt.primary + 80), width = Le32(gpt.primary + 84);
		for (uint32_t index = 0; error == 0 && index < count; index++) {
			const uint8_t* entry = gpt.table + (size_t)index * width;
			static const uint8_t empty[16] = {0};
			if (memcmp(entry, empty, 16) == 0) continue;
			FFU_DEPLOYMENT_PARTITION part = {.number = index + 1,
				.offset = Le64(entry + 32) * sector, .bytes = (Le64(entry + 40) - Le64(entry + 32) + 1) * sector};
			memcpy(part.type, entry, 16); memcpy(part.id, entry + 16, 16);
			error = consumer(context, &part);
			if (error < 0) error = EIO;
		}
	}
	if (error == 0) *result = gpt.deployment;
	free(gpt.table); FreeFfu(&layout, &buffer);
	return error;
}

static int EmitFfuBytes(compressed_data_t consumer, void* context, const uint8_t* data,
	uint64_t offset, uint64_t bytes, uint64_t* transferred)
{
	while (bytes != 0) {
		if (IsRufusJobCancelled()) return ECANCELED;
		const size_t count = bytes < FFU_TRANSFER ? (size_t)bytes : FFU_TRANSFER;
		const int error = consumer(context, data, count, offset);
		if (error != 0) return error < 0 ? EIO : error;
		*transferred += count; offset += count; data += count; bytes -= count;
	}
	return 0;
}

int VisitFfuImageBlocks(const IMAGE_FILE_SOURCE* source, uint64_t capacity, unsigned sector,
	const uint8_t expected_layout[32], FFU_VISIT_OPERATION operation, compressed_data_t validate_base,
	compressed_data_t consumer, void* context, uint64_t* transferred)
{
	if (source == NULL || expected_layout == NULL || consumer == NULL || transferred == NULL) return EINVAL;
	*transferred = 0;
	if (operation != FFU_DEPLOY && operation != FFU_READBACK) return EINVAL;
	FFU_LAYOUT layout = {0}; FFU_BUFFER buffer = {0}; FFU_GPT gpt = {0};
	uint8_t digest[32];
	int error = LoadFfu(source, INT64_MAX, &layout, &buffer, digest);
	if (error == 0 && memcmp(digest, expected_layout, 32) != 0) error = ESTALE;
	if (error == 0 && (sector != layout.sector_bytes || layout.block_bytes % sector != 0)) error = ENOTSUP;
	if (error == 0 && (capacity > INT64_MAX || capacity % sector != 0)) error = EINVAL;
	if (error == 0 && !layout.optimization.enabled && capacity < layout.disk_bytes) error = EINVAL;
	if (error == 0) error = PrepareFfuGpt(source, &layout, &buffer, capacity, &gpt);
	for (size_t index = 0; error == 0 && gpt.deployment.optimized && index < layout.map_count; index++) {
		FFU_MAP* entry = &layout.map[index];
		if (entry->method != 0 || entry->run_start < gpt.shift_start) continue;
		uint64_t position;
		error = AddFfuDelta(entry->position, gpt.shift_blocks, &position);
		if (error == 0 && position > UINT32_MAX) error = EOVERFLOW;
		if (error == 0) entry->position = position;
	}
	if (error == 0) error = ResolveFfuMap(&layout, capacity);
	if (error == 0 && operation == FFU_DEPLOY && layout.validation_count != 0) {
		if (validate_base == NULL) error = EINVAL;
		size_t cursor = 0;
		for (uint32_t i = 0; error == 0 && i < layout.validation_count; i++) {
			const uint8_t* entry = layout.validation + cursor;
			uint64_t offset = (uint64_t)Le32(entry) * layout.sector_bytes + Le32(entry + 4);
			uint32_t remaining = Le32(entry + 8);
			cursor += 12;
			while (error == 0 && remaining != 0) {
				if (IsRufusJobCancelled()) { error = ECANCELED; break; }
				const size_t bytes = remaining < FFU_TRANSFER ? remaining : FFU_TRANSFER;
				error = validate_base(context, layout.validation + cursor, bytes, offset);
				if (error < 0) error = EIO;
				cursor += bytes; offset += bytes; remaining -= (uint32_t)bytes;
			}
		}
	}
	/* Emit the final non-overlapping map. Omitted areas stay untouched. GPT
	 * patches supersede their covered bytes and are emitted once afterward. */
	for (size_t i = 0; error == 0 && i < layout.map_count; i++) {
		const FFU_MAP* entry = &layout.map[i]; const uint8_t* payload;
		error = FfuPayload(source, &layout, entry->payload, &buffer, &payload);
		uint64_t position = entry->position, end = position + entry->bytes;
		for (unsigned j = 0; error == 0 && j < gpt.count; j++) {
			const FFU_PATCH* patch = &gpt.patch[j];
			if (patch->offset >= end || patch->offset + patch->bytes <= position) continue;
			if (patch->offset > position)
				error = EmitFfuBytes(consumer, context, payload + entry->skip + position - entry->position,
					position, patch->offset - position, transferred);
			position = patch->offset + patch->bytes < end ? patch->offset + patch->bytes : end;
		}
		if (error == 0 && position < end)
			error = EmitFfuBytes(consumer, context, payload + entry->skip + position - entry->position,
				position, end - position, transferred);
	}
	for (unsigned i = 0; error == 0 && i < gpt.count; i++)
		error = EmitFfuBytes(consumer, context, gpt.patch[i].data, gpt.patch[i].offset, gpt.patch[i].bytes, transferred);
	free(gpt.table); FreeFfu(&layout, &buffer);
	return error;
}
