/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "compressed_bzip2.h"
#include "rufus_reporting.h"

#include <errno.h>
#include <limits.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zlib.h>

enum { BZIP_IO_BYTES = 64 * 1024, BZIP_BLOCK_BYTES = 100000, BZIP_MAX_LEVEL = 9 };

static int ReadBunzipInput(void* context, uint8_t* buffer, size_t capacity);

/* Reuse Rufus's original bit/Huffman/BWT/output algorithms. The optional field
 * supplies per-operation pread state; it is absent in the Windows build. */
#define BB_BUFSIZE BZIP_IO_BYTES
#define RUFUS_BUNZIP_IO_STATE void* native_io;
#define RUFUS_BUNZIP_READ(bd, buffer, size) ReadBunzipInput((bd)->native_io, buffer, size)
#include "bled/bunzip_core.inc"

typedef struct {
	bunzip_data decoder;
	jmp_buf jump;
	const IMAGE_FILE_SOURCE* source;
	uint64_t read_offset, end_offset;
	uint32_t* zip_crc;
	int error;
	uint8_t input[BZIP_IO_BYTES], output[BZIP_IO_BYTES];
} BZIP_READER;

/* Fill in batches, tolerating short reads and EINTR without moving the caller's
 * offset. EOF before the retained size is I/O failure, not a decoder trailer. */
static int ReadBunzipInput(void* context, uint8_t* buffer, size_t capacity)
{
	BZIP_READER* reader = context;
	size_t available = 0;
	while (available < capacity && reader->read_offset < reader->end_offset) {
		if (IsRufusJobCancelled()) {
			reader->error = ECANCELED;
			return -1;
		}
		const uint64_t remaining = reader->end_offset - reader->read_offset;
		const size_t space = capacity - available;
		const size_t requested = remaining < space ? (size_t)remaining : space;
		const ssize_t count = pread(reader->source->fd, buffer + available,
			requested, (off_t)reader->read_offset);
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0) {
			reader->error = count < 0 ? errno : EIO;
			return -1;
		}
		reader->read_offset += (uint64_t)count;
		available += (size_t)count;
	}
	return (int)available;
}

static void InitializeBunzipCrc(bunzip_data* decoder)
{
	/* BZIP2's big-endian CRC32, kept in each decoder as on Windows. */
	for (unsigned i = 0; i < 256; i++) {
		uint32_t crc = (uint32_t)i << 24;
		for (unsigned bit = 0; bit < 8; bit++)
			crc = (crc << 1) ^ ((crc & UINT32_C(0x80000000)) ? UINT32_C(0x04c11db7) : 0);
		decoder->crc32Table[i] = crc;
	}
}

static int InitializeBunzip(BZIP_READER* reader, int available)
{
	bunzip_data* decoder = &reader->decoder;
	memset(decoder, 0, sizeof(*decoder));
	decoder->inbuf = reader->input;
	decoder->inbufCount = available;
	decoder->jmpbuf = &reader->jump;
	decoder->native_io = reader;
	InitializeBunzipCrc(decoder);
	const unsigned header = get_bits(decoder, 32);
	const unsigned level = header - UINT32_C(0x425a6830);
	if (level < 1 || level > BZIP_MAX_LEVEL)
		return EBADMSG;
	decoder->dbufSize = BZIP_BLOCK_BYTES * level;
	decoder->dbuf = calloc(1, decoder->dbufSize * sizeof(*decoder->dbuf));
	return decoder->dbuf == NULL ? ENOMEM : 0;
}

