/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "partition_image.h"
#include "partition_layout.h"
#include "rufus_reporting.h"

#include <libfdisk.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <unistd.h>

enum {
	GPT_SECTOR_BYTES = 512,
	GPT_ENTRY_COUNT = 128,
	GPT_ENTRY_BYTES = 128,
	GPT_TABLE_SECTORS = GPT_ENTRY_COUNT * GPT_ENTRY_BYTES / GPT_SECTOR_BYTES,
	GPT_PRIMARY_ENTRIES_LBA = 2,  // Protective MBR and primary header precede the table.
	GPT_FIRST_USABLE_LBA = GPT_PRIMARY_ENTRIES_LBA + GPT_TABLE_SECTORS,
	GPT_TRAILING_SECTORS = GPT_TABLE_SECTORS + 1,  // Backup table and its header.
	GPT_MIN_IMAGE_BYTES = 2 * 1024 * 1024,
	GPT_MAX_SECTORS_PER_TRACK = 63,
	GPT_MAX_CLUSTER_BYTES = 32 * 1024
};

static const char main_type[] = "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7";
static const char main_name[] = "Main Data Partition";

static int CheckImageTarget(int fd, const GPT_IMAGE_OPTIONS* options)
{
	struct stat info;
	uint64_t size;
	int sector_size = 0, read_only = 1;
	int flags = fcntl(fd, F_GETFL);

	if (flags < 0 || fstat(fd, &info) != 0)
		return errno;
	const bool block_request = options->device != 0;
	const bool expected_type = block_request ? S_ISBLK(info.st_mode) : S_ISREG(info.st_mode);
	if (!expected_type)
		return ENOTSUP;
	const bool read_write = (flags & O_ACCMODE) == O_RDWR;
	const bool incompatible_flags = (flags & (O_APPEND | O_DIRECT | O_PATH)) != 0;
	if (!read_write || incompatible_flags)
		return EINVAL;
	if (block_request) {
		if ((uint64_t)info.st_rdev != options->device)
			return ESTALE;
		if (ioctl(fd, BLKGETSIZE64, &size) != 0)
			return errno;
		if (ioctl(fd, BLKSSZGET, &sector_size) != 0)
			return errno;
		if (ioctl(fd, BLKROGET, &read_only) != 0)
			return errno;
		if (read_only)
			return EROFS;
		if (sector_size != GPT_SECTOR_BYTES)
			return ENOTSUP;
	} else {
		if (info.st_size < 0)
			return ESTALE;
		size = (uint64_t)info.st_size;
	}
	if (size != options->disk_size)
		return ESTALE;
	return IsRufusJobCancelled() ? ECANCELED : 0;
}

static int PrepareGpt(struct fdisk_context* context, int fd,
	const GPT_IMAGE_OPTIONS* options, const GPT_IMAGE_RESULT* result)
{
	struct fdisk_partition* partition = NULL;
	struct fdisk_parttype* type;
	struct fdisk_script* settings;
	char first_usable_lba[32];
	size_t number = 0;
	int status;

	fdisk_disable_dialogs(context, 1);
	fdisk_enable_wipe(context, 0);
	fdisk_enable_bootbits_protection(context, 0);
	status = fdisk_assign_device_by_fd(context, fd, "Rufus target", 0);
	if (status != 0)
		return status;
	const bool expected_sector_size = fdisk_get_sector_size(context) == GPT_SECTOR_BYTES;
	const bool expected_sector_count = fdisk_get_nsectors(context) == options->disk_size / GPT_SECTOR_BYTES;
	if (!expected_sector_size || !expected_sector_count)
		return -ESTALE;
	// Preserve the Windows usable range, rather than libfdisk's 1 MiB default
	// for the first usable sector. The actual partition still starts at 1 MiB.
	// A context alignment override alone is reset when replacing an old label.
	// The library's in-memory header settings persist through label creation.
	settings = fdisk_new_script(context);
	if (settings == NULL)
		return -ENOMEM;
	snprintf(first_usable_lba, sizeof(first_usable_lba), "%d", GPT_FIRST_USABLE_LBA);
	status = fdisk_script_set_header(settings, "first-lba", first_usable_lba);
	if (status == 0)
		status = fdisk_set_script(context, settings);
	if (status == 0)
		status = fdisk_create_disklabel(context, "gpt");
	fdisk_set_script(context, NULL);
	fdisk_unref_script(settings);
	if (status != 0)
		return status;
	status = fdisk_gpt_set_npartitions(context, GPT_ENTRY_COUNT);
	if (status != 0)
		return status;
	type = fdisk_label_get_parttype_from_string(fdisk_get_label(context, "gpt"), main_type);
	if (type == NULL)
		return -ENOTSUP;
	partition = fdisk_new_partition();
	if (partition == NULL)
		return -ENOMEM;
	status = fdisk_partition_set_partno(partition, 0);
	if (status == 0)
		status = fdisk_partition_set_start(partition, result->offset / GPT_SECTOR_BYTES);
	if (status == 0)
		status = fdisk_partition_set_size(partition, result->size / GPT_SECTOR_BYTES);
	if (status == 0)
		status = fdisk_partition_size_explicit(partition, 1);
	if (status == 0)
		status = fdisk_partition_set_type(partition, type);
	if (status == 0)
		status = fdisk_partition_set_name(partition, main_name);
	if (status == 0)
		status = fdisk_add_partition(context, partition, &number);
	fdisk_unref_partition(partition);
	if (status != 0)
		return status;
	return number == 0 ? 0 : -EBADMSG;
}

