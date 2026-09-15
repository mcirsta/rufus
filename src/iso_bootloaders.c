/*
 * Rufus: The Reliable USB Formatting Utility
 * Bootloader content stages extracted from iso.c
 * Copyright © 2011-2026 Pete Batard <pete@akeo.ie>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "iso_bootloaders.h"
#include "iso_read.h"
#include "image_file.h"
#include "syslinux_scan.h"
#include "syslinux_version.h"
#include "grub_inspect.h"
#include "efi_boot_info.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void uprintf(const char* format, ...);

int ScanSyslinuxBootloaders(const char* image, iso_extension_mask_t mask,
	const ISO_SCAN_STATE* state, const uint16_t embedded_version[2])
{
	RUFUS_IMG_REPORT* report = state->report;
	const char* source = NULL;
	uint8_t* buf;
	char* ext;
	uint16_t version;
	uint32_t i, size;
	int result = ISO_SCAN_OK;

	if (IsIsoScanCancelled())
		return ISO_SCAN_CANCELLED;
	if (!SelectSyslinuxConfig(report, state->config_path)) {
		if (!report->has_syslinux)
			return ISO_SCAN_OK;
		uprintf("  Syslinux configuration paths exceed the supported length");
		return ISO_SCAN_ERROR;
	}
	for (i = 0; i < state->isolinux_path->Index; i++) {
		if (IsIsoScanCancelled())
			return ISO_SCAN_CANCELLED;
		size = ReadISOFileToBufferExt(image, state->isolinux_path->String[i], &buf, mask);
		if (size == 0) {
			uprintf("  Could not access %s", state->isolinux_path->String[i]);
			result = ISO_SCAN_PARTIAL;
			continue;
		}
		version = GetSyslinuxVersion((char*)buf, (size_t)size + 1, &ext);
		source = SelectSyslinuxCandidate(report, source, state->isolinux_path->String[i], version, ext);
		free(buf);
	}
	if (IsIsoScanCancelled())
		return ISO_SCAN_CANCELLED;
	SelectSyslinuxVersion(report, source, state->has_ldlinux_c32, embedded_version);
	return result;
}

static int ScanBiosGrub(const char* image, iso_extension_mask_t mask, RUFUS_IMG_REPORT* report)
{
	char path[128];
	uint8_t* buf;
	uint32_t size;
	uint8_t directory = report->has_grub2 & 0x7f;

	if (directory == 0)
		return ISO_SCAN_OK;
	if (directory > 2)
		return ISO_SCAN_ERROR;
	if (IsIsoScanCancelled())
		return ISO_SCAN_CANCELLED;
	snprintf(path, sizeof(path), "%s/normal.mod", grub_dirname[directory - 1]);
	report->grub2_version[0] = 0;
	size = ReadISOFileToBufferExt(image, path, &buf, mask);
	if (size == 0)
		uprintf("  Could not read Grub version from '%s'", path);
	else
		GetGrubVersion((char*)buf, (size_t)size + 1, path, report);
	free(buf);
	if (report->grub2_version[0] == 0) {
		uprintf("  Could not detect Grub version");
		report->has_grub2 &= 0x80;
	}
	return size == 0 ? ISO_SCAN_PARTIAL : ISO_SCAN_OK;
}

int ScanGrubAndEfiBootloaders(const char* image, iso_extension_mask_t mask, RUFUS_IMG_REPORT* report)
{
	StrArray filesystems = { 0 };
	uint8_t* buf;
	uint32_t size;
	size_t i, length = 0;
	char names[256] = { 0 };
	int result;

	if (IsIsoScanCancelled())
		return ISO_SCAN_CANCELLED;
	result = ScanBiosGrub(image, mask, report);
	if (result == ISO_SCAN_ERROR || result == ISO_SCAN_CANCELLED)
		return result;
	StrArrayCreate(&filesystems, 8);
	if (filesystems.String == NULL)
		return ISO_SCAN_ERROR;
	for (i = 0; i < sizeof(report->efi_boot_entry) / sizeof(report->efi_boot_entry[0]); i++) {
		const efi_boot_entry_t* entry = &report->efi_boot_entry[i];
		if (!entry->path[0] || (entry->type != EBT_GRUB && entry->type != EBT_MAIN))
			continue;
		if (IsIsoScanCancelled()) {
			result = ISO_SCAN_CANCELLED;
			goto out;
		}
		size = ReadISOFileToBufferExt(image, entry->path, &buf, mask);
		if (size == 0) {
			if (entry->type == EBT_GRUB)
				uprintf("  Could not read Grub version from '%s'", entry->path);
			else
				uprintf("  Could not parse '%s'", entry->path);
			result = ISO_SCAN_PARTIAL;
			continue;
		}
		if (entry->type == EBT_GRUB) {
			report->has_grub2 |= 0x80;
			GetGrubVersion((char*)buf, (size_t)size + 1, entry->path, report);
			if (!GetGrubFs((char*)buf, (size_t)size + 1, &filesystems)) {
				uprintf("  Could not store GRUB filesystem names");
				free(buf);
				result = ISO_SCAN_ERROR;
				goto out;
			}
		} else {
			GetEfiBootInfo((char*)buf, size, entry->path);
		}
		free(buf);
	}
	if (IsIsoScanCancelled()) {
		result = ISO_SCAN_CANCELLED;
		goto out;
	}
	for (i = 0; i < filesystems.Index; i++) {
		snprintf(names + length, sizeof(names) - length, "%s%s",
			i == 0 ? "" : ", ", filesystems.String[i]);
		length = strlen(names);
	}
	report->has_grub2_fs |= GetGrubFsMask(&filesystems);
	if (*names)
		uprintf("  Supported GRUB filesystems: %s", names);
out:
	StrArrayDestroy(&filesystems);
	return result;
}
