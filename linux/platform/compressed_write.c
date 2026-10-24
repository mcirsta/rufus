/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "compressed_write.h"
#include "compressed_vtsi.h"
#include "compressed_ffu.h"
#include "hash_native.h"
#include "target_io.h"
#include "rufus_reporting.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

enum {
	COMPRESSED_TRANSFER_BYTES = 64 * 1024
};

typedef struct {
	HASH_CONTEXT context;
	uint64_t bytes;
} DECODED_HASH;

typedef struct {
	DECODED_HASH hash;
	const COMPRESSED_WRITE_PLAN* plan;
	RAW_WRITE_RESULT* result;
	int target;
	bool verify;
	size_t buffered;
	uint8_t expected[COMPRESSED_TRANSFER_BYTES], actual[COMPRESSED_TRANSFER_BYTES];
} COMPRESSED_TRANSFER;

typedef struct {
	DECODED_HASH hash;
	HASH_CONTEXT layout_hash;
	const IMAGE_FILE_SOURCE* source;
	const COMPRESSED_WRITE_PLAN* plan;
	RAW_WRITE_RESULT* result;
	int target;
	bool verify;
	uint64_t transferred;
	uint8_t expected[COMPRESSED_TRANSFER_BYTES], actual[COMPRESSED_TRANSFER_BYTES];
} VTSI_TRANSFER;

typedef struct {
	HASH_CONTEXT hash;
	unsigned sector_size;
} VTSI_LAYOUT_HASH;

static int HashDecodedBytes(void* context, const uint8_t* bytes, size_t size, uint64_t offset)
{
	DECODED_HASH* hash = context;
	const bool ordered_chunk = offset == hash->bytes && size <= COMPRESSED_TRANSFER_BYTES;
	if (!ordered_chunk)
		return EIO;
	hash_write[HASH_SHA256](&hash->context, bytes, size);
	hash->bytes += size;
	return 0;
}

static void HashVtsiLayoutValue(HASH_CONTEXT* hash, uint64_t value)
{
	uint8_t bytes[8];
	for (unsigned i = 0; i < sizeof(bytes); i++)
		bytes[i] = (uint8_t)(value >> (8 * i));
	hash_write[HASH_SHA256](hash, bytes, sizeof(bytes));
}

static void HashVtsiSegmentMap(HASH_CONTEXT* hash, uint64_t disk_offset,
	uint64_t source_offset, uint64_t bytes)
{
	HashVtsiLayoutValue(hash, disk_offset);
	HashVtsiLayoutValue(hash, source_offset);
	HashVtsiLayoutValue(hash, bytes);
}

static int FreezeVtsiSegment(void* context, uint64_t disk_offset, uint64_t source_offset,
	uint64_t bytes)
{
	VTSI_LAYOUT_HASH* layout = context;
	if (disk_offset % layout->sector_size != 0 || bytes % layout->sector_size != 0)
		return ENOTSUP;
	HashVtsiSegmentMap(&layout->hash, disk_offset, source_offset, bytes);
	return 0;
}

