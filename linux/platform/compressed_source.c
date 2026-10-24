/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "compressed_source.h"
#include "compressed_bzip2.h"
#include "compressed_lzma.h"
#include "compressed_lzw.h"
#include "compressed_xz.h"
#include "compressed_zip.h"
#include "compressed_zstd.h"
#include "compressed_vtsi.h"
#include "compressed_vhd.h"
#include "compressed_vhdx.h"
#include "compressed_ffu.h"
#include "rufus_reporting.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zlib.h>

enum {
	COMPRESSED_IO_BYTES = 64 * 1024,
	GZIP_DECODER_MEMORY_BYTES = 256 * 1024,
	GZIP_WINDOW_BITS = 16 + MAX_WBITS
};

typedef struct GZIP_ALLOCATION {
	max_align_t alignment;
	struct GZIP_ALLOCATION *previous, *next;
	size_t bytes;
} GZIP_ALLOCATION;

typedef struct {
	z_stream stream;
	GZIP_ALLOCATION* allocation;
	size_t memory;
	uint8_t input[COMPRESSED_IO_BYTES], output[COMPRESSED_IO_BYTES];
	uint64_t read_offset, consumed, end_offset;
} GZIP_READER;

/* Keep provider allocations per operation and bounded. Tracking also handles
 * partial initialization failures without a nonlocal jump over cleanup. */
static voidpf AllocateGzip(voidpf opaque, uInt items, uInt size)
{
	GZIP_READER* reader = opaque;
	if (size != 0 && items > (GZIP_DECODER_MEMORY_BYTES - sizeof(GZIP_ALLOCATION)) / size)
		return NULL;
	const size_t bytes = (size_t)items * size + sizeof(GZIP_ALLOCATION);
	if (bytes > GZIP_DECODER_MEMORY_BYTES - reader->memory)
		return NULL;
	GZIP_ALLOCATION* allocation = calloc(1, bytes);
	if (allocation == NULL)
		return NULL;
	allocation->bytes = bytes;
	allocation->next = reader->allocation;
	if (reader->allocation != NULL)
		reader->allocation->previous = allocation;
	reader->allocation = allocation;
	reader->memory += bytes;
	return allocation + 1;
}

static void FreeGzip(voidpf opaque, voidpf memory)
{
	GZIP_READER* reader = opaque;
	if (memory == NULL)
		return;
	GZIP_ALLOCATION* allocation = (GZIP_ALLOCATION*)memory - 1;
	if (allocation->previous != NULL)
		allocation->previous->next = allocation->next;
	else
		reader->allocation = allocation->next;
	if (allocation->next != NULL)
		allocation->next->previous = allocation->previous;
	reader->memory -= allocation->bytes;
	free(allocation);
}

static int GzipError(int status)
{
	if (status == Z_MEM_ERROR)
		return ENOMEM;
	if (status == Z_VERSION_ERROR)
		return ENOTSUP;
	return status == Z_DATA_ERROR || status == Z_NEED_DICT ? EBADMSG : EIO;
}

/* Compact only between inflate calls. pread does not move the caller's offset.
 * Read in batches; repeated short reads and EINTR cannot masquerade as EOF. */
static int FillGzipInput(GZIP_READER* reader, const IMAGE_FILE_SOURCE* source, unsigned minimum)
{
	z_stream* stream = &reader->stream;
	const uint64_t size = reader->end_offset;
	if (stream->avail_in >= minimum)
		return 0;
	if (stream->avail_in != 0)
		memmove(reader->input, stream->next_in, stream->avail_in);
	stream->next_in = reader->input;
	while (stream->avail_in < sizeof(reader->input) && reader->read_offset < size) {
		if (IsRufusJobCancelled())
			return ECANCELED;
		const size_t space = sizeof(reader->input) - stream->avail_in;
		const uint64_t remaining = size - reader->read_offset;
		const size_t requested = remaining < space ? (size_t)remaining : space;
		const ssize_t count = pread(source->fd, reader->input + stream->avail_in,
			requested, (off_t)reader->read_offset);
		if (count < 0 && errno == EINTR)
			continue;
		if (count < 0)
			return errno;
		if (count == 0)
			return EIO;
		reader->read_offset += (uint64_t)count;
		stream->avail_in += (uInt)count;
	}
	return 0;
}