static int CheckGptBounds(struct fdisk_context* context, const GPT_IMAGE_OPTIONS* options)
{
	const uint64_t total_sectors = options->disk_size / GPT_SECTOR_BYTES;
	const uint64_t backup_header_lba = total_sectors - 1;
	const uint64_t last_usable_lba = backup_header_lba - GPT_TRAILING_SECTORS;
	const struct { int id; uint64_t value; } fields[] = {
		{ GPT_LABELITEM_FIRSTLBA, GPT_FIRST_USABLE_LBA },
		{ GPT_LABELITEM_LASTLBA, last_usable_lba },
		{ GPT_LABELITEM_ALTLBA, backup_header_lba },
		{ GPT_LABELITEM_ENTRIESLBA, GPT_PRIMARY_ENTRIES_LBA },
		{ GPT_LABELITEM_ENTRIESALLOC, GPT_ENTRY_COUNT }
	};
	struct fdisk_labelitem* item = fdisk_new_labelitem();
	uint64_t value;
	int error = 0;

	if (item == NULL)
		return ENOMEM;
	for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
		const bool read_item = fdisk_get_disklabel_item(context, fields[i].id, item) == 0 &&
			fdisk_labelitem_get_data_u64(item, &value) == 0;
		const bool expected_value = read_item && value == fields[i].value;
		if (!expected_value) {
			error = EBADMSG;
			break;
		}
	}
	fdisk_unref_labelitem(item);
	return error;
}

static int CheckGptLayout(struct fdisk_context* context, const GPT_IMAGE_OPTIONS* options,
	const GPT_IMAGE_RESULT* result)
{
	struct fdisk_table* table = NULL;
	struct fdisk_partition* partition;
	struct fdisk_parttype* type;
	const char* name;
	const char* type_guid;
	int error = EBADMSG;

	if (!fdisk_is_label(context, GPT))
		return EBADMSG;
	if (CheckGptBounds(context, options) != 0)
		return EBADMSG;
	if (fdisk_verify_disklabel(context) != 0)
		return EBADMSG;
	if (fdisk_get_partitions(context, &table) != 0)
		return EBADMSG;
	if (fdisk_table_get_nents(table) != 1)
		goto out;
	partition = fdisk_table_get_partition(table, 0);
	type = fdisk_partition_get_type(partition);
	name = fdisk_partition_get_name(partition);
	type_guid = type != NULL ? fdisk_parttype_get_string(type) : NULL;
	const bool type_matches = type_guid != NULL && strcasecmp(type_guid, main_type) == 0;
	const bool name_matches = name != NULL && strcmp(name, main_name) == 0;
	const bool partition_number_matches = fdisk_partition_get_partno(partition) == 0;
	const bool offset_matches = fdisk_partition_get_start(partition) == result->offset / GPT_SECTOR_BYTES;
	const bool size_matches = fdisk_partition_get_size(partition) == result->size / GPT_SECTOR_BYTES;
	if (!type_matches || !name_matches)
		goto out;
	if (partition_number_matches && offset_matches && size_matches)
		error = 0;
out:
	fdisk_unref_table(table);
	return error;
}

