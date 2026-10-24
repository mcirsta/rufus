/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "compressed_lzma.h"
#include "rufus_reporting.h"

#include <errno.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zlib.h>

enum {
	LZMA_IO_BYTES = 64 * 1024,
	LZMA_HEADER_BYTES = 13,
	ZIP_LZMA_HEADER_BYTES = 9,
	LZMA_DICTIONARY_MAX_BYTES = 1024 * 1024 * 1024,
	LZMA_PROBABILITY_MAX_BYTES = (1846 + (768 << 12)) * 2
};

typedef struct {
	jmp_buf jump;
	const IMAGE_FILE_SOURCE* source;
	uint64_t read_offset, end_offset, declared, maximum, zip_declared;
	uint32_t* zip_crc;
	bool zip_format;
	void *dictionary, *probabilities, *range;
	compressed_data_t consumer;
	void* context;
	COMPRESSED_IMAGE_RESULT* completed;
	int error;
} LZMA_READER;

static void AbortLzma(LZMA_READER* reader, int error)
{
	reader->error = error;
	longjmp(reader->jump, 1);
}

static void PollLzma(LZMA_READER* reader)
{
	if (IsRufusJobCancelled())
		AbortLzma(reader, ECANCELED);
}

static void CheckLzmaPosition(LZMA_READER* reader, uint64_t position)
{
	PollLzma(reader);
	/* A large supported dictionary must not postpone a small caller's output
	 * limit until that dictionary has been filled. Matches have at most 273 bytes. */
	if (position > reader->maximum)
		AbortLzma(reader, EFBIG);
}

static void* AllocateLzma(LZMA_READER* reader, void** owned, size_t bytes, size_t maximum)
{
	if (bytes > maximum)
		AbortLzma(reader, ENOMEM);
	*owned = calloc(1, bytes);
	if (*owned == NULL)
		AbortLzma(reader, ENOMEM);
	return *owned;
}

/* Retain ownership and the caller's file offset. Batch short reads and EINTR;
 * premature EOF relative to the retained stat is an I/O failure. */
static int ReadLzmaInput(LZMA_READER* reader, void* data, unsigned capacity)
{
	size_t available = 0;
	while (available < capacity && reader->read_offset < reader->end_offset) {
		PollLzma(reader);
		const uint64_t remaining = reader->end_offset - reader->read_offset;
		const size_t space = capacity - available;
		const size_t requested = remaining < space ? (size_t)remaining : space;
		const ssize_t count = pread(reader->source->fd, (uint8_t*)data + available,
			requested, (off_t)reader->read_offset);
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			AbortLzma(reader, count < 0 ? errno : EIO);
		reader->read_offset += (uint64_t)count;
		available += (size_t)count;
	}
	PollLzma(reader);
	return (int)available;
}

static int ReadLzmaHeader(LZMA_READER* reader, void* data, unsigned size)
{
	int count;
	if (reader->zip_format) {
		uint8_t prefix[ZIP_LZMA_HEADER_BYTES];
		count = ReadLzmaInput(reader, prefix, sizeof(prefix));
		if (count != ZIP_LZMA_HEADER_BYTES)
			return count;
		/* ZIP-LZMA carries two SDK version bytes, a property length and
		 * LZMA1 properties. The same five-byte property layout is usable
		 * across SDK versions; the ZIP flag chooses EOS or declared size. */
		const unsigned property_bytes = prefix[2] | (unsigned)prefix[3] << 8;
		if (property_bytes != 5)
			AbortLzma(reader, ENOTSUP);
		memcpy(data, prefix + 4, 5);
		for (unsigned i = 0; i < 8; i++)
			((uint8_t*)data)[5 + i] = (uint8_t)(reader->zip_declared >> (8 * i));
		count = LZMA_HEADER_BYTES;
	} else {
		count = ReadLzmaInput(reader, data, size);
		if (count != LZMA_HEADER_BYTES)
			return count;
	}
	const uint8_t* header = data;
	uint32_t dictionary = 0;
	reader->declared = 0;
	for (unsigned i = 0; i < 4; i++)
		dictionary |= (uint32_t)header[1 + i] << (8 * i);
	for (unsigned i = 0; i < 8; i++)
		reader->declared |= (uint64_t)header[5 + i] << (8 * i);
	if (header[0] < 225 && dictionary > LZMA_DICTIONARY_MAX_BYTES)
		AbortLzma(reader, ENOTSUP);
	return count;
}

static void CheckLzmaDistance(LZMA_READER* reader, uint32_t distance, uint64_t produced, uint32_t dictionary)
{
	/* Malformed distances must not read unwritten history, including the
	 * original matched-literal path that lacks the other path's buffer bound. */
	if (distance == 0 || distance > produced || distance > dictionary)
		AbortLzma(reader, EBADMSG);
}

typedef struct {
	LZMA_READER* src_fd;
	size_t mem_output_size_max;
} transformer_state_t;

static ssize_t transformer_write(transformer_state_t* state, const void* data, size_t size)
{
	LZMA_READER* reader = state->src_fd;
	COMPRESSED_IMAGE_RESULT* completed = reader->completed;
	const uint8_t* bytes = data;
	/* Reject the original known-size match overshoot before delivering it.
	 * There is no container checksum; do not infer one from the size field. */
	if (reader->declared != UINT64_MAX && size > reader->declared - completed->decoded_bytes)
		AbortLzma(reader, EBADMSG);
	size_t offset = 0;
	while (offset < size) {
		PollLzma(reader);
		const size_t remaining = size - offset;
		const size_t count = remaining < LZMA_IO_BYTES ? remaining : LZMA_IO_BYTES;
		if (count > reader->maximum - completed->decoded_bytes)
			AbortLzma(reader, EFBIG);
		if (reader->consumer != NULL) {
			const int error = reader->consumer(reader->context, bytes + offset, count, completed->decoded_bytes);
			if (error != 0)
				AbortLzma(reader, error > 0 ? error : EIO);
		}
		if (reader->zip_crc != NULL)
			*reader->zip_crc = (uint32_t)crc32(*reader->zip_crc, bytes + offset, (uInt)count);
		const size_t prefix_space = sizeof(completed->prefix) - completed->prefix_size;
		const size_t copy = count < prefix_space ? count : prefix_space;
		memcpy(completed->prefix + completed->prefix_size, bytes + offset, copy);
		completed->prefix_size += copy;
		completed->decoded_bytes += count;
		offset += count;
	}
	return (ssize_t)size;
}