int PrepareCompressedWrite(const IMAGE_FILE_SOURCE* source, const char* source_path,
	uint64_t maximum_output, unsigned sector_size, bool ignore_boot_marker, COMPRESSED_WRITE_PLAN* plan)
{
	COMPRESSED_WRITE_PLAN completed = {0};
	DECODED_HASH hash = {0};
	if (plan == NULL)
		return EINVAL;
	memset(plan, 0, sizeof(*plan));
	int error = CompressedImageWriteSize(maximum_output, sector_size, &completed.target_bytes);
	/* The output bound need not itself fit after rounding: the actual stream may
	 * be much smaller. Validate sector rules here and actual geometry after decode. */
	if (error != 0 && error != EOVERFLOW)
		return error;
	if (maximum_output > INT64_MAX)
		return EINVAL;
	error = CheckImageFileSource(source, source_path);
	if (error != 0)
		return error;
	const int compression = GetImageCompressionType(source_path);
	const bool supported = compression == BLED_COMPRESSION_GZIP || compression == BLED_COMPRESSION_XZ
		|| compression == BLED_COMPRESSION_BZIP2 || compression == BLED_COMPRESSION_LZMA
		|| compression == BLED_COMPRESSION_LZW || compression == BLED_COMPRESSION_ZIP
		|| compression == BLED_COMPRESSION_ZSTD || compression == BLED_COMPRESSION_VTSI
		|| compression == IMG_COMPRESSION_FFU
		|| compression == IMG_COMPRESSION_VHD || compression == IMG_COMPRESSION_VHDX;
	if (!supported)
		return ENOTSUP;
	error = InitializeNativeHashes();
	if (error != 0)
		return error;
	hash_init[HASH_SHA256](&hash.context);
	error = ReadCompressedImageSource(source, source_path, maximum_output, HashDecodedBytes, &hash, &completed.image);
	if (error != 0)
		return error;
	const int marker = GetCompressedBootMarker(completed.image.prefix, completed.image.prefix_size, ignore_boot_marker);
	if (marker == 0 && compression != IMG_COMPRESSION_FFU)
		return ENOTSUP;
	const uint64_t required = completed.image.ffu_minimum_bytes != 0
		? completed.image.ffu_minimum_bytes : completed.image.decoded_bytes;
	error = CompressedImageWriteSize(required, sector_size, &completed.target_bytes);
	if (error != 0)
		return error;
	if (compression == BLED_COMPRESSION_VTSI) {
		uint64_t disk_size = 0, data_bytes = 0;
		VTSI_LAYOUT_HASH layout = {.sector_size = sector_size};
		hash_init[HASH_SHA256](&layout.hash);
		error = VisitVtsiImageSegments(source, source_path, maximum_output,
			FreezeVtsiSegment, &layout, &disk_size, &data_bytes);
		if (error != 0)
			return error;
		if (disk_size != completed.image.decoded_bytes || data_bytes == 0)
			return ENOTSUP;
		HashVtsiLayoutValue(&layout.hash, disk_size);
		HashVtsiLayoutValue(&layout.hash, data_bytes);
		hash_final[HASH_SHA256](&layout.hash);
		memcpy(completed.layout_sha256, layout.hash.buf, sizeof(completed.layout_sha256));
	}
	if (compression == IMG_COMPRESSION_FFU)
		memcpy(completed.layout_sha256, completed.image.layout_sha256, sizeof(completed.layout_sha256));
	hash_final[HASH_SHA256](&hash.context);
	memcpy(completed.decoded_sha256, hash.context.buf, sizeof(completed.decoded_sha256));
	completed.source_info = source->info;
	completed.sector_size = sector_size;
	completed.ignore_boot_marker = ignore_boot_marker;
	error = CheckImageFileSource(source, source_path);
	if (error != 0)
		return error;
	if (IsRufusJobCancelled())
		return ECANCELED;
	*plan = completed;
	return 0;
}

static int CheckFrozenSource(const IMAGE_FILE_SOURCE* source, const char* path, const COMPRESSED_WRITE_PLAN* plan)
{
	int error = CheckImageFileSource(source, path);
	if (error != 0)
		return error;
	IMAGE_FILE_SOURCE frozen = *source;
	frozen.info = plan->source_info;
	return CheckImageFileSource(&frozen, path);
}

static bool SameCompressedImage(const COMPRESSED_IMAGE_RESULT* expected, const COMPRESSED_IMAGE_RESULT* actual)
{
	const bool same_sizes = expected->source_bytes == actual->source_bytes
		&& expected->decoded_bytes == actual->decoded_bytes && expected->prefix_size == actual->prefix_size
		&& expected->ffu_minimum_bytes == actual->ffu_minimum_bytes;
	const bool same_stream = expected->compression_type == actual->compression_type
		&& expected->members == actual->members && expected->trailing_bytes == actual->trailing_bytes
		&& expected->ffu_store == actual->ffu_store && expected->ffu_stores == actual->ffu_stores;
	if (!same_sizes || !same_stream || expected->has_boot_marker != actual->has_boot_marker)
		return false;
	return memcmp(expected->prefix, actual->prefix, expected->prefix_size) == 0;
}

