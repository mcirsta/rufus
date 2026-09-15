/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "partition_image.h"
#include "partition_layout.h"
#include "rufus_reporting.h"

#include <libfdisk.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static const char main_type[] = "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7";
static const char main_name[] = "Main Data Partition";

static int CheckImageTarget(int fd, const GPT_IMAGE_OPTIONS* options)
{
	struct stat info;
	int flags = fcntl(fd, F_GETFL);

	if (flags < 0 || fstat(fd, &info) != 0)
		return errno;
	if (!S_ISREG(info.st_mode))
		return ENOTSUP;
	if ((flags & O_ACCMODE) != O_RDWR || (flags & (O_APPEND | O_DIRECT | O_PATH)))
		return EINVAL;
	if (info.st_size < 0 || (uint64_t)info.st_size != options->disk_size)
		return ESTALE;
	return IsRufusJobCancelled() ? ECANCELED : 0;
}

static int PrepareGpt(struct fdisk_context* context, int fd,
	const GPT_IMAGE_OPTIONS* options, const GPT_IMAGE_RESULT* result)
{
	struct fdisk_partition* partition = NULL;
	struct fdisk_parttype* type;
	struct fdisk_script* settings;
	size_t number = 0;
	int status;

	fdisk_disable_dialogs(context, 1);
	fdisk_enable_wipe(context, 0);
	fdisk_enable_bootbits_protection(context, 0);
	status = fdisk_assign_device_by_fd(context, fd, "Rufus image", 0);
	if (status != 0) return status;
	if (fdisk_get_sector_size(context) != 512 || fdisk_get_nsectors(context) != options->disk_size / 512)
		return -ESTALE;
	// Preserve the Windows usable range, rather than libfdisk's 1 MiB default
	// for the first usable sector. The actual partition still starts at 1 MiB.
	// A context alignment override alone is reset when replacing an old label.
	// The library's in-memory header settings persist through label creation.
	settings = fdisk_new_script(context);
	if (settings == NULL) return -ENOMEM;
	status = fdisk_script_set_header(settings, "first-lba", "34");
	if (!status) status = fdisk_set_script(context, settings);
	if (!status) status = fdisk_create_disklabel(context, "gpt");
	fdisk_set_script(context, NULL);
	fdisk_unref_script(settings);
	if (status != 0) return status;
	status = fdisk_gpt_set_npartitions(context, 128);
	if (status != 0) return status;
	type = fdisk_label_get_parttype_from_string(fdisk_get_label(context, "gpt"), main_type);
	if (type == NULL) return -ENOTSUP;
	partition = fdisk_new_partition();
	if (partition == NULL) return -ENOMEM;
	status = fdisk_partition_set_partno(partition, 0);
	if (!status) status = fdisk_partition_set_start(partition, result->offset / 512);
	if (!status) status = fdisk_partition_set_size(partition, result->size / 512);
	if (!status) status = fdisk_partition_size_explicit(partition, 1);
	if (!status) status = fdisk_partition_set_type(partition, type);
	if (!status) status = fdisk_partition_set_name(partition, main_name);
	if (!status) status = fdisk_add_partition(context, partition, &number);
	fdisk_unref_partition(partition);
	if (status != 0) return status;
	return number == 0 ? 0 : -EBADMSG;
}

static int CheckGptBounds(struct fdisk_context* context, const GPT_IMAGE_OPTIONS* options)
{
	const struct { int id; uint64_t value; } fields[] = {
		{ GPT_LABELITEM_FIRSTLBA, 34 }, { GPT_LABELITEM_LASTLBA, options->disk_size / 512 - 34 },
		{ GPT_LABELITEM_ALTLBA, options->disk_size / 512 - 1 },
		{ GPT_LABELITEM_ENTRIESLBA, 2 }, { GPT_LABELITEM_ENTRIESALLOC, 128 }
	};
	struct fdisk_labelitem* item = fdisk_new_labelitem();
	uint64_t value;
	int error = 0;

	if (item == NULL) return ENOMEM;
	for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
		if (fdisk_get_disklabel_item(context, fields[i].id, item) != 0 ||
			fdisk_labelitem_get_data_u64(item, &value) != 0 || value != fields[i].value) {
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
	int error = EBADMSG;

	if (!fdisk_is_label(context, GPT) || CheckGptBounds(context, options) != 0 ||
		fdisk_verify_disklabel(context) != 0 || fdisk_get_partitions(context, &table) != 0)
		return EBADMSG;
	if (fdisk_table_get_nents(table) != 1)
		goto out;
	partition = fdisk_table_get_partition(table, 0);
	type = fdisk_partition_get_type(partition);
	name = fdisk_partition_get_name(partition);
	if (type != NULL && name != NULL && fdisk_parttype_get_string(type) != NULL &&
		strcasecmp(fdisk_parttype_get_string(type), main_type) == 0 && strcmp(name, main_name) == 0 &&
		fdisk_partition_get_partno(partition) == 0 &&
		fdisk_partition_get_start(partition) == result->offset / 512 &&
		fdisk_partition_get_size(partition) == result->size / 512)
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
	error = -fdisk_assign_device_by_fd(context, fd, "Rufus image", 1);
	if (error == 0) {
		// Recovery from one damaged GPT copy is useful for discovery, but cannot
		// count as successful verification of the table we just wrote.
		if (fdisk_has_label(context) && fdisk_label_is_changed(fdisk_get_label(context, NULL)))
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
	if (options == NULL || options->sector_size != 512 || options->sectors_per_track == 0 ||
		options->sectors_per_track > 63 || options->cluster_size < 512 || options->cluster_size > 32768 ||
		(options->cluster_size & (options->cluster_size - 1)) || options->disk_size % 512 ||
		options->disk_size < 2 * 1024 * 1024 || options->disk_size > INT64_MAX)
		return EINVAL;
	error = CheckImageTarget(fd, options);
	if (error != 0)
		return error;
	result->offset = GetPartitionStartOffset(512, options->sectors_per_track, options->cluster_size, false);
	result->size = GetMainPartitionSize(result->offset, options->disk_size - 33 * 512,
		512, options->sectors_per_track, options->cluster_size);
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
	if (error == 0) error = CheckImageTarget(fd, options);
	if (error == 0) error = CheckGptLayout(context, options, result);
	if (error == 0) error = CheckImageTarget(fd, options);
	if (error != 0)
		goto out;
	result->started = true;
	error = -fdisk_write_disklabel(context);
	while (fsync(fd) != 0) {
		if (errno == EINTR) continue;
		result->sync_error = errno;
		break;
	}
	if (error == 0) error = result->sync_error;
	if (error == 0) error = CheckImageTarget(fd, options);
	if (error == 0) error = ReadBackGpt(fd, options, result);
out:
	if (context != NULL) fdisk_unref_context(context);
	if (lseek(fd, position, SEEK_SET) != position && error == 0) error = errno;
	return error;
}
