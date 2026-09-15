/*
 * Rufus: The Reliable USB Formatting Utility
 * Syslinux scan policy extracted from iso.c
 * Copyright © 2011-2026 Pete Batard <pete@akeo.ie>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "syslinux_scan.h"
#include "syslinux_version.h"

#include <stdio.h>
#include <string.h>

extern void uprintf(const char* format, ...);

bool SelectSyslinuxConfig(RUFUS_IMG_REPORT* report, const StrArray* configs)
{
	const char* selected = NULL;
	size_t i, length, shortest = sizeof(report->cfg_path) - 1;

	report->has_syslinux = configs->Index != 0;
	report->needs_syslinux_overwrite = 0;
	report->cfg_path[0] = 0;
	report->sl_detected_version = report->sl_version = 0;
	report->sl_detected_version_ext[0] = report->sl_version_ext[0] = 0;
	report->sl_version_str[0] = 0;
	report->sl_version_inferred = 0;
	for (i = 0; i < configs->Index; i++) {
		// openSUSE's root config does not work; keep Rufus's loader-directory choice.
		// This alone does not fix its ISO9660-only live scripts (openSUSE/kiwi#354).
		if (_stricmp(configs->String[i], "/boot/i386/loader/isolinux.cfg") == 0 ||
			_stricmp(configs->String[i], "/boot/x86_64/loader/isolinux.cfg") == 0) {
			selected = configs->String[i];
			report->needs_syslinux_overwrite = 1;
			break;
		}
		// At equal length choose the latest, as required by Tails' duplicate configs.
		length = strlen(configs->String[i]);
		if (length <= shortest) {
			selected = configs->String[i];
			shortest = length;
		}
	}
	if (selected == NULL)
		return false;
	memcpy(report->cfg_path, selected, strlen(selected) + 1);
	uprintf("  Will use '%s' for Syslinux", report->cfg_path);
	return true;
}

const char* SelectSyslinuxCandidate(RUFUS_IMG_REPORT* report, const char* selected_source,
	const char* source, uint16_t version, const char* ext)
{
	size_t directory_length;
	bool select = report->sl_detected_version == 0;

	if (!select && (report->sl_detected_version != version ||
		strcmp(report->sl_detected_version_ext, ext) != 0)) {
		uprintf("  Found conflicting Isolinux versions:\r\n  '%s' (%d.%02d%s) vs '%s' (%d.%02d%s)",
			selected_source, SL_MAJOR(report->sl_detected_version), SL_MINOR(report->sl_detected_version),
			report->sl_detected_version_ext, source, SL_MAJOR(version), SL_MINOR(version), ext);
		// Retain Rufus's prefix comparison exactly, including a zero-length root
		// prefix and matches without a directory boundary after the prefix.
		for (directory_length = strlen(report->cfg_path); directory_length > 0 &&
			report->cfg_path[directory_length] != '/'; directory_length--);
		select = _strnicmp(report->cfg_path, source, directory_length) == 0;
	}
	if (select) {
		report->sl_detected_version = version;
		snprintf(report->sl_detected_version_ext, sizeof(report->sl_detected_version_ext), "%s", ext);
		selected_source = source;
	}
	return selected_source;
}

void SelectSyslinuxVersion(RUFUS_IMG_REPORT* report, const char* source,
	bool has_ldlinux_c32, const uint16_t embedded_version[2])
{
	report->sl_version = 0;
	report->sl_version_str[0] = report->sl_version_ext[0] = 0;
	report->sl_version_inferred = 0;
	if (!report->has_syslinux)
		return;
	if (report->sl_detected_version != 0) {
		report->sl_version = report->sl_detected_version;
		memcpy(report->sl_version_ext, report->sl_detected_version_ext, sizeof(report->sl_version_ext));
		snprintf(report->sl_version_str, sizeof(report->sl_version_str), "%d.%02d",
			SL_MAJOR(report->sl_version), SL_MINOR(report->sl_version));
		uprintf("  Detected Syslinux version: %s%s (from '%s')",
			report->sl_version_str, report->sl_version_ext, source);
		if (has_ldlinux_c32 != (SL_MAJOR(report->sl_version) >= 5))
			uprintf("  Warning: Conflict between Isolinux version and the presence of ldlinux.c32...");
	} else {
		report->sl_version = embedded_version[has_ldlinux_c32 ? 1 : 0];
		if (report->sl_version == 0) {
			uprintf("  Warning: Could not detect Isolinux version - No fallback version available");
			return;
		}
		report->sl_version_inferred = 1;
		snprintf(report->sl_version_str, sizeof(report->sl_version_str), "%d.%02d",
			SL_MAJOR(report->sl_version), SL_MINOR(report->sl_version));
		uprintf("  Warning: Could not detect Isolinux version - Forcing to %s (embedded)",
			report->sl_version_str);
	}
}