/* Only I/O, ownership, cancellation and malformed-history guards are adapted.
 * The range coder, probability models and dictionary algorithm are shared with
 * Windows. All three allocations remain owned outside the C jump boundary. */
#define FAST_FUNC
#define ALWAYS_INLINE inline __attribute__((always_inline))
#define IF_DESKTOP(x) x
#define IF_NOT_DESKTOP(x)
#define IF_NOT_FEATURE_LZMA_FAST(x) x
#define BB_BUFSIZE LZMA_IO_BYTES
#define SECTOR_ALIGNMENT 4096
#define PRAGMA_BEGIN_PACKED
#define PRAGMA_END_PACKED
#define PACKED __attribute__((packed))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define SWAP_LE32(x) (x)
#define SWAP_LE64(x) (x)
#else
#define SWAP_LE32(x) __builtin_bswap32(x)
#define SWAP_LE64(x) __builtin_bswap64(x)
#endif
#define RUFUS_LZMA_INPUT_TYPE LZMA_READER*
#define RUFUS_LZMA_64BIT_POSITION 1
#define RUFUS_LZMA_POLL(state, position) CheckLzmaPosition((state)->src_fd, position)
#define RUFUS_LZMA_DISTANCE(state, distance, produced, dictionary) CheckLzmaDistance((state)->src_fd, distance, produced, dictionary)
#define full_read ReadLzmaHeader
#define safe_read ReadLzmaInput
#define aligned_xmalloc(bytes) AllocateLzma(xstate->src_fd, &xstate->src_fd->dictionary, bytes, LZMA_DICTIONARY_MAX_BYTES)
#define xmalloc(bytes) AllocateLzma(xstate->src_fd, &xstate->src_fd->probabilities, bytes, LZMA_PROBABILITY_MAX_BYTES)
#define xzalloc(bytes) AllocateLzma(fd, &fd->range, bytes, sizeof(rc_t) + LZMA_IO_BYTES)
#define aligned_free(pointer) ((void)(pointer))
#define free(pointer) ((void)(pointer))
#define bb_simple_error_msg(message) ((void)0)
#define bb_simple_error_msg_and_die(message) AbortLzma(rc->fd, EBADMSG)
#define unpack_lzma_stream DecodeOriginalLzma
#include "bled/unlzma_core.inc"
#undef free
#undef aligned_free

static int DecodeLzma(LZMA_READER* reader)
{
	if (setjmp(reader->jump) != 0)
		return reader->error;
	transformer_state_t state = {.src_fd = reader};
	const long long decoded = DecodeOriginalLzma(&state);
	if (decoded < 0)
		return EBADMSG;
	if (reader->declared != UINT64_MAX && reader->completed->decoded_bytes != reader->declared)
		return EBADMSG;
	const rc_t* range = reader->range;
	const uint64_t consumed = reader->read_offset - (uint64_t)(range->buffer_end - range->ptr);
	reader->completed->trailing_bytes = reader->completed->source_bytes - consumed;
	reader->completed->members = 1;
	return 0;
}

int ReadLzmaImageSource(const IMAGE_FILE_SOURCE* source, uint64_t maximum_output,
	compressed_data_t consumer, void* context, COMPRESSED_IMAGE_RESULT* completed)
{
	LZMA_READER* reader = calloc(1, sizeof(*reader));
	if (reader == NULL)
		return ENOMEM;
	reader->source = source;
	reader->end_offset = completed->source_bytes;
	reader->maximum = maximum_output;
	reader->consumer = consumer;
	reader->context = context;
	reader->completed = completed;
	const int error = DecodeLzma(reader);
	free(reader->range);
	free(reader->probabilities);
	free(reader->dictionary);
	free(reader);
	return error;
}

int ReadZipLzmaSource(const IMAGE_FILE_SOURCE* source, uint64_t offset, uint64_t size,
	uint64_t declared, bool has_eos, uint64_t maximum_output, compressed_data_t consumer, void* context,
	COMPRESSED_IMAGE_RESULT* completed, uint32_t* crc)
{
	const uint64_t source_bytes = (uint64_t)source->info.st_size;
	if (offset > source_bytes || size > source_bytes - offset || crc == NULL)
		return EINVAL;
	if (size < ZIP_LZMA_HEADER_BYTES)
		return EBADMSG;
	LZMA_READER* reader = calloc(1, sizeof(*reader));
	if (reader == NULL)
		return ENOMEM;
	reader->source = source;
	reader->read_offset = offset;
	reader->end_offset = offset + size;
	reader->zip_crc = crc;
	reader->zip_format = true;
	reader->zip_declared = has_eos ? UINT64_MAX : declared;
	reader->maximum = maximum_output;
	reader->consumer = consumer;
	reader->context = context;
	reader->completed = completed;
	*crc = (uint32_t)crc32(0, NULL, 0);
	const int error = DecodeLzma(reader);
	free(reader->range);
	free(reader->probabilities);
	free(reader->dictionary);
	free(reader);
	return error;
}
