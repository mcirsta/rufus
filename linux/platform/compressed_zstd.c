/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "compressed_zstd.h"
#include "rufus_reporting.h"
#include "bled/zstd.h"
#include "bled/zstd_errors.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum {
	ZSTD_INPUT_MAX_BYTES = 128 * 1024 + 3,
	ZSTD_OUTPUT_BYTES = 64 * 1024,
	ZSTD_WINDOW_LOG_MAX = 27
};

typedef struct {
	const IMAGE_FILE_SOURCE* source;
	uint64_t read_offset;
	size_t input_capacity;
	uint8_t* input;
} ZSTD_READER;

static int ZstdError(size_t status)
{
	const ZSTD_ErrorCode code = ZSTD_getErrorCode(status);
	if (code == ZSTD_error_memory_allocation)
		return ENOMEM;
	if (code == ZSTD_error_frameParameter_windowTooLarge)
		return EFBIG;
	if (code == ZSTD_error_version_unsupported || code == ZSTD_error_frameParameter_unsupported
		|| code == ZSTD_error_dictionary_wrong)
		return ENOTSUP;
	return EBADMSG;
}

/* Keep the caller's descriptor position unchanged. A short read is ordinary
 * streaming input; an EOF before the retained stat is a source I/O failure. */
static int FillZstdInput(ZSTD_READER* reader, ZSTD_inBuffer* input)
{
	const size_t pending = input->size - input->pos;
	if (pending != 0)
		memmove(reader->input, reader->input + input->pos, pending);
	input->src = reader->input;
	input->pos = 0;
	input->size = pending;
	const uint64_t source_bytes = (uint64_t)reader->source->info.st_size;
	if (reader->read_offset == source_bytes)
		return 0;
	if (pending == reader->input_capacity)
		return EBADMSG;
	const size_t space = reader->input_capacity - pending;
	const uint64_t remaining = source_bytes - reader->read_offset;
	const size_t requested = remaining < space ? (size_t)remaining : space;
	for (;;) {
		if (IsRufusJobCancelled())
			return ECANCELED;
		const ssize_t count = pread(reader->source->fd, reader->input + pending,
			requested, (off_t)reader->read_offset);
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			return count < 0 ? errno : EIO;
		reader->read_offset += (uint64_t)count;
		input->size += (size_t)count;
		return 0;
	}
}

static int DecodeZstd(ZSTD_READER* reader, ZSTD_DStream* decoder, uint64_t maximum,
	compressed_data_t consumer, void* context, COMPRESSED_IMAGE_RESULT* completed)
{
	uint8_t output[ZSTD_OUTPUT_BYTES];
	ZSTD_inBuffer input = {.src = reader->input};
	int error = FillZstdInput(reader, &input);
	if (error != 0)
		return error;
	for (;;) {
		if (IsRufusJobCancelled())
			return ECANCELED;
		ZSTD_outBuffer decoded = {.dst = output, .size = sizeof(output)};
		const size_t before = input.pos;
		const size_t status = ZSTD_decompressStream(decoder, &decoded, &input);
		if (ZSTD_isError(status))
			return ZstdError(status);
		if (IsRufusJobCancelled())
			return ECANCELED;
		if (decoded.pos > maximum - completed->decoded_bytes)
			return EFBIG;
		if (decoded.pos != 0) {
			if (consumer != NULL) {
				error = consumer(context, output, decoded.pos, completed->decoded_bytes);
				if (error != 0)
					return error > 0 ? error : EIO;
			}
			const size_t prefix_space = sizeof(completed->prefix) - completed->prefix_size;
			const size_t copy = decoded.pos < prefix_space ? decoded.pos : prefix_space;
			memcpy(completed->prefix + completed->prefix_size, output, copy);
			completed->prefix_size += copy;
			completed->decoded_bytes += decoded.pos;
		}
		if (status == 0) {
			completed->members++;
			if (input.pos == input.size && reader->read_offset == completed->source_bytes)
				return 0;
		}
		const bool no_progress = input.pos == before && decoded.pos == 0;
		const bool input_exhausted = input.pos == input.size;
		if (no_progress || input_exhausted) {
			if (reader->read_offset < completed->source_bytes) {
				error = FillZstdInput(reader, &input);
				if (error != 0)
					return error;
			} else if (no_progress) {
				return EBADMSG;
			}
		}
	}
}

int ReadZstdImageSource(const IMAGE_FILE_SOURCE* source, uint64_t maximum_output,
	compressed_data_t consumer, void* context, COMPRESSED_IMAGE_RESULT* completed)
{
	if (completed->source_bytes == 0)
		return EBADMSG;
	const size_t input_capacity = ZSTD_DStreamInSize();
	if (input_capacity == 0 || input_capacity > ZSTD_INPUT_MAX_BYTES)
		return ENOTSUP;
	ZSTD_READER reader = {.source = source, .input_capacity = input_capacity};
	reader.input = malloc(input_capacity);
	if (reader.input == NULL)
		return ENOMEM;
	ZSTD_DStream* decoder = ZSTD_createDStream();
	if (decoder == NULL) {
		free(reader.input);
		return ENOMEM;
	}
	const size_t initialized = ZSTD_initDStream(decoder);
	const size_t bounded = ZSTD_isError(initialized) ? initialized
		: ZSTD_DCtx_setParameter(decoder, ZSTD_d_windowLogMax, ZSTD_WINDOW_LOG_MAX);
	const int error = ZSTD_isError(bounded) ? ZstdError(bounded)
		: DecodeZstd(&reader, decoder, maximum_output, consumer, context, completed);
	ZSTD_freeDStream(decoder);
	free(reader.input);
	return error;
}
