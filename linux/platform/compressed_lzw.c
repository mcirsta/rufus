/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "compressed_lzw.h"
#include "rufus_reporting.h"

#include <errno.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { LZW_IO_BYTES = 64 * 1024 };

typedef struct {
	jmp_buf jump;
	const IMAGE_FILE_SOURCE* source;
	uint64_t read_offset, maximum;
	void *input, *output, *suffix, *prefix;
	compressed_data_t consumer;
	void* context;
	COMPRESSED_IMAGE_RESULT* completed;
	int error;
} LZW_READER;

static void AbortLzw(LZW_READER* reader, int error)
{
	reader->error = error;
	longjmp(reader->jump, 1);
}

static void PollLzw(LZW_READER* reader)
{
	if (IsRufusJobCancelled())
		AbortLzw(reader, ECANCELED);
}

static void* AllocateLzw(LZW_READER* reader, void** owned, size_t bytes)
{
	*owned = calloc(1, bytes);
	if (*owned == NULL)
		AbortLzw(reader, ENOMEM);
	return *owned;
}

/* Batch short reads and EINTR against the retained size. The original decoder
 * terminates at natural EOF; an earlier EOF is an I/O failure. */
static int ReadLzwInput(LZW_READER* reader, void* data, unsigned capacity)
{
	const uint64_t size = (uint64_t)reader->source->info.st_size;
	size_t available = 0;
	while (available < capacity && reader->read_offset < size) {
		PollLzw(reader);
		const uint64_t remaining = size - reader->read_offset;
		const size_t space = capacity - available;
		const size_t requested = remaining < space ? (size_t)remaining : space;
		const ssize_t count = pread(reader->source->fd, (uint8_t*)data + available,
			requested, (off_t)reader->read_offset);
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			AbortLzw(reader, count < 0 ? errno : EIO);
		reader->read_offset += (uint64_t)count;
		available += (size_t)count;
	}
	PollLzw(reader);
	return (int)available;
}

typedef struct {
	LZW_READER* src_fd;
	size_t mem_output_size_max;
} transformer_state_t;

static int check_signature16(transformer_state_t* state, unsigned magic)
{
	uint8_t bytes[2];
	return ReadLzwInput(state->src_fd, bytes, sizeof(bytes)) != sizeof(bytes)
		|| (unsigned)(bytes[0] + 256 * bytes[1]) != magic;
}

static ssize_t transformer_write(transformer_state_t* state, const void* data, size_t size)
{
	LZW_READER* reader = state->src_fd;
	COMPRESSED_IMAGE_RESULT* completed = reader->completed;
	PollLzw(reader);
	if (size > reader->maximum - completed->decoded_bytes)
		AbortLzw(reader, EFBIG);
	if (reader->consumer != NULL) {
		const int error = reader->consumer(reader->context, data, size, completed->decoded_bytes);
		if (error != 0)
			AbortLzw(reader, error > 0 ? error : EIO);
	}
	const size_t prefix_space = sizeof(completed->prefix) - completed->prefix_size;
	const size_t copy = size < prefix_space ? size : prefix_space;
	memcpy(completed->prefix + completed->prefix_size, data, copy);
	completed->prefix_size += copy;
	completed->decoded_bytes += size;
	return (ssize_t)size;
}

/* Keep all four allocations outside the jump boundary. A failing consumer
 * aborts here, including the original wrapper's unchecked final callback. */
#define FAST_FUNC
#define IF_DESKTOP(x) x
#define BB_BUFSIZE LZW_IO_BYTES
#define COMPRESS_MAGIC 0x9d1f
#define RUFUS_LZW_INPUT_ALLOC(state, bytes) AllocateLzw((state)->src_fd, &(state)->src_fd->input, bytes)
#define RUFUS_LZW_OUTPUT_ALLOC(state, bytes) AllocateLzw((state)->src_fd, &(state)->src_fd->output, bytes)
#define RUFUS_LZW_SUFFIX_ALLOC(state, bytes) AllocateLzw((state)->src_fd, &(state)->src_fd->suffix, bytes)
#define RUFUS_LZW_PREFIX_ALLOC(state, bytes) AllocateLzw((state)->src_fd, &(state)->src_fd->prefix, bytes)
#define RUFUS_LZW_POLL(state) PollLzw((state)->src_fd)
/* A saturated nine-bit dictionary must stay at nine bits. The original can
 * silently discard output by advancing to ten; current ncompress fixes both
 * initial and CLEAR resets this way. Other original width rules stay shared. */
#define RUFUS_LZW_INITIAL_MAXCODE(bits, limit) ((bits) == INIT_BITS ? (limit) : MAXCODE(INIT_BITS) - 1)
#define safe_read ReadLzwInput
#define full_read ReadLzwInput
#define aligned_free(pointer) ((void)(pointer))
#define free(pointer) ((void)(pointer))
/* The original invalid-code branch can return its preceding successful write
 * count. Every diagnostic here denotes malformed input; abort on the actual
 * error, rather than accepting that positive count as stream completion. */
#define bb_simple_error_msg(message) AbortLzw(xstate->src_fd, EBADMSG)
#define bb_error_msg(...) AbortLzw(xstate->src_fd, EBADMSG)
#define bb_simple_error_msg_and_die(message) AbortLzw(xstate->src_fd, EBADMSG)
#define unpack_Z_stream DecodeOriginalLzw
#include "bled/uncompress_core.inc"
#undef free
#undef aligned_free

static int DecodeLzw(LZW_READER* reader)
{
	if (setjmp(reader->jump) != 0)
		return reader->error;
	transformer_state_t state = {.src_fd = reader};
	const long long decoded = DecodeOriginalLzw(&state);
	if (decoded < 0)
		return EBADMSG;
	reader->completed->members = 1;
	/* Unix compress has no size, checksum or end marker. The last incomplete
 * code bits are ignored, as in the original. Every input byte is consumed. */
	reader->completed->trailing_bytes = 0;
	return 0;
}

int ReadLzwImageSource(const IMAGE_FILE_SOURCE* source, uint64_t maximum_output,
	compressed_data_t consumer, void* context, COMPRESSED_IMAGE_RESULT* completed)
{
	LZW_READER* reader = calloc(1, sizeof(*reader));
	if (reader == NULL)
		return ENOMEM;
	reader->source = source;
	reader->maximum = maximum_output;
	reader->consumer = consumer;
	reader->context = context;
	reader->completed = completed;
	const int error = DecodeLzw(reader);
	free(reader->prefix);
	free(reader->suffix);
	free(reader->output);
	free(reader->input);
	free(reader);
	return error;
}
