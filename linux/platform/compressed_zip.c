/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Retained-source adapter for Rufus's first-central-entry ZIP image policy. */
#include "compressed_zip.h"
#include "compressed_bzip2.h"
#include "compressed_lzma.h"
#include "compressed_xz.h"
#include "rufus_reporting.h"

#include <endian.h>
#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zlib.h>

/* Keep the original packed record definitions shared with the Windows reader. */
#define BB_BIG_ENDIAN (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#define PRAGMA_BEGIN_PACKED
#define PRAGMA_END_PACKED
#define PACKED __attribute__((packed))
#define BUILD_BUG_ON(condition) ((void)sizeof(char[1 - 2 * !!(condition)]))
#define SWAP_LE16 le16toh
#define SWAP_LE32 le32toh
#define SWAP_LE64 le64toh
#include "bled/unzip_format.inc"

enum { ZIP_IO_BYTES = 65536, ZIP_NAME_MAX = 4095, ZIP_LOCAL_BYTES = 4 + ZIP_HEADER_LEN,
	ZIP_CDF_BYTES = 4 + CDF_HEADER_LEN, ZIP_END_BYTES = 22, ZIP64_END_BYTES = 4 + CDE64_LEN };

typedef struct {
	uint64_t local, packed, decoded;
	uint32_t crc, disk;
	uint16_t method, flags;
} ZIP_ENTRY;

typedef struct zip_archive {
	const IMAGE_FILE_SOURCE* source;
	uint64_t size, directory, directory_end, next, expected, visited, data_offset;
	bool split, allow_empty, selected;
	ZIP_ENTRY entry;
	uint8_t input[ZIP_IO_BYTES];
} ZIP_READER;

static uint16_t Zip16(const void* bytes)
{
	uint16_t value;
	memcpy(&value, bytes, sizeof(value));
	return le16toh(value);
}

static uint32_t Zip32(const void* bytes)
{
	uint32_t value;
	memcpy(&value, bytes, sizeof(value));
	return le32toh(value);
}

static uint64_t Zip64(const void* bytes)
{
	uint64_t value;
	memcpy(&value, bytes, sizeof(value));
	return le64toh(value);
}

/* Malformed on-file ranges are format failures; an earlier real EOF is I/O.
 * No seek changes the caller's retained descriptor or its ownership. */
static int ReadZipAt(ZIP_READER* reader, uint64_t offset, void* bytes, size_t size)
{
	if (offset > reader->size || size > reader->size - offset)
		return EBADMSG;
	size_t done = 0;
	while (done < size) {
		if (IsRufusJobCancelled())
			return ECANCELED;
		const ssize_t count = pread(reader->source->fd, (uint8_t*)bytes + done,
			size - done, (off_t)(offset + done));
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			return count < 0 ? errno : EIO;
		done += (size_t)count;
	}
	return IsRufusJobCancelled() ? ECANCELED : 0;
}

/* Same last-acceptable-end-record search and 64-KiB window as the original.
 * Guard complete ordinary/ZIP64 records before their packed fields are read. */
static int FindZipDirectory(ZIP_READER* reader)
{
	const size_t size = reader->size < ZIP_IO_BYTES ? (size_t)reader->size : ZIP_IO_BYTES;
	const uint64_t base = reader->size - size;
	int error = ReadZipAt(reader, base, reader->input, size);
	if (error != 0)
		return error;
	bool found = false;
	for (size_t i = 0; i + ZIP_END_BYTES <= size; i++) {
		if (IsRufusJobCancelled())
			return ECANCELED;
		const uint8_t* bytes = reader->input + i;
		const uint32_t magic = Zip32(bytes);
		uint64_t offset, directory_size, count, total;
		bool split;
		if (magic == 0x06054b50) {
			cde_t end;
			memcpy(end.raw, bytes + 4, sizeof(end.raw));
			offset = le32toh(end.fmt.cdf_offset);
			if (offset == UINT32_MAX)
				continue;
			directory_size = le32toh(end.fmt.cdf_size);
			count = le16toh(end.fmt.cdf_entries_on_this_disk);
			total = le16toh(end.fmt.cdf_entries_total);
			split = le16toh(end.fmt.this_disk_no) != 0 || le16toh(end.fmt.disk_with_cdf_no) != 0;
			const size_t comment = Zip16(bytes + 20);
			if (comment > size - i - ZIP_END_BYTES)
				continue;
		} else if (magic == 0x06064b50) {
			if (size - i < ZIP64_END_BYTES)
				continue;
			cde64_t end;
			memcpy(end.raw, bytes + 4, sizeof(end.raw));
			const uint64_t record_size = le64toh(end.fmt.size);
			if (record_size < CDE64_LEN - 8 || record_size > size - i - 12)
				continue;
			offset = le64toh(end.fmt.cdf_offset);
			directory_size = le64toh(end.fmt.cdf_size);
			count = le64toh(end.fmt.cdf_entries_on_this_disk);
			total = le64toh(end.fmt.cdf_entries_total);
			split = le32toh(end.fmt.this_disk_no) != 0 || le32toh(end.fmt.disk_with_cdf_no) != 0;
		} else {
			continue;
		}
		/* A ZIP64 directory can precede an ordinary end record whose entry
		 * count alone is a sentinel, while its 32-bit offset still fits. */
		const bool zip64_count = reader->allow_empty && found && count == UINT16_MAX
			&& reader->expected >= count && reader->directory == offset
			&& reader->directory_end - reader->directory == directory_size;
		if (zip64_count) continue;
		const uint64_t end_offset = base + i;
		const bool empty = reader->allow_empty && offset == end_offset && directory_size == 0 && total == 0;
		if (!empty && (offset >= end_offset || directory_size > end_offset - offset))
			continue;
		reader->directory = offset;
		reader->expected = total;
		reader->next = offset;
		reader->directory_end = offset + directory_size;
		reader->split = split || count != total;
		found = true;
	}
	if (!found)
		return EBADMSG;
	return reader->split ? ENOTSUP : 0;
}

