/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "partition_image.h"
#include "target_io.h"
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
	IMAGE_SECTOR_BYTES = 512,
	GPT_ENTRY_COUNT = 128,
	GPT_ENTRY_BYTES = 128,
	GPT_TABLE_SECTORS = GPT_ENTRY_COUNT * GPT_ENTRY_BYTES / IMAGE_SECTOR_BYTES,
	GPT_PRIMARY_ENTRIES_LBA = 2,  // Protective MBR and primary header precede the table.
	GPT_FIRST_USABLE_LBA = GPT_PRIMARY_ENTRIES_LBA + GPT_TABLE_SECTORS,
	GPT_TRAILING_SECTORS = GPT_TABLE_SECTORS + 1,  // Backup table and its header.
	IMAGE_MIN_BYTES = 2 * 1024 * 1024,
	IMAGE_MAX_SECTORS_PER_TRACK = 63,
	FAT32_MAX_CLUSTER_BYTES = 64 * 1024,
	FAT16_MAX_CLUSTER_BYTES = 64 * 1024,
	IMAGE_MAX_CLUSTER_BYTES = 32 * 1024 * 1024, // exFAT; each formatter validates its own limits.
	IMAGE_BUFFER_BYTES = 64 * 1024,
	CLEAR_TAIL_DIVISOR = 8
};

enum {
	WINDOWS_ESP_BYTES = 260 * 1024 * 1024,
	WINDOWS_MSR_BYTES = 128 * 1024 * 1024
};

enum {
	MBR_BOOT_BYTES = 440,
	MBR_DISK_SIGNATURE_OFFSET = MBR_BOOT_BYTES,
	MBR_TABLE_OFFSET = 446,
	MBR_ENTRY_BYTES = 16,
	MBR_TABLE_ENTRIES = 4,
	MBR_ACTIVE = 0x80,
	MBR_MASQUERADE = 0x81,
	MBR_FAT32_LBA = 0x0c,
	MBR_FAT16_LBA = 0x0e,
	MBR_NTFS_EXFAT = 0x07,
	MBR_LINUX = 0x83,
	MBR_ESP = 0xef,
	MBR_BIOS_COMPAT = 0xea,
	NTFS_MAX_CLUSTER_BYTES = 64 * 1024,
	EXT_MIN_DISK_BYTES = 256 * 1024 * 1024
};

static const char main_type[] = "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7";
static const char esp_type[] = "C12A7328-F81F-11D2-BA4B-00A0C93EC93B";
static const char esp_name[] = "EFI System Partition";
static const char msr_type[] = "E3C9E316-0B5C-4DB8-817D-F92DF00215AE";
static const char msr_name[] = "Microsoft Reserved Partition";
static const char persistence_type[] = "0FC63DAF-8483-4772-8E79-3D69D8477DE4";
static const char persistence_name[] = "Linux Persistence";
static const char main_name[] = "Main Data Partition";
static const char uefi_ntfs_name[] = "UEFI:NTFS";
static const uint64_t no_drive_letter = UINT64_C(1) << 63;

static bool ImageSectorGeometry(uint32_t logical, uint32_t physical)
{
	if (physical == 0) physical = logical;
	const bool logical_valid = logical >= 512 && logical <= 4096 && (logical & (logical - 1)) == 0;
	const bool physical_valid = physical >= logical && physical <= 65536 && (physical & (physical - 1)) == 0;
	return logical_valid && physical_valid;
}

static int CheckImageTarget(int fd, uint64_t disk_size, uint64_t device,
	uint32_t sector_size, uint32_t physical_sector_size)
{
	WRITABLE_TARGET target;
	int error = InspectWritableTarget(fd, device, &target);
	if (error != 0) return error;
	if (physical_sector_size == 0) physical_sector_size = sector_size;
	if (device != 0 && (target.sector_size != sector_size || target.physical_sector_size != physical_sector_size))
		return ESTALE;
	if (target.size != disk_size) return ESTALE;
	return IsRufusJobCancelled() ? ECANCELED : 0;
}

static int AssignImageDevice(struct fdisk_context* context, int fd, bool read_only,
	uint32_t sector_size, uint32_t physical_sector_size)
{
	if (physical_sector_size == 0) physical_sector_size = sector_size;
	int status = fdisk_save_user_sector_size(context, physical_sector_size, sector_size);
	if (status == 0) status = fdisk_assign_device_by_fd(context, fd, "Rufus target", read_only);
	return status;
}

typedef struct {
	uint64_t offset, size, attributes;
	const char* name;
	const char* type;
} GPT_PARTITION_VALUES;

static GPT_PARTITION_VALUES GptPartitionValues(const GPT_IMAGE_OPTIONS* options,
	const GPT_IMAGE_RESULT* result, size_t number)
{
	if (options->windows_to_go) {
		if (number == 0)
			return (GPT_PARTITION_VALUES){result->esp_offset, result->esp_size, 0, esp_name, esp_type};
		if (number == 1)
			return (GPT_PARTITION_VALUES){result->msr_offset, result->msr_size, 0, msr_name, msr_type};
		return (GPT_PARTITION_VALUES){result->offset, result->size, 0, main_name, main_type};
	}
	const bool persistence = number == 1 && result->persistence_size != 0;
	const bool uefi_ntfs = number != 0 && !persistence;
	const uint64_t offset = persistence ? result->persistence_offset : uefi_ntfs ? result->uefi_ntfs_offset : result->offset;
	const uint64_t size = persistence ? result->persistence_size : uefi_ntfs ? result->uefi_ntfs_size : result->size;
	const bool esp = number == 0 && options->efi_system;
	const char* name = persistence ? persistence_name : uefi_ntfs ? uefi_ntfs_name : esp ? esp_name : main_name;
	return (GPT_PARTITION_VALUES){offset, size, uefi_ntfs ? no_drive_letter : 0, name,
		persistence ? persistence_type : esp ? esp_type : main_type};
}

static size_t GptPartitionCount(const GPT_IMAGE_OPTIONS* options)
{
	return options->windows_to_go ? 3 : 1 + (options->persistence_size != 0) + (options->uefi_ntfs_size != 0);
}

