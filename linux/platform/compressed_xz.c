/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "compressed_xz.h"
#include "hash_native.h"
#include "rufus_reporting.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zlib.h>

/* Use the original XZ Embedded algorithms, dictionary limit and host BCJ set.
 * Supply native allocation/CRC facilities without BusyBox's global state,
 * Windows platform definitions or nonlocal error exits. */
#define BB_PLATFORM_H 1
#define NOINLINE __attribute__((__noinline__))
#define XZ_EXTERN static
#define XZ_INTERNAL_CRC32 0
#define XZ_DEC_DYNALLOC
#include "bled/xz_config.h"
#include "bled/xz_stream.h"

enum { XZ_IO_BYTES = 64 * 1024, XZ_DICTIONARY_BYTES = 64 * 1024 * 1024 };

#undef kmalloc
#undef vmalloc
#define kmalloc(size, flags) calloc(1, size)
#define vmalloc(size) calloc(1, size)

#ifndef RUFUS_XZ_LEGACY_CHECKS
typedef struct {
	HASH_CONTEXT* sha256;
	const uint64_t* crc_table;
	uint64_t crc64;
	size_t position;
	uint8_t expected[SHA256_HASHSIZE];
	bool initialized, finalized;
} NATIVE_XZ_CHECK;

/* XZ's reflected CRC64/ECMA-182, with inverted initial and final values. */
static void xz_check_update(NATIVE_XZ_CHECK* check, enum xz_check type,
	const uint8_t* bytes, size_t size)
{
	if (!check->initialized) {
		check->initialized = true;
		check->crc64 = UINT64_MAX;
		if (type == XZ_CHECK_SHA256)
			hash_init[HASH_SHA256](check->sha256);
	}
	if (type == XZ_CHECK_CRC64) {
		for (size_t i = 0; i < size; i++)
			check->crc64 = check->crc_table[(check->crc64 ^ bytes[i]) & 0xff] ^ (check->crc64 >> 8);
	} else if (type == XZ_CHECK_SHA256 && size != 0)
		hash_write[HASH_SHA256](check->sha256, bytes, size);
}

static enum xz_ret xz_check_validate(NATIVE_XZ_CHECK* check, enum xz_check type, struct xz_buf* buffer)
{
	if (type != XZ_CHECK_CRC64 && type != XZ_CHECK_SHA256)
		return XZ_OPTIONS_ERROR;
	if (!check->finalized) {
		xz_check_update(check, type, NULL, 0);
		if (type == XZ_CHECK_SHA256) {
			hash_final[HASH_SHA256](check->sha256);
			memcpy(check->expected, check->sha256->buf, SHA256_HASHSIZE);
		} else {
			const uint64_t crc = ~check->crc64;
			for (unsigned i = 0; i < 8; i++)
				check->expected[i] = (uint8_t)(crc >> (8 * i));
		}
		check->finalized = true;
	}
	const size_t size = type == XZ_CHECK_CRC64 ? 8 : SHA256_HASHSIZE;
	while (check->position < size) {
		if (buffer->in_pos == buffer->in_size)
			return XZ_OK;
		if (buffer->in[buffer->in_pos++] != check->expected[check->position++])
			return XZ_DATA_ERROR;
	}
	check->initialized = check->finalized = false;
	check->position = 0;
	return XZ_STREAM_END;
}
#define XZ_DEC_EXTENDED_CHECK NATIVE_XZ_CHECK
#endif

static uint32_t xz_crc32(const uint8_t* bytes, size_t size, uint32_t crc);

