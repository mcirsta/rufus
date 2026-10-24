/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "fat_boot.h"
#include "rufus_reporting.h"
#include "syslinux/libfat/fat.h"
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "ms-sys/inc/fat16.h"
#include "ms-sys/inc/fat32.h"
#include "ms-sys/inc/partition_info.h"

enum {
	SECTOR_BYTES = 512,
	FAT32_RECORD_SECTORS = 3,
	FAT32_BACKUP_SECTOR = 6,
	FAT32_FSINFO_SECTOR = 1,
	FSINFO_HINT_OFFSET = FAT32_FSINFO_SECTOR * SECTOR_BYTES + 488,
	FSINFO_HINT_BYTES = 2 * sizeof(le32_t),
	BIOS_FIRST_DISK = 0x80,
	PREFIX_BYTES = FAT32_RECORD_SECTORS * SECTOR_BYTES,
	WINDOWS_TAIL_SECTOR = 12,
	REACTOS_TAIL_SECTOR = 14,
	MAX_RECORD_BYTES = (REACTOS_TAIL_SECTOR + 1) * SECTOR_BYTES,
	MAX_COPIES = 2
};

static unsigned TailSector(FAT_BOOT_CODE code)
{
    if (code == FAT_BOOT_REACTOS) return REACTOS_TAIL_SECTOR;
    return code == FAT_BOOT_WINDOWS || code == FAT_BOOT_WINPE ? WINDOWS_TAIL_SECTOR : 0;
}

static int PatchRecord(unsigned char* prefix, size_t size, bool fat16, FAT_BOOT_CODE boot_code)
{
	FILE* file = fmemopen(prefix, size + 1, "r+b");
	if (file == NULL)
		return errno;
	// These are explicit native writer modes, not source/version inference.
	// Final legacy BOOTMGR mode still passes FAT_BOOT_WINDOWS and applies its
	// existing FSInfo/drive fixup after the original PE writer below.
	RUFUS_IMG_REPORT report = { 0 };
	report.has_bootmgr = boot_code == FAT_BOOT_WINDOWS;
	report.winpe = boot_code == FAT_BOOT_WINPE ? WINPE_I386 : 0;
	report.has_kolibrios = boot_code == FAT_BOOT_KOLIBRI;
	const int boot_type = boot_code == FAT_BOOT_FREEDOS ? BT_FREEDOS : boot_code == FAT_BOOT_REACTOS ? BT_REACTOS : BT_IMAGE;
	const enum rufus_pbr_type record = GetPartitionBootRecord(boot_type, fat16 ? FS_FAT16 : FS_FAT32, &report);
	int error = EBADMSG;
	const bool expected_type = fat16 ? is_fat_16_fs(file) : is_fat_32_fs(file);
	if (expected_type) {
		// Match WritePBR: bKeepLabel=0, followed by the physical BIOS drive ID.
		const bool freedos = record == RUFUS_PBR_FAT16_FREEDOS || record == RUFUS_PBR_FAT32_FREEDOS;
		const bool reactos = record == RUFUS_PBR_FAT16_REACTOS || record == RUFUS_PBR_FAT32_REACTOS;
		const bool kolibri = record == RUFUS_PBR_FAT32_KOLIBRIOS;
		const bool windows_pe = record == RUFUS_PBR_FAT32_PE;
		const bool legacy_winpe = record == RUFUS_PBR_FAT32_NT;
		int written;
		if (freedos)
			written = fat16 ? write_fat_16_fd_br(file, 0) : write_fat_32_fd_br(file, 0);
		else if (reactos)
			written = fat16 ? write_fat_16_ros_br(file, 0) : write_fat_32_ros_br(file, 0);
		else if (kolibri)
			written = write_fat_32_kos_br(file, 0);
		else if (windows_pe)
			written = write_fat_32_pe_br(file, 0);
		else if (legacy_winpe)
			written = write_fat_32_nt_br(file, 0);
		else
			written = fat16 ? write_fat_16_br(file, 0) : write_fat_32_br(file, 0);
		if (written)
			written = fat16 ? write_partition_physical_disk_drive_id_fat16(file) :
				write_partition_physical_disk_drive_id_fat32(file);
		bool matches = false;
		if (written && freedos)
			matches = fat16 ? entire_fat_16_fd_br_matches(file) : entire_fat_32_fd_br_matches(file);
		else if (written && reactos)
			matches = fat16 ? entire_fat_16_ros_br_matches(file) : entire_fat_32_ros_br_matches(file);
		else if (written && kolibri)
			matches = entire_fat_32_kos_br_matches(file);
		else if (written && windows_pe)
			matches = entire_fat_32_pe_br_matches(file);
		else if (written && legacy_winpe)
			matches = entire_fat_32_nt_br_matches(file);
		else if (written)
			matches = fat16 ? entire_fat_16_br_matches(file) : entire_fat_32_br_matches(file);
		error = matches ? 0 : EIO;
	}
	if (fclose(file) != 0 && error == 0)
		error = EIO;
	return error;
}