static int AddGptPartition(struct fdisk_context* context, const GPT_IMAGE_OPTIONS* options,
	const GPT_IMAGE_RESULT* result, size_t expected_number)
{
	const GPT_PARTITION_VALUES values = GptPartitionValues(options, result, expected_number);
	struct fdisk_parttype* type = fdisk_label_get_parttype_from_string(fdisk_get_label(context, "gpt"),
		values.type);
	struct fdisk_partition* partition;
	size_t number = 0;
	int status;

	if (type == NULL)
		return -ENOTSUP;
	partition = fdisk_new_partition();
	if (partition == NULL)
		return -ENOMEM;
	status = fdisk_partition_set_partno(partition, expected_number);
	if (status == 0)
		status = fdisk_partition_set_start(partition, values.offset / options->sector_size);
	if (status == 0)
		status = fdisk_partition_set_size(partition, values.size / options->sector_size);
	if (status == 0)
		status = fdisk_partition_size_explicit(partition, 1);
	if (status == 0)
		status = fdisk_partition_set_type(partition, type);
	if (status == 0)
		status = fdisk_partition_set_name(partition, values.name);
	if (status == 0)
		status = fdisk_add_partition(context, partition, &number);
	fdisk_unref_partition(partition);
	if (status != 0)
		return status;
	if (number != expected_number)
		return -EBADMSG;
	// Same GPT type/attribute as drive.c. Declaring the boot partition as an
	// ESP can interfere with Windows Setup; it must remain Basic Data.
	return fdisk_gpt_set_partition_attrs(context, number, values.attributes);
}

static int PrepareGpt(struct fdisk_context* context, int fd,
	const GPT_IMAGE_OPTIONS* options, const GPT_IMAGE_RESULT* result)
{
	struct fdisk_script* settings;
	char first_usable_lba[32], last_usable_lba[32];
	int status;

	fdisk_disable_dialogs(context, 1);
	fdisk_enable_wipe(context, 0);
	fdisk_enable_bootbits_protection(context, 0);
	status = AssignImageDevice(context, fd, false, options->sector_size, options->physical_sector_size);
	if (status != 0)
		return status;
	const bool expected_sector_size = fdisk_get_sector_size(context) == options->sector_size;
	const bool expected_sector_count = fdisk_get_nsectors(context) == options->disk_size / options->sector_size;
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
	snprintf(last_usable_lba, sizeof(last_usable_lba), "%llu",
		(unsigned long long)(options->disk_size / options->sector_size - GPT_TRAILING_SECTORS - 1));
	status = fdisk_script_set_header(settings, "first-lba", first_usable_lba);
	if (status == 0) status = fdisk_script_set_header(settings, "last-lba", last_usable_lba);
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
	const size_t count = GptPartitionCount(options);
	for (size_t i = 0; status == 0 && i < count; i++)
		status = AddGptPartition(context, options, result, i);
	return status;
}

