/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "native_input.h"
#include "wimlib/wim.h"
#include "iso_file.h"

#include <errno.h>
#include <limits.h>

int OpenWimFromIsoReader(ISO_FILE_READER* reader, int flags, WIMStruct** wim)
{
	int result;

	if (wim == NULL)
		return WIMLIB_ERR_INVALID_PARAM;
	*wim = NULL;
	if (reader == NULL || ((reader->iso == NULL) == (reader->udf_file == NULL)) ||
		reader->size > INT64_MAX ||
		(flags & ~(WIMLIB_OPEN_FLAG_CHECK_INTEGRITY | WIMLIB_OPEN_FLAG_ERROR_IF_SPLIT |
			WIMLIB_OPEN_FLAG_WRITE_ACCESS)))
		return WIMLIB_ERR_INVALID_PARAM;
	if (flags & WIMLIB_OPEN_FLAG_WRITE_ACCESS)
		return WIMLIB_ERR_WIM_IS_READONLY;
	if (IsIsoScanCancelled())
		return WIMLIB_ERR_ABORTED_BY_PROGRESS;
	result = open_wim_as_WIMStruct(reader, flags | RUFUS_WIM_OPEN_FROM_READER, wim, NULL, NULL);
	if (IsIsoScanCancelled())
	{
		wimlib_free(*wim);
		*wim = NULL;
		return WIMLIB_ERR_ABORTED_BY_PROGRESS;
	}
	return result;
}

uint64_t WimReaderSize(const ISO_FILE_READER* reader)
{
	return reader->size;
}

int WimReaderRead(struct filedes* fd, void* buffer, size_t count, off_t offset)
{
	int result;

	if (IsIsoScanCancelled())
	{
		errno = ECANCELED;
		return WIMLIB_ERR_ABORTED_BY_PROGRESS;
	}
	if (offset < 0 || (uint64_t)offset > fd->iso_reader->size ||
		count > fd->iso_reader->size - (uint64_t)offset)
	{
		errno = EINVAL;
		return WIMLIB_ERR_UNEXPECTED_END_OF_FILE;
	}
	result = ReadIsoFileRange(fd->iso_reader, (uint64_t)offset, buffer, count);
	if (result == ISO_SCAN_OK)
		return 0;
	if (result == ISO_SCAN_CANCELLED)
	{
		errno = ECANCELED;
		return WIMLIB_ERR_ABORTED_BY_PROGRESS;
	}
	errno = EIO;
	return WIMLIB_ERR_READ;
}

off_t WimReaderSeek(struct filedes* fd, off_t offset)
{
	if (offset < 0 || (uint64_t)offset > fd->iso_reader->size)
	{
		errno = EINVAL;
		return -1;
	}
	fd->offset = offset;
	return offset;
}