static int DecodeGzip(GZIP_READER* reader, const IMAGE_FILE_SOURCE* source, uint64_t maximum,
	compressed_data_t consumer, void* context, COMPRESSED_IMAGE_RESULT* completed, uint32_t* raw_crc)
{
	z_stream* stream = &reader->stream;
	for (;;) {
		if (IsRufusJobCancelled())
			return ECANCELED;
		int error = FillGzipInput(reader, source, 1);
		if (error != 0)
			return error;
		if (IsRufusJobCancelled())
			return ECANCELED;
		stream->next_out = reader->output;
		stream->avail_out = sizeof(reader->output);
		const uInt before = stream->avail_in;
		const int status = inflate(stream, Z_NO_FLUSH);
		const size_t consumed = before - stream->avail_in;
		const size_t produced = sizeof(reader->output) - stream->avail_out;
		reader->consumed += consumed;
		if (IsRufusJobCancelled())
			return ECANCELED;
		if (produced > maximum - completed->decoded_bytes)
			return EFBIG;
		if (produced != 0) {
			if (raw_crc != NULL)
				*raw_crc = (uint32_t)crc32(*raw_crc, reader->output, (uInt)produced);
			if (consumer != NULL) {
				error = consumer(context, reader->output, produced, completed->decoded_bytes);
				if (error != 0)
					return error > 0 ? error : EIO;
			}
			const size_t remaining = sizeof(completed->prefix) - completed->prefix_size;
			const size_t copy = produced < remaining ? produced : remaining;
			memcpy(completed->prefix + completed->prefix_size, reader->output, copy);
			completed->prefix_size += copy;
			completed->decoded_bytes += produced;
		}
		if (status == Z_STREAM_END) {
			completed->members++;
			if (raw_crc != NULL) {
				completed->trailing_bytes = completed->source_bytes - reader->consumed;
				return 0;
			}
			error = FillGzipInput(reader, source, 2);
			if (error != 0)
				return error;
			const bool next_member = stream->avail_in >= 2
				&& stream->next_in[0] == 0x1f && stream->next_in[1] == 0x8b;
			if (!next_member) {
				completed->trailing_bytes = completed->source_bytes - reader->consumed;
				return 0;
			}
			z_const Bytef* next = stream->next_in;
			const uInt available = stream->avail_in;
			const int reset = inflateReset2(stream, GZIP_WINDOW_BITS);
			if (reset != Z_OK)
				return GzipError(reset);
			stream->next_in = next;
			stream->avail_in = available;
		} else {
			if (status != Z_OK && status != Z_BUF_ERROR)
				return GzipError(status);
			if (consumed == 0 && produced == 0)
				return EBADMSG;
		}
	}
}

static int InflateImageSource(GZIP_READER* reader, const IMAGE_FILE_SOURCE* source, int window_bits,
	uint64_t maximum_output, compressed_data_t consumer, void* context,
	COMPRESSED_IMAGE_RESULT* completed, uint32_t* raw_crc)
{
	int error = 0;
	reader->stream.zalloc = AllocateGzip;
	reader->stream.zfree = FreeGzip;
	reader->stream.opaque = reader;
	const int initialized = inflateInit2(&reader->stream, window_bits);
	if (initialized != Z_OK)
		error = GzipError(initialized);
	else {
		error = DecodeGzip(reader, source, maximum_output, consumer, context, completed, raw_crc);
		const int ended = inflateEnd(&reader->stream);
		if (error == 0 && ended != Z_OK)
			error = GzipError(ended);
	}
	while (reader->allocation != NULL)
		FreeGzip(reader, reader->allocation + 1);
	return error;
}

static int ReadGzipImageSource(const IMAGE_FILE_SOURCE* source, uint64_t maximum_output,
	compressed_data_t consumer, void* context, COMPRESSED_IMAGE_RESULT* completed)
{
	GZIP_READER reader = {.end_offset = (uint64_t)source->info.st_size};
	int error = FillGzipInput(&reader, source, 2);
	if (error != 0)
		return error;
	/* Original gunzip dispatches only the initial Unix-compress signature,
	 * before allocating gzip state. Keep the filename association as gzip;
	 * LZW rereads the retained source without changing its descriptor offset. */
	const bool unix_compress = reader.stream.avail_in >= 2
		&& reader.stream.next_in[0] == 0x1f && reader.stream.next_in[1] == 0x9d;
	if (unix_compress)
		return ReadLzwImageSource(source, maximum_output, consumer, context, completed);
	return InflateImageSource(&reader, source, GZIP_WINDOW_BITS, maximum_output, consumer, context, completed, NULL);
}

