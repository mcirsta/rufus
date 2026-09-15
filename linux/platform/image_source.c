/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "image_source.h"
#include "image_file.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

extern int IsIsoScanCancelled(void);

static bool SameImageFile(const struct stat* initial, const struct stat* current)
{
	return S_ISREG(current->st_mode) && initial->st_dev == current->st_dev &&
		initial->st_ino == current->st_ino && initial->st_size == current->st_size &&
		initial->st_mtim.tv_sec == current->st_mtim.tv_sec &&
		initial->st_mtim.tv_nsec == current->st_mtim.tv_nsec &&
		initial->st_ctim.tv_sec == current->st_ctim.tv_sec &&
		initial->st_ctim.tv_nsec == current->st_ctim.tv_nsec;
}

void CloseImageFileSource(IMAGE_FILE_SOURCE* source)
{
	if (source == NULL)
		return;
	if (source->fd >= 0)
		close(source->fd);
	memset(source, 0, sizeof(*source));
	source->fd = -1;
}

int OpenImageFileSource(const char* path, IMAGE_FILE_SOURCE* source)
{
	struct stat current;
	uint8_t signature[512];
	size_t size = 0;
	size_t limit;
	ssize_t count;
	int error;

	if (source == NULL)
		return EINVAL;
	memset(source, 0, sizeof(*source));
	source->fd = -1;
	if (path == NULL || *path == 0)
		return EINVAL;
	if (IsIsoScanCancelled())
		return ECANCELED;
	source->fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
	if (source->fd < 0)
		return errno;
	error = fstat(source->fd, &source->info) == 0 ? 0 : errno;
	if (error != 0)
		goto fail;
	if (!S_ISREG(source->info.st_mode) || source->info.st_size < 0) {
		error = EINVAL;
		goto fail;
	}
	limit = source->info.st_size < (off_t)sizeof(signature) ?
		(size_t)source->info.st_size : sizeof(signature);
	while (size < limit) {
		if (IsIsoScanCancelled()) {
			error = ECANCELED;
			goto fail;
		}
		count = pread(source->fd, signature + size, limit - size, (off_t)size);
		if (count < 0 && errno == EINTR)
			continue;
		if (count < 0) {
			error = errno;
			goto fail;
		}
		if (count == 0)
			break;
		size += (size_t)count;
	}
	error = fstat(source->fd, &current) == 0 ? 0 : errno;
	if (error == 0 && !SameImageFile(&source->info, &current))
		error = ESTALE;
	if (error == 0 && size != limit)
		error = EIO;
	if (IsIsoScanCancelled())
		error = ECANCELED;
	if (error != 0)
		goto fail;
	source->is_wim = IsWimImageSignature(signature, size);
	source->has_boot_marker = IsBootImageSignature(signature, size);
	return 0;
fail:
	CloseImageFileSource(source);
	return error;
}

int CheckImageFileSource(const IMAGE_FILE_SOURCE* source, const char* path)
{
	struct stat current;

	if (source == NULL || source->fd < 0 || path == NULL || *path == 0)
		return EINVAL;
	if (IsIsoScanCancelled())
		return ECANCELED;
	if (fstat(source->fd, &current) != 0)
		return errno;
	if (!SameImageFile(&source->info, &current))
		return ESTALE;
	if (stat(path, &current) != 0)
		return errno;
	if (IsIsoScanCancelled())
		return ECANCELED;
	return SameImageFile(&source->info, &current) ? 0 : ESTALE;
}