static int TransferPrefix(int fd, unsigned char* buffer, size_t size, uint64_t offset,
	bool writing, FAT_BOOT_RESULT* result)
{
	size_t done = 0;
	while (done < size) {
		if (IsRufusJobCancelled())
			return ECANCELED;
		const ssize_t count = writing ? pwrite(fd, buffer + done, size - done, (off_t)(offset + done)) :
			pread(fd, buffer + done, size - done, (off_t)(offset + done));
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			return count == 0 ? EIO : errno;
		done += (size_t)count;
		if (writing)
			result->bytes_written += (uint64_t)count;
	}
	return 0;
}

static int PrepareRecords(int fd, const FAT_FORMAT_OPTIONS* options, bool fat16, FAT_BOOT_CODE boot_code,
	bool winpe_bootmgr, bool masquerade_disk,
	unsigned char records[MAX_COPIES][MAX_RECORD_BYTES + 1], FAT_BOOT_RESULT* result)
{
	const size_t prefix_size = fat16 ? SECTOR_BYTES : PREFIX_BYTES;
	const unsigned tail_sector = fat16 ? 0 : TailSector(boot_code);
	const bool has_tail = tail_sector != 0;
	int error = TransferPrefix(fd, records[0], prefix_size, options->offset, false, result);
	if (error != 0)
		return error;
	struct fat_bootsect boot;
	memcpy(&boot, records[0], sizeof(boot));
	if (!fat16) {
		const unsigned record_sectors = has_tail ? tail_sector + 1 : FAT32_RECORD_SECTORS;
		const bool records_reserved = read16(&boot.bsResSectors) >= FAT32_BACKUP_SECTOR + record_sectors;
		const bool expected_backup = read16(&boot.u.fat32.bpb_bkbootsec) == FAT32_BACKUP_SECTOR;
		const bool expected_fsinfo = read16(&boot.u.fat32.bpb_fsinfo) == FAT32_FSINFO_SECTOR;
		if (!records_reserved || !expected_backup || !expected_fsinfo)
			return ENOTSUP;
		error = TransferPrefix(fd, records[1], prefix_size,
			options->offset + FAT32_BACKUP_SECTOR * SECTOR_BYTES, false, result);
		if (error != 0)
			return error;
		// Require the backup's BPB/type/signature to describe the same filesystem.
		// Boot code and FSInfo allocation hints may differ between the copies.
		const size_t bpb_start = offsetof(struct fat_bootsect, bsBytesPerSec);
		const size_t bpb_end = offsetof(struct fat_bootsect, u.fat32.bsCode);
		const bool same_bpb = memcmp(records[0] + bpb_start, records[1] + bpb_start, bpb_end - bpb_start) == 0;
		struct fat_bootsect backup;
		memcpy(&backup, records[1], sizeof(backup));
		const bool valid_signature = read16(&backup.bsSignature) == BS_SIGNATURE;
		if (!same_bpb || !valid_signature)
			return EBADMSG;
	}
	const int copies = fat16 ? 1 : MAX_COPIES;
	for (int copy = 0; copy < copies; copy++) {
		unsigned char hints[FSINFO_HINT_BYTES];
		if (winpe_bootmgr)
			memcpy(hints, records[copy] + FSINFO_HINT_OFFSET, sizeof(hints));
		if (has_tail) {
			const uint64_t tail_offset = options->offset +
				(tail_sector + copy * FAT32_BACKUP_SECTOR) * SECTOR_BYTES;
			error = TransferPrefix(fd, records[copy] + tail_sector * SECTOR_BYTES,
				SECTOR_BYTES, tail_offset, false, result);
			if (error != 0)
				return error;
		}
		const size_t record_size = has_tail ? (tail_sector + 1) * SECTOR_BYTES : prefix_size;
		error = PatchRecord(records[copy], record_size, fat16, boot_code);
		if (error != 0)
			return error;
		if (winpe_bootmgr) {
			// The verified original PE code overlaps allocation hints, and its
			// stock drive ID cannot follow WinPE's 0x81 masquerading. Change only
			// those metadata fields after closing the original memory writer.
			memcpy(records[copy] + FSINFO_HINT_OFFSET, hints, sizeof(hints));
			records[copy][offsetof(struct fat_bootsect, u.fat32.bsDriveNumber)] =
				BIOS_FIRST_DISK + (unsigned)masquerade_disk;
		}
	}
	return 0;
}

