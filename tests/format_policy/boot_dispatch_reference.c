/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Original CP22n25 format.c branches, frozen before extraction. Only logging,
 * native writer calls and EFI sanity checks are replaced by returned choices.
 * Do not derive this oracle from the production selectors. */
#include "format_policy.h"
/* Original rufus.h macro needed by the frozen KolibriOS branch. */
#define IS_FAT(fs) ((fs == FS_FAT16) || (fs == FS_FAT32))

enum rufus_mbr_type ReferenceMasterBootRecord(const RUFUS_IMG_REPORT* report,
    const RUFUS_BOOT_OPTIONS* options)
{
    const RUFUS_IMG_REPORT img_report = *report;
    const int boot_type = options->boot_type, fs_type = options->fs_type;
    const int target_type = options->target_type;
    const bool allow_dual_uefi_bios = options->allow_dual_uefi_bios;
    const bool use_rufus_mbr = options->use_rufus_mbr;
    const bool needs_masquerading = options->needs_masquerading;
    if (options->gpt) return RUFUS_MBR_PROTECTIVE;
	// What follows is really a case statement with complex conditions listed
	// by order of preference
	if ((boot_type == BT_IMAGE) && HAS_WINDOWS(img_report) && (allow_dual_uefi_bios) && (target_type == TT_BIOS))
		goto windows_mbr;

	// Non bootable or forced UEFI (zeroed MBR)
	if ((boot_type == BT_NON_BOOTABLE) || (target_type == TT_UEFI)) {
		return RUFUS_MBR_ZERO;
	}

	// Syslinux
	if ( (boot_type == BT_SYSLINUX_V4) || (boot_type == BT_SYSLINUX_V6) ||
		 ((boot_type == BT_IMAGE) && HAS_SYSLINUX(img_report)) ) {
		return RUFUS_MBR_SYSLINUX;
	}

	// Grub 2.0
	if ( ((boot_type == BT_IMAGE) && (img_report.has_grub2)) || (boot_type == BT_GRUB2) ) {
		return RUFUS_MBR_GRUB2;
	}

	// Grub4DOS
	if ( ((boot_type == BT_IMAGE) && (img_report.has_grub4dos)) || (boot_type == BT_GRUB4DOS) ) {
		return RUFUS_MBR_GRUB4DOS;
	}

	// ReactOS
	if (boot_type == BT_REACTOS) {
		return RUFUS_MBR_REACTOS;
	}

	// KolibriOS
	if ( (boot_type == BT_IMAGE) && HAS_KOLIBRIOS(img_report) && (IS_FAT(fs_type))) {
		return RUFUS_MBR_KOLIBRIOS;
	}

	// If everything else failed, fall back to a conventional Windows/Rufus MBR
windows_mbr:
	if (needs_masquerading || use_rufus_mbr) {
		return RUFUS_MBR_RUFUS;
	} else {
		return RUFUS_MBR_WIN7;
	}

}

enum rufus_boot_install_type ReferenceBootInstallType(const RUFUS_IMG_REPORT* report,
    const RUFUS_BOOT_OPTIONS* options)
{
    const RUFUS_IMG_REPORT img_report = *report;
    const int boot_type = options->boot_type, target_type = options->target_type;
    const bool allow_dual_uefi_bios = options->allow_dual_uefi_bios;
	if (boot_type != BT_NON_BOOTABLE) {
		if (boot_type == BT_UEFI_NTFS) {
			return RUFUS_BOOT_INSTALL_NONE;
		} else if (target_type == TT_UEFI) {
			return RUFUS_BOOT_INSTALL_EFI_CHECK;
		} else if ( (boot_type == BT_SYSLINUX_V4) || (boot_type == BT_SYSLINUX_V6) ||
			((boot_type == BT_IMAGE) && (HAS_SYSLINUX(img_report) || HAS_REACTOS(img_report)) &&
				(!HAS_WINDOWS(img_report) || !allow_dual_uefi_bios)) ) {
			return RUFUS_BOOT_INSTALL_SYSLINUX;
		} else {
			return RUFUS_BOOT_INSTALL_PBR;
		}
	}
	return RUFUS_BOOT_INSTALL_NONE;
}

/* Original CP22n26 WritePBR selection branches. Geometry, native writes and
 * loop/error handling stay outside this returned-choice reference. */
enum rufus_pbr_type ReferencePartitionBootRecord(int boot_type, int actual_fs_type,
    const RUFUS_IMG_REPORT* report)
{
    const RUFUS_IMG_REPORT img_report = *report;
    switch (actual_fs_type) {
	case FS_FAT16:
		if (boot_type == BT_FREEDOS) {
			return RUFUS_PBR_FAT16_FREEDOS;
		} else if (boot_type == BT_REACTOS) {
			return RUFUS_PBR_FAT16_REACTOS;
		} else if ((boot_type == BT_IMAGE) && HAS_KOLIBRIOS(img_report)) {
			return RUFUS_PBR_UNSUPPORTED;
		} else {
			return RUFUS_PBR_FAT16_STANDARD;
		}
	case FS_FAT32:
		if (boot_type == BT_FREEDOS) {
				return RUFUS_PBR_FAT32_FREEDOS;
			} else if (boot_type == BT_REACTOS) {
				return RUFUS_PBR_FAT32_REACTOS;
			} else if ((boot_type == BT_IMAGE) && HAS_KOLIBRIOS(img_report)) {
				return RUFUS_PBR_FAT32_KOLIBRIOS;
			} else if ((boot_type == BT_IMAGE) && HAS_BOOTMGR(img_report)) {
				return RUFUS_PBR_FAT32_PE;
			} else if ((boot_type == BT_IMAGE) && HAS_WINPE(img_report)) {
				return RUFUS_PBR_FAT32_NT;
			} else {
				return RUFUS_PBR_FAT32_STANDARD;
			}
    case FS_NTFS: return RUFUS_PBR_NTFS;
    case FS_EXT2:
    case FS_EXT3:
    case FS_EXT4: return RUFUS_PBR_NONE;
    default: return RUFUS_PBR_UNSUPPORTED;
    }
}
