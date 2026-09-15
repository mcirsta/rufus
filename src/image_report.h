/*
 * Rufus: The Reliable USB Formatting Utility
 * Copyright © 2011-2026 Pete Batard <pete@akeo.ie>
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
#pragma once

#include <stdint.h>

enum ISO_SCAN_RESULT {
	ISO_SCAN_OK = 0,
	ISO_SCAN_ERROR = 1,
	ISO_SCAN_CANCELLED = 2,
	ISO_SCAN_PARTIAL = 3
};

#define MAX_WININST 4  // Maximum install[.wim|.esd] entries per image
#define MAX_ISO_TO_ESP_SIZE UINT64_C(1073741824) // Maximum size for the ISO → ESP option

enum EFI_BOOT_TYPE {
	EBT_MAIN = 0,
	EBT_GRUB,
	EBT_MOKMANAGER,
	EBT_BOOTMGR
};

/* Special handling for old .c32 files we need to replace */
#define NB_OLD_C32          2
#define OLD_C32_NAMES       { "menu.c32", "vesamenu.c32" }
#define OLD_C32_THRESHOLD   { 53500, 148000 }

/* ISO details that the application may want */
#define WINPE_I386          0x0007
#define WINPE_AMD64         0x0023
#define WINPE_MININT        0x01C0
#define SPECIAL_WIM_VERSION 0x000E0000
#define HAS_KOLIBRIOS(r)    (r.has_kolibrios)
#define HAS_REACTOS(r)      (r.reactos_path[0] != 0)
#define HAS_GRUB(r)         ((r.has_grub2) || (r.has_grub4dos))
#define HAS_SYSLINUX(r)     (r.has_syslinux)
#define HAS_BOOTMGR_BIOS(r) (r.has_bootmgr)
#define HAS_BOOTMGR_EFI(r)  (r.has_bootmgr_efi)
#define HAS_BOOTMGR(r)      (HAS_BOOTMGR_BIOS(r) || HAS_BOOTMGR_EFI(r))
#define HAS_REGULAR_EFI(r)  (r.has_efi & 0x7FFE)
#define HAS_WININST(r)      (r.wininst_index != 0)
#define HAS_WINPE(r)        (((r.winpe & WINPE_I386) == WINPE_I386)||((r.winpe & WINPE_AMD64) == WINPE_AMD64)||((r.winpe & WINPE_MININT) == WINPE_MININT))
#define HAS_WINDOWS(r)      (HAS_BOOTMGR(r) || (r.uses_minint) || HAS_WINPE(r))
#define HAS_WIN7_EFI(r)     ((r.has_efi == 1) && HAS_WININST(r))
#define HAS_FATLESS_GRUB(r) ((r.has_grub2 & 0x80) && !(r.has_grub2_fs & 0x1))
#define HAS_NTFSLESS_GRUB(r)((r.has_grub2 & 0x80) && !(r.has_grub2_fs & 0x4))
#define IS_WINDOWS_1X(r)    (r.has_bootmgr_efi && (r.win_version.major >= 10))
#define IS_WINDOWS_11(r)    (r.has_bootmgr_efi && (r.win_version.major >= 11))
#define HAS_EFI_IMG(r)      (r.efi_img_path[0] != 0)
#define IS_DD_BOOTABLE(r)   (r.is_bootable_img > 0)
#define IS_DD_ONLY(r)       ((r.is_bootable_img > 0) && (!r.is_iso || r.disable_iso))
#define IS_EFI_BOOTABLE(r)  (r.has_efi != 0)
#define IS_BIOS_BOOTABLE(r) (HAS_BOOTMGR(r) || HAS_SYSLINUX(r) || HAS_WINPE(r) || HAS_GRUB(r) || HAS_REACTOS(r) || HAS_KOLIBRIOS(r))
#define HAS_WINTOGO(r)      (HAS_BOOTMGR(r) && IS_EFI_BOOTABLE(r) && HAS_WININST(r))
#define HAS_PERSISTENCE(r)  ((HAS_SYSLINUX(r) || HAS_GRUB(r)) && !(HAS_WINDOWS(r) || HAS_REACTOS(r) || HAS_KOLIBRIOS(r)))
#define SYMLINKS_RR         0x01
#define SYMLINKS_UDF        0x02

