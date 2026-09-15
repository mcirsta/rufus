/*
 * Rufus: The Reliable USB Formatting Utility
 * Shared ISO/UDF file ranges, extracted from the embedded EFI reader
 * Copyright © 2011-2026 Pete Batard <pete@akeo.ie>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "iso_file.h"

#include <string.h>

void CloseIsoFileReader(ISO_FILE_READER* reader)
{
	if (reader == NULL)
		return;
	udf_dirent_free(reader->udf_file);
	memset(reader, 0, sizeof(*reader));
}

int OpenIsoFileReader(ISO_FILE_READER* reader, iso9660_t* iso, udf_t* udf, const char* path)
{
	iso9660_stat_t* file = NULL;
	udf_dirent_t* root = NULL;
	int result = ISO_SCAN_PARTIAL;

	if (reader == NULL)
		return ISO_SCAN_ERROR;
	memset(reader, 0, sizeof(*reader));
	if ((iso == NULL) == (udf == NULL) || path == NULL)
		return ISO_SCAN_ERROR;
	if (IsIsoScanCancelled())
		return ISO_SCAN_CANCELLED;
	if (udf != NULL)
	{
		root = udf_get_root(udf, true, 0);
		if (root == NULL)
			goto out;
		reader->udf_file = udf_fopen(root, path);
		if (reader->udf_file == NULL || reader->udf_file->fe.icb_tag.file_type != ICBTAG_FILE_TYPE_REGULAR)
			goto out;
		reader->size = udf_get_file_length(reader->udf_file);
	}
	else
	{
		file = iso9660_ifs_stat_translate(iso, path);
		// Some generators give empty files a non-data extent sentinel. No
		// sector can be read from an empty file, so only nonempty files need it.
		if (file == NULL || file->type != _STAT_FILE || (file->lsn < 0 && file->total_size != 0))
			goto out;
		reader->iso = iso;
		reader->lsn = file->lsn;
		reader->size = file->total_size;
	}
	if (reader->size <= INT64_MAX)
		result = ISO_SCAN_OK;
out:
	iso9660_stat_free(file);
	udf_dirent_free(root);
	if (IsIsoScanCancelled())
		result = ISO_SCAN_CANCELLED;
	if (result != ISO_SCAN_OK)
		CloseIsoFileReader(reader);
	return result;
}

static int FillIsoFileCache(ISO_FILE_READER* reader, uint64_t offset)
{
	uint64_t available, lsn, position;
	size_t blocks, done = 0;
	ssize_t count;

	reader->cache_size = 0;
	reader->cache_start = offset / ISO_BLOCKSIZE * ISO_BLOCKSIZE;
	available = reader->size - reader->cache_start;
	if (available > sizeof(reader->cache))
		available = sizeof(reader->cache);
	if (reader->udf_file != NULL)
	{
		// UDF reads stop at extent boundaries. Continue at the next logical
		// offset, allowing the existing reader to map each fragmented extent.
		while (done < available)
		{
			position = reader->cache_start + done;
			if (IsIsoScanCancelled())
				return ISO_SCAN_CANCELLED;
			if ((off_t)position < 0 || (uint64_t)(off_t)position != position ||
				!udf_setpos(reader->udf_file, (off_t)position))
				return ISO_SCAN_PARTIAL;
			blocks = (size_t)((available - done + UDF_BLOCKSIZE - 1) / UDF_BLOCKSIZE);
			count = udf_read_block(reader->udf_file, reader->cache + done, blocks);
			if (count <= 0 || (uint64_t)count > available - done)
				return ISO_SCAN_PARTIAL;
			done += (size_t)count;
		}
	}
	else
	{
		blocks = (size_t)((available + ISO_BLOCKSIZE - 1) / ISO_BLOCKSIZE);
		lsn = (uint64_t)reader->lsn + reader->cache_start / ISO_BLOCKSIZE;
		if (lsn > INT32_MAX || blocks > (uint64_t)INT32_MAX - lsn + 1 ||
			iso9660_iso_seek_read(reader->iso, reader->cache, (lsn_t)lsn, blocks)
				!= (long)(blocks * ISO_BLOCKSIZE))
			return ISO_SCAN_PARTIAL;
	}
	if (IsIsoScanCancelled())
		return ISO_SCAN_CANCELLED;
	reader->cache_size = (size_t)available;
	return ISO_SCAN_OK;
}

int ReadIsoFileRange(ISO_FILE_READER* reader, uint64_t offset, void* buffer, size_t count)
{
	uint8_t* output = buffer;
	size_t cached, size;
	int result;

	if (reader == NULL || (reader->iso == NULL && reader->udf_file == NULL) ||
		(buffer == NULL && count != 0) || offset > reader->size || count > reader->size - offset)
		return ISO_SCAN_ERROR;
	if (IsIsoScanCancelled())
		return ISO_SCAN_CANCELLED;
	while (count != 0)
	{
		if (IsIsoScanCancelled())
			return ISO_SCAN_CANCELLED;
		if (offset < reader->cache_start || offset - reader->cache_start >= reader->cache_size)
		{
			result = FillIsoFileCache(reader, offset);
			if (result != ISO_SCAN_OK)
				return result;
		}
		cached = (size_t)(offset - reader->cache_start);
		size = reader->cache_size - cached;
		if (size > count)
			size = count;
		memcpy(output, reader->cache + cached, size);
		output += size;
		offset += size;
		count -= size;
	}
	return ISO_SCAN_OK;
}
