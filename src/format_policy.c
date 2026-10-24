/*
 * Rufus: The Reliable USB Formatting Utility
 * Formatting function calls
 * Copyright © 2011-2025 Pete Batard <pete@akeo.ie>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include "format_policy.h"
#include <ctype.h>
#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

bool CanUseRuntimeUefiValidation(const RUFUS_IMG_REPORT* report, int boot_type,
	bool windows_to_go, bool bios_target, bool allow_dual_uefi_bios)
{
	if (report == NULL || boot_type != BT_IMAGE)
		return false;
	const bool efi_copy = IS_EFI_BOOTABLE((*report)) && !IS_DD_ONLY((*report));
	const bool forced_windows_bios = bios_target && HAS_WINDOWS((*report)) && !allow_dual_uefi_bios;
	return efi_copy && !windows_to_go && !forced_windows_bios;
}
enum rufus_mbr_type GetMasterBootRecord(const RUFUS_IMG_REPORT* report,
	const RUFUS_BOOT_OPTIONS* options)
{
	const bool image = options->boot_type == BT_IMAGE;
	const bool windows_dual_bios = image && HAS_WINDOWS((*report)) &&
		options->allow_dual_uefi_bios && options->target_type == TT_BIOS;
	const bool fat = options->fs_type == FS_FAT16 || options->fs_type == FS_FAT32;

	if (options->gpt)
		return RUFUS_MBR_PROTECTIVE;
	// Windows with Alt+E bypasses the other BIOS providers.
	if (!windows_dual_bios) {
		if (options->boot_type == BT_NON_BOOTABLE || options->target_type == TT_UEFI)
			return RUFUS_MBR_ZERO;
		if (options->boot_type == BT_SYSLINUX_V4 || options->boot_type == BT_SYSLINUX_V6 ||
			(image && HAS_SYSLINUX((*report))))
			return RUFUS_MBR_SYSLINUX;
		if (options->boot_type == BT_GRUB2 || (image && report->has_grub2))
			return RUFUS_MBR_GRUB2;
		if (options->boot_type == BT_GRUB4DOS || (image && report->has_grub4dos))
			return RUFUS_MBR_GRUB4DOS;
		if (options->boot_type == BT_REACTOS)
			return RUFUS_MBR_REACTOS;
		if (image && HAS_KOLIBRIOS((*report)) && fat)
			return RUFUS_MBR_KOLIBRIOS;
	}
	return options->needs_masquerading || options->use_rufus_mbr ? RUFUS_MBR_RUFUS : RUFUS_MBR_WIN7;
}

enum rufus_boot_install_type GetBootInstallType(const RUFUS_IMG_REPORT* report,
	const RUFUS_BOOT_OPTIONS* options)
{
	if (options->boot_type == BT_NON_BOOTABLE || options->boot_type == BT_UEFI_NTFS)
		return RUFUS_BOOT_INSTALL_NONE;
	if (options->target_type == TT_UEFI)
		return RUFUS_BOOT_INSTALL_EFI_CHECK;
	const bool standalone_syslinux = options->boot_type == BT_SYSLINUX_V4 || options->boot_type == BT_SYSLINUX_V6;
	const bool image_syslinux = options->boot_type == BT_IMAGE && (HAS_SYSLINUX((*report)) || HAS_REACTOS((*report)));
	const bool windows_dual = HAS_WINDOWS((*report)) && options->allow_dual_uefi_bios;
	if (standalone_syslinux || (image_syslinux && !windows_dual))
		return RUFUS_BOOT_INSTALL_SYSLINUX;
	return RUFUS_BOOT_INSTALL_PBR;
}

enum rufus_pbr_type GetPartitionBootRecord(int boot_type, int actual_fs_type,
	const RUFUS_IMG_REPORT* report)
{
	switch (actual_fs_type) {
	case FS_FAT16:
		if (boot_type == BT_FREEDOS)
			return RUFUS_PBR_FAT16_FREEDOS;
		if (boot_type == BT_REACTOS)
			return RUFUS_PBR_FAT16_REACTOS;
		if (boot_type == BT_IMAGE && HAS_KOLIBRIOS((*report)))
			return RUFUS_PBR_UNSUPPORTED;
		return RUFUS_PBR_FAT16_STANDARD;
	case FS_FAT32:
		if (boot_type == BT_FREEDOS)
			return RUFUS_PBR_FAT32_FREEDOS;
		if (boot_type == BT_REACTOS)
			return RUFUS_PBR_FAT32_REACTOS;
		if (boot_type == BT_IMAGE) {
			if (HAS_KOLIBRIOS((*report)))
				return RUFUS_PBR_FAT32_KOLIBRIOS;
			if (HAS_BOOTMGR((*report)))
				return RUFUS_PBR_FAT32_PE;
			if (HAS_WINPE((*report)))
				return RUFUS_PBR_FAT32_NT;
		}
		return RUFUS_PBR_FAT32_STANDARD;
	case FS_NTFS:
		return RUFUS_PBR_NTFS;
	case FS_EXT2:
	case FS_EXT3:
	case FS_EXT4:
		return RUFUS_PBR_NONE;
	default:
		return RUFUS_PBR_UNSUPPORTED;
	}
}

int GetSecondaryBootType(int boot_type, const RUFUS_IMG_REPORT* report, bool gpt)
{
	int sub_type = boot_type;
	// Syslinux has precedence over Grub.
	if (boot_type == BT_IMAGE && !HAS_SYSLINUX((*report))) {
		if (report->has_grub4dos)
			sub_type = BT_GRUB4DOS;
		if (report->has_grub2)
			sub_type = BT_GRUB2;
	}
	// Use BT_MAX for the protective message.
	if (boot_type != BT_NON_BOOTABLE && gpt)
		sub_type = BT_MAX;
	return sub_type;
}

#define KB UINT64_C(1024)
#define MB (KB * KB)
#define GB (MB * KB)
#define TB (GB * KB)
#define MAX_FAT32_SIZE (2 * TB)
#define FAT32_CLUSTER_THRESHOLD 1.011f

bool IsFat32Compatible(const RUFUS_IMG_REPORT* report, bool allow_dual_uefi_bios)
{
	enum { SINGLE_LARGE_INSTALL_WIM = 0x11 };
	const bool files_fit = report->has_4GB_file == 0 && !HAS_FATLESS_GRUB((*report));
	// The low nibble counts large files; bit 0x10 marks an oversized install.wim.
	const bool single_large_wim = report->has_4GB_file == SINGLE_LARGE_INSTALL_WIM && allow_dual_uefi_bios;
	return (files_fit || single_large_wim) && !report->needs_ntfs;
}

uint32_t GetAllowedFileSystems(const RUFUS_IMG_REPORT* report,
	const RUFUS_FS_OPTIONS* options, bool* forced_fat)
{
	uint32_t allowed = 0;
	const uint32_t fat = FS_MASK(FS_FAT16) | FS_MASK(FS_FAT32);
	const bool windows_bios = HAS_WINDOWS((*report)) && options->target_type == TT_BIOS;
	*forced_fat = false;
	switch (options->boot_type) {
	case BT_NON_BOOTABLE:
		allowed = FS_MASK(FS_MAX) - 1;
		break;
	case BT_MSDOS:
	case BT_FREEDOS:
		allowed = fat;
		break;
	case BT_IMAGE:
		if (!options->image_selected || !HAS_NTFSLESS_GRUB((*report)))
			allowed |= FS_MASK(FS_NTFS);
		// Only disable FAT32 if we have NTFS enabled; otherwise keep the warning.
		if (options->image_selected && !IsFat32Compatible(report, options->allow_dual_uefi_bios)) {
			if (allowed & FS_MASK(FS_NTFS))
				break;
			*forced_fat = true;
		}
		if (!windows_bios || options->allow_dual_uefi_bios) {
			if (!options->windows_to_go)
				allowed |= fat;
		}
		break;
	case BT_GRUB2:
		allowed |= FS_MASK(FS_EXT2) | FS_MASK(FS_EXT3) | FS_MASK(FS_EXT4);
		// Fall through
	case BT_SYSLINUX_V6:
	case BT_GRUB4DOS:
		allowed |= FS_MASK(FS_NTFS);
		// Fall through
	case BT_SYSLINUX_V4:
	case BT_REACTOS:
		allowed |= fat;
		break;
	case BT_UEFI_NTFS:
		allowed = FS_MASK(FS_NTFS) | FS_MASK(FS_EXFAT);
		break;
	}
	return allowed;
}

int GetPreferredFileSystem(const RUFUS_IMG_REPORT* report,
	const RUFUS_FS_OPTIONS* options, uint32_t fs_mask, int preselected_fs)
{
	if (!options->image_selected)
		return FS_UNKNOWN;
	const bool valid_preselection = preselected_fs >= FS_FAT16 && preselected_fs < FS_MAX;
	if (valid_preselection && (fs_mask & FS_MASK(preselected_fs)))
		return preselected_fs;

	const bool fat32_available = (fs_mask & FS_MASK(FS_FAT32)) != 0;
	const bool fat16_available = (fs_mask & FS_MASK(FS_FAT16)) != 0;
	const bool ntfs_available = (fs_mask & FS_MASK(FS_NTFS)) != 0;
	const bool small_efi_image = IS_EFI_BOOTABLE((*report)) && options->target_type == TT_UEFI
		&& !options->windows_to_go && !report->has_4GB_file;
	const bool fat_bootloader = HAS_SYSLINUX((*report)) || HAS_REACTOS((*report)) || HAS_KOLIBRIOS((*report));
	const bool windows_bootloader = HAS_BOOTMGR((*report)) || HAS_WINPE((*report));

	// Syslinux and EFI have precedence over bootmgr (unless BIOS is selected).
	if (fat_bootloader || small_efi_image) {
		if (fat32_available)
			return FS_FAT32;
		if (fat16_available && !HAS_KOLIBRIOS((*report)))
			return FS_FAT16;
	} else if (options->windows_to_go || windows_bootloader) {
		if (fat32_available && !report->has_4GB_file && options->allow_dual_uefi_bios)
			return FS_FAT32;
		if (ntfs_available)
			return FS_NTFS;
	}
	return FS_UNKNOWN;
}

uint8_t GetExtraPartitions(const RUFUS_IMG_REPORT* report,
	const RUFUS_PARTITION_OPTIONS* options)
{
	uint8_t extra_partitions = 0;
	const bool image = options->boot_type == BT_IMAGE;
	const bool extracting_image = image && !options->write_as_image;
	const bool uefi_windows_to_go = options->windows_to_go && options->target_type == TT_UEFI && options->gpt;
	const bool ntfs_or_exfat = options->fs_type == FS_NTFS || options->fs_type == FS_EXFAT;
	const bool uefi_image = image && IS_EFI_BOOTABLE((*report));
	const bool bios_only = options->target_type == TT_BIOS && !options->allow_dual_uefi_bios;
	const bool windows_installer = image && HAS_BOOTMGR_BIOS((*report)) && !options->windows_to_go;

	if (extracting_image && HAS_PERSISTENCE((*report)) && options->persistence_size)
		extra_partitions |= XP_PERSISTENCE;
	// Every GPT disk we RUN Windows from needs an MSR, as in FormatThread.
	// https://learn.microsoft.com/en-us/windows-hardware/manufacture/desktop/windows-and-gpt-faq#disks-that-require-an-msr
	if (uefi_windows_to_go)
		extra_partitions |= XP_ESP | XP_MSR;
	// An EFI image on NTFS/exFAT or the explicit UEFI:NTFS option needs its loader.
	else if ((uefi_image && ntfs_or_exfat) || options->boot_type == BT_UEFI_NTFS) {
		extra_partitions |= XP_UEFI_NTFS;
		// Omit it for a BIOS-only Windows installer unless dual boot is enabled.
		if (windows_installer && bios_only)
			extra_partitions &= ~XP_UEFI_NTFS;
	}
	if (options->old_bios_fixes)
		extra_partitions |= XP_COMPAT;
	return extra_partitions;
}

/* English, binary-size branch of stdio.c's SizeToHumanReadable, followed by
 * ToValidLabel's decimal separator replacement. No localized unit names or
 * process-wide locale changes: the formatted decimal separator becomes '_'. */
