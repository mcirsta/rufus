/*
 * Rufus: The Reliable USB Formatting Utility
 * GRUB inspection extracted from iso.c; label normalization from parser.c
 * Copyright © 2011-2026 Pete Batard <pete@akeo.ie>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "grub_inspect.h"

#include <string.h>

extern void uprintf(const char* format, ...);

/*
 * Sanitize an ISO volume label or GRUB version, so that we can use it for bootloader lookup.
 * Note that this call modifies the string passed as parameter.
 */
static int sanitize_label(char* label)
{
	// Notice: Do not add "-beta" to this list as we have existing GRUB lookups for
	// "grub-2.02-beta2" and stuff...
	static const char* remove[] = { "-i386", "-i686", "-amd64", "-x86-64", ".x86-64",
		"-x64", "-armhf", "-arm64", "-aarch64", "-32-bit", "-64-bit", "-32bit", "-64bit",
		"-intel", "-cd", "-dvd", "-standard", "-live", "-install", "-server", "-net",
		"-desktop", "-lts", "-studio", "-baseos", "-kde", "-xfce", "-lxde", "-gnome",
		"-mate", "-unstable", "-debug", "-release", "-final", "-stream", "-cinnamon",
		"-cinn", "-leap", "-tumbleweed", "-budgie", "-ws", "-iot", "-ostree", ".iso"
	};
	size_t i, len;
	char *s;

	len = strlen(label);
	for (i = 0; i < len; i++) {
		char c = label[i];
		// Convert to lowercase
		if (c >= 'A' && c <= 'Z')
			c += 0x20;
		// Convert non alphanum (except '.') to dash
		if ((c < '0' && c != '.') || (c > '9' && c < 'a') || (c > 'z'))
			c = '-';
		label[i] = c;
	}

	// Remove all leading '-'
	for (i = 0; i < len && label[i] == '-'; i++);
	if (i != 0)
		memmove(label, &label[i], len - i + 1);
	len = strlen(label);
	if (len <= 1)
		return -1;

	// Remove all trailing '-'
	for (i = len - 1; i > 0 && label[i] == '-'; i--)
		label[i] = 0;
	len = strlen(label);
	if (len <= 1)
		return -1;

	// Remove all duplicate '-' (non-optimized!)
	for (i = 0; len >= 2 && i < len - 2; i++) {
		if (label[i] == '-' && label[i + 1] == '-') {
			memmove(&label[i + 1], &label[i + 2], len - i - 1);
			len--;
			i--;
		}
	}

	// Remove specific substrings
	for (i = 0; i < (sizeof(remove) / sizeof(remove[0])); i++) {
		s = strstr(label, remove[i]);
		if (s != NULL)
			memmove(s, &s[strlen(remove[i])], strlen(&s[strlen(remove[i])]) + 1);
	}

	return 0;
}

static bool ReadGrubVersion(const char* buf, size_t size, char* version, size_t capacity)
{
	const char* markers[] = { "GRUB  version %s", "GRUB version %s" };
	const char debug_marker[] = "grub_debug_is_enabled";
	const size_t margin = 32;
	const char *text, *terminator = NULL;
	size_t i, j, length;
	bool has_debug = false;

	version[0] = 0;
	if (buf == NULL || size <= margin)
		return false;
	for (i = 0; i < size - margin; i++) {
		for (j = 0; j < sizeof(markers) / sizeof(markers[0]); j++) {
			length = strlen(markers[j]);
			if (memcmp(buf + i, markers[j], length) != 0)
				continue;
			// CentOS adds a newline before the separator NUL. Retain Rufus's skip.
			if (buf[i + length + 1] == 0)
				i++;
			text = buf + i + length + 1;
			// Cache the end for overlapping candidates, including a missing NUL.
			if (terminator == NULL || terminator < text) {
				terminator = memchr(text, 0, size - (size_t)(text - buf));
				if (terminator == NULL)
					terminator = buf + size;
			}
			if (terminator == buf + size)
				continue;
			length = (size_t)(terminator - text);
			if (length >= capacity)
				length = capacity - 1;
			memcpy(version, text, length);
			version[length] = 0;
		}
		if (memcmp(buf + i, debug_marker, sizeof(debug_marker) - 1) == 0)
			has_debug = true;
	}
	return has_debug;
}