static int CheckPlan(const COMPRESSED_WRITE_PLAN* plan)
{
	if (plan == NULL)
		return EINVAL;
	const COMPRESSED_IMAGE_RESULT* image = &plan->image;
	const bool supported = image->compression_type == BLED_COMPRESSION_GZIP
		|| image->compression_type == BLED_COMPRESSION_XZ || image->compression_type == BLED_COMPRESSION_BZIP2
		|| image->compression_type == BLED_COMPRESSION_LZMA || image->compression_type == BLED_COMPRESSION_LZW
		|| image->compression_type == BLED_COMPRESSION_ZIP
		|| image->compression_type == BLED_COMPRESSION_ZSTD
		|| image->compression_type == BLED_COMPRESSION_VTSI
		|| image->compression_type == IMG_COMPRESSION_FFU
		|| image->compression_type == IMG_COMPRESSION_VHD || image->compression_type == IMG_COMPRESSION_VHDX;
	const bool full_prefix = image->prefix_size == sizeof(image->prefix) && image->decoded_bytes >= sizeof(image->prefix);
	if (!supported || !full_prefix || image->members == 0)
		return EINVAL;
	const bool source_size_matches = plan->source_info.st_size > 0 && image->source_bytes == (uint64_t)plan->source_info.st_size;
	if (!source_size_matches || image->trailing_bytes > image->source_bytes)
		return EINVAL;
	const bool marker = GetCompressedBootMarker(image->prefix, image->prefix_size, false) == 1;
	if (marker != image->has_boot_marker)
		return EINVAL;
	if (image->compression_type != IMG_COMPRESSION_FFU
		&& GetCompressedBootMarker(image->prefix, image->prefix_size, plan->ignore_boot_marker) == 0)
		return ENOTSUP;
	uint64_t size;
	if (image->ffu_minimum_bytes != 0 && (image->compression_type != IMG_COMPRESSION_FFU
		|| image->ffu_minimum_bytes > image->decoded_bytes)) return EINVAL;
	const uint64_t required = image->ffu_minimum_bytes != 0 ? image->ffu_minimum_bytes : image->decoded_bytes;
	const int error = CompressedImageWriteSize(required, plan->sector_size, &size);
	if (error != 0)
		return error;
	return size == plan->target_bytes ? 0 : EINVAL;
}

static int InspectCompressedTarget(const IMAGE_FILE_SOURCE* source, int fd, const COMPRESSED_WRITE_PLAN* plan,
	uint64_t device, uint64_t capacity, WRITABLE_TARGET* target)
{
	int error = InspectWritableTarget(fd, device, target);
	if (error != 0)
		return error;
	if (device != 0) {
		if ((uint64_t)source->info.st_dev == device)
			return EINVAL;
		if (target->size != capacity || target->sector_size != plan->sector_size)
			return ESTALE;
		if (capacity % plan->sector_size != 0)
			return EINVAL;
	} else {
		const bool same_file = target->info.st_dev == source->info.st_dev && target->info.st_ino == source->info.st_ino;
		if (same_file)
			return EINVAL;
	}
	return plan->target_bytes <= target->size ? 0 : ENOSPC;
}

static int CheckCompressedTarget(const IMAGE_FILE_SOURCE* source, int fd, const COMPRESSED_WRITE_PLAN* plan,
	uint64_t device, uint64_t capacity, const WRITABLE_TARGET* initial)
{
	WRITABLE_TARGET current;
	const int error = InspectCompressedTarget(source, fd, plan, device, capacity, &current);
	if (error != 0)
		return error == ENOSPC ? ESTALE : error;
	const bool same_file = current.info.st_dev == initial->info.st_dev && current.info.st_ino == initial->info.st_ino;
	return same_file && current.size == initial->size ? 0 : ESTALE;
}

/* Match the raw writer's cancellable positioned I/O and partial-work counters.
 * Every initial transfer is sector aligned; short system calls resume exactly. */
static int TransferCompressedBlock(COMPRESSED_TRANSFER* transfer, size_t size)
{
	RAW_WRITE_RESULT* result = transfer->result;
	size_t position = 0;
	const uint64_t offset = transfer->verify ? result->bytes_verified : result->bytes_written;
	while (position < size) {
		if (IsRufusJobCancelled())
			return ECANCELED;
		ssize_t count;
		if (transfer->verify)
			count = pread(transfer->target, transfer->actual + position, size - position, (off_t)(offset + position));
		else
			count = pwrite(transfer->target, transfer->expected + position, size - position, (off_t)(offset + position));
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			return count == 0 ? EIO : errno;
		position += (size_t)count;
		if (!transfer->verify) {
			result->bytes_written += (uint64_t)count;
			_UpdateProgressWithInfo(RAW_WRITE_COPY, 0, result->bytes_written, transfer->plan->target_bytes, 0);
		}
	}
	if (transfer->verify) {
		if (memcmp(transfer->expected, transfer->actual, size) != 0)
			return EBADMSG;
		result->bytes_verified += size;
		_UpdateProgressWithInfo(RAW_WRITE_VERIFY, 0, result->bytes_verified, transfer->plan->target_bytes, 0);
	}
	return 0;
}