static void SizeLabel(uint64_t size, uint16_t label[VOLUME_LABEL_CAPACITY])
{
	static const char* suffixes[] = { "bytes", "KB", "MB", "GB", "TB", "PB", "EB" };
	size_t suffix;
	double hr_size = (double)size;
	char number[24], formatted[VOLUME_LABEL_CAPACITY];
	for (suffix = 0; suffix < sizeof(suffixes) / sizeof(suffixes[0]) - 1; suffix++) {
		if (hr_size < 1024.0)
			break;
		hr_size /= 1024.0;
	}
	int decimals = (hr_size * 10.0 - floor(hr_size) * 10.0) < 0.5 ? 0 : 1;
	snprintf(number, sizeof(number), "%.*f", decimals, hr_size);
	const char* separator = localeconv()->decimal_point;
	char* decimal = separator[0] ? strstr(number, separator) : NULL;
	if (decimal != NULL) {
		size_t length = strlen(separator);
		memmove(decimal + 1, decimal + length, strlen(decimal + length) + 1);
		*decimal = '_';
	}
	snprintf(formatted, sizeof(formatted), "%s %s", number, suffixes[suffix]);
	for (size_t i = 0; i <= strlen(formatted); i++)
		label[i] = (unsigned char)formatted[i];
}

bool NormalizeVolumeLabel(const uint16_t* source, uint16_t label[VOLUME_LABEL_CAPACITY],
	bool fat, uint64_t disk_size)
{
	size_t i, j, k;
	bool found;
	const char unauthorized[] = "*?,;:/\\|+=<>[]\"";
	const char to_underscore[] = "\t.";
	const size_t limit = fat ? FAT_VOLUME_LABEL_LENGTH : VOLUME_LABEL_CAPACITY - 1;

	/* Same filtering order as format.c. Stop once the original final length
	 * limit is reached; later source characters cannot change the result. */
	for (i = 0, k = 0; source[i] != 0 && k < limit; i++) {
		if (fat) {
			found = false;
			for (j = 0; j < strlen(unauthorized); j++) {
				if (source[i] == (unsigned char)unauthorized[j]) {
					found = true;
					break;
				}
			}
			if (source[i] >= 0x80) {
				label[k++] = '_';
				found = true;
			}
			if (found)
				continue;
		}
		found = false;
		for (j = 0; j < strlen(to_underscore); j++) {
			if (source[i] == (unsigned char)to_underscore[j]) {
				label[k++] = '_';
				found = true;
				break;
			}
		}
		if (!found)
			label[k++] = fat ? (uint16_t)toupper(source[i]) : source[i];
	}
	label[k] = 0;
	if (fat) {
		for (i = 0, j = 0; label[i] != 0; i++)
			if (label[i] == '_')
				j++;
		if (i < 2 * j) {
			SizeLabel(disk_size, label);
			return true;
		}
	}
	return false;
}