/* ZIP64 stores only fields whose ordinary values are sentinels, in this order.
 * Use bounded size_t arithmetic; the original uint16_t TLV cursor can wrap. */
static int ResolveZipExtra(ZIP_READER* reader, uint64_t offset, size_t size, ZIP_ENTRY* entry)
{
	int error = ReadZipAt(reader, offset, reader->input, size);
	if (error != 0)
		return error;
	bool zip64_seen = false;
	for (size_t at = 0; at < size; ) {
		if (size - at < EXTRA_HEADER_LEN)
			return EBADMSG;
		const unsigned tag = Zip16(reader->input + at);
		const size_t length = Zip16(reader->input + at + 2);
		at += EXTRA_HEADER_LEN;
		if (length > size - at)
			return EBADMSG;
		if (tag == 1) {
			if (zip64_seen)
				return EBADMSG;
			zip64_seen = true;
			const uint8_t* field = reader->input + at;
			size_t remaining = length;
			uint64_t* values[] = {&entry->decoded, &entry->packed, &entry->local};
			for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
				if (*values[i] != UINT32_MAX)
					continue;
				if (remaining < 8)
					return EBADMSG;
				*values[i] = Zip64(field);
				field += 8;
				remaining -= 8;
			}
			if (entry->disk == UINT16_MAX) {
				if (remaining < 4)
					return EBADMSG;
				entry->disk = Zip32(field);
			}
		}
		at += length;
	}
	if (!zip64_seen && (entry->decoded == UINT32_MAX || entry->packed == UINT32_MAX
		|| entry->local == UINT32_MAX || entry->disk == UINT16_MAX))
		return EBADMSG;
	return entry->disk != 0 ? ENOTSUP : 0;
}

