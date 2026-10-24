/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "uefi_ntfs.h"
#include <errno.h>
#include <string.h>

static int CheckBootPartition(int fd, const GPT_IMAGE_OPTIONS* gpt,
	const MBR_IMAGE_OPTIONS* mbr, uint64_t* offset, uint64_t* size)
{
	int error;
	if (mbr != NULL) {
		MBR_IMAGE_RESULT layout;
		error = CheckMbrImage(fd, mbr, &layout);
		*offset = layout.uefi_ntfs_offset;
		*size = layout.uefi_ntfs_size;
	} else {
		GPT_IMAGE_RESULT layout;
		error = CheckGptImagePartition(fd, gpt, &layout);
		*offset = layout.uefi_ntfs_offset;
		*size = layout.uefi_ntfs_size;
	}
	return error;
}

static int WriteBootImage(const IMAGE_FILE_SOURCE* source, const char* source_path,
	int target_fd, const GPT_IMAGE_OPTIONS* gpt, const MBR_IMAGE_OPTIONS* mbr, RAW_WRITE_RESULT* result)
{
	if (result == NULL)
		return EINVAL;
	memset(result, 0, sizeof(*result));
	if (gpt == NULL && mbr == NULL)
		return EINVAL;
	const uint64_t payload_size = mbr != NULL ? mbr->uefi_ntfs_size : gpt->uefi_ntfs_size;
	const uint64_t disk_size = mbr != NULL ? mbr->disk_size : gpt->disk_size;
	const uint64_t device = mbr != NULL ? mbr->device : gpt->device;
	if (payload_size == 0)
		return EINVAL;
	int error = CheckImageFileSource(source, source_path);
	if (error != 0)
		return error;
	if ((uint64_t)source->info.st_size != payload_size)
		return EINVAL;
	uint64_t offset = 0, size = 0;
	error = CheckBootPartition(target_fd, gpt, mbr, &offset, &size);
	if (error == 0) {
		if (device != 0)
			error = WriteRawImageToDeviceExtent(source, source_path, target_fd, device,
				disk_size, offset, size, result);
		else
			error = WriteRawImageToExtent(source, source_path, target_fd, offset, size, result);
	}
	if (error == 0) {
		error = CheckBootPartition(target_fd, gpt, mbr, &offset, &size);
		if (error != 0)
			result->phase = RAW_WRITE_VERIFY;
	}
	return error;
}

int WriteUefiNtfsImage(const IMAGE_FILE_SOURCE* source, const char* source_path,
	int target_fd, const GPT_IMAGE_OPTIONS* options, RAW_WRITE_RESULT* result)
{
	return WriteBootImage(source, source_path, target_fd, options, NULL, result);
}

int WriteMbrUefiNtfsImage(const IMAGE_FILE_SOURCE* source, const char* source_path,
	int target_fd, const MBR_IMAGE_OPTIONS* options, RAW_WRITE_RESULT* result)
{
	return WriteBootImage(source, source_path, target_fd, NULL, options, result);
}