uint32_t GetLargeFat32DefaultClusterSize(uint64_t volume_size)
{
	if (volume_size < 64 * MB) return 512;
	if (volume_size < 128 * MB) return 1 * KB;
	if (volume_size < 256 * MB) return 2 * KB;
	if (volume_size < 8 * GB) return 4 * KB;
	if (volume_size < 16 * GB) return 8 * KB;
	if (volume_size < 32 * GB) return 16 * KB;
	if (volume_size < 2 * TB) return 32 * KB;
	return 64 * KB;
}

RUFUS_CLUSTER_SIZES GetFat16ClusterSizes(uint64_t disk_size, uint32_t sector_size)
{
	RUFUS_CLUSTER_SIZES sizes = { 0, 0 };
	const bool valid_sector = sector_size != 0 && (sector_size & (sector_size - 1)) == 0;
	if (!valid_sector)
		return sizes;
	// Original FAT16 rules from SetFileSystemAndClusterSize.
	if (disk_size < 4 * GB) {
		sizes.Allowed = 0x00001E00;
		for (int64_t i = 32; i <= 4096; i <<= 1) {
			if (disk_size < i * MB) {
				sizes.Default = 16 * (uint32_t)i;
				break;
			}
			sizes.Allowed <<= 1;
		}
		sizes.Allowed &= 0x0001FE00;
	}
	sizes.Allowed &= ~(sector_size - 1);
	if ((sizes.Default & sizes.Allowed) == 0)
		sizes.Default = sizes.Allowed & (0u - sizes.Allowed);
	return sizes;
}

