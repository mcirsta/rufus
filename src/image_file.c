/*
 * Rufus: The Reliable USB Formatting Utility
 * ISO file rules, shared by scanning and extraction
 * Copyright © 2011-2026 Pete Batard <pete@akeo.ie>
 * Based on libcdio's iso & udf samples:
 * Copyright © 2003-2014 Rocky Bernstein <rocky@gnu.org>
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

#include "image_file.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define ARRAYSIZE(a) (sizeof(a) / sizeof((a)[0]))
#define GB INT64_C(1073741824)
#define safe_strlen(s) ((s) == NULL ? 0 : strlen(s))
#define safe_stricmp(a, b) _stricmp((a) == NULL ? "<NULL>" : (a), (b))
#define safe_strnicmp(a, b, n) _strnicmp((a) == NULL ? "<NULL>" : (a), (b), (n))
#define safe_strstr(a, b) strstr((a) == NULL ? "<NULL>" : (a), (b))
#define static_strcpy(dst, src) CopyIsoPath(dst, sizeof(dst), src)
#define static_sprintf(dst, ...) snprintf(dst, sizeof(dst), __VA_ARGS__)

bool IsWimImageSignature(const void* data, size_t size)
{
	static const uint8_t signature[8] = { 'M', 'S', 'W', 'I', 'M', 0, 0, 0 };

	return data != NULL && size >= sizeof(signature) && memcmp(data, signature, sizeof(signature)) == 0;
}

bool IsBootImageSignature(const void* data, size_t size)
{
	const uint8_t* sector = data;

	return sector != NULL && size >= 512 && sector[510] == 0x55 && sector[511] == 0xaa;
}

bool SanitizeImagePath(char* path)
{
	size_t i, j;
	bool changed = false;
	char unauthorized[] = { '*', '?', '<', '>', ':', '|' };

	for (i = 0; path[i] != 0; i++) {
		for (j = 0; j < sizeof(unauthorized); j++) {
			if (path[i] == unauthorized[j]) {
				path[i] = '_';
				changed = true;
			}
		}
	}
	return changed;
}

void GetIsoSyslinuxConfig(const RUFUS_IMG_REPORT* report, bool exists, ISO_SYSLINUX_CONFIG* config)
{
	const char* directory_end;
	size_t length;

	memset(config, 0, sizeof(*config));
	if (!HAS_SYSLINUX((*report)))
		return;
	if (exists && !report->needs_syslinux_overwrite)
		return;
	config->create = true;
	config->backup = exists;
	snprintf(config->text, sizeof(config->text),
		"DEFAULT loadconfig\n\nLABEL loadconfig\n  CONFIG %s\n", report->cfg_path);
	directory_end = strrchr(report->cfg_path, '/');
	if (directory_end != NULL && directory_end != report->cfg_path) {
		length = strlen(config->text);
		snprintf(config->text + length, sizeof(config->text) - length,
			"  APPEND %.*s/\n", (int)(directory_end - report->cfg_path), report->cfg_path);
	}
}

// Same bounded, terminating copy as Rufus's safe_strcp, for image report paths.
static void CopyIsoPath(char* dst, size_t capacity, const char* src)
{
	size_t count = strlen(src) + 1;

	if (count > capacity)
		count = capacity;
	memmove(dst, src, count);
	dst[count - 1] = 0;
}

static const char* bootmgr_name = "bootmgr";
const char* bootmgr_efi_name = "bootmgr.efi";
static const char* grldr_name = "grldr";
static const char* ldlinux_c32 = "ldlinux.c32";
const char* md5sum_name[2] = { "md5sum.txt", "MD5SUMS" };
static const char* casper_dirname = "/casper";
static const char* proxmox_dirname = "/proxmox";
const char* efi_dirname = "/efi/boot";
const char* efi_bootname[3] = { "boot", "grub", "mm" };
const char* efi_archname[ARCH_MAX] = { "", "ia32", "x64", "arm", "aa64", "ia64", "riscv64", "loongarch64", "ebc" };
const char* sources_str = "/sources";
const char* wininst_name[3] = { "install.wim", "install.esd", "install.swm" };
// We only support GRUB/BIOS (x86) that uses a standard config dir (/boot/grub/i386-pc/)
// If the disc was mastered properly, GRUB/EFI will take care of itself
const char* grub_dirname[2] = { "/boot/grub/i386-pc", "/boot/grub2/i386-pc" };
static const char* grub_cfg[] = { "grub.cfg", "loopback.cfg" };
static const char* menu_cfg = "menu.cfg";
// NB: Do not alter the order of the array below without validating hardcoded indexes in GetIsoFileProps
const char* syslinux_cfg[5] = { "isolinux.cfg", "syslinux.cfg", "extlinux.conf", "txt.cfg", "live.cfg" };
static const char* isolinux_bin[] = { "isolinux.bin", "boot.bin" };
static const char* pe_dirname[] = { "/i386", "/amd64", "/minint" };
static const char* pe_file[] = { "ntdetect.com", "setupldr.bin", "txtsetup.sif" };
static const char* reactos_name[] = { "setupldr.sys", "freeldr.sys" };
static const char* kolibri_name = "kolibri.img";
static const char* manjaro_marker = ".miso";
static const char* pop_os_name = "pop-os";
const char* old_c32_name[NB_OLD_C32] = OLD_C32_NAMES;
static const int64_t old_c32_threshold[NB_OLD_C32] = OLD_C32_THRESHOLD;

bool GetEfiSyslinuxConfigName(const RUFUS_IMG_REPORT* report, const ISO_FILE_PROPS* props,
	const char* dirname, const char* basename, char name[ISO_SYSLINUX_CONFIG_NAME_CAPACITY])
{
	name[0] = 0;
	if (!props->is_syslinux_cfg || report->has_efi_syslinux)
		return false;
	bool efi_directory = safe_stricmp(dirname, efi_dirname) == 0;
	bool isolinux_config = safe_stricmp(basename, syslinux_cfg[0]) == 0;
	if (!efi_directory || !isolinux_config)
		return false;
	strcpy(name, basename);
	memcpy(name, "sys", 3);
	return true;
}

void GetIsoFileProps(const char* dirname, int64_t file_length, const char* basename,
	ISO_FILE_PROPS* props)
{
	size_t i, len;

	// Check for an isolinux/syslinux config file anywhere
	memset(props, 0, sizeof(*props));
	for (i = 0; i < ARRAYSIZE(syslinux_cfg); i++) {
		if (safe_stricmp(basename, syslinux_cfg[i]) == 0) {
			props->is_cfg = true;	// Required for "extlinux.conf"
			props->is_syslinux_cfg = true;
			props->is_config_path = (i < 3);
		}
	}

	// Check for archiso loader/entries/*.conf files
	if (safe_stricmp(dirname, "/loader/entries") == 0) {
		len = safe_strlen(basename);
		props->is_conf = ((len > 4) && (_stricmp(&basename[len - 5], ".conf") == 0));
	}

	// Check for an old incompatible c32 file anywhere
	for (i = 0; i < NB_OLD_C32; i++) {
		if ((safe_stricmp(basename, old_c32_name[i]) == 0) && (file_length <= old_c32_threshold[i]))
			props->is_old_c32[i] = true;
	}

	// Check for config files that may need patching
	len = safe_strlen(basename);
	if ((len >= 4) && safe_stricmp(&basename[len - 4], ".cfg") == 0) {
		props->is_cfg = true;
		for (i = 0; i < ARRAYSIZE(grub_cfg); i++) {
			if (safe_stricmp(basename, grub_cfg[i]) == 0)
				props->is_grub_cfg = true;
		}
		if (safe_stricmp(basename, menu_cfg) == 0) {
			props->is_menu_cfg = true;
		}
	}

	props->is_ldlinux_c32 = safe_stricmp(basename, ldlinux_c32) == 0;
	props->is_root_ldlinux_sys = dirname != NULL && dirname[0] == 0 && safe_stricmp(basename, "ldlinux.sys") == 0;
	for (i = 0; i < ARRAYSIZE(isolinux_bin); i++) {
		if (safe_stricmp(basename, isolinux_bin[i]) == 0)
			props->is_isolinux = true;
	}
}

static void CheckIsoEfiFile(RUFUS_IMG_REPORT* report, const char* dirname, int64_t file_length,
	const char* basename, const char* fullpath)
{
	size_t i, j, k;
	char bootloader_name[32];

	if (dirname != NULL && dirname[0] == 0) {
		if (safe_stricmp(basename, bootmgr_efi_name) == 0) {
			// We may extract the bootloaders for revocation validation later but
			// to do so, since we're working with case sensitive file systems, we
			// must store all found UEFI bootloader paths with the right case.
			for (j = 0; j < ARRAYSIZE(report->efi_boot_entry); j++) {
				if (report->efi_boot_entry[j].path[0] == 0) {
					report->efi_boot_entry[j].type = EBT_BOOTMGR;
					static_strcpy(report->efi_boot_entry[j].path, fullpath);
					break;
				}
			}
			report->has_efi |= 1;
			report->has_bootmgr_efi = true;
		}
	}

	// Check for the first 'efi*.img' we can find (that hopefully contains EFI boot files)
	if (!HAS_EFI_IMG((*report)) && (safe_strlen(basename) >= 7) &&
		(safe_strnicmp(basename, "efi", 3) == 0) &&
		(safe_stricmp(&basename[strlen(basename) - 4], ".img") == 0))
		static_strcpy(report->efi_img_path, fullpath);

	// Special case for Lenovo UEFI firmware update ISOs, that use emulated El-Torito HDD images
	if (!HAS_EFI_IMG((*report)) && _stricmp(fullpath, "/[BOOT]/0-Boot-HardDisk.img") == 0)
		static_strcpy(report->efi_img_path, fullpath);

	// Check for the EFI boot entries. Note that because of Bazzite maintainers' disregard for end users
	// (evidenced in https://github.com/ublue-os/bazzite/issues/4374) and Fedora's disregards for standards
	// (evidenced in pushing for '/efi/fedora/' to store bootloaders, instead of sticking to '/efi/boot/')
	// we check for anything starting with '/efi/' instead of just '/efi/boot/').
	if (safe_strnicmp(dirname, "/efi/", 5) == 0) {
		for (k = 0; k < ARRAYSIZE(efi_bootname); k++) {
			for (i = 0; i < ARRAYSIZE(efi_archname); i++) {
				static_sprintf(bootloader_name, "%s%s.efi", efi_bootname[k], efi_archname[i]);
				if (safe_stricmp(basename, bootloader_name) == 0) {
					if (k == 0)
						report->has_efi |= (2 << i);	// start at 2 since "bootmgr.efi" is bit 0
					for (j = 0; j < ARRAYSIZE(report->efi_boot_entry); j++) {
						if (report->efi_boot_entry[j].path[0] == 0) {
							report->efi_boot_entry[j].type = (uint8_t)k;
							static_strcpy(report->efi_boot_entry[j].path, fullpath);
							break;
						}
					}
				}
			}
		}
		// Linux Mint Edge 21.2/Mint 21.3 have an invalid /EFI/boot/bootx64.efi
		// because it's a symbolic link to a file that does not exist on the media.
		// This is originally due to a Debian bug that was fixed in:
		// https://salsa.debian.org/live-team/live-build/-/commit/5bff71fea2dd54adcd6c428d3f1981734079a2f7
		// Because of this, if we detect a small bootx64.efi file, we assert that it's a
		// broken link and try to extract a "good" version from the El-Torito image.
		if ((safe_stricmp(basename, "bootx64.efi") == 0) && (file_length < 256)) {
			report->has_efi |= 0x4000;
			static_strcpy(report->efi_img_path, "[BOOT]/1-Boot-NoEmul.img");
		}
	}
}

static void CheckIsoWindowsFile(RUFUS_IMG_REPORT* report, const char* dirname, int64_t file_length,
	const char* basename, const char* fullpath)
{
	size_t i, j;

	if (dirname != NULL) {
		size_t dir_length = strlen(dirname), suffix_length = strlen(sources_str);
		if (safe_stricmp(&dirname[dir_length > suffix_length ? dir_length - suffix_length : 0], sources_str) == 0) {
			// Check for "install.###" in "###/sources/"
			for (i = 0; i < ARRAYSIZE(wininst_name); i++) {
				if (safe_stricmp(basename, wininst_name[i]) == 0) {
					if (report->wininst_index < MAX_WININST) {
						static_sprintf(report->wininst_path[report->wininst_index],
							"?:%s", fullpath);
						report->wininst_index++;
						if (file_length >= 4 * GB)
							report->has_4GB_file |= (0x10 << i);
					}
				}
			}
		}
	}

	// Check for "\sources\\$OEM$\\$$\\Panther\\unattend.xml"
	if ((safe_stricmp(dirname, "/sources/$OEM$/$$/Panther") == 0) &&
		(safe_stricmp(basename, "unattend.xml") == 0))
		report->has_panther_unattend = true;

	// Check for PE (XP) specific files in "/i386", "/amd64" or "/minint"
	for (i = 0; i < ARRAYSIZE(pe_dirname); i++)
		if (safe_stricmp(dirname, pe_dirname[i]) == 0)
			for (j=0; j<ARRAYSIZE(pe_file); j++)
				if (safe_stricmp(basename, pe_file[j]) == 0) {
					report->winpe |= (1<<j)<<(ARRAYSIZE(pe_dirname)*i);
					if (j == 2)
						static_strcpy(report->winpe_path[i], fullpath);
				}
}

void CheckIsoScanFile(RUFUS_IMG_REPORT* report, const char* dirname, int64_t file_length,
	const char* basename, const char* fullpath, const ISO_FILE_PROPS* props)
{
	size_t i;

	if (props->is_syslinux_cfg && safe_stricmp(basename, syslinux_cfg[1]) == 0 &&
		safe_stricmp(dirname, efi_dirname) == 0)
		report->has_efi_syslinux = true;

	// Check for GRUB artifacts
	for (i = 0; i < ARRAYSIZE(grub_dirname); i++) {
		if (safe_stricmp(dirname, grub_dirname[i]) == 0)
			report->has_grub2 = (uint8_t)i + 1;
	}

	// Check for a '/casper#####' directory (non-empty)
	if (safe_strnicmp(dirname, casper_dirname, strlen(casper_dirname)) == 0) {
		report->uses_casper = true;
		if (safe_strstr(dirname, pop_os_name) != NULL)
			report->disable_iso = true;
	}

	// Check for a '/proxmox' directory
	if (safe_stricmp(dirname, proxmox_dirname) == 0) {
		report->disable_iso = true;
	}

	// Check for various files and directories in root (dirname = "")
	if ((dirname != NULL) && (dirname[0] == 0)) {
		if (safe_stricmp(basename, bootmgr_name) == 0) {
			report->has_bootmgr = true;
		}
		if (safe_stricmp(basename, grldr_name) == 0) {
			report->has_grub4dos = true;
		}
		if (safe_stricmp(basename, kolibri_name) == 0) {
			report->has_kolibrios = true;
		}
		if (safe_stricmp(basename, manjaro_marker) == 0) {
			report->disable_iso = true;
		}
		for (i = 0; i < ARRAYSIZE(md5sum_name); i++) {
			if (safe_stricmp(basename, md5sum_name[i]) == 0)
				report->has_md5sum = (uint8_t)(i + 1);
		}
	}

	// Check for ReactOS presence anywhere
	if (report->reactos_path[0] == 0) {
		for (i = 0; i < ARRAYSIZE(reactos_name); i++)
			if (safe_stricmp(basename, reactos_name[i]) == 0)
				static_strcpy(report->reactos_path, fullpath);
	}

	CheckIsoEfiFile(report, dirname, file_length, basename, fullpath);

	CheckIsoWindowsFile(report, dirname, file_length, basename, fullpath);

	for (i = 0; i < NB_OLD_C32; i++) {
		if (props->is_old_c32[i])
			report->has_old_c32[i] = true;
	}
	if (file_length >= 4 * GB && (report->has_4GB_file & 0x0f) != 0x0f)
		report->has_4GB_file++;
}
