/*
 * Rufus: The Reliable USB Formatting Utility
 * Syslinux version detection extracted from syslinux.c
 * Copyright © 2011-2026 Pete Batard <pete@akeo.ie>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "syslinux_version.h"

#include <stdlib.h>
#include <string.h>

static char empty_version_ext[] = "";

static uint16_t ParseSyslinuxVersion(char* text, char** suffix)
{
	unsigned long version[2];
	char *end, *minor;

	version[0] = strtoul(text, &end, 10);
	if (end == text || version[0] >= 256 || *end != '.')
		return 0;
	minor = end + 1;
	version[1] = strtoul(minor, &end, 10);
	if (end == minor || version[1] >= 256)
		return 0;
	*suffix = end;
	return (uint16_t)((version[0] << 8) + version[1]);
}

static char* NormalizeSyslinuxSuffix(char* version, char* suffix)
{
	const char unauthorized[] = { '<', '>', ':', '|', '*', '?', '\\', '/' };
	size_t j, k, version_length = (size_t)(suffix - version);

	// A version without a suffix already ends in NUL; preserve that terminator.
	if (*suffix == 0)
		return empty_version_ext;
	*suffix = '/';
	// Remove the x.yz- duplicate if present, as Rufus does for distro builds.
	for (j = 0; j < version_length && suffix[1 + j] != 0 && version[j] == suffix[1 + j]; j++);
	if (suffix[j + 1] == '-')
		j++;
	if (j >= 4) {
		suffix[j] = '/';
		suffix += j;
	}
	// Arch Linux may append stars; retain internal spaces, but trim spaces/stars.
	for (j = strlen(suffix) - 1; j > 0; j--) {
		if (suffix[j] != ' ' && suffix[j] != '*')
			break;
		suffix[j] = 0;
	}
	for (j = 1; suffix[j] != 0; j++) {
		for (k = 0; k < sizeof(unauthorized); k++) {
			if (suffix[j] == unauthorized[k])
				suffix[j] = '_';
		}
	}
	return suffix[1] == 0 ? empty_version_ext : suffix;
}

uint16_t GetSyslinuxVersion(char* buf, size_t buf_size, char** ext)
{
	const char linux_marker[] = { 'L', 'I', 'N', 'U', 'X', ' ' };
	char *text, *suffix, *terminator = NULL;
	size_t i;
	uint16_t version;

	if (ext == NULL)
		return 0;
	*ext = empty_version_ext;
	if (buf == NULL || buf_size < 256)
		return 0;
	// Skip the early incomplete version in ldlinux.sys, retaining Rufus's range.
	for (i = 64; i < buf_size - 64; i++) {
		if (memcmp(buf + i, linux_marker, sizeof(linux_marker)) != 0)
			continue;
		if (memcmp(buf + i - 3, "ISO", 3) != 0 && memcmp(buf + i - 3, "SYS", 3) != 0)
			continue;
		text = buf + i + sizeof(linux_marker);
		// Reuse the terminator when several candidates occur in the same string.
		if (terminator == NULL || terminator < text) {
			terminator = memchr(text, 0, buf_size - i - sizeof(linux_marker));
			if (terminator == NULL)
				return 0;
		}
		version = ParseSyslinuxVersion(text, &suffix);
		if (version == 0)
			continue;
		*ext = NormalizeSyslinuxSuffix(text, suffix);
		return version;
	}
	return 0;
}