void GetGrubVersion(const char* buf, size_t buf_size, const char* source,
	RUFUS_IMG_REPORT* report)
{
	char version[sizeof(report->grub2_version)];
	const char* suffixes[4];
	size_t i, used, length;
	bool has_debug, append_label;

	if (report == NULL)
		return;
	has_debug = ReadGrubVersion(buf, buf_size, version, sizeof(version));
	uprintf("  Detected GRUB version: %s (from '%s')", version, source);
	if (report->grub2_version[0] != 0 || version[0] == 0 || version[0] == '0')
		return;

	// Preserve Rufus's core.img lookup rules for nonstandard /boot/grub2 prefixes,
	// patched builds exposing grub_debug_is_enabled, and ambiguous GRUB 2.06 builds.
	// The suffix order must agree with the existing payload server.
	append_label = strcmp(version, "2.06") == 0;
	suffixes[0] = (report->has_grub2 & 0x7f) > 1 ? "-nonstandard" : "";
	suffixes[1] = has_debug ? "-gdie" : "";
	suffixes[2] = append_label ? "-" : "";
	suffixes[3] = append_label ? report->label : "";
	used = strlen(version);
	memcpy(report->grub2_version, version, used + 1);
	for (i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
		length = strlen(suffixes[i]);
		if (length > sizeof(report->grub2_version) - used - 1)
			length = sizeof(report->grub2_version) - used - 1;
		memcpy(report->grub2_version + used, suffixes[i], length);
		used += length;
	}
	report->grub2_version[used] = 0;
	sanitize_label(report->grub2_version);
}

bool NeedsGrub2CoreLookup(const RUFUS_IMG_REPORT* report, bool mbr, bool bios,
	const char* embedded_version)
{
	if (report == NULL || embedded_version == NULL)
		return false;
	const bool bios_mbr = mbr && bios;
	if (!bios_mbr)
		return false;
	const bool detected = report->has_grub2 && report->grub2_version[0] != 0;
	if (!detected)
		return false;
	const bool terminated = memchr(report->grub2_version, 0, sizeof(report->grub2_version)) != NULL;
	if (!terminated)
		return false;
	return strcmp(report->grub2_version, embedded_version) != 0;
}

size_t GetGrub2DownloadVersionLength(const RUFUS_IMG_REPORT* report, size_t before)
{
	if (report == NULL || before == 0)
		return 0;
	const char* version = report->grub2_version;
	const char* end = memchr(version, 0, sizeof(report->grub2_version));
	if (end == NULL)
		return 0;
	size_t length = (size_t)(end - version);
	if (before <= length)
		length = before - 1;
	for (; length > 0; length--) {
		const char c = version[length];
		if (c == 0 || c == '.' || c == '-')
			return length;
	}
	return 0;
}

bool GetGrubFs(const char* buf, size_t buf_size, StrArray* filesystems)
{
	const char marker[] = "fshelp";
	const size_t margin = 32;
	const char* name;
	size_t i;

	if (filesystems == NULL)
		return false;
	if (buf == NULL || buf_size <= margin)
		return true;
	for (i = 0; i < buf_size - margin; i++) {
		if (memcmp(buf + i, marker, sizeof(marker)) != 0)
			continue;
		name = buf + i + sizeof(marker);
		// The search margin leaves room for all 12 bytes inside the input span.
		if (*name != 0 && memchr(name, 0, 12) != NULL) {
			if (StrArrayAddUnique(filesystems, name, true) < 0)
				return false;
		}
	}
	return true;
}

uint8_t GetGrubFsMask(const StrArray* filesystems)
{
	const char* names[] = { "fat", "exfat", "ntfs" };
	size_t i, j;
	uint8_t mask = 0;

	if (filesystems == NULL)
		return 0;
	for (i = 0; i < filesystems->Index; i++) {
		for (j = 0; j < sizeof(names) / sizeof(names[0]); j++) {
			if (_stricmp(filesystems->String[i], names[j]) == 0)
				mask |= (1 << j);
		}
	}
	return mask;
}