static int SelectZipEntry(ZIP_READER* reader, uint64_t central_offset, ZIP_ENTRY* entry,
	uint64_t* data_offset, ZIP_ARCHIVE_ENTRY* description, uint64_t* next)
{
	uint8_t signature[4];
	cdf_header_t central;
	if (central_offset > reader->directory_end || reader->directory_end - central_offset < ZIP_CDF_BYTES)
		return EBADMSG;
	int error = ReadZipAt(reader, central_offset, signature, sizeof(signature));
	if (error != 0)
		return error;
	if (Zip32(signature) != 0x02014b50)
		return EBADMSG;
	error = ReadZipAt(reader, central_offset + 4, central.raw, sizeof(central.raw));
	if (error != 0)
		return error;
	ZIP_ENTRY indexed = {.local = le32toh(central.fmt.relative_offset_of_local_header),
		.packed = le32toh(central.fmt.cmpsize), .decoded = le32toh(central.fmt.ucmpsize),
		.crc = le32toh(central.fmt.crc32), .disk = le16toh(central.fmt.disk_number_start)};
	const size_t name = le16toh(central.fmt.filename_len), extra = le16toh(central.fmt.extra_len);
	const size_t variable = name + extra + le16toh(central.fmt.file_comment_length);
	if (variable > reader->directory_end - central_offset - ZIP_CDF_BYTES)
		return EBADMSG;
	error = ResolveZipExtra(reader, central_offset + ZIP_CDF_BYTES + name, extra, &indexed);
	if (error != 0)
		return error;
	/* DD consumes the first CDF entry even if it is a directory, as on Windows.
	 * Do not select a different entry by its name, type or boot marker. */
	if (indexed.local > reader->directory || ZIP_LOCAL_BYTES > reader->directory - indexed.local)
		return EBADMSG;
	error = ReadZipAt(reader, indexed.local, signature, sizeof(signature));
	if (error != 0)
		return error;
	if (Zip32(signature) != 0x04034b50)
		return EBADMSG;
	zip_header_t local;
	error = ReadZipAt(reader, indexed.local + 4, local.raw, sizeof(local.raw));
	if (error != 0)
		return error;
	entry->flags = le16toh(local.fmt.zip_flags);
	entry->method = le16toh(local.fmt.method);
	entry->packed = le32toh(local.fmt.cmpsize);
	entry->decoded = le32toh(local.fmt.ucmpsize);
	entry->crc = le32toh(local.fmt.crc32);
	if (entry->flags & 1)
		return ENOTSUP;
	if (entry->flags & 8) {
		entry->packed = indexed.packed;
		entry->decoded = indexed.decoded;
		entry->crc = indexed.crc;
	}
	const size_t local_name = le16toh(local.fmt.filename_len), local_extra = le16toh(local.fmt.extra_len);
	if (local_name > ZIP_NAME_MAX)
		return EBADMSG;
	const uint64_t header_end = indexed.local + ZIP_LOCAL_BYTES;
	if (local_name + local_extra > reader->directory - header_end)
		return EBADMSG;
	error = ResolveZipExtra(reader, header_end + local_name, local_extra, entry);
	if (error != 0)
		return error;
	*data_offset = header_end + local_name + local_extra;
	if (entry->packed > reader->directory - *data_offset)
		return EBADMSG;
	if (description != NULL) {
		const bool same_header = local_name == name && entry->method == le16toh(central.fmt.method)
			&& entry->flags == le16toh(central.fmt.cdf_flags);
		const bool same_content = entry->packed == indexed.packed && entry->decoded == indexed.decoded
			&& entry->crc == indexed.crc;
		if (!same_header || !same_content || local_name == 0) return EBADMSG;
		error = ReadZipAt(reader, header_end, description->name, local_name);
		if (error == 0) error = ReadZipAt(reader, central_offset + ZIP_CDF_BYTES, reader->input, name);
		if (error != 0) return error;
		if (memcmp(description->name, reader->input, name) != 0) return EBADMSG;
		description->name[local_name] = 0;
		description->name_bytes = local_name;
		description->size = entry->decoded;
		description->flags = entry->flags;
		description->attributes = le32toh(central.fmt.external_attributes);
		const char last = description->name[local_name - 1];
		description->directory = (description->attributes & 0x40000010) != 0 || last == '/' || last == '\\';
		*next = central_offset + ZIP_CDF_BYTES + variable;
	}
	return 0;
}

static int ReadStoredZip(ZIP_READER* reader, const ZIP_ENTRY* entry, uint64_t offset,
	uint64_t maximum, compressed_data_t consumer, void* context, COMPRESSED_IMAGE_RESULT* completed)
{
	if (entry->packed != entry->decoded)
		return EBADMSG;
	if (entry->packed > maximum)
		return EFBIG;
	uint32_t crc = (uint32_t)crc32(0, NULL, 0);
	while (completed->decoded_bytes < entry->packed) {
		const uint64_t remaining = entry->packed - completed->decoded_bytes;
		const size_t size = remaining < ZIP_IO_BYTES ? (size_t)remaining : ZIP_IO_BYTES;
		int error = ReadZipAt(reader, offset + completed->decoded_bytes, reader->input, size);
		if (error != 0)
			return error;
		if (consumer != NULL) {
			error = consumer(context, reader->input, size, completed->decoded_bytes);
			if (error != 0)
				return error > 0 ? error : EIO;
		}
		if (IsRufusJobCancelled())
			return ECANCELED;
		crc = (uint32_t)crc32(crc, reader->input, (uInt)size);
		const size_t space = sizeof(completed->prefix) - completed->prefix_size;
		const size_t copy = size < space ? size : space;
		memcpy(completed->prefix + completed->prefix_size, reader->input, copy);
		completed->prefix_size += copy;
		completed->decoded_bytes += size;
	}
	if (crc != entry->crc)
		return EBADMSG;
	completed->members = 1;
	completed->trailing_bytes = reader->size - offset - entry->packed;
	return 0;
}