/* Aggregate even tiny concatenated members into bounded batches. A pending tail
 * is committed only after complete stream/source/fingerprint validation. */
static int TransferDecodedBytes(void* context, const uint8_t* bytes, size_t size, uint64_t offset)
{
	COMPRESSED_TRANSFER* transfer = context;
	int error = HashDecodedBytes(&transfer->hash, bytes, size, offset);
	if (error != 0)
		return error;
	while (size != 0) {
		const size_t space = sizeof(transfer->expected) - transfer->buffered;
		const size_t count = size < space ? size : space;
		memcpy(transfer->expected + transfer->buffered, bytes, count);
		transfer->buffered += count;
		bytes += count;
		size -= count;
		if (transfer->buffered == sizeof(transfer->expected)) {
			error = TransferCompressedBlock(transfer, transfer->buffered);
			if (error != 0)
				return error;
			transfer->buffered = 0;
		}
	}
	return 0;
}

static int TransferCompressedImage(const IMAGE_FILE_SOURCE* source, const char* path,
	const COMPRESSED_WRITE_PLAN* plan, int fd, bool verify, RAW_WRITE_RESULT* result)
{
	COMPRESSED_IMAGE_RESULT image;
	COMPRESSED_TRANSFER transfer = {.plan = plan, .result = result, .target = fd, .verify = verify};
	hash_init[HASH_SHA256](&transfer.hash.context);
	int error = ReadCompressedImageSource(source, path, plan->image.decoded_bytes, TransferDecodedBytes, &transfer, &image);
	if (error != 0)
		return error;
	hash_final[HASH_SHA256](&transfer.hash.context);
	const bool same_hash = memcmp(plan->decoded_sha256, transfer.hash.context.buf, sizeof(plan->decoded_sha256)) == 0;
	if (!SameCompressedImage(&plan->image, &image) || !same_hash)
		return ESTALE;
	if (transfer.buffered != 0) {
		uint64_t padded;
		error = CompressedImageWriteSize(transfer.buffered, plan->sector_size, &padded);
		if (error != 0)
			return error;
		memset(transfer.expected + transfer.buffered, 0, (size_t)padded - transfer.buffered);
		error = TransferCompressedBlock(&transfer, (size_t)padded);
		if (error != 0)
			return error;
	}
	const uint64_t transferred = verify ? result->bytes_verified : result->bytes_written;
	return transferred == plan->target_bytes ? 0 : EIO;
}

static int HashVtsiGap(VTSI_TRANSFER* transfer, uint64_t end)
{
	static const uint8_t zeros[COMPRESSED_TRANSFER_BYTES];
	if (end < transfer->hash.bytes || end > transfer->plan->image.decoded_bytes)
		return EBADMSG;
	while (transfer->hash.bytes < end) {
		if (IsRufusJobCancelled())
			return ECANCELED;
		const uint64_t remaining = end - transfer->hash.bytes;
		const size_t size = remaining < sizeof(zeros) ? (size_t)remaining : sizeof(zeros);
		hash_write[HASH_SHA256](&transfer->hash.context, zeros, size);
		transfer->hash.bytes += size;
	}
	return 0;
}

static int ReadVtsiBlock(int fd, uint8_t* bytes, size_t size, uint64_t offset)
{
	size_t position = 0;
	while (position < size) {
		if (IsRufusJobCancelled())
			return ECANCELED;
		const ssize_t count = pread(fd, bytes + position, size - position, (off_t)(offset + position));
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			return count < 0 ? errno : EIO;
		position += (size_t)count;
	}
	return 0;
}

