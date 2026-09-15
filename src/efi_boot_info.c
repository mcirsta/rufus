/*
 * Rufus: The Reliable USB Formatting Utility
 * EFI bootloader banner reporting extracted from iso.c
 * Copyright © 2011-2026 Pete Batard <pete@akeo.ie>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "efi_boot_info.h"

#include <ctype.h>
#include <string.h>

extern void uprintf(const char* format, ...);

void GetEfiBootInfo(const char* buf, size_t buf_size, const char* source)
{
	const struct {
		const char* label;
		const char* search_string;
	} boot_info[] = {
		{ "Shim", "UEFI SHIM\n$Version: " },
		// Rufus uses LoaderInfo rather than the optional systemd metadata footer.
		{ "systemd-boot", "#### LoaderInfo: systemd-boot " },
	};
	const size_t margin = 64;
	size_t i, j, k, length;

	if (buf == NULL || buf_size <= margin)
		return;
	for (i = 0; i < buf_size - margin; i++) {
		for (j = 0; j < sizeof(boot_info) / sizeof(boot_info[0]); j++) {
			length = strlen(boot_info[j].search_string);
			if (memcmp(buf + i, boot_info[j].search_string, length) != 0)
				continue;
			i += length;
			for (k = 0; k < 32 && i + k < buf_size - 1 &&
				!isspace((unsigned char)buf[i + k]); k++);
			// Precision bounds the output without replacing a byte in the input.
			uprintf("  Detected %s version: %.*s (from '%s')", boot_info[j].label,
				(int)k, buf + i, source);
			return;
		}
	}
}