static int CheckGptBounds(struct fdisk_context* context, const GPT_IMAGE_OPTIONS* options)
{
	const uint64_t total_sectors = options->disk_size / options->sector_size;
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

static int CheckGptPartition(struct fdisk_context* context, struct fdisk_partition* partition,
	const GPT_IMAGE_OPTIONS* options, const GPT_IMAGE_RESULT* result, size_t number)
{
	if (partition == NULL)
		return EBADMSG;
	struct fdisk_parttype* type = fdisk_partition_get_type(partition);
	const char* name = fdisk_partition_get_name(partition);
	const char* type_guid = type != NULL ? fdisk_parttype_get_string(type) : NULL;
	const GPT_PARTITION_VALUES values = GptPartitionValues(options, result, number);
	uint64_t attrs = 0;
	const bool type_matches = type_guid != NULL && strcasecmp(type_guid, values.type) == 0;
	const bool name_matches = name != NULL && strcmp(name, values.name) == 0;
	const bool partition_number_matches = fdisk_partition_get_partno(partition) == number;
	const bool offset_matches = fdisk_partition_get_start(partition) == values.offset / options->sector_size;
	const bool size_matches = fdisk_partition_get_size(partition) == values.size / options->sector_size;
	const bool attrs_read = fdisk_gpt_get_partition_attrs(context, number, &attrs) == 0;
	const bool attrs_match = attrs_read && attrs == values.attributes;
	if (!type_matches || !name_matches || !attrs_match)
		return EBADMSG;
	if (!partition_number_matches || !offset_matches || !size_matches)
		return EBADMSG;
	return 0;
}

static int CheckGptLayout(struct fdisk_context* context, const GPT_IMAGE_OPTIONS* options,
	const GPT_IMAGE_RESULT* result)
{
	struct fdisk_table* table = NULL;
	const size_t count = GptPartitionCount(options);
	int error = EBADMSG;

	if (!fdisk_is_label(context, GPT))
		return EBADMSG;
	if (CheckGptBounds(context, options) != 0)
		return EBADMSG;
	if (fdisk_verify_disklabel(context) != 0)
		return EBADMSG;
	if (fdisk_get_partitions(context, &table) != 0)
		return EBADMSG;
	if (fdisk_table_get_nents(table) != count)
		goto out;
	for (size_t i = 0; i < count; i++) {
		error = CheckGptPartition(context, fdisk_table_get_partition(table, i), options, result, i);
		if (error != 0)
			break;
	}
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
	error = -AssignImageDevice(context, fd, true, options->sector_size, options->physical_sector_size);
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

static uint64_t NextWindowsPartition(uint64_t offset, uint64_t size,
	const GPT_IMAGE_OPTIONS* options)
{
	const uint64_t track_bytes = (uint64_t)options->sector_size * options->sectors_per_track;
	if (size > UINT64_MAX - offset) return 0;
	const uint64_t end = offset + size;
	if (end > UINT64_MAX - (track_bytes - 1)) return 0;
	const uint64_t next = ((end + track_bytes - 1) / track_bytes * track_bytes)
		/ options->cluster_size * options->cluster_size;
	return next >= end ? next : 0;
}

int PlanGptImageLayout(const GPT_IMAGE_OPTIONS* options, GPT_IMAGE_RESULT* result)
{
	if (result == NULL)
		return EINVAL;
	memset(result, 0, sizeof(*result));
	if (options == NULL)
		return EINVAL;
	if (options->efi_system && options->uefi_ntfs_size != 0) return EINVAL;
	if (options->sector_size != 512 && options->uefi_ntfs_size != 0) return ENOTSUP;
	const bool conflicting_windows_layout = options->windows_to_go &&
		(options->efi_system || options->uefi_ntfs_size != 0 || options->persistence_size != 0);
	if (conflicting_windows_layout) return EINVAL;
	const bool supported_sector_size = ImageSectorGeometry(options->sector_size, options->physical_sector_size);
	const bool supported_track_size = options->sectors_per_track > 0 &&
		options->sectors_per_track <= IMAGE_MAX_SECTORS_PER_TRACK;
	const bool cluster_in_range = options->cluster_size >= options->sector_size &&
		options->cluster_size <= IMAGE_MAX_CLUSTER_BYTES;
	const bool cluster_is_power_of_two = options->cluster_size != 0 &&
		(options->cluster_size & (options->cluster_size - 1)) == 0;
	const bool sector_aligned = options->sector_size != 0 && options->disk_size % options->sector_size == 0;
	const bool supported_capacity = options->disk_size >= IMAGE_MIN_BYTES &&
		options->disk_size <= INT64_MAX;
	if (!supported_sector_size || !supported_track_size)
		return EINVAL;
	if (!cluster_in_range || !cluster_is_power_of_two)
		return EINVAL;
	if (!sector_aligned || !supported_capacity)
		return EINVAL;
	uint64_t data_end = options->disk_size - GPT_TRAILING_SECTORS * options->sector_size;
	if (options->uefi_ntfs_size != 0) {
		result->uefi_ntfs_size = GetTrailingPartitionSize(options->uefi_ntfs_size,
			options->sector_size, options->sectors_per_track);
		result->uefi_ntfs_offset = GetTrailingPartitionOffset(data_end, result->uefi_ntfs_size,
			options->sector_size, options->sectors_per_track);
		if (result->uefi_ntfs_offset == 0)
			return ENOSPC;
		data_end = result->uefi_ntfs_offset;
	}
	if (options->persistence_size != 0) {
		if (options->persistence_size < EXT_MIN_DISK_BYTES) return EINVAL;
		if (options->persistence_size >= options->disk_size) return ENOSPC;
		result->persistence_size = GetTrailingPartitionSize(options->persistence_size,
			options->sector_size, options->sectors_per_track);
		result->persistence_offset = GetTrailingPartitionOffset(data_end, result->persistence_size,
			options->sector_size, options->sectors_per_track);
		if (result->persistence_offset == 0) return ENOSPC;
		data_end = result->persistence_offset;
	}
	result->offset = GetPartitionStartOffset(options->sector_size, options->sectors_per_track, options->cluster_size, false);
	if (options->windows_to_go) {
		result->esp_offset = result->offset;
		result->esp_size = WINDOWS_ESP_BYTES;
		result->msr_offset = NextWindowsPartition(result->esp_offset, result->esp_size, options);
		result->msr_size = WINDOWS_MSR_BYTES;
		if (result->msr_offset == 0) return ENOSPC;
		result->offset = NextWindowsPartition(result->msr_offset, result->msr_size, options);
		if (result->offset == 0 || result->offset >= data_end) return ENOSPC;
	}
	result->size = GetMainPartitionSize(result->offset, data_end,
		options->sector_size, options->sectors_per_track, options->cluster_size);
	if (result->size == 0)
		return ENOSPC;
	return 0;
}

int PlanGptImagePartition(int fd, const GPT_IMAGE_OPTIONS* options, GPT_IMAGE_RESULT* result)
{
	GPT_IMAGE_RESULT layout;
	int error;

	if (result == NULL)
		return EINVAL;
	memset(result, 0, sizeof(*result));
	error = PlanGptImageLayout(options, &layout);
	if (error == 0)
		error = CheckImageTarget(fd, options->disk_size, options->device, options->sector_size, options->physical_sector_size);
	if (error == 0)
		*result = layout;
	return error;
}

int CheckGptImagePartition(int fd, const GPT_IMAGE_OPTIONS* options, GPT_IMAGE_RESULT* result)
{
	GPT_IMAGE_RESULT layout;
	if (result == NULL)
		return EINVAL;
	memset(result, 0, sizeof(*result));
	int error = PlanGptImagePartition(fd, options, &layout);
	if (error != 0)
		return error;
	off_t position = lseek(fd, 0, SEEK_CUR);
	if (position < 0)
		return errno;
	error = ReadBackGpt(fd, options, &layout);
	if (lseek(fd, position, SEEK_SET) != position && error == 0)
		error = errno;
	if (error == 0)
		error = CheckImageTarget(fd, options->disk_size, options->device, options->sector_size, options->physical_sector_size);
	if (error == 0)
		*result = layout;
	return error;
}

int CreateGptImagePartition(int fd, const GPT_IMAGE_OPTIONS* options, GPT_IMAGE_RESULT* result)
{
	struct fdisk_context* context = NULL;
	off_t position;
	int error = PlanGptImagePartition(fd, options, result);
	if (error != 0)
		return error;
	position = lseek(fd, 0, SEEK_CUR);
	if (position < 0)
		return errno;
	context = fdisk_new_context();
	error = ENOMEM;
	if (context == NULL)
		goto out;
	error = -PrepareGpt(context, fd, options, result);
	if (error == 0)
		error = CheckImageTarget(fd, options->disk_size, options->device, options->sector_size, options->physical_sector_size);
	if (error == 0)
		error = CheckGptLayout(context, options, result);
	if (error == 0)
		error = CheckImageTarget(fd, options->disk_size, options->device, options->sector_size, options->physical_sector_size);
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
		error = CheckImageTarget(fd, options->disk_size, options->device, options->sector_size, options->physical_sector_size);
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

int PlanMbrImageLayout(const MBR_IMAGE_OPTIONS* options, MBR_IMAGE_RESULT* result)
{
	if (result == NULL)
		return EINVAL;
	memset(result, 0, sizeof(*result));
	if (options == NULL)
		return EINVAL;
	const bool fat = options->file_system == FS_FAT16 || options->file_system == FS_FAT32;
	const bool native_data = options->file_system == FS_NTFS || options->file_system == FS_EXFAT;
	const bool ext = options->file_system == FS_EXT2 || options->file_system == FS_EXT3;
	const bool udf = options->file_system == FS_UDF;
	const bool supported_boot_code = options->boot_code == MBR_BOOT_ZERO ||
		options->boot_code == MBR_BOOT_SYSLINUX || options->boot_code == MBR_BOOT_RUFUS ||
		options->boot_code == MBR_BOOT_WIN7 || options->boot_code == MBR_BOOT_GRUB2 ||
		options->boot_code == MBR_BOOT_RUFUS_MASQUERADE || options->boot_code == MBR_BOOT_GRUB4DOS ||
		options->boot_code == MBR_BOOT_REACTOS || options->boot_code == MBR_BOOT_KOLIBRIOS;
	if (!supported_boot_code)
		return ENOTSUP;
	const bool bios_boot = options->boot_code != MBR_BOOT_ZERO;
	if (options->sector_size != 512 && (bios_boot || options->uefi_ntfs_size != 0)) return ENOTSUP;
	if (options->masquerade_disk && !bios_boot)
		return ENOTSUP;
	if (options->old_bios_fixes && !bios_boot) return ENOTSUP;
	const bool windows_boot = options->boot_code == MBR_BOOT_RUFUS || options->boot_code == MBR_BOOT_WIN7 ||
		options->boot_code == MBR_BOOT_RUFUS_MASQUERADE;
	const bool ntfs_boot_code = windows_boot || options->boot_code == MBR_BOOT_SYSLINUX ||
		options->boot_code == MBR_BOOT_GRUB2 || options->boot_code == MBR_BOOT_GRUB4DOS;
	const bool ntfs_bios = options->file_system == FS_NTFS && ntfs_boot_code;
	const bool ext_grub2 = ext && options->boot_code == MBR_BOOT_GRUB2;
	const bool default_data = !bios_boot && (ext || udf);
	const bool supported_filesystem = fat || ntfs_bios || ext_grub2 || default_data || (!bios_boot && native_data);
	if (!supported_filesystem)
		return ENOTSUP;
	const bool supported_payload = options->uefi_ntfs_size == 0 || ntfs_bios || (native_data && !bios_boot);
	if (!supported_payload)
		return ENOTSUP;
	uint32_t max_cluster_bytes;
	switch (options->file_system) {
	case FS_FAT16: max_cluster_bytes = FAT16_MAX_CLUSTER_BYTES; break;
	case FS_FAT32: max_cluster_bytes = FAT32_MAX_CLUSTER_BYTES; break;
	case FS_NTFS: max_cluster_bytes = NTFS_MAX_CLUSTER_BYTES; break;
	case FS_EXT2:
	case FS_EXT3: max_cluster_bytes = RUFUS_EXT_DEFAULT_CLUSTER; break;
	default: max_cluster_bytes = IMAGE_MAX_CLUSTER_BYTES; break;
	}
	const bool supported_sectors = ImageSectorGeometry(options->sector_size, options->physical_sector_size);
	const bool supported_tracks = options->sectors_per_track > 0 &&
		options->sectors_per_track <= IMAGE_MAX_SECTORS_PER_TRACK;
	// Rufus's sole ext UI choice is its 0x100 "Default" sentinel. Its
	// partition layout uses that value; FormatExtFs selects the actual block size.
	const bool cluster_in_range = (ext_grub2 || default_data) ? options->cluster_size == RUFUS_EXT_DEFAULT_CLUSTER :
		options->cluster_size >= options->sector_size && options->cluster_size <= max_cluster_bytes;
	const bool cluster_power_of_two = options->cluster_size != 0 &&
		(options->cluster_size & (options->cluster_size - 1)) == 0;
	const bool disk_aligned = options->sector_size != 0 && options->disk_size % options->sector_size == 0;
	const uint64_t max_disk_bytes = (UINT64_C(1) << 32) * options->sector_size;
	const bool disk_in_range = options->disk_size >= IMAGE_MIN_BYTES && options->disk_size <= max_disk_bytes;
	const bool ext_size_available = !ext || options->disk_size >= EXT_MIN_DISK_BYTES;
	if (!supported_sectors || !supported_tracks)
		return EINVAL;
	if (!cluster_in_range || !cluster_power_of_two)
		return EINVAL;
	if (!disk_aligned || !disk_in_range || !ext_size_available)
		return EINVAL;
	uint64_t data_end = options->disk_size;
	if (options->uefi_ntfs_size != 0) {
		result->uefi_ntfs_size = GetTrailingPartitionSize(options->uefi_ntfs_size,
			options->sector_size, options->sectors_per_track);
		result->uefi_ntfs_offset = GetTrailingPartitionOffset(data_end, result->uefi_ntfs_size,
			options->sector_size, options->sectors_per_track);
		if (result->uefi_ntfs_offset == 0)
			return ENOSPC;
		data_end = result->uefi_ntfs_offset;
	} else if (options->old_bios_fixes) {
		/* Windows uses one final track unless UEFI:NTFS occupies that role. */
		result->compatibility_size = (uint64_t)options->sector_size * options->sectors_per_track;
		result->compatibility_offset = GetTrailingPartitionOffset(data_end, result->compatibility_size,
			options->sector_size, options->sectors_per_track);
		if (result->compatibility_offset == 0) return ENOSPC;
		data_end = result->compatibility_offset;
	}
	if (options->persistence_size != 0) {
		if (options->persistence_size < EXT_MIN_DISK_BYTES) return EINVAL;
		if (options->persistence_size >= options->disk_size) return ENOSPC;
		result->persistence_size = GetTrailingPartitionSize(options->persistence_size,
			options->sector_size, options->sectors_per_track);
		result->persistence_offset = GetTrailingPartitionOffset(data_end, result->persistence_size,
			options->sector_size, options->sectors_per_track);
		if (result->persistence_offset == 0) return ENOSPC;
		data_end = result->persistence_offset;
	}
	result->offset = GetPartitionStartOffset(options->sector_size, options->sectors_per_track,
		options->cluster_size, options->old_bios_fixes);
	result->size = GetMainPartitionSize(result->offset, data_end,
		options->sector_size, options->sectors_per_track, options->cluster_size);
	return result->size != 0 ? 0 : ENOSPC;
}

static unsigned MbrPartitionType(int file_system)
{
	// Same types as drive.c:CreatePartition and format.c:WriteMBR.
	switch (file_system) {
	case FS_FAT16: return MBR_FAT16_LBA;
	case FS_FAT32: return MBR_FAT32_LBA;
	case FS_EXT2:
	case FS_EXT3: return MBR_LINUX;
	default: return MBR_NTFS_EXFAT; // Planner has already rejected other filesystems.
	}
}

static int PrepareMbr(struct fdisk_context* context, int fd,
	const MBR_IMAGE_OPTIONS* options, const MBR_IMAGE_RESULT* layout)
{
	fdisk_disable_dialogs(context, 1);
	fdisk_enable_wipe(context, 0);
	fdisk_enable_bootbits_protection(context, 0);
	int status = AssignImageDevice(context, fd, false, options->sector_size, options->physical_sector_size);
	if (status != 0)
		return status;
	const bool expected_sectors = fdisk_get_sector_size(context) == options->sector_size &&
		fdisk_get_nsectors(context) == options->disk_size / options->sector_size;
	if (!expected_sectors)
		return -ESTALE;
	// Do not leave an old GPT backup behind and call the conversion complete.
	if (fdisk_has_label(context) && !fdisk_is_label(context, DOS))
		return -ENOTSUP;
	if (options->old_bios_fixes) {
		status = fdisk_save_user_grain(context, options->sector_size);
		if (status != 0) return status;
	}
	status = fdisk_create_disklabel(context, "dos");
	// A legacy partition may precede libfdisk's ordinary 1 MiB lower bound.
	if (status == 0 && options->old_bios_fixes)
		fdisk_set_first_lba(context, layout->offset / options->sector_size);
	char signature[16];
	snprintf(signature, sizeof(signature), "0x%08x", options->disk_signature);
	if (status == 0)
		status = fdisk_set_disklabel_id_from_string(context, signature);
	if (status != 0)
		return status;
	const size_t count = 1 + (options->persistence_size != 0) + (options->uefi_ntfs_size != 0 || layout->compatibility_size != 0);
	for (size_t index = 0; index < count; index++) {
		const bool persistence = index == 1 && layout->persistence_size != 0;
		const bool boot = index != 0 && !persistence && layout->uefi_ntfs_size != 0;
		const bool compatibility = index != 0 && !persistence && !boot;
		const unsigned partition_type = persistence ? MBR_LINUX : boot ? MBR_ESP : compatibility ? MBR_BIOS_COMPAT : MbrPartitionType(options->file_system);
		struct fdisk_parttype* type = fdisk_label_get_parttype_from_code(fdisk_get_label(context, "dos"), partition_type);
		if (type == NULL)
			return -ENOTSUP;
		struct fdisk_partition* partition = fdisk_new_partition();
		if (partition == NULL)
			return -ENOMEM;
		const uint64_t offset = persistence ? layout->persistence_offset : boot ? layout->uefi_ntfs_offset : compatibility ? layout->compatibility_offset : layout->offset;
		const uint64_t size = persistence ? layout->persistence_size : boot ? layout->uefi_ntfs_size : compatibility ? layout->compatibility_size : layout->size;
		size_t number = index;
		status = fdisk_partition_set_partno(partition, index);
		if (status == 0)
			status = fdisk_partition_set_start(partition, offset / options->sector_size);
		if (status == 0)
			status = fdisk_partition_set_size(partition, size / options->sector_size);
		if (status == 0)
			status = fdisk_partition_size_explicit(partition, 1);
		if (status == 0)
			status = fdisk_partition_set_type(partition, type);
		if (status == 0)
			status = fdisk_add_partition(context, partition, &number);
		fdisk_unref_partition(partition);
		if (status != 0)
			return status;
		if (number != index)
			return -EBADMSG;
		const bool active = !boot && !persistence && !compatibility && (options->boot_code != MBR_BOOT_ZERO || options->uefi_ntfs_size != 0);
		if (active) {
			status = fdisk_toggle_partition_flag(context, number, DOS_FLAG_ACTIVE);
			if (status != 0)
				return status;
		}
	}
	return 0;
}

static int CheckMbrLayout(struct fdisk_context* context, const MBR_IMAGE_OPTIONS* options,
	const MBR_IMAGE_RESULT* layout, bool stored_table)
{
	struct fdisk_table* table = NULL;
	if (!fdisk_is_label(context, DOS) || fdisk_verify_disklabel(context) != 0)
		return EBADMSG;
	if (fdisk_get_partitions(context, &table) != 0)
		return EBADMSG;
	const size_t count = 1 + (options->persistence_size != 0) + (options->uefi_ntfs_size != 0 || layout->compatibility_size != 0);
	int error = fdisk_table_get_nents(table) == count ? 0 : EBADMSG;
	for (size_t i = 0; error == 0 && i < count; i++) {
		struct fdisk_partition* partition = fdisk_table_get_partition(table, i);
		struct fdisk_parttype* type = fdisk_partition_get_type(partition);
		const bool persistence = i == 1 && layout->persistence_size != 0;
		const bool boot = i != 0 && !persistence && layout->uefi_ntfs_size != 0;
		const bool compatibility = i != 0 && !persistence && !boot;
		const unsigned expected_type = persistence ? MBR_LINUX : boot ? MBR_ESP : compatibility ? MBR_BIOS_COMPAT : MbrPartitionType(options->file_system);
		const bool active = !boot && !persistence && !compatibility && (options->boot_code != MBR_BOOT_ZERO || options->uefi_ntfs_size != 0);
		// The in-memory DOS table uses 0x80. After publication, ReadBackMbr
		// checks the exact 0x81 byte; libfdisk considers that flag non-bootable.
		const bool masqueraded = stored_table && (options->masquerade_disk || options->boot_code == MBR_BOOT_RUFUS_MASQUERADE);
		const bool fdisk_active = active && !masqueraded;
		const uint64_t offset = persistence ? layout->persistence_offset : boot ? layout->uefi_ntfs_offset : compatibility ? layout->compatibility_offset : layout->offset;
		const uint64_t size = persistence ? layout->persistence_size : boot ? layout->uefi_ntfs_size : compatibility ? layout->compatibility_size : layout->size;
		const bool correct_type = type != NULL && fdisk_parttype_get_code(type) == expected_type;
		const bool correct_number = fdisk_partition_get_partno(partition) == i;
		const bool correct_active = !!fdisk_partition_is_bootable(partition) == fdisk_active;
		const bool correct_extent = fdisk_partition_get_start(partition) == offset / options->sector_size &&
			fdisk_partition_get_size(partition) == size / options->sector_size;
		if (!correct_type || !correct_number || !correct_active || !correct_extent)
			error = EBADMSG;
	}
	fdisk_unref_table(table);
	return error;
}

static void SetMbrCode(unsigned char* sector, MBR_BOOT_CODE boot_code)
{
	// Reuse the exact payloads from the corresponding Win32 ms-sys writers.
	#include "ms-sys/inc/mbr_syslinux.h"
	#include "ms-sys/inc/mbr_rufus.h"
	#include "ms-sys/inc/mbr_win7.h"
	#include "ms-sys/inc/mbr_grub2.h"
	#include "ms-sys/inc/mbr_grub.h"
	#include "ms-sys/inc/mbr_zero.h"
	#include "ms-sys/inc/mbr_reactos.h"
	#include "ms-sys/inc/mbr_kolibri.h"
	_Static_assert(sizeof(mbr_syslinux_0x0) == MBR_BOOT_BYTES, "Unexpected Syslinux MBR size");
	_Static_assert(sizeof(mbr_rufus_0x0) == MBR_BOOT_BYTES, "Unexpected Rufus MBR size");
	_Static_assert(sizeof(mbr_win7_0x0) == MBR_BOOT_BYTES, "Unexpected Windows 7 MBR size");
	_Static_assert(sizeof(mbr_grub2_0x0) == 432, "Unexpected GRUB2 MBR size");
	_Static_assert(sizeof(mbr_grub_0x0) == 423, "Unexpected GRUB4DOS MBR size");
	_Static_assert(sizeof(mbr_zero_0x0) == MBR_TABLE_OFFSET, "Unexpected zero MBR size");
	switch (boot_code) {
	case MBR_BOOT_SYSLINUX:
		memcpy(sector, mbr_syslinux_0x0, sizeof(mbr_syslinux_0x0));
		break;
	case MBR_BOOT_RUFUS:
	case MBR_BOOT_RUFUS_MASQUERADE:
		memcpy(sector, mbr_rufus_0x0, sizeof(mbr_rufus_0x0));
		if (boot_code == MBR_BOOT_RUFUS_MASQUERADE)
			sector[MBR_TABLE_OFFSET] = MBR_MASQUERADE;
		break;
	case MBR_BOOT_WIN7:
		memcpy(sector, mbr_win7_0x0, sizeof(mbr_win7_0x0));
		break;
	case MBR_BOOT_GRUB2:
		memcpy(sector, mbr_grub2_0x0, sizeof(mbr_grub2_0x0));
		break;
	case MBR_BOOT_GRUB4DOS:
		memcpy(sector, mbr_grub_0x0, sizeof(mbr_grub_0x0));
		break;
	case MBR_BOOT_REACTOS:
		memcpy(sector, mbr_reactos_0x0, sizeof(mbr_reactos_0x0));
		break;
	case MBR_BOOT_KOLIBRIOS:
		memcpy(sector, mbr_kolibri_0x0, sizeof(mbr_kolibri_0x0));
		break;
	case MBR_BOOT_ZERO:
		memcpy(sector, mbr_zero_0x0, sizeof(mbr_zero_0x0));
		break;
	}
	sector[IMAGE_SECTOR_BYTES - 2] = 0x55;
	sector[IMAGE_SECTOR_BYTES - 1] = 0xaa;
}

static int TransferImageBytes(int fd, unsigned char* buffer, size_t size, uint64_t offset,
	bool write_bytes, uint64_t* transferred)
{
	size_t done = 0;
	while (done < size) {
		if (IsRufusJobCancelled())
			return ECANCELED;
		ssize_t count = write_bytes ? pwrite(fd, buffer + done, size - done, (off_t)(offset + done)) :
			pread(fd, buffer + done, size - done, (off_t)(offset + done));
		if (count < 0 && errno == EINTR)
			continue;
		if (count < 0)
			return errno;
		if (count == 0)
			return EIO;
		done += (size_t)count;
		if (transferred != NULL)
			*transferred += (uint64_t)count;
	}
	return 0;
}

static int ReadBackMbr(int fd, const MBR_IMAGE_OPTIONS* options, const MBR_IMAGE_RESULT* layout)
{
	unsigned char sector[IMAGE_SECTOR_BYTES], expected[IMAGE_SECTOR_BYTES];
	int error = TransferImageBytes(fd, sector, sizeof(sector), 0, false, NULL);
	if (error != 0)
		return error;
	memcpy(expected, sector, sizeof(expected));
	SetMbrCode(expected, options->boot_code);
	const uint32_t signature = options->boot_code != MBR_BOOT_ZERO || options->retain_disk_signature ? options->disk_signature : 0;
	for (size_t i = 0; i < sizeof(options->disk_signature); i++)
		expected[MBR_DISK_SIGNATURE_OFFSET + i] = (unsigned char)(signature >> (i * 8));
	// Reserved bytes and unused entries must not retain old table information.
	memset(expected + MBR_DISK_SIGNATURE_OFFSET + sizeof(options->disk_signature), 0,
		MBR_TABLE_OFFSET - MBR_DISK_SIGNATURE_OFFSET - sizeof(options->disk_signature));
	const size_t count = 1 + (options->persistence_size != 0) + (options->uefi_ntfs_size != 0 || layout->compatibility_size != 0);
	const bool active = options->boot_code != MBR_BOOT_ZERO || options->uefi_ntfs_size != 0;
	const bool masqueraded = options->masquerade_disk || options->boot_code == MBR_BOOT_RUFUS_MASQUERADE;
	expected[MBR_TABLE_OFFSET] = masqueraded ? MBR_MASQUERADE : (active ? MBR_ACTIVE : 0);
	for (size_t i = 1; i < count; i++)
		expected[MBR_TABLE_OFFSET + i * MBR_ENTRY_BYTES] = 0;
	memset(expected + MBR_TABLE_OFFSET + count * MBR_ENTRY_BYTES, 0, (MBR_TABLE_ENTRIES - count) * MBR_ENTRY_BYTES);
	if (memcmp(sector, expected, sizeof(sector)) != 0)
		return EBADMSG;
	struct fdisk_context* context = fdisk_new_context();
	if (context == NULL)
		return ENOMEM;
	fdisk_disable_dialogs(context, 1);
	error = -AssignImageDevice(context, fd, true, options->sector_size, options->physical_sector_size);
	if (error == 0)
		error = CheckMbrLayout(context, options, layout, true);
	fdisk_unref_context(context);
	return error;
}

int CheckMbrImage(int fd, const MBR_IMAGE_OPTIONS* options, MBR_IMAGE_RESULT* result)
{
	int error = PlanMbrImageLayout(options, result);
	if (error == 0)
		error = CheckImageTarget(fd, options->disk_size, options->device, options->sector_size, options->physical_sector_size);
	if (error != 0)
		return error;
	off_t position = lseek(fd, 0, SEEK_CUR);
	if (position < 0)
		return errno;
	error = ReadBackMbr(fd, options, result);
	if (lseek(fd, position, SEEK_SET) != position && error == 0)
		error = errno;
	if (error == 0)
		error = CheckImageTarget(fd, options->disk_size, options->device, options->sector_size, options->physical_sector_size);
	return error;
}

int CreateMbrImage(int fd, const MBR_IMAGE_OPTIONS* options, MBR_IMAGE_RESULT* result)
{
	int error = PlanMbrImageLayout(options, result);
	if (error == 0)
		error = CheckImageTarget(fd, options->disk_size, options->device, options->sector_size, options->physical_sector_size);
	if (error != 0)
		return error;
	off_t position = lseek(fd, 0, SEEK_CUR);
	if (position < 0)
		return errno;
	struct fdisk_context* context = fdisk_new_context();
	if (context == NULL)
		return ENOMEM;
	error = -PrepareMbr(context, fd, options, result);
	if (error == 0)
		error = CheckMbrLayout(context, options, result, false);
	if (error == 0)
		error = CheckImageTarget(fd, options->disk_size, options->device, options->sector_size, options->physical_sector_size);
	if (error == 0) {
		result->started = true;
		error = -fdisk_write_disklabel(context);
		if (error == 0 && IsRufusJobCancelled())
			error = ECANCELED;
		unsigned char sector[IMAGE_SECTOR_BYTES];
		if (error == 0)
			error = TransferImageBytes(fd, sector, sizeof(sector), 0, false, NULL);
		if (error == 0) {
			SetMbrCode(sector, options->boot_code);
			if (options->masquerade_disk)
				sector[MBR_TABLE_OFFSET] = MBR_MASQUERADE;
			if (options->retain_disk_signature)
				for (size_t i = 0; i < sizeof(options->disk_signature); i++)
					sector[MBR_DISK_SIGNATURE_OFFSET + i] = (unsigned char)(options->disk_signature >> (8 * i));
			error = TransferImageBytes(fd, sector, sizeof(sector), 0, true, NULL);
		}
		while (fsync(fd) != 0) {
			if (errno == EINTR)
				continue;
			result->sync_error = errno;
			break;
		}
		if (error == 0)
			error = result->sync_error;
		if (error == 0)
			error = CheckImageTarget(fd, options->disk_size, options->device, options->sector_size, options->physical_sector_size);
		if (error == 0)
			error = ReadBackMbr(fd, options, result);
		if (error == 0)
			error = CheckImageTarget(fd, options->disk_size, options->device, options->sector_size, options->physical_sector_size);
	}
	fdisk_unref_context(context);
	if (lseek(fd, position, SEEK_SET) != position && error == 0)
		error = errno;
	return error;
}

static int WriteSecondaryBootImage(int fd, const MBR_IMAGE_OPTIONS* options,
	const unsigned char* core, uint64_t core_size, bool grub4dos, MBR_CORE_RESULT* result)
{
	if (result == NULL)
		return EINVAL;
	memset(result, 0, sizeof(*result));
	if (options == NULL || core == NULL)
		return EINVAL;
	const bool valid_size = grub4dos ? core_size > IMAGE_SECTOR_BYTES : core_size >= IMAGE_SECTOR_BYTES;
	if (!valid_size)
		return EINVAL;
	// Windows skips grldr.mbr's first sector; GRUB2's core has no MBR prefix.
	if (grub4dos) {
		core += IMAGE_SECTOR_BYTES;
		core_size -= IMAGE_SECTOR_BYTES;
	}
	// WriteSBR selects its provider independently of WriteMBR. Alt+E can retain
	// Windows code, including WinPE's final masqueraded partition flag.
	const bool windows_mbr = options->boot_code == MBR_BOOT_RUFUS ||
		options->boot_code == MBR_BOOT_WIN7 || options->boot_code == MBR_BOOT_RUFUS_MASQUERADE;
	const MBR_BOOT_CODE provider_mbr = grub4dos ? MBR_BOOT_GRUB4DOS : MBR_BOOT_GRUB2;
	// ReactOS without a Syslinux config can select a GRUB SBR independently
	// of its Syslinux PBR. The native installer verifies its initial Syslinux
	// MBR; the helper publishes the source-selected MBR after it exits.
	const bool supported_mbr = options->boot_code == provider_mbr || windows_mbr ||
		options->boot_code == MBR_BOOT_SYSLINUX;
	if (!supported_mbr)
		return ENOTSUP;
	MBR_IMAGE_RESULT layout;
	int error = CheckMbrImage(fd, options, &layout);
	if (error != 0)
		return error;
	// WriteSBR's prepared payload starts immediately after the MBR. Do not
	// round its length or overwrite the remaining embedding gap.
	const bool has_gap = layout.offset >= IMAGE_SECTOR_BYTES;
	const bool fits_gap = has_gap && core_size <= layout.offset - IMAGE_SECTOR_BYTES;
	if (!fits_gap)
		return ENOSPC;
	unsigned char buffer[IMAGE_BUFFER_BYTES];
	while (result->bytes_written < core_size && error == 0) {
		const uint64_t position = result->bytes_written;
		const uint64_t remaining = core_size - position;
		const size_t chunk = remaining < sizeof(buffer) ? (size_t)remaining : sizeof(buffer);
		memcpy(buffer, core + position, chunk);
		error = TransferImageBytes(fd, buffer, chunk, IMAGE_SECTOR_BYTES + position,
			true, &result->bytes_written);
	}
	if (result->bytes_written != 0) {
		while (fsync(fd) != 0) {
			if (errno == EINTR)
				continue;
			result->sync_error = errno;
			break;
		}
	}
	if (error == 0)
		error = result->sync_error;
	uint64_t verified = 0;
	while (verified < core_size && error == 0) {
		const uint64_t remaining = core_size - verified;
		const size_t chunk = remaining < sizeof(buffer) ? (size_t)remaining : sizeof(buffer);
		error = TransferImageBytes(fd, buffer, chunk, IMAGE_SECTOR_BYTES + verified, false, NULL);
		if (error == 0 && memcmp(buffer, core + verified, chunk) != 0)
			error = EBADMSG;
		verified += chunk;
	}
	if (error == 0)
		error = CheckMbrImage(fd, options, &layout);
	return error;
}

int WriteGrub2CoreImage(int fd, const MBR_IMAGE_OPTIONS* options,
	const unsigned char* core, uint64_t core_size, MBR_CORE_RESULT* result)
{
	return WriteSecondaryBootImage(fd, options, core, core_size, false, result);
}

int WriteGrub4dosSbrImage(int fd, const MBR_IMAGE_OPTIONS* options,
	const unsigned char* grldr_mbr, uint64_t resource_size, MBR_CORE_RESULT* result)
{
	return WriteSecondaryBootImage(fd, options, grldr_mbr, resource_size, true, result);
}

static int ClearDiskRange(int fd, uint64_t offset, uint64_t size, bool* started)
{
	unsigned char buffer[IMAGE_BUFFER_BYTES] = { 0 };
	while (size != 0) {
		if (IsRufusJobCancelled())
			return ECANCELED;
		size_t chunk = size < sizeof(buffer) ? (size_t)size : sizeof(buffer);
		*started = true;
		int error = TransferImageBytes(fd, buffer, chunk, offset, true, NULL);
		if (error != 0)
			return error;
		offset += chunk;
		size -= chunk;
	}
	return 0;
}

static int CheckClearedRange(int fd, uint64_t offset, uint64_t size)
{
	static const unsigned char zeroes[IMAGE_BUFFER_BYTES] = { 0 };
	unsigned char buffer[IMAGE_BUFFER_BYTES];
	while (size != 0) {
		if (IsRufusJobCancelled())
			return ECANCELED;
		size_t chunk = size < sizeof(buffer) ? (size_t)size : sizeof(buffer);
		int error = TransferImageBytes(fd, buffer, chunk, offset, false, NULL);
		if (error != 0)
			return error;
		if (memcmp(buffer, zeroes, chunk) != 0)
			return EBADMSG;
		offset += chunk;
		size -= chunk;
	}
	return 0;
}

typedef struct {
	uint64_t offset;
	uint64_t size;
} IMAGE_CLEAR_RANGE;

static int ClearTargetMetadata(int fd, uint64_t disk_size, uint64_t device,
	uint32_t sector_size, uint32_t physical_sector_size, uint64_t first_bytes, IMAGE_CLEAR_RANGE extra, bool* started, int* sync_error)
{
	int error;
	const uint64_t remaining_bytes = disk_size - first_bytes;
	const uint64_t tail_limit = (uint64_t)MAX_SECTORS_TO_CLEAR * sector_size / CLEAR_TAIL_DIVISOR;
	const uint64_t tail_bytes = remaining_bytes < tail_limit ? remaining_bytes : tail_limit;
	const uint64_t tail_offset = disk_size - tail_bytes;
	const IMAGE_CLEAR_RANGE ranges[] = {{0, first_bytes}, {tail_offset, tail_bytes}, extra};
	error = 0;
	for (size_t i = 0; error == 0 && i < sizeof(ranges) / sizeof(*ranges); i++)
		error = ClearDiskRange(fd, ranges[i].offset, ranges[i].size, started);
	if (*started) {
		while (fsync(fd) != 0) {
			if (errno == EINTR)
				continue;
			*sync_error = errno;
			break;
		}
	}
	if (error == 0)
		error = *sync_error;
	if (error == 0)
		error = CheckImageTarget(fd, disk_size, device, sector_size, physical_sector_size);
	for (size_t i = 0; error == 0 && i < sizeof(ranges) / sizeof(*ranges); i++)
		error = CheckClearedRange(fd, ranges[i].offset, ranges[i].size);
	return error;
}

int PrepareMbrTarget(int fd, const MBR_IMAGE_OPTIONS* options, MBR_IMAGE_RESULT* result)
{
	int error = PlanMbrImageLayout(options, result);
	if (error == 0)
		error = CheckImageTarget(fd, options->disk_size, options->device, options->sector_size, options->physical_sector_size);
	if (error != 0)
		return error;
	// Match format.c:ClearMBRGPT and drive.c:ClearPartition. With the ordinary
	// 1 MiB start, the disk and partition prefixes overlap; clear their union.
	const uint64_t clear_bytes = (uint64_t)MAX_SECTORS_TO_CLEAR * options->sector_size;
	const uint64_t partition_prefix = result->size < clear_bytes ? result->size : clear_bytes;
	uint64_t first_bytes = result->offset + partition_prefix;
	if (first_bytes < clear_bytes)
		first_bytes = clear_bytes;
	if (first_bytes > options->disk_size)
		first_bytes = options->disk_size;
	const IMAGE_CLEAR_RANGE boot_prefix = {result->uefi_ntfs_offset,
		result->uefi_ntfs_size < clear_bytes ? result->uefi_ntfs_size : clear_bytes};
	error = ClearTargetMetadata(fd, options->disk_size, options->device,
		options->sector_size, options->physical_sector_size, first_bytes, boot_prefix, &result->started, &result->sync_error);
	if (error != 0)
		return error;
	MBR_IMAGE_RESULT created;
	error = CreateMbrImage(fd, options, &created);
	// Creation can fail before its first write after clearing already happened.
	// Do not replace the clearing stage's started flag with the creator's flag.
	result->sync_error = created.sync_error;
	return error;
}

int PlanSfdImageLayout(const SFD_IMAGE_OPTIONS* options, SFD_IMAGE_RESULT* result)
{
	if (result == NULL)
		return EINVAL;
	memset(result, 0, sizeof(*result));
	if (options == NULL)
		return EINVAL;
	const bool supported_sector_size = ImageSectorGeometry(options->sector_size, options->physical_sector_size);
	const bool supported_capacity = options->disk_size >= options->sector_size && options->disk_size <= INT64_MAX;
	const bool sector_aligned = options->sector_size != 0 && options->disk_size % options->sector_size == 0;
	if (!supported_sector_size || !supported_capacity || !sector_aligned)
		return EINVAL;
	result->size = options->disk_size;
	return 0;
}

int PrepareSfdTarget(int fd, const SFD_IMAGE_OPTIONS* options, SFD_IMAGE_RESULT* result)
{
	int error = PlanSfdImageLayout(options, result);
	if (error == 0)
		error = CheckImageTarget(fd, options->disk_size, options->device, options->sector_size, options->physical_sector_size);
	if (error != 0)
		return error;
	// Win32 skips CreatePartition for SFD: no partition prefix or table follows.
	const uint64_t clear_bytes = (uint64_t)MAX_SECTORS_TO_CLEAR * options->sector_size;
	const uint64_t first_bytes = options->disk_size < clear_bytes ? options->disk_size : clear_bytes;
	const IMAGE_CLEAR_RANGE no_extra_partition = {0, 0};
	return ClearTargetMetadata(fd, options->disk_size, options->device,
		options->sector_size, options->physical_sector_size, first_bytes, no_extra_partition, &result->started, &result->sync_error);
}
