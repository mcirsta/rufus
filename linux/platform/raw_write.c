/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "raw_write.h"
#include "target_io.h"
#include "rufus_reporting.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define COPY_BUFFER_SIZE (1024 * 1024)

enum {
	MIN_SECTOR_BYTES = 512,
	MAX_SECTOR_BYTES = 64 * 1024
};

int CheckRawBlockTarget(int fd, uint64_t device, uint64_t capacity, uint64_t image_size)
{
	const bool valid_plan = device != 0 && capacity != 0 && image_size != 0;
	if (!valid_plan)
		return ESTALE;
	WRITABLE_TARGET target;
	const int error = InspectWritableTarget(fd, device, &target);
	if (error != 0)
		return error == ENOTSUP ? ENOTBLK : error;
	const uint64_t size = target.size;
	const unsigned sector = target.sector_size;
	if (size != capacity)
		return ESTALE;
	if (image_size > size)
		return ENOSPC;
	bool supported_sector_size = sector >= MIN_SECTOR_BYTES && sector <= MAX_SECTOR_BYTES;
	if (!supported_sector_size)
		return EINVAL;
	bool power_of_two = (sector & (sector - 1)) == 0;
	bool sector_aligned = image_size % sector == 0;
	if (!power_of_two || !sector_aligned)
		return EINVAL;
	return 0;
}

static int CheckFiles(const IMAGE_FILE_SOURCE* source, const char* path, int target,
	uint64_t device, uint64_t capacity, uint64_t offset, uint64_t extent_size, struct stat* target_info)
{
	int error = CheckImageFileSource(source, path);
	int flags;
	uint64_t size;

	if (error != 0)
		return error;
	if (source->info.st_size <= 0)
		return EINVAL;
	flags = fcntl(source->fd, F_GETFL);
	if (flags < 0)
		return errno;
	if ((flags & O_ACCMODE) == O_WRONLY || (flags & (O_DIRECT | O_PATH)))
		return EINVAL;
	if (device != 0) {
		if ((uint64_t)source->info.st_dev == device)
			return EINVAL;
		error = CheckRawBlockTarget(target, device, capacity, (uint64_t)source->info.st_size);
		if (error != 0)
			return error;
		int sector = 0;
		if (ioctl(target, BLKSSZGET, &sector) != 0)
			return errno;
		const bool aligned_extent = sector > 0 && offset % sector == 0 && extent_size % sector == 0;
		if (!aligned_extent)
			return EINVAL;
		size = capacity;
	} else {
		WRITABLE_TARGET inspected;
		error = InspectWritableTarget(target, 0, &inspected);
		if (error != 0)
			return error;
		*target_info = inspected.info;
		const bool same_file = target_info->st_dev == source->info.st_dev && target_info->st_ino == source->info.st_ino;
		if (same_file)
			return EINVAL;
		size = inspected.size;
	}
	if (offset > size)
		return ENOSPC;
	const uint64_t available = size - offset;
	if (extent_size == 0)
		extent_size = available; // Existing whole-image entry point.
	const bool extent_fits = extent_size <= available;
	const bool source_fits = (uint64_t)source->info.st_size <= extent_size;
	if (!extent_fits || !source_fits)
		return ENOSPC;
	const bool addressable_copy = offset <= INT64_MAX && (uint64_t)source->info.st_size <= INT64_MAX - offset;
	if (!addressable_copy)
		return EOVERFLOW;
	return 0;
}

static int ReadBuffer(int fd, uint8_t* buffer, size_t size, uint64_t offset)
{
	ssize_t count;
	size_t position = 0;

	while (position < size) {
		if (IsRufusJobCancelled())
			return ECANCELED;
		count = pread(fd, buffer + position, size - position, (off_t)(offset + position));
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			return count == 0 ? EIO : errno;
		position += (size_t)count;
	}
	return 0;
}

static int WriteBuffer(int fd, const uint8_t* buffer, size_t size, uint64_t offset,
	uint64_t total, RAW_WRITE_RESULT* result)
{
	ssize_t count;
	size_t position = 0;

	while (position < size) {
		if (IsRufusJobCancelled())
			return ECANCELED;
		count = pwrite(fd, buffer + position, size - position, (off_t)(offset + result->bytes_written));
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			return count == 0 ? EIO : errno;
		position += (size_t)count;
		result->bytes_written += (uint64_t)count;
		_UpdateProgressWithInfo(RAW_WRITE_COPY, 0, result->bytes_written, total, 0);
	}
	return 0;
}

static int CopyImage(int source, int target, uint64_t offset, uint64_t total, uint8_t* buffer,
	RAW_WRITE_RESULT* result)
{
	size_t size;
	int error;

	while (result->bytes_written < total) {
		size = total - result->bytes_written > COPY_BUFFER_SIZE ? COPY_BUFFER_SIZE :
			(size_t)(total - result->bytes_written);
		error = ReadBuffer(source, buffer, size, result->bytes_written);
		if (error == 0)
			error = WriteBuffer(target, buffer, size, offset, total, result);
		if (error != 0)
			return error;
	}
	return 0;
}