typedef struct {
	uint16_t major;
	uint16_t minor;
	uint16_t build;
	uint16_t revision;
} winver_t;

/*
 * We can't use the Microsoft enums as we want to have RISC-V and LoongArch
 * and we also translate these to IDR_BASE + Value for resource items, which,
 * if we were to use the Microsoft IMAGE_FILE_MACHINE constants, would force
 * us to leave a 65536 sized gap between bases in order to properly map
 * 0x8664 (x86_64), 0xaa64 (ARM64) and so on...
 * NB: When editing this, make sure to also update the values in resource.h!
 */
enum ArchType {
	ARCH_UNKNOWN = 0,
	ARCH_X86_32,
	ARCH_X86_64,
	ARCH_ARM_32,
	ARCH_ARM_64,
	ARCH_IA_64,
	ARCH_RISCV_64,
	ARCH_LOONGARCH_64,
	ARCH_EBC,
	ARCH_MAX
};

typedef struct {
	uint8_t type;
	char path[64];
} efi_boot_entry_t;

typedef struct {
	char label[192];					// 3*64 to account for UTF-8
	char usb_label[192];				// converted USB label for workaround
	char cfg_path[128];					// path to the ISO's isolinux.cfg
	char reactos_path[128];				// path to the ISO's freeldr.sys or setupldr.sys
	char wininst_path[MAX_WININST][64];	// path to the Windows install image(s)
	efi_boot_entry_t efi_boot_entry[64];// types and paths of detected UEFI bootloaders
	char efi_img_path[128];				// path to an efi.img file
	char winpe_path[3][32];				// Original-case txtsetup.sif paths: i386, amd64, minint
	uint64_t image_size;
	uint64_t projected_size;
	int64_t mismatch_size;
	uint32_t wininst_version;
	uint8_t is_iso;
	int8_t is_bootable_img;
	uint8_t is_vhd;
	uint8_t is_windows_img;
	uint8_t disable_iso;
	uint8_t rh8_derivative;
	uint16_t winpe;
	uint16_t has_efi;
	uint8_t has_secureboot_bootloader;
	uint8_t has_md5sum;
	uint8_t wininst_index;
	uint8_t has_symlinks;
	uint8_t has_4GB_file;
	uint8_t has_long_filename;
	uint8_t has_deep_directories;
	uint8_t has_bootmgr;
	uint8_t has_bootmgr_efi;
	uint8_t has_autorun;
	uint8_t has_old_c32[NB_OLD_C32];
	uint8_t has_old_vesamenu;
	uint8_t has_efi_syslinux;
	uint8_t has_grub4dos;
	uint8_t has_grub2;
	uint8_t has_grub2_fs;
	uint8_t has_compatresources_dll;
	uint8_t has_panther_unattend;
	uint8_t has_kolibrios;
	uint8_t needs_syslinux_overwrite;
	uint8_t needs_ntfs;
	uint8_t uses_casper;
	uint8_t uses_minint;
	uint8_t winpe_options_known;			// OsLoadOptions checked, including successful absence
	uint8_t compression_type;
	winver_t win_version;	// Windows ISO version
	uint16_t sl_version;	// Syslinux version selected for installation
	char sl_version_str[12];
	char sl_version_ext[32];
	char grub2_version[192];
	uint8_t has_syslinux;	// Configuration found, even if its loader version is unknown
	uint8_t sl_version_inferred;	// Selected version came from the embedded-version fallback
	uint16_t sl_detected_version;	// Version of the chosen image loader, zero when unknown
	char sl_detected_version_ext[32];
} RUFUS_IMG_REPORT;
