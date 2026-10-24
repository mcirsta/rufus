/*
 * Rufus: The Reliable USB Formatting Utility
 * WinPE loader preparation separated from wue.c
 * Copyright © 2022-2026 Pete Batard <pete@akeo.ie>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "winpe_setup.h"
#include "image_report.h"
#include <limits.h>
#include <stddef.h>
#include <string.h>

extern void uprintf(const char* format, ...);

WINPE_SETUP_PLAN GetWinPeSetupPlan(uint16_t winpe, bool uses_minint)
{
	static const char* basedir[] = { "i386", "amd64", "minint" };
	const bool has_i386 = (winpe & WINPE_I386) == WINPE_I386;
	const bool has_amd64 = (winpe & WINPE_AMD64) == WINPE_AMD64;
	const bool has_minint = (winpe & WINPE_MININT) == WINPE_MININT;
	const bool masquerade = (has_i386 || has_amd64 || has_minint) && !uses_minint;
	WINPE_SETUP_PLAN plan = { 0, NULL, NULL, !uses_minint, masquerade, WINPE_PATCH_LOADER };
	if (has_amd64)
		plan.index = 1;
	else if (has_minint)
		plan.index = 2;
	plan.setup_directory = basedir[plan.index];
	plan.loader_directory = basedir[2 * (plan.index / 2)];
	if (winpe & WINPE_MININT)
		plan.action = uses_minint ? WINPE_KEEP_LOADER : WINPE_UNSUPPORTED_LAYOUT;
	return plan;
}

bool PatchWinPeLoader(char* buffer, uint32_t size, unsigned int index, bool uses_minint)
{
	const char* basedir[] = { "i386", "amd64" };
	const char* patch_str_org[] = { "\\minint\\txtsetup.sif", "\\minint\\system32\\" };
	const char* patch_str_rep[2][2] = { { "\\i386\\txtsetup.sif", "\\i386\\system32\\" },
		{ "\\amd64\\txtsetup.sif", "\\amd64\\system32\\" } };
	const char* win_nt_bt_org = "$win_nt$.~bt";
	const char* rdisk_zero = "rdisk(0)";
	const uint32_t crc_offset = 0x2060;
	const uint32_t scan_tail = 32; /* Retain the original excluded tail. */
	const uint32_t rdisk_number_offset = 6;
	const bool invalid_buffer = buffer == NULL && size != 0;
	const bool invalid_index = index >= sizeof(basedir) / sizeof(basedir[0]);
	if (invalid_buffer || invalid_index)
		return false;
	const uint32_t scan_limit = size > scan_tail ? size - scan_tail : 0;

	// Remove CRC check for 32 bit part of setupldr.bin from Win2k3.
	if (size > crc_offset + 1 && buffer[crc_offset] == 0x74 && buffer[crc_offset + 1] == 0x03) {
		buffer[crc_offset] = (char)0xeb;
		buffer[crc_offset + 1] = 0x1a;
		uprintf("  0x00002060: 0x74 0x03 -> 0xEB 0x1A (disable Win2k3 CRC check)\n");
	}
	for (uint32_t i = 1; i < scan_limit; i++) {
		for (size_t j = 0; j < sizeof(patch_str_org) / sizeof(patch_str_org[0]); j++) {
			if (i >= size)
				break; // A preceding match advances the shared scan offset.
			const size_t original_length = strlen(patch_str_org[j]);
			const size_t remaining = size - i;
			const bool match_fits = original_length - 1 <= remaining;
			if (!match_fits)
				continue;
			if (_strnicmp(&buffer[i], patch_str_org[j], original_length - 1) == 0) {
				const size_t replacement_length = strlen(patch_str_rep[index][j]);
				const bool replacement_fits = replacement_length + 1 <= remaining;
				if (!replacement_fits)
					continue;
				const int log_length = remaining > INT_MAX ? INT_MAX : (int)remaining;
				uprintf("  0x%08X: '%.*s' -> '%s'\n", i, log_length, &buffer[i], patch_str_rep[index][j]);
				strcpy(&buffer[i], patch_str_rep[index][j]);
				i += (uint32_t)(original_length > replacement_length ? original_length : replacement_length);
			}
		}
	}
	if (!uses_minint) {
		for (uint32_t i = 0; i < scan_limit; i++) {
			if (_strnicmp(&buffer[i], rdisk_zero, strlen(rdisk_zero) - 1) == 0) {
				buffer[i + rdisk_number_offset] = 0x31;
				uprintf("  0x%08X: '%s' -> 'rdisk(%c)'\n", i, rdisk_zero, buffer[i + rdisk_number_offset]);
			}
			if (_strnicmp(&buffer[i], win_nt_bt_org, strlen(win_nt_bt_org) - 1) == 0) {
				const size_t original_length = strlen(win_nt_bt_org);
				const size_t replacement_length = strlen(basedir[index]);
				const uint32_t remaining = size - i;
				const int log_length = remaining > INT_MAX ? INT_MAX : (int)remaining;
				uprintf("  0x%08X: '%.*s' -> '%s%.*s'\n", i, log_length, &buffer[i], basedir[index],
					log_length - (int)original_length, &buffer[i + original_length]);
				strcpy(&buffer[i], basedir[index]);
				buffer[i + replacement_length] = buffer[i + original_length];
				buffer[i + replacement_length + 1] = 0;
			}
		}
	}
	return true;
}