static int DecodeBzip2(BZIP_READER* reader, uint64_t maximum,
	compressed_data_t consumer, void* context, COMPRESSED_IMAGE_RESULT* completed)
{
	/* Every allocation is owned by reader, outside the jump boundary. No changed
	 * automatic local is inspected after longjmp; the caller always frees state. */
	if (setjmp(reader->jump) != 0)
		return reader->error != 0 ? reader->error : EBADMSG;
	int available = 0;
	for (;;) {
		if (IsRufusJobCancelled())
			return ECANCELED;
		int error = InitializeBunzip(reader, available);
		if (error != 0)
			return error;
		bunzip_data* decoder = &reader->decoder;
		for (;;) {
			if (IsRufusJobCancelled())
				return ECANCELED;
			const int unfilled = read_bunzip(decoder, (char*)reader->output, sizeof(reader->output));
			if (unfilled < 0)
				return unfilled == RETVAL_OBSOLETE_INPUT ? ENOTSUP : EBADMSG;
			if (IsRufusJobCancelled())
				return ECANCELED;
			const size_t produced = sizeof(reader->output) - (size_t)unfilled;
			if (produced > maximum - completed->decoded_bytes)
				return EFBIG;
			if (produced != 0) {
				if (consumer != NULL) {
					error = consumer(context, reader->output, produced, completed->decoded_bytes);
					if (error != 0)
						return error > 0 ? error : EIO;
				}
				if (reader->zip_crc != NULL)
					*reader->zip_crc = (uint32_t)crc32(*reader->zip_crc, reader->output, (uInt)produced);
				const size_t remaining = sizeof(completed->prefix) - completed->prefix_size;
				const size_t copy = produced < remaining ? produced : remaining;
				memcpy(completed->prefix + completed->prefix_size, reader->output, copy);
				completed->prefix_size += copy;
				completed->decoded_bytes += produced;
			}
			if (decoder->writeCount == RETVAL_LAST_BLOCK)
				break;
		}
		/* An empty stream has no data block. Its combined check still has to
		 * match; the original Windows wrapper can return success on that mismatch. */
		if (decoder->headerCRC != decoder->totalCRC)
			return EBADMSG;
		completed->members++;
		available = decoder->inbufCount - decoder->inbufPos;
		const uint64_t consumed = reader->read_offset - (uint64_t)available;
		memmove(reader->input, decoder->inbuf + decoder->inbufPos, (size_t)available);
		free(decoder->dbuf);
		decoder->dbuf = NULL;
		if (available < 2) {
			const int count = ReadBunzipInput(reader, reader->input + available,
				sizeof(reader->input) - (size_t)available);
			if (count < 0)
				return reader->error;
			available += count;
		}
		const bool next_member = available >= 2 && reader->input[0] == 'B' && reader->input[1] == 'Z';
		if (!next_member) {
			completed->trailing_bytes = completed->source_bytes - consumed;
			return 0;
		}
	}
}

int ReadBzip2ImageSource(const IMAGE_FILE_SOURCE* source, uint64_t maximum_output,
	compressed_data_t consumer, void* context, COMPRESSED_IMAGE_RESULT* completed)
{
	BZIP_READER* reader = calloc(1, sizeof(*reader));
	if (reader == NULL)
		return ENOMEM;
	reader->source = source;
	reader->end_offset = completed->source_bytes;
	const int error = DecodeBzip2(reader, maximum_output, consumer, context, completed);
	free(reader->decoder.dbuf);
	free(reader);
	return error;
}

int ReadZipBzip2Source(const IMAGE_FILE_SOURCE* source, uint64_t offset, uint64_t size,
	uint64_t maximum_output, compressed_data_t consumer, void* context,
	COMPRESSED_IMAGE_RESULT* completed, uint32_t* crc)
{
	const uint64_t source_bytes = (uint64_t)source->info.st_size;
	if (offset > source_bytes || size > source_bytes - offset || crc == NULL)
		return EINVAL;
	BZIP_READER* reader = calloc(1, sizeof(*reader));
	if (reader == NULL)
		return ENOMEM;
	reader->source = source;
	reader->read_offset = offset;
	reader->end_offset = offset + size;
	reader->zip_crc = crc;
	*crc = (uint32_t)crc32(0, NULL, 0);
	const int error = DecodeBzip2(reader, maximum_output, consumer, context, completed);
	free(reader->decoder.dbuf);
	free(reader);
	return error;
}