RUFUS_CLUSTER_SIZES GetFat32ClusterSizes(uint64_t disk_size, uint32_t sector_size)
{
	RUFUS_CLUSTER_SIZES sizes = { 0, 0 };
	int64_t i;
	const bool valid_sector = sector_size != 0 && (sector_size & (sector_size - 1)) == 0;
	if (!valid_sector)
		return sizes;
	// FAT 32
	// > 32GB FAT32 is not supported by MS and FormatEx but is achieved using fat32format
	// See: http://ridgecrop.co.uk/index.htm?fat32format.htm
	// < 32 MB FAT32 is not allowed by FormatEx, so we don't bother
	if ((disk_size >= 32 * MB) && (disk_size < MAX_FAT32_SIZE)) {
		sizes.Allowed = 0x000001F8;
		for (i = 32; i <= (32 * 1024); i <<= 1) {			// 32 MB -> 32 GB
			if (disk_size*1.0f < i * MB * FAT32_CLUSTER_THRESHOLD) {	// MS
				sizes.Default = 8*(uint32_t)i;
				break;
			}
			sizes.Allowed <<= 1;
		}
		sizes.Allowed &= 0x0001FE00;

		// Default cluster sizes in the 256MB to 32 GB range do not follow the rule above
		if ((disk_size >= 256 * MB) && (disk_size < 32 * GB)) {
			for (i = 8; i <= 32; i <<= 1) {			// 256 MB -> 32 GB
				if (disk_size * 1.0f < i * GB * FAT32_CLUSTER_THRESHOLD) {
					sizes.Default = ((uint32_t)i / 2) * KB;
					break;
				}
			}
		}
		// More adjustments for large drives
		if (disk_size >= 32 * GB) {
			sizes.Allowed &= 0x0001C000;
			sizes.Default = 0x00008000;
		}
	}

	// Original sector-size adjustment from SetFileSystemAndClusterSize.
	sizes.Allowed &= ~(sector_size - 1);
	if ((sizes.Default & sizes.Allowed) == 0)
		sizes.Default = sizes.Allowed & (0u - sizes.Allowed);
	return sizes;
}