static int TransferVtsiSegment(void* context, uint64_t disk_offset, uint64_t source_offset,
	uint64_t bytes)
{
	VTSI_TRANSFER* transfer = context;
	const unsigned sector_size = transfer->plan->sector_size;
	if (disk_offset % sector_size != 0 || bytes % sector_size != 0)
		return ENOTSUP;
	HashVtsiSegmentMap(&transfer->layout_hash, disk_offset, source_offset, bytes);
	int error = HashVtsiGap(transfer, disk_offset);
	if (error != 0)
		return error;
	uint64_t position = 0;
	while (position < bytes) {
		if (IsRufusJobCancelled())
			return ECANCELED;
		const uint64_t remaining = bytes - position;
		const size_t size = remaining < sizeof(transfer->expected) ? (size_t)remaining : sizeof(transfer->expected);
		error = ReadVtsiBlock(transfer->source->fd, transfer->expected, size, source_offset + position);
		if (error != 0)
			return error;
		hash_write[HASH_SHA256](&transfer->hash.context, transfer->expected, size);
		transfer->hash.bytes += size;
		if (transfer->verify) {
			error = ReadVtsiBlock(transfer->target, transfer->actual, size, disk_offset + position);
			if (error != 0)
				return error;
			if (memcmp(transfer->expected, transfer->actual, size) != 0)
				return EBADMSG;
			transfer->result->bytes_verified += size;
		} else {
			size_t written = 0;
			while (written < size) {
				if (IsRufusJobCancelled())
					return ECANCELED;
				const ssize_t count = pwrite(transfer->target, transfer->expected + written,
					size - written, (off_t)(disk_offset + position + written));
				if (count < 0 && errno == EINTR)
					continue;
				if (count <= 0)
					return count < 0 ? errno : EIO;
				written += (size_t)count;
				transfer->result->bytes_written += (uint64_t)count;
			}
		}
		position += size;
		const int phase = transfer->verify ? RAW_WRITE_VERIFY : RAW_WRITE_COPY;
		_UpdateProgressWithInfo(phase, 0, disk_offset + position, transfer->plan->target_bytes, 0);
	}
	transfer->transferred += bytes;
	return 0;
}

/* Windows VTSI seeks for each segment and does not clear gaps. Verify the
 * written extents, while the frozen digest covers the zero-filled logical
 * image used for analysis. Counters record actual payload I/O, not skips. */
static int TransferVtsiImage(const IMAGE_FILE_SOURCE* source, const char* path,
	const COMPRESSED_WRITE_PLAN* plan, int fd, bool verify, RAW_WRITE_RESULT* result)
{
	VTSI_TRANSFER transfer = {.source = source, .plan = plan, .result = result,
		.target = fd, .verify = verify};
	hash_init[HASH_SHA256](&transfer.hash.context);
	hash_init[HASH_SHA256](&transfer.layout_hash);
	uint64_t disk_size = 0, data_bytes = 0;
	int error = VisitVtsiImageSegments(source, path, plan->image.decoded_bytes,
		TransferVtsiSegment, &transfer, &disk_size, &data_bytes);
	if (error != 0)
		return error;
	if (disk_size != plan->image.decoded_bytes || transfer.transferred != data_bytes)
		return ESTALE;
	HashVtsiLayoutValue(&transfer.layout_hash, disk_size);
	HashVtsiLayoutValue(&transfer.layout_hash, data_bytes);
	hash_final[HASH_SHA256](&transfer.layout_hash);
	if (memcmp(plan->layout_sha256, transfer.layout_hash.buf, sizeof(plan->layout_sha256)) != 0)
		return ESTALE;
	error = HashVtsiGap(&transfer, disk_size);
	if (error != 0)
		return error;
	hash_final[HASH_SHA256](&transfer.hash.context);
	if (memcmp(plan->decoded_sha256, transfer.hash.context.buf, sizeof(plan->decoded_sha256)) != 0)
		return ESTALE;
	const uint64_t transferred = verify ? result->bytes_verified : result->bytes_written;
	if (transferred != data_bytes)
		return EIO;
	_UpdateProgressWithInfo(verify ? RAW_WRITE_VERIFY : RAW_WRITE_COPY,
		0, plan->target_bytes, plan->target_bytes, 0);
	return 0;
}

typedef struct {
	int fd;
	bool verify;
	uint64_t capacity;
	RAW_WRITE_RESULT* result;
	const IMAGE_FILE_SOURCE* source;
	const char* source_path;
	const COMPRESSED_WRITE_PLAN* plan;
	uint64_t device;
	const WRITABLE_TARGET* initial;
	uint8_t actual[COMPRESSED_TRANSFER_BYTES];
} FFU_TRANSFER;

