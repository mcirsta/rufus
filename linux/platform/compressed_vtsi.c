/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "compressed_vtsi.h"
#include "rufus_reporting.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

enum {
	VTSI_SECTOR_BYTES = 512,
	VTSI_SEGMENT_BYTES = 24,
	VTSI_TABLE_BATCH_SEGMENTS = 1024,
	VTSI_TRANSFER_BYTES = 64 * 1024,
	VTSI_FOOTER_CHECKSUM_OFFSET = 24,
	VTSI_SEGMENT_COUNT_OFFSET = 28,
	VTSI_SEGMENT_CHECKSUM_OFFSET = 32,
	VTSI_SEGMENT_OFFSET_OFFSET = 36
};

typedef struct {
	const IMAGE_FILE_SOURCE* source;
	uint64_t table_offset;
	uint32_t total, next, batch_start, batch_count;
	uint8_t bytes[VTSI_SEGMENT_BYTES * VTSI_TABLE_BATCH_SEGMENTS];
} VTSI_TABLE_READER;

typedef struct {
	uint64_t disk_size, table_offset, data_bytes;
	uint32_t count, segment_checksum;
} VTSI_LAYOUT;

static uint16_t ReadLe16(const uint8_t* bytes)
{
	return (uint16_t)bytes[0] | (uint16_t)bytes[1] << 8;
}