static int SyncTarget(int target)
{
	while (fsync(target) != 0) {
		if (errno != EINTR)
			return errno;
	}
	return 0;
}

static int VerifyImage(int source, int target, uint64_t offset, uint64_t total, uint8_t* buffer,
	RAW_WRITE_RESULT* result)
{
	uint8_t* actual = buffer + COPY_BUFFER_SIZE;
	size_t size;
	int error;

	while (result->bytes_verified < total) {
		size = total - result->bytes_verified > COPY_BUFFER_SIZE ? COPY_BUFFER_SIZE :
			(size_t)(total - result->bytes_verified);
		error = ReadBuffer(source, buffer, size, result->bytes_verified);
		if (error == 0)
			error = ReadBuffer(target, actual, size, offset + result->bytes_verified);
		if (error != 0)
			return error;
		if (memcmp(buffer, actual, size) != 0)
			return EBADMSG;
		result->bytes_verified += size;
		_UpdateProgressWithInfo(RAW_WRITE_VERIFY, 0, result->bytes_verified, total, 0);
	}
	return 0;
}

static int WriteImage(const IMAGE_FILE_SOURCE* source, const char* source_path,
	int target_fd, uint64_t device, uint64_t capacity, uint64_t offset, uint64_t extent_size,
	RAW_WRITE_RESULT* result)
{
	struct stat initial_target, current_target;
	uint8_t* buffer;
	int error;

	if (result == NULL)
		return EINVAL;
	memset(result, 0, sizeof(*result));
	error = CheckFiles(source, source_path, target_fd, device, capacity, offset, extent_size, &initial_target);
	if (error != 0)
		return error;
	buffer = malloc(2 * COPY_BUFFER_SIZE);
	if (buffer == NULL)
		return ENOMEM;
	result->phase = RAW_WRITE_COPY;
	_UpdateProgressWithInfo(RAW_WRITE_COPY, 0, 0, (uint64_t)source->info.st_size, 1);
	error = CopyImage(source->fd, target_fd, offset, (uint64_t)source->info.st_size, buffer, result);
	if (error == 0)
		result->phase = RAW_WRITE_SYNC;
	if (result->bytes_written != 0) {
		_UpdateProgressWithInfo(RAW_WRITE_SYNC, 0, 0, 0, 1);
		result->sync_error = SyncTarget(target_fd);
	}
	if (error == 0)
		error = result->sync_error;
	if (error == 0)
		error = CheckImageFileSource(source, source_path);
	if (error == 0) {
		result->phase = RAW_WRITE_VERIFY;
		_UpdateProgressWithInfo(RAW_WRITE_VERIFY, 0, 0, (uint64_t)source->info.st_size, 1);
		error = VerifyImage(source->fd, target_fd, offset, (uint64_t)source->info.st_size, buffer, result);
	}
	if (error == 0)
		error = CheckImageFileSource(source, source_path);
	if (error == 0 && device != 0)
		error = CheckRawBlockTarget(target_fd, device, capacity, (uint64_t)source->info.st_size);
	if (error == 0 && device == 0) {
		if (fstat(target_fd, &current_target) != 0)
			error = errno;
		else if (current_target.st_size != initial_target.st_size)
			error = ESTALE;
	}
	if (error == 0 && IsRufusJobCancelled())
		error = ECANCELED;
	if (error == 0)
		result->phase = RAW_WRITE_DONE;
	free(buffer);
	return error;
}

int WriteRawImage(const IMAGE_FILE_SOURCE* source, const char* source_path,
	int target_fd, RAW_WRITE_RESULT* result)
{
	return WriteImage(source, source_path, target_fd, 0, 0, 0, 0, result);
}

int WriteRawImageToExtent(const IMAGE_FILE_SOURCE* source, const char* source_path,
	int target_fd, uint64_t offset, uint64_t size, RAW_WRITE_RESULT* result)
{
	if (size == 0) {
		if (result != NULL)
			memset(result, 0, sizeof(*result));
		return EINVAL;
	}
	return WriteImage(source, source_path, target_fd, 0, 0, offset, size, result);
}

int WriteRawImageToDevice(const IMAGE_FILE_SOURCE* source, const char* source_path,
	int target_fd, uint64_t device, uint64_t capacity, RAW_WRITE_RESULT* result)
{
	if (device == 0) {
		if (result != NULL)
			memset(result, 0, sizeof(*result));
		return EINVAL;
	}
	return WriteImage(source, source_path, target_fd, device, capacity, 0, 0, result);
}

int WriteRawImageToDeviceExtent(const IMAGE_FILE_SOURCE* source, const char* source_path,
	int target_fd, uint64_t device, uint64_t capacity, uint64_t offset, uint64_t size, RAW_WRITE_RESULT* result)
{
	if (device == 0 || size == 0) {
		if (result != NULL)
			memset(result, 0, sizeof(*result));
		return EINVAL;
	}
	return WriteImage(source, source_path, target_fd, device, capacity, offset, size, result);
}