static int ReadZipEntryData(ZIP_READER* reader, const ZIP_ENTRY* entry, uint64_t offset,
	uint64_t maximum_output, compressed_data_t consumer, void* context, COMPRESSED_IMAGE_RESULT* completed)
{
	const IMAGE_FILE_SOURCE* source = reader->source;
	int error = 0;
	if (entry->method == 0) {
		error = ReadStoredZip(reader, entry, offset, maximum_output, consumer, context, completed);
	} else if (entry->method == 8) {
		uint32_t crc;
		error = ReadZipDeflateSource(source, offset, entry->packed, maximum_output, consumer, context, completed, &crc);
		if (error == 0 && crc != entry->crc)
			error = EBADMSG;
		/* Like the original deflate path, publish actual output even when
		 * its declared length differs, provided the complete CRC matches. */
	} else if (entry->method == 12) {
		uint32_t crc;
		error = ReadZipBzip2Source(source, offset, entry->packed, maximum_output,
			consumer, context, completed, &crc);
		if (error == 0 && crc != entry->crc)
			error = EBADMSG;
	} else if (entry->method == 14) {
		uint32_t crc;
		error = ReadZipLzmaSource(source, offset, entry->packed,
			entry->decoded, (entry->flags & 2) != 0, maximum_output,
			consumer, context, completed, &crc);
		if (error == 0 && crc != entry->crc)
			error = EBADMSG;
	} else if (entry->method == 95) {
		uint32_t crc;
		error = ReadZipXzSource(source, offset, entry->packed, maximum_output,
			consumer, context, completed, &crc);
		if (error == 0 && crc != entry->crc)
			error = EBADMSG;
	} else {
		error = ENOTSUP;
	}
	return error;
}

int ReadZipImageSource(const IMAGE_FILE_SOURCE* source, uint64_t maximum_output,
	compressed_data_t consumer, void* context, COMPRESSED_IMAGE_RESULT* completed)
{
	ZIP_READER* reader = calloc(1, sizeof(*reader));
	if (reader == NULL) return ENOMEM;
	reader->source = source;
	reader->size = (uint64_t)source->info.st_size;
	ZIP_ENTRY entry = {0};
	uint64_t offset = 0;
	int error = FindZipDirectory(reader);
	if (error == 0) error = SelectZipEntry(reader, reader->directory, &entry, &offset, NULL, NULL);
	if (error == 0) error = ReadZipEntryData(reader, &entry, offset, maximum_output, consumer, context, completed);
	free(reader);
	return error;
}

int OpenZipArchive(const IMAGE_FILE_SOURCE* source, ZIP_ARCHIVE** archive)
{
	if (archive == NULL) return EINVAL;
	*archive = NULL;
	if (source == NULL || source->fd < 0 || source->info.st_size < ZIP_END_BYTES) return EINVAL;
	ZIP_READER* reader = calloc(1, sizeof(*reader));
	if (reader == NULL) return ENOMEM;
	reader->source = source;
	reader->size = (uint64_t)source->info.st_size;
	reader->allow_empty = true;
	const int error = FindZipDirectory(reader);
	if (error != 0) { free(reader); return error; }
	*archive = reader;
	return 0;
}

int NextZipArchiveEntry(ZIP_ARCHIVE* archive, ZIP_ARCHIVE_ENTRY* entry)
{
	if (archive == NULL || entry == NULL) return EINVAL;
	memset(entry, 0, sizeof(*entry));
	archive->selected = false;
	if (archive->next == archive->directory_end)
		return archive->visited == archive->expected ? ENOENT : EBADMSG;
	if (archive->visited >= archive->expected) return EBADMSG;
	memset(&archive->entry, 0, sizeof(archive->entry));
	const int error = SelectZipEntry(archive, archive->next, &archive->entry,
		&archive->data_offset, entry, &archive->next);
	if (error == 0) { archive->visited++; archive->selected = true; }
	return error;
}

int ReadZipArchiveEntry(ZIP_ARCHIVE* archive, uint64_t maximum_output,
	compressed_data_t consumer, void* context, COMPRESSED_IMAGE_RESULT* completed)
{
	if (completed == NULL) return EINVAL;
	memset(completed, 0, sizeof(*completed));
	if (archive == NULL || !archive->selected || maximum_output == 0) return EINVAL;
	int error = ReadZipEntryData(archive, &archive->entry, archive->data_offset,
		maximum_output, consumer, context, completed);
	if (error == 0 && completed->decoded_bytes != archive->entry.decoded) error = EBADMSG;
	return error;
}

void CloseZipArchive(ZIP_ARCHIVE* archive)
{
	free(archive);
}