static int TransferRecords(int fd, uint64_t volume_offset, bool fat16, unsigned tail_sector,
	unsigned char records[MAX_COPIES][MAX_RECORD_BYTES + 1], bool writing, FAT_BOOT_RESULT* result)
{
	const int copies = fat16 ? 1 : MAX_COPIES;
	const int regions = tail_sector != 0 ? 2 : 1;
	for (int region = 0; region < regions; region++) {
		const size_t position = region == 0 ? 0 : tail_sector * SECTOR_BYTES;
		const size_t size = region == 0 && !fat16 ? PREFIX_BYTES : SECTOR_BYTES;
		for (int copy = 0; copy < copies; copy++) {
			unsigned char actual[PREFIX_BYTES];
			unsigned char* expected = records[copy] + position;
			const uint64_t offset = volume_offset + copy * FAT32_BACKUP_SECTOR * SECTOR_BYTES + position;
			int error = TransferPrefix(fd, writing ? expected : actual, size, offset, writing, result);
			if (error != 0)
				return error;
			if (!writing && memcmp(actual, expected, size) != 0)
				return EBADMSG;
		}
	}
	return 0;
}

static int CheckVolume(int fd, uint64_t device, uint64_t capacity, bool fat16,
	const FAT_FORMAT_OPTIONS* options)
{
	FORMAT_RESULT geometry;
	if (device != 0)
		return fat16 ? CheckFat16Device(fd, device, capacity, options, &geometry) :
			CheckFat32Device(fd, device, capacity, options, &geometry);
	return fat16 ? CheckFat16Image(fd, options, &geometry) : CheckFat32Image(fd, options, &geometry);
}