static uint32_t ReadLe32(const uint8_t* bytes)
{
	return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8
		| (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

static uint64_t ReadLe64(const uint8_t* bytes)
{
	return (uint64_t)ReadLe32(bytes) | (uint64_t)ReadLe32(bytes + 4) << 32;
}

static int ReadVtsiBytes(const IMAGE_FILE_SOURCE* source, uint64_t offset, uint8_t* bytes, size_t size)
{
	const uint64_t source_bytes = (uint64_t)source->info.st_size;
	if (offset > source_bytes || size > source_bytes - offset)
		return EBADMSG;
	while (size != 0) {
		if (IsRufusJobCancelled())
			return ECANCELED;
		const ssize_t count = pread(source->fd, bytes, size, (off_t)offset);
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			return count < 0 ? errno : EIO;
		bytes += count;
		size -= (size_t)count;
		offset += (uint64_t)count;
	}
	return 0;
}

static int NextVtsiSegment(VTSI_TABLE_READER* reader, uint8_t segment[VTSI_SEGMENT_BYTES])
{
	if (reader->next >= reader->total)
		return EBADMSG;
	if (reader->next >= reader->batch_start + reader->batch_count) {
		reader->batch_start = reader->next;
		const uint32_t remaining = reader->total - reader->next;
		reader->batch_count = remaining < VTSI_TABLE_BATCH_SEGMENTS ? remaining : VTSI_TABLE_BATCH_SEGMENTS;
		const int error = ReadVtsiBytes(reader->source,
			reader->table_offset + (uint64_t)reader->next * VTSI_SEGMENT_BYTES,
			reader->bytes, (size_t)reader->batch_count * VTSI_SEGMENT_BYTES);
		if (error != 0)
			return error;
	}
	memcpy(segment, reader->bytes + (size_t)(reader->next - reader->batch_start) * VTSI_SEGMENT_BYTES,
		VTSI_SEGMENT_BYTES);
	reader->next++;
	return 0;
}

static uint32_t VtsiChecksum(const uint8_t* bytes, size_t size)
{
	uint32_t sum = 0;
	for (size_t i = 0; i < size; i++)
		sum += bytes[i];
	return ~sum;
}

static int EmitVtsiBytes(const uint8_t* bytes, size_t size, compressed_data_t consumer,
	void* context, COMPRESSED_IMAGE_RESULT* completed)
{
	if (IsRufusJobCancelled())
		return ECANCELED;
	if (consumer != NULL) {
		const int error = consumer(context, bytes, size, completed->decoded_bytes);
		if (error != 0)
			return error > 0 ? error : EIO;
	}
	const size_t prefix_space = sizeof(completed->prefix) - completed->prefix_size;
	const size_t copy = size < prefix_space ? size : prefix_space;
	memcpy(completed->prefix + completed->prefix_size, bytes, copy);
	completed->prefix_size += copy;
	completed->decoded_bytes += size;
	return 0;
}

static int ScanVtsiSegments(const IMAGE_FILE_SOURCE* source, uint64_t table_offset,
	uint32_t count, uint64_t disk_size, uint32_t expected_checksum, uint64_t* data_bytes)
{
	uint8_t segment[VTSI_SEGMENT_BYTES];
	VTSI_TABLE_READER reader = {.source = source, .table_offset = table_offset, .total = count};
	uint32_t sum = 0;
	uint64_t previous_end = 0, data_end = 0;
	for (uint32_t i = 0; i < count; i++) {
		const int error = NextVtsiSegment(&reader, segment);
		if (error != 0)
			return error;
		for (size_t j = 0; j < sizeof(segment); j++)
			sum += segment[j];
		const uint64_t start_sector = ReadLe64(segment);
		const uint64_t sector_count = ReadLe64(segment + 8);
		const uint64_t disk_sectors = disk_size / VTSI_SECTOR_BYTES;
		if (start_sector < previous_end || start_sector > disk_sectors
			|| sector_count > disk_sectors - start_sector)
			return EBADMSG;
		previous_end = start_sector + sector_count;
		if (sector_count > (table_offset - data_end) / VTSI_SECTOR_BYTES)
			return EBADMSG;
		data_end += sector_count * VTSI_SECTOR_BYTES;
	}
	if (~sum != expected_checksum)
		return EBADMSG;
	*data_bytes = data_end;
	return 0;
}

/* The original writer consumes segment data consecutively and seeks its output
 * for each disk offset; the on-disk data_offset field is not used there. The
 * source reader exposes a complete logical byte stream, filling sparse gaps
 * with zero bytes for analysis. No target is written by this provider. */
static int DecodeVtsi(const IMAGE_FILE_SOURCE* source, uint64_t table_offset, uint32_t count,
	uint64_t disk_size, uint32_t segment_checksum, compressed_data_t consumer,
	void* context, COMPRESSED_IMAGE_RESULT* completed)
{
	static const uint8_t zeros[VTSI_TRANSFER_BYTES];
	uint8_t buffer[VTSI_TRANSFER_BYTES];
	uint8_t segment[VTSI_SEGMENT_BYTES];
	VTSI_TABLE_READER reader = {.source = source, .table_offset = table_offset, .total = count};
	uint64_t data_offset = 0;
	uint32_t sum = 0;
	for (uint32_t i = 0; i < count; i++) {
		int error = NextVtsiSegment(&reader, segment);
		if (error != 0)
			return error;
		for (size_t j = 0; j < sizeof(segment); j++)
			sum += segment[j];
		const uint64_t start_sector = ReadLe64(segment);
		const uint64_t sector_count = ReadLe64(segment + 8);
		const uint64_t disk_sectors = disk_size / VTSI_SECTOR_BYTES;
		if (start_sector > disk_sectors || sector_count > disk_sectors - start_sector
			|| start_sector * VTSI_SECTOR_BYTES < completed->decoded_bytes
			|| data_offset > table_offset
			|| sector_count > (table_offset - data_offset) / VTSI_SECTOR_BYTES)
			return EBADMSG;
		const uint64_t start = start_sector * VTSI_SECTOR_BYTES;
		uint64_t bytes_left = sector_count * VTSI_SECTOR_BYTES;
		while (completed->decoded_bytes < start) {
			const uint64_t remaining = start - completed->decoded_bytes;
			const size_t size = remaining < sizeof(buffer) ? (size_t)remaining : sizeof(buffer);
			error = EmitVtsiBytes(zeros, size, consumer, context, completed);
			if (error != 0)
				return error;
		}
		while (bytes_left != 0) {
			const size_t size = bytes_left < sizeof(buffer) ? (size_t)bytes_left : sizeof(buffer);
			error = ReadVtsiBytes(source, data_offset, buffer, size);
			if (error != 0)
				return error;
			error = EmitVtsiBytes(buffer, size, consumer, context, completed);
			if (error != 0)
				return error;
			data_offset += size;
			bytes_left -= size;
		}
	}
	if (~sum != segment_checksum)
		return ESTALE;
	while (completed->decoded_bytes < disk_size) {
		const uint64_t remaining = disk_size - completed->decoded_bytes;
		const size_t size = remaining < sizeof(buffer) ? (size_t)remaining : sizeof(buffer);
		const int error = EmitVtsiBytes(zeros, size, consumer, context, completed);
		if (error != 0)
			return error;
	}
	completed->members = 1;
	return 0;
}

static int LoadVtsiLayout(const IMAGE_FILE_SOURCE* source, uint64_t maximum_output, VTSI_LAYOUT* layout)
{
	const uint64_t source_bytes = (uint64_t)source->info.st_size;
	if (source_bytes < VTSI_SECTOR_BYTES || source_bytes % VTSI_SECTOR_BYTES != 0)
		return EBADMSG;
	const uint64_t footer_offset = source_bytes - VTSI_SECTOR_BYTES;
	uint8_t footer[VTSI_SECTOR_BYTES];
	int error = ReadVtsiBytes(source, footer_offset, footer, sizeof(footer));
	if (error != 0)
		return error;
	const uint8_t magic[8] = {'V', 'E', 'N', 'T', 'O', 'Y', 0, 0};
	if (memcmp(footer, magic, sizeof(magic)) != 0 || ReadLe16(footer + 8) != 1)
		return EBADMSG;
	const uint32_t footer_checksum = ReadLe32(footer + VTSI_FOOTER_CHECKSUM_OFFSET);
	memset(footer + VTSI_FOOTER_CHECKSUM_OFFSET, 0, sizeof(uint32_t));
	if (VtsiChecksum(footer, sizeof(footer)) != footer_checksum)
		return EBADMSG;
	const uint64_t disk_size = ReadLe64(footer + 12);
	if (disk_size == 0 || disk_size % VTSI_SECTOR_BYTES != 0)
		return EBADMSG;
	if (disk_size > maximum_output)
		return EFBIG;
	const uint32_t count = ReadLe32(footer + VTSI_SEGMENT_COUNT_OFFSET);
	const uint32_t segment_checksum = ReadLe32(footer + VTSI_SEGMENT_CHECKSUM_OFFSET);
	const uint64_t table_offset = ReadLe64(footer + VTSI_SEGMENT_OFFSET_OFFSET);
	if (table_offset > footer_offset || count > (footer_offset - table_offset) / VTSI_SEGMENT_BYTES)
		return EBADMSG;
	uint64_t data_bytes = 0;
	error = ScanVtsiSegments(source, table_offset, count, disk_size, segment_checksum, &data_bytes);
	if (error != 0)
		return error;
	*layout = (VTSI_LAYOUT){.disk_size = disk_size, .table_offset = table_offset,
		.data_bytes = data_bytes, .count = count, .segment_checksum = segment_checksum};
	return 0;
}

int ReadVtsiImageSource(const IMAGE_FILE_SOURCE* source, uint64_t maximum_output,
	compressed_data_t consumer, void* context, COMPRESSED_IMAGE_RESULT* completed)
{
	VTSI_LAYOUT layout;
	const int error = LoadVtsiLayout(source, maximum_output, &layout);
	if (error != 0)
		return error;
	return DecodeVtsi(source, layout.table_offset, layout.count, layout.disk_size,
		layout.segment_checksum,
		consumer, context, completed);
}

int VisitVtsiImageSegments(const IMAGE_FILE_SOURCE* source, const char* path, uint64_t maximum_output,
	vtsi_segment_t visitor, void* context, uint64_t* disk_size, uint64_t* data_bytes)
{
	if (disk_size == NULL || data_bytes == NULL)
		return EINVAL;
	*disk_size = *data_bytes = 0;
	if (maximum_output == 0 || maximum_output > INT64_MAX)
		return EINVAL;
	int error = CheckImageFileSource(source, path);
	if (error != 0)
		return error;
	VTSI_LAYOUT layout;
	error = LoadVtsiLayout(source, maximum_output, &layout);
	if (error != 0)
		return error;
	VTSI_TABLE_READER reader = {.source = source, .table_offset = layout.table_offset,
		.total = layout.count};
	uint8_t segment[VTSI_SEGMENT_BYTES];
	uint64_t source_offset = 0, previous_end = 0;
	uint32_t sum = 0;
	const uint64_t disk_sectors = layout.disk_size / VTSI_SECTOR_BYTES;
	for (uint32_t i = 0; i < layout.count; i++) {
		error = NextVtsiSegment(&reader, segment);
		if (error != 0)
			return error;
		for (size_t j = 0; j < sizeof(segment); j++)
			sum += segment[j];
		const uint64_t start_sector = ReadLe64(segment);
		const uint64_t sector_count = ReadLe64(segment + 8);
		if (start_sector < previous_end || start_sector > disk_sectors
			|| sector_count > disk_sectors - start_sector
			|| source_offset > layout.table_offset
			|| sector_count > (layout.table_offset - source_offset) / VTSI_SECTOR_BYTES)
			return EBADMSG;
		const uint64_t bytes = sector_count * VTSI_SECTOR_BYTES;
		if (visitor != NULL) {
			error = visitor(context, start_sector * VTSI_SECTOR_BYTES, source_offset, bytes);
			if (error != 0)
				return error > 0 ? error : EIO;
		}
		previous_end = start_sector + sector_count;
		source_offset += bytes;
	}
	if (source_offset != layout.data_bytes || ~sum != layout.segment_checksum)
		return ESTALE;
	error = CheckImageFileSource(source, path);
	if (error != 0)
		return error;
	if (IsRufusJobCancelled())
		return ECANCELED;
	*disk_size = layout.disk_size;
	*data_bytes = layout.data_bytes;
	return 0;
}