int ReadZipDeflateSource(const IMAGE_FILE_SOURCE* source, uint64_t offset, uint64_t size,
	uint64_t maximum_output, compressed_data_t consumer, void* context,
	COMPRESSED_IMAGE_RESULT* completed, uint32_t* crc)
{
	const uint64_t source_bytes = (uint64_t)source->info.st_size;
	if (offset > source_bytes || size > source_bytes - offset || crc == NULL)
		return EINVAL;
	GZIP_READER reader = {.read_offset = offset, .consumed = offset, .end_offset = offset + size};
	*crc = (uint32_t)crc32(0, NULL, 0);
	return InflateImageSource(&reader, source, -MAX_WBITS, maximum_output, consumer, context, completed, crc);
}

int ReadCompressedImageSource(const IMAGE_FILE_SOURCE* source, const char* path,
	uint64_t maximum_output, compressed_data_t consumer, void* context, COMPRESSED_IMAGE_RESULT* result)
{
	COMPRESSED_IMAGE_RESULT completed = {0};
	if (result == NULL)
		return EINVAL;
	memset(result, 0, sizeof(*result));
	if (maximum_output == 0 || maximum_output > INT64_MAX)
		return EINVAL;
	int error = CheckImageFileSource(source, path);
	if (error != 0)
		return error;
	const int flags = fcntl(source->fd, F_GETFL);
	if (flags < 0)
		return errno;
	const bool readable = (flags & O_ACCMODE) != O_WRONLY;
	const bool ordinary_io = (flags & (O_DIRECT | O_PATH)) == 0;
	if (!readable || !ordinary_io || source->info.st_size < 0)
		return EINVAL;
	completed.compression_type = GetImageCompressionType(path);
	const bool supported = completed.compression_type == BLED_COMPRESSION_GZIP
		|| completed.compression_type == BLED_COMPRESSION_XZ || completed.compression_type == BLED_COMPRESSION_BZIP2
		|| completed.compression_type == BLED_COMPRESSION_LZMA || completed.compression_type == BLED_COMPRESSION_LZW
		|| completed.compression_type == BLED_COMPRESSION_ZIP
		|| completed.compression_type == BLED_COMPRESSION_ZSTD
		|| completed.compression_type == BLED_COMPRESSION_VTSI
		|| completed.compression_type == IMG_COMPRESSION_FFU
		|| completed.compression_type == IMG_COMPRESSION_VHD || completed.compression_type == IMG_COMPRESSION_VHDX;
	if (!supported)
		return ENOTSUP;
	completed.source_bytes = (uint64_t)source->info.st_size;
	if (completed.compression_type == BLED_COMPRESSION_ZIP)
		error = ReadZipImageSource(source, maximum_output, consumer, context, &completed);
	else if (completed.compression_type == BLED_COMPRESSION_XZ)
		error = ReadXzImageSource(source, maximum_output, consumer, context, &completed);
	else if (completed.compression_type == BLED_COMPRESSION_BZIP2)
		error = ReadBzip2ImageSource(source, maximum_output, consumer, context, &completed);
	else if (completed.compression_type == BLED_COMPRESSION_LZMA)
		error = ReadLzmaImageSource(source, maximum_output, consumer, context, &completed);
	else if (completed.compression_type == BLED_COMPRESSION_LZW)
		error = ReadLzwImageSource(source, maximum_output, consumer, context, &completed);
	else if (completed.compression_type == BLED_COMPRESSION_ZSTD)
		error = ReadZstdImageSource(source, maximum_output, consumer, context, &completed);
	else if (completed.compression_type == BLED_COMPRESSION_VTSI)
		error = ReadVtsiImageSource(source, maximum_output, consumer, context, &completed);
	else if (completed.compression_type == IMG_COMPRESSION_FFU)
		error = ReadFfuImageSource(source, maximum_output, consumer, context, &completed);
	else if (completed.compression_type == IMG_COMPRESSION_VHD)
		error = ReadVhdImageSource(source, maximum_output, consumer, context, &completed);
	else if (completed.compression_type == IMG_COMPRESSION_VHDX)
		error = ReadVhdxImageSource(source, maximum_output, consumer, context, &completed);
	else
		error = ReadGzipImageSource(source, maximum_output, consumer, context, &completed);
	if (error == 0)
		error = CheckImageFileSource(source, path);
	if (IsRufusJobCancelled())
		error = ECANCELED;
	if (error != 0)
		return error;
	completed.has_boot_marker = GetCompressedBootMarker(completed.prefix, completed.prefix_size, false) == 1;
	*result = completed;
	return 0;
}