static int InstallRecords(int fd, uint64_t device, uint64_t capacity, int file_system,
	FAT_BOOT_CODE boot_code, bool winpe_bootmgr, bool masquerade_disk, const FAT_FORMAT_OPTIONS* options,
	FAT_BOOT_RESULT* result)
{
	if (result == NULL)
		return EINVAL;
	memset(result, 0, sizeof(*result));
	if (options == NULL) return EINVAL;
	if (options->sector_size != 0 && options->sector_size != SECTOR_BYTES) return ENOTSUP;
	const bool supported_code = boot_code == FAT_BOOT_STANDARD || boot_code == FAT_BOOT_FREEDOS ||
		boot_code == FAT_BOOT_WINDOWS || boot_code == FAT_BOOT_WINPE || boot_code == FAT_BOOT_REACTOS ||
		boot_code == FAT_BOOT_KOLIBRI;
	if (!supported_code)
		return EINVAL;
	const bool fat16 = file_system == FS_FAT16;
	if (!fat16 && file_system != FS_FAT32)
		return EINVAL;
	if (fat16 && boot_code == FAT_BOOT_KOLIBRI)
		return ENOTSUP;
	int error = CheckVolume(fd, device, capacity, fat16, options);
	if (error != 0)
		return error;
	struct stat before, after;
	if (fstat(fd, &before) != 0)
		return errno;
	unsigned char records[MAX_COPIES][MAX_RECORD_BYTES + 1] = { { 0 } };
	error = PrepareRecords(fd, options, fat16, boot_code, winpe_bootmgr, masquerade_disk, records, result);
	if (error != 0)
		return error;
	const unsigned tail_sector = fat16 ? 0 : TailSector(boot_code);
	// The original BOOTMGR/NT writers have a separate sector-12 tail. Transfer only the
	// prefix and tail: full record spans would overlap the other backup copy.
	error = TransferRecords(fd, options->offset, fat16, tail_sector, records, true, result);
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
	if (error == 0)
		error = TransferRecords(fd, options->offset, fat16, tail_sector, records, false, result);
	if (error == 0) {
		// Validate remaining geometry with the installed BPB label. Drive ID and
		// allocation hints were checked by exact readback; the directory label
		// was never written.
		FAT_FORMAT_OPTIONS installed = *options;
		installed.label[0] = '\0';
		error = CheckVolume(fd, device, capacity, fat16, &installed);
	}
	if (error == 0) {
		if (fstat(fd, &after) != 0)
			error = errno;
		else if (after.st_size != before.st_size)
			error = ESTALE;
	}
	if (error == 0 && IsRufusJobCancelled())
		error = ECANCELED;
	return error;
}

int InstallFatBootRecords(int fd, int file_system, FAT_BOOT_CODE boot_code, const FAT_FORMAT_OPTIONS* options,
	FAT_BOOT_RESULT* result)
{
	return InstallRecords(fd, 0, 0, file_system, boot_code, false, false, options, result);
}

int InstallWinPeFatBootRecords(int fd, bool masquerade_disk, const FAT_FORMAT_OPTIONS* options,
	FAT_BOOT_RESULT* result)
{
	return InstallRecords(fd, 0, 0, FS_FAT32, FAT_BOOT_WINDOWS, true, masquerade_disk, options, result);
}

int InstallFatDeviceBootRecords(int fd, uint64_t device, uint64_t capacity, int file_system,
	FAT_BOOT_CODE boot_code, const FAT_FORMAT_OPTIONS* options, FAT_BOOT_RESULT* result)
{
	if (result == NULL)
		return EINVAL;
	memset(result, 0, sizeof(*result));
	if (device == 0 || capacity == 0)
		return EINVAL;
	return InstallRecords(fd, device, capacity, file_system, boot_code, false, false, options, result);
}

int InstallWinPeFatDeviceBootRecords(int fd, uint64_t device, uint64_t capacity, bool masquerade_disk,
	const FAT_FORMAT_OPTIONS* options, FAT_BOOT_RESULT* result)
{
	if (result == NULL)
		return EINVAL;
	memset(result, 0, sizeof(*result));
	if (device == 0 || capacity == 0)
		return EINVAL;
	return InstallRecords(fd, device, capacity, FS_FAT32, FAT_BOOT_WINDOWS, true, masquerade_disk, options, result);
}

int InstallLegacyFatDeviceBootRecords(int fd, uint64_t device, uint64_t capacity,
	FAT_BOOT_CODE boot_code, bool masquerade_disk, const FAT_FORMAT_OPTIONS* options, FAT_BOOT_RESULT* result)
{
	if (result == NULL)
		return EINVAL;
	memset(result, 0, sizeof(*result));
	if (device == 0 || capacity == 0)
		return EINVAL;
	if (boot_code != FAT_BOOT_WINDOWS && boot_code != FAT_BOOT_WINPE)
		return EINVAL;
	return InstallRecords(fd, device, capacity, FS_FAT32, boot_code, true, masquerade_disk, options, result);
}