static int TransferFfuBlock(void* context, const uint8_t* data, size_t bytes, uint64_t offset)
{
	FFU_TRANSFER* transfer = context;
	int error = 0;
	if (transfer->verify) {
		error = ReadVtsiBlock(transfer->fd, transfer->actual, bytes, offset);
		if (error == 0 && memcmp(data, transfer->actual, bytes) != 0) error = EBADMSG;
		if (error == 0) transfer->result->bytes_verified += bytes;
	} else {
		if (transfer->result->bytes_written == 0) {
			/* Base comparisons can take time. Repeat source/path identity
			 * after all of them, immediately before the first target write. */
			error = CheckFrozenSource(transfer->source, transfer->source_path, transfer->plan);
			if (error == 0)
				error = CheckCompressedTarget(transfer->source, transfer->fd, transfer->plan,
					transfer->device, transfer->capacity, transfer->initial);
			if (error != 0) {
				transfer->result->phase = RAW_WRITE_PREPARE;
				return error;
			}
			transfer->result->phase = RAW_WRITE_COPY;
			_UpdateProgressWithInfo(RAW_WRITE_COPY, 0, 0, transfer->capacity, 1);
			error = CheckFrozenSource(transfer->source, transfer->source_path, transfer->plan);
			if (error != 0) return error;
		}
		transfer->result->phase = RAW_WRITE_COPY;
		size_t position = 0;
		while (position < bytes) {
			if (IsRufusJobCancelled()) return ECANCELED;
			const ssize_t count = pwrite(transfer->fd, data + position, bytes - position, (off_t)(offset + position));
			if (count < 0 && errno == EINTR) continue;
			if (count <= 0) return count < 0 ? errno : EIO;
			position += (size_t)count; transfer->result->bytes_written += (uint64_t)count;
		}
	}
	const uint64_t done = transfer->verify ? transfer->result->bytes_verified : transfer->result->bytes_written;
	_UpdateProgressWithInfo(transfer->verify ? RAW_WRITE_VERIFY : RAW_WRITE_COPY, 0, done, transfer->capacity, 0);
	return error;
}

static int CompareFfuBase(void* context, const uint8_t* expected, size_t bytes, uint64_t offset)
{
	FFU_TRANSFER* transfer = context;
	if (bytes > sizeof(transfer->actual)) return EINVAL;
	transfer->result->phase = RAW_WRITE_PREPARE;
	const int error = ReadVtsiBlock(transfer->fd, transfer->actual, bytes, offset);
	if (error != 0) return error;
	if (IsRufusJobCancelled()) return ECANCELED;
	if (memcmp(expected, transfer->actual, bytes) != 0) return ESTALE;
	transfer->result->phase = RAW_WRITE_COPY;
	return 0;
}

static int TransferFfuImage(const IMAGE_FILE_SOURCE* source, const char* source_path, const COMPRESSED_WRITE_PLAN* plan,
	int target, uint64_t device, const WRITABLE_TARGET* initial, bool verify, RAW_WRITE_RESULT* result)
{
	const uint64_t capacity = initial->size;
	FFU_TRANSFER transfer = {.fd = target, .verify = verify, .capacity = capacity, .result = result,
		.source = source, .source_path = source_path, .plan = plan, .device = device, .initial = initial};
	uint64_t transferred = 0;
	int error = VisitFfuImageBlocks(source, capacity, plan->sector_size, plan->layout_sha256,
		verify ? FFU_READBACK : FFU_DEPLOY, verify ? NULL : CompareFfuBase,
		TransferFfuBlock, &transfer, &transferred);
	if (error == 0 && (transferred == 0 || transferred > capacity)) error = EIO;
	if (error == 0 && verify && transferred != result->bytes_written) error = EIO;
	if (error == 0)
		_UpdateProgressWithInfo(verify ? RAW_WRITE_VERIFY : RAW_WRITE_COPY, 0, capacity, capacity, 0);
	return error;
}