RUFUS_CLUSTER_SIZES GetNtfsClusterSizes(uint64_t disk_size, uint32_t sector_size)
{
	RUFUS_CLUSTER_SIZES sizes = { 0, 0 };
	const bool valid_sector = sector_size != 0 && (sector_size & (sector_size - 1)) == 0;
	if (!valid_sector)
		return sizes;
	if (disk_size < 256 * TB) {
		sizes.Allowed = 0x0001F000;
		for (int64_t i = 16; i <= 256; i <<= 1) {
			if (disk_size < i * TB) {
				sizes.Default = ((uint32_t)i / 4) * KB;
				break;
			}
		}
	}
	sizes.Allowed &= ~(sector_size - 1);
	if ((sizes.Default & sizes.Allowed) == 0)
		sizes.Default = sizes.Allowed & (0u - sizes.Allowed);
	return sizes;
}

RUFUS_CLUSTER_SIZES GetExfatClusterSizes(uint64_t disk_size, uint32_t sector_size)
{
	RUFUS_CLUSTER_SIZES sizes = { 0, 0 };
	const bool valid_sector = sector_size != 0 && (sector_size & (sector_size - 1)) == 0;
	if (!valid_sector)
		return sizes;
	// Original exFAT rules from SetFileSystemAndClusterSize.
	if (disk_size < 256 * TB) {
		sizes.Allowed = 0x03FFFE00;
		if (disk_size < 256 * MB)
			sizes.Default = 4 * KB;
		else if (disk_size < 32 * GB)
			sizes.Default = 32 * KB;
		else
			sizes.Default = 128 * KB;
	}
	sizes.Allowed &= ~(sector_size - 1);
	if ((sizes.Default & sizes.Allowed) == 0)
		sizes.Default = sizes.Allowed & (0u - sizes.Allowed);
	return sizes;
}

/* Keep the Windows quick-format eligibility separate from provider behavior. */
bool SupportsFullFormat(int filesystem, uint64_t disk_bytes)
{
    if (filesystem < FS_FAT16 || filesystem >= FS_MAX || filesystem == FS_REFS) return false;
    return filesystem != FS_FAT32 || disk_bytes <= (UINT64_C(32) << 30);
}