static int ReadBackGpt(int fd, const GPT_IMAGE_OPTIONS* options, const GPT_IMAGE_RESULT* result)
{
	struct fdisk_context* context = fdisk_new_context();
	int error = ENOMEM;

	if (context == NULL)
		return error;
	fdisk_disable_dialogs(context, 1);
	error = -fdisk_assign_device_by_fd(context, fd, "Rufus target", 1);
	if (error == 0) {
		// Recovery from one damaged GPT copy is useful for discovery, but cannot
		// count as successful verification of the table we just wrote.
		const bool recovered_damaged_label = fdisk_has_label(context) &&
			fdisk_label_is_changed(fdisk_get_label(context, NULL));
		if (recovered_damaged_label)
			error = EBADMSG;
		else
			error = CheckGptLayout(context, options, result);
	}
	fdisk_unref_context(context);
	return error;
}

int CreateGptImagePartition(int fd, const GPT_IMAGE_OPTIONS* options, GPT_IMAGE_RESULT* result)
{
	struct fdisk_context* context = NULL;
	off_t position;
	int error;

	if (result == NULL)
		return EINVAL;
	memset(result, 0, sizeof(*result));
	if (options == NULL)
		return EINVAL;
	const bool supported_sector_size = options->sector_size == GPT_SECTOR_BYTES;
	const bool supported_track_size = options->sectors_per_track > 0 &&
		options->sectors_per_track <= GPT_MAX_SECTORS_PER_TRACK;
	const bool cluster_in_range = options->cluster_size >= GPT_SECTOR_BYTES &&
		options->cluster_size <= GPT_MAX_CLUSTER_BYTES;
	const bool cluster_is_power_of_two = options->cluster_size != 0 &&
		(options->cluster_size & (options->cluster_size - 1)) == 0;
	const bool sector_aligned = options->disk_size % GPT_SECTOR_BYTES == 0;
	const bool supported_capacity = options->disk_size >= GPT_MIN_IMAGE_BYTES &&
		options->disk_size <= INT64_MAX;
	if (!supported_sector_size || !supported_track_size)
		return EINVAL;
	if (!cluster_in_range || !cluster_is_power_of_two)
		return EINVAL;
	if (!sector_aligned || !supported_capacity)
		return EINVAL;
	error = CheckImageTarget(fd, options);
	if (error != 0)
		return error;
	const uint64_t data_end = options->disk_size - GPT_TRAILING_SECTORS * GPT_SECTOR_BYTES;
	result->offset = GetPartitionStartOffset(GPT_SECTOR_BYTES, options->sectors_per_track, options->cluster_size, false);
	result->size = GetMainPartitionSize(result->offset, data_end,
		GPT_SECTOR_BYTES, options->sectors_per_track, options->cluster_size);
	if (result->size == 0)
		return ENOSPC;
	position = lseek(fd, 0, SEEK_CUR);
	if (position < 0)
		return errno;
	context = fdisk_new_context();
	error = ENOMEM;
	if (context == NULL)
		goto out;
	error = -PrepareGpt(context, fd, options, result);
	if (error == 0)
		error = CheckImageTarget(fd, options);
	if (error == 0)
		error = CheckGptLayout(context, options, result);
	if (error == 0)
		error = CheckImageTarget(fd, options);
	if (error != 0)
		goto out;
	result->started = true;
	error = -fdisk_write_disklabel(context);
	while (fsync(fd) != 0) {
		if (errno == EINTR)
			continue;
		result->sync_error = errno;
		break;
	}
	if (error == 0)
		error = result->sync_error;
	if (error == 0)
		error = CheckImageTarget(fd, options);
	if (error == 0)
		error = ReadBackGpt(fd, options, result);
out:
	if (context != NULL)
		fdisk_unref_context(context);
	const bool position_restored = lseek(fd, position, SEEK_SET) == position;
	if (!position_restored && error == 0)
		error = errno;
	return error;
}