static int WriteCompressedImage(const IMAGE_FILE_SOURCE* source, const char* source_path,
	const COMPRESSED_WRITE_PLAN* plan, int target_fd, uint64_t device, uint64_t capacity, RAW_WRITE_RESULT* result)
{
	if (result == NULL)
		return EINVAL;
	memset(result, 0, sizeof(*result));
	int error = CheckPlan(plan);
	if (error != 0)
		return error;
	error = CheckFrozenSource(source, source_path, plan);
	if (error != 0)
		return error;
	WRITABLE_TARGET initial;
	error = InspectCompressedTarget(source, target_fd, plan, device, capacity, &initial);
	if (error != 0)
		return error;
	_UpdateProgressWithInfo(RAW_WRITE_PREPARE, 0, 0, 0, 1);
	COMPRESSED_WRITE_PLAN actual;
	error = PrepareCompressedWrite(source, source_path, plan->image.decoded_bytes,
		plan->sector_size, plan->ignore_boot_marker, &actual);
	if (error != 0)
		return error == EFBIG ? ESTALE : error;
	const bool same_hash = memcmp(plan->decoded_sha256, actual.decoded_sha256, sizeof(plan->decoded_sha256)) == 0;
	const bool same_layout = memcmp(plan->layout_sha256, actual.layout_sha256,
		sizeof(plan->layout_sha256)) == 0;
	if (!SameCompressedImage(&plan->image, &actual.image) || !same_hash || !same_layout)
		return ESTALE;
	error = CheckFrozenSource(source, source_path, plan);
	if (error == 0)
		error = CheckCompressedTarget(source, target_fd, plan, device, capacity, &initial);
	if (error != 0)
		return error;
	const bool ffu = plan->image.compression_type == IMG_COMPRESSION_FFU;
	const uint64_t progress_bytes = ffu ? initial.size : plan->target_bytes;
	result->phase = RAW_WRITE_COPY;
	if (!ffu)
		_UpdateProgressWithInfo(RAW_WRITE_COPY, 0, 0, progress_bytes, 1);
	if (ffu)
		error = TransferFfuImage(source, source_path, plan, target_fd, device, &initial, false, result);
	else if (plan->image.compression_type == BLED_COMPRESSION_VTSI)
		error = TransferVtsiImage(source, source_path, plan, target_fd, false, result);
	else
		error = TransferCompressedImage(source, source_path, plan, target_fd, false, result);
	if (error == 0)
		result->phase = RAW_WRITE_SYNC;
	if (result->bytes_written != 0) {
		_UpdateProgressWithInfo(RAW_WRITE_SYNC, 0, 0, 0, 1);
		while (fsync(target_fd) != 0) {
			if (errno != EINTR) {
				result->sync_error = errno;
				break;
			}
		}
	}
	if (error == 0)
		error = result->sync_error;
	if (error == 0)
		error = CheckFrozenSource(source, source_path, plan);
	if (error == 0)
		error = CheckCompressedTarget(source, target_fd, plan, device, capacity, &initial);
	if (error == 0) {
		result->phase = RAW_WRITE_VERIFY;
		_UpdateProgressWithInfo(RAW_WRITE_VERIFY, 0, 0, progress_bytes, 1);
		if (ffu)
			error = TransferFfuImage(source, source_path, plan, target_fd, device, &initial, true, result);
		else if (plan->image.compression_type == BLED_COMPRESSION_VTSI)
			error = TransferVtsiImage(source, source_path, plan, target_fd, true, result);
		else
			error = TransferCompressedImage(source, source_path, plan, target_fd, true, result);
	}
	if (error == 0)
		error = CheckFrozenSource(source, source_path, plan);
	if (error == 0)
		error = CheckCompressedTarget(source, target_fd, plan, device, capacity, &initial);
	if (error == 0 && IsRufusJobCancelled())
		error = ECANCELED;
	if (error == 0)
		result->phase = RAW_WRITE_DONE;
	return error;
}

int WriteCompressedImageToFile(const IMAGE_FILE_SOURCE* source, const char* source_path,
	const COMPRESSED_WRITE_PLAN* plan, int target_fd, RAW_WRITE_RESULT* result)
{
	return WriteCompressedImage(source, source_path, plan, target_fd, 0, 0, result);
}

int WriteCompressedImageToDevice(const IMAGE_FILE_SOURCE* source, const char* source_path,
	const COMPRESSED_WRITE_PLAN* plan, int target_fd, uint64_t device, uint64_t capacity, RAW_WRITE_RESULT* result)
{
	if (device == 0 || capacity == 0) {
		if (result != NULL)
			memset(result, 0, sizeof(*result));
		return EINVAL;
	}
	return WriteCompressedImage(source, source_path, plan, target_fd, device, capacity, result);
}