/* Keep upstream switch fallthroughs without restyling the shared algorithms. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#include "bled/xz_dec_bcj.c"
#include "bled/xz_dec_lzma2.c"
#include "bled/xz_dec_stream.c"
#pragma GCC diagnostic pop

_Static_assert(sizeof(struct xz_dec) + sizeof(struct xz_dec_lzma2) < 64 * 1024,
	"XZ decoder states must stay within the qualified memory bound");

static uint32_t xz_crc32(const uint8_t* bytes, size_t size, uint32_t crc)
{
	return (uint32_t)crc32_z(crc, bytes, size);
}

static int XzError(enum xz_ret status)
{
	if (status == XZ_MEM_ERROR)
		return ENOMEM;
	if (status == XZ_MEMLIMIT_ERROR)
		return EFBIG;
	if (status == XZ_OPTIONS_ERROR || status == XZ_UNSUPPORTED_CHECK)
		return ENOTSUP;
	return status == XZ_FORMAT_ERROR || status == XZ_DATA_ERROR || status == XZ_BUF_ERROR ? EBADMSG : EIO;
}

static int ReadXzSlice(const IMAGE_FILE_SOURCE* source, uint64_t offset, uint64_t size,
	uint64_t maximum_output, compressed_data_t consumer, void* context,
	COMPRESSED_IMAGE_RESULT* completed, uint32_t* zip_crc)
{
	uint8_t input[XZ_IO_BYTES], output[XZ_IO_BYTES];
	uint64_t read_offset = offset, consumed = offset;
	const uint64_t end_offset = offset + size;
	int error = InitializeNativeHashes();
	if (error != 0)
		return error;
	struct xz_dec* decoder = xz_dec_init(XZ_DYNALLOC, XZ_DICTIONARY_BYTES);
	if (decoder == NULL)
		return ENOMEM;
#ifndef RUFUS_XZ_LEGACY_CHECKS
	HASH_CONTEXT sha256;
	uint64_t crc_table[256];
	for (unsigned i = 0; i < 256; i++) {
		uint64_t crc = i;
		for (unsigned bit = 0; bit < 8; bit++)
			crc = (crc >> 1) ^ ((crc & 1) ? UINT64_C(0xc96c5795d7870f42) : 0);
		crc_table[i] = crc;
	}
	decoder->extended_check.sha256 = &sha256;
	decoder->extended_check.crc_table = crc_table;
#endif
	struct xz_buf buffer = {.in = input, .out = output, .out_size = sizeof(output)};
	for (;;) {
		if (IsRufusJobCancelled()) {
			error = ECANCELED;
			break;
		}
		if (buffer.in_pos == buffer.in_size && read_offset < end_offset) {
			const uint64_t remaining = end_offset - read_offset;
			const size_t requested = remaining < sizeof(input) ? (size_t)remaining : sizeof(input);
			const ssize_t count = pread(source->fd, input, requested, (off_t)read_offset);
			if (count < 0 && errno == EINTR)
				continue;
			if (count <= 0) {
				error = count < 0 ? errno : EIO;
				break;
			}
			read_offset += (uint64_t)count;
			buffer.in_pos = 0;
			buffer.in_size = (size_t)count;
		}
		if (IsRufusJobCancelled()) {
			error = ECANCELED;
			break;
		}
		const size_t before = buffer.in_pos;
		buffer.out_pos = 0;
		const enum xz_ret status = xz_dec_run(decoder, &buffer);
		consumed += buffer.in_pos - before;
		if (IsRufusJobCancelled()) {
			error = ECANCELED;
			break;
		}
		if (buffer.out_pos > maximum_output - completed->decoded_bytes) {
			error = EFBIG;
			break;
		}
		if (buffer.out_pos != 0) {
			if (consumer != NULL)
				error = consumer(context, output, buffer.out_pos, completed->decoded_bytes);
			if (error != 0) {
				if (error < 0)
					error = EIO;
				break;
			}
			if (zip_crc != NULL)
				*zip_crc = (uint32_t)crc32(*zip_crc, output, (uInt)buffer.out_pos);
			const size_t remaining = sizeof(completed->prefix) - completed->prefix_size;
			const size_t copy = buffer.out_pos < remaining ? buffer.out_pos : remaining;
			memcpy(completed->prefix + completed->prefix_size, output, copy);
			completed->prefix_size += copy;
			completed->decoded_bytes += buffer.out_pos;
		}
		if (status == XZ_STREAM_END) {
			/* Windows' unpack_xz_stream stops at the first stream, including
			 * when followed by padding, another stream or unrelated data. */
			completed->members = 1;
			completed->trailing_bytes = completed->source_bytes - consumed;
			break;
		}
		if (status == XZ_UNSUPPORTED_CHECK) {
#ifndef RUFUS_XZ_LEGACY_CHECKS
			const bool supported = decoder->check_type == XZ_CHECK_CRC64 || decoder->check_type == XZ_CHECK_SHA256;
			if (!supported) {
				error = ENOTSUP;
				break;
			}
#endif
			continue;
		}
		if (status != XZ_OK) {
			error = XzError(status);
			break;
		}
	}
	xz_dec_end(decoder);
	return error;
}

int ReadXzImageSource(const IMAGE_FILE_SOURCE* source, uint64_t maximum_output,
	compressed_data_t consumer, void* context, COMPRESSED_IMAGE_RESULT* completed)
{
	return ReadXzSlice(source, 0, completed->source_bytes, maximum_output,
		consumer, context, completed, NULL);
}

int ReadZipXzSource(const IMAGE_FILE_SOURCE* source, uint64_t offset, uint64_t size,
	uint64_t maximum_output, compressed_data_t consumer, void* context,
	COMPRESSED_IMAGE_RESULT* completed, uint32_t* crc)
{
	const uint64_t source_bytes = (uint64_t)source->info.st_size;
	if (offset > source_bytes || size > source_bytes - offset || crc == NULL)
		return EINVAL;
	*crc = (uint32_t)crc32(0, NULL, 0);
	return ReadXzSlice(source, offset, size, maximum_output, consumer, context, completed, crc);
}
