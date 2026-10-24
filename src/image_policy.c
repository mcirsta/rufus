/*
 * Rufus: The Reliable USB Formatting Utility
 * Image policy extracted from ImageScanThread in rufus.c
 * Copyright © 2011-2026 Pete Batard <pete@akeo.ie>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */
#include "image_policy.h"
#include "cregex.h"

#include <stddef.h>
#include <string.h>

/* Implemented by the host's existing reporting code. */
extern void uprintf(const char* format, ...);

static bool IsRedHatDerivative(const char* label)
{
	// Labels requiring the inst.stage2 -> inst.repo workaround in ISO mode.
	// See https://github.com/rhinstaller/anaconda/pull/3529.
	static const char* const patterns[] = {
		"^AlmaLinux-[8-9].*",
		"^Fedora.*-3[3-9].*",
		"^CentOS.*-[8-9].*",
		"^Circle.*-[8-9].*",
		"^OL-[8-9].*",
		"^RHEL-[8-9].*",
		"^Rocky-[8-9].*",
		"^MIRACLE-LINUX-[8-9].*",
	};
	const char* matches[REGEX_VM_MAX_MATCHES];
	cregex_node_t* node;
	cregex_program_t* program;
	bool matched;

	for (size_t i = 0; i < sizeof(patterns) / sizeof(patterns[0]); i++) {
		node = cregex_parse(patterns[i]);
		program = NULL;
		if (node != NULL) {
			program = cregex_compile_node(node);
			cregex_parse_free(node);
		}
		if (program == NULL) {
			uprintf("Internal error: Failed to parse '%s'", patterns[i]);
			return false;
		}
		matched = cregex_program_run(program, label, matches, REGEX_VM_MAX_MATCHES) > 0;
		cregex_compile_free(program);
		if (matched)
			return true;
	}
	return false;
}

static bool RequiresDd(const RUFUS_IMG_REPORT* report)
{
	return IS_DD_BOOTABLE((*report)) && (!report->is_iso || report->disable_iso ||
		(!IS_BIOS_BOOTABLE((*report)) && !IS_EFI_BOOTABLE((*report))));
}

bool ApplyIsoPolicy(RUFUS_IMG_REPORT* report)
{
	// These SUSE images require an ISO9660 live device.
	// See https://bugzilla.suse.com/show_bug.cgi?id=1243236.
	static const char* const dd_only_labels[] = {
		"Install-SUSE",
		"Install-LEAP",
		"openSUSE-Tumbleweed"
	};

	if (!report->is_iso)
		return false;
	if (!report->rh8_derivative)
		report->rh8_derivative = IsRedHatDerivative(report->label);

	for (size_t i = 0; i < sizeof(dd_only_labels) / sizeof(dd_only_labels[0]); i++) {
		if (strncmp(report->label, dd_only_labels[i], strlen(dd_only_labels[i])) == 0) {
			report->disable_iso = true;
			break;
		}
	}

	// EFI GRUB must support a target filesystem; large files need exFAT or NTFS.
	if (report->has_grub2 & 0x80) {
		if (!(report->has_grub2_fs & 0x7) || (report->has_4GB_file && !(report->has_grub2_fs & 0x6)))
			report->disable_iso = true;
	}

	// A DD-capable hybrid needs DD mode when its ISO extraction path is unsupported.
	return RequiresDd(report);
}

bool CanWriteIsoToEsp(const RUFUS_IMG_REPORT* report, bool gpt, bool fat)
{
	return report != NULL && report->projected_size < MAX_ISO_TO_ESP_SIZE
		&& HAS_REGULAR_EFI((*report)) && gpt && fat;
}

IMAGE_WRITE_CHOICES GetImageWriteChoices(const RUFUS_IMG_REPORT* report,
	uint64_t persistence_size, bool gpt, bool fat)
{
	IMAGE_WRITE_CHOICES choices = { { IMAGE_WRITE_NONE }, 0 };

	if (RequiresDd(report)) {
		choices.mode[choices.count++] = IMAGE_WRITE_DD;
		return choices;
	}
	if (!report->is_iso)
		return choices;
	choices.mode[choices.count++] = IMAGE_WRITE_ISO;
	if (IS_DD_BOOTABLE((*report)) && persistence_size == 0) {
		if (CanWriteIsoToEsp(report, gpt, fat))
			choices.mode[choices.count++] = IMAGE_WRITE_ESP;
		choices.mode[choices.count++] = IMAGE_WRITE_DD;
	}
	return choices;
}

IMAGE_WRITE_CHOICES GetFinalImageWriteChoices(const RUFUS_IMG_REPORT* report,
	uint64_t persistence_size, bool gpt, bool fat)
{
	IMAGE_WRITE_CHOICES choices = GetImageWriteChoices(report, persistence_size, gpt, fat);
	if (choices.count == 1 && choices.mode[0] == IMAGE_WRITE_ISO && CanWriteIsoToEsp(report, gpt, fat))
		choices.mode[choices.count++] = IMAGE_WRITE_ESP;
	return choices;
}

enum IMAGE_WRITE_MODE GetImageWriteMode(const IMAGE_WRITE_CHOICES* choices, int selected)
{
	for (int i = 0; i < choices->count; i++) {
		if (selected == (1 << i))
			return choices->mode[i];
	}
	return IMAGE_WRITE_NONE;
}
