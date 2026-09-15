/*
 * Rufus: The Reliable USB Formatting Utility
 * Image file reading extracted from iso.c
 * Copyright © 2011-2026 Pete Batard <pete@akeo.ie>
 * Based on libcdio's ISO and UDF samples:
 * Copyright © 2003-2014 Rocky Bernstein <rocky@gnu.org>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "iso_read.h"
#include <cdio/udf.h>

#include <stdlib.h>

extern void uprintf(const char* format, ...);

static uint32_t ReadUdfFileToBuffer(udf_t* udf, const char* iso_file, uint8_t** buf, uint32_t max_size)
{
	udf_dirent_t *root = NULL, *file = NULL;
	uint64_t file_length;
	uint32_t nblocks, ret = 0;
	ssize_t read_size;

	root = udf_get_root(udf, true, 0);
	if (root == NULL) {
		uprintf("Could not locate UDF root directory");
		goto out;
	}
	file = udf_fopen(root, iso_file);
	if (file == NULL) {
		uprintf("Could not locate file %s in ISO image", iso_file);
		goto out;
	}
	if (udf_is_dir(file)) {
		uprintf("Cannot read directory %s as a file", iso_file);
		goto out;
	}
	file_length = udf_get_file_length(file);
	if (file_length > max_size) {
		uprintf("File exceeds the read limit (%u bytes)", max_size);
		goto out;
	}
	if (file_length == 0)
		goto out;
	nblocks = (uint32_t)((file_length + UDF_BLOCKSIZE - 1) / UDF_BLOCKSIZE);
	*buf = malloc(nblocks * UDF_BLOCKSIZE + 1);
	if (*buf == NULL) {
		uprintf("Could not allocate buffer for file %s", iso_file);
		goto out;
	}
	read_size = udf_read_block(file, *buf, nblocks);
	if (read_size < 0 || (uint64_t)read_size != file_length) {
		uprintf("Error reading UDF file %s", iso_file);
		goto out;
	}
	ret = (uint32_t)file_length;
	(*buf)[ret] = 0;

out:
	udf_dirent_free(root);
	udf_dirent_free(file);
	if (ret == 0) {
		free(*buf);
		*buf = NULL;
	}
	return ret;
}

static uint32_t ReadIsoFileToBuffer(const char* iso, const char* iso_file, uint8_t** buf,
	iso_extension_mask_t extension_mask, uint32_t max_size)
{
	iso9660_t* image = NULL;
	iso9660_stat_t* file = NULL;
	uint64_t file_length;
	uint32_t nblocks, ret = 0;

	// Extensions retain names such as Rock Ridge's 'i386-pc' directory.
	image = iso9660_open_ext(iso, extension_mask);
	if (image == NULL) {
		uprintf("Unable to open image '%s'", iso);
		goto out;
	}
	file = iso9660_ifs_stat_translate(image, iso_file);
	if (file == NULL) {
		uprintf("Could not get ISO-9660 file information for file %s", iso_file);
		goto out;
	}
	if (file->type == _STAT_DIR) {
		uprintf("Cannot read directory %s as a file", iso_file);
		goto out;
	}
	file_length = file->total_size;
	if (file_length > max_size) {
		uprintf("File exceeds the read limit (%u bytes)", max_size);
		goto out;
	}
	if (file_length == 0)
		goto out;
	nblocks = (uint32_t)((file_length + ISO_BLOCKSIZE - 1) / ISO_BLOCKSIZE);
	*buf = malloc(nblocks * ISO_BLOCKSIZE + 1);
	if (*buf == NULL) {
		uprintf("Could not allocate buffer for file %s", iso_file);
		goto out;
	}
	if (iso9660_iso_seek_read(image, *buf, file->lsn, nblocks) != nblocks * ISO_BLOCKSIZE) {
		uprintf("Error reading ISO file %s", iso_file);
		goto out;
	}
	ret = (uint32_t)file_length;
	(*buf)[ret] = 0;

out:
	iso9660_stat_free(file);
	iso9660_close(image);
	if (ret == 0) {
		free(*buf);
		*buf = NULL;
	}
	return ret;
}

uint32_t ReadISOFileToBufferLimited(const char* iso, const char* iso_file, uint8_t** buf,
	iso_extension_mask_t extension_mask, uint32_t max_size)
{
	udf_t* udf;
	uint32_t ret;

	if (buf == NULL)
		return 0;
	*buf = NULL;
	if (iso == NULL || iso_file == NULL || max_size == 0 || max_size > ISO_READ_MAX_SIZE)
		return 0;
	// Preserve Rufus's UDF preference. Only a failed open tries the ISO view.
	udf = udf_open(iso);
	if (udf == NULL)
		return ReadIsoFileToBuffer(iso, iso_file, buf, extension_mask, max_size);
	ret = ReadUdfFileToBuffer(udf, iso_file, buf, max_size);
	udf_close(udf);
	return ret;
}

uint32_t ReadISOFileToBufferExt(const char* iso, const char* iso_file, uint8_t** buf,
	iso_extension_mask_t extension_mask)
{
	return ReadISOFileToBufferLimited(iso, iso_file, buf, extension_mask, ISO_READ_MAX_SIZE);
}
