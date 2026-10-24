/*
 * Rufus: The Reliable USB Formatting Utility
 * Token replacement rules extracted from parser.c
 * Copyright © 2012-2025 Pete Batard <pete@akeo.ie>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "config_text.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define safe_strlen(s) ((s) == NULL ? 0 : strlen(s))

bool IsConfigSection(const wchar_t* line, const wchar_t* section)
{
	const size_t spaces = wcsspn(line, L" \t");
	return _wcsnicmp(line + spaces, section, wcslen(section)) == 0;
}

size_t SplitConfigToken(wchar_t* line, const wchar_t* token, const wchar_t* source,
	size_t offsets[CONFIG_TEXT_MAX_REPLACEMENTS + 1])
{
	static const wchar_t space[] = L" \t";
	size_t i, j, spaces;
	wchar_t* match;

	offsets[0] = 0;
	if (token[0] == 0 || source[0] == 0)
		return 0;
	i = wcsspn(line, space);
	if (_wcsnicmp(&line[i], token, wcslen(token)) != 0)
		return 0;
	i += wcslen(token);
	spaces = wcsspn(&line[i], space);
	if (spaces == 0)
		return 0;
	i += spaces;
	for (j = 0; j < CONFIG_TEXT_MAX_REPLACEMENTS; j++) {
		match = wcsstr(&line[i], source);
		if (match == NULL)
			break;
		i = (size_t)(match - line) + wcslen(source);
		offsets[j + 1] = i;
		*match = 0;
	}
	return j;
}

/*
 * Replace all 'c' characters in string 'src' with the substring 'rep'.
 * The returned string is allocated and must be freed by the caller.
 */
char* replace_char(const char* src, const char c, const char* rep)
{
	size_t i, j, k, count=0, str_len = safe_strlen(src), rep_len = safe_strlen(rep);
	char* res;

	if ((src == NULL) || (rep == NULL))
		return NULL;
	for (i=0; i<str_len; i++) {
		if (src[i] == c)
			count++;
	}
	res = (char*)malloc(str_len + count*rep_len + 1);
	if (res == NULL)
		return NULL;
	for (i=0,j=0; i<str_len; i++) {
		if (src[i] == c) {
			for(k=0; k<rep_len; k++)
				res[j++] = rep[k];
		} else {
// Since the VS Code Analysis tool is dumb...
#if defined(_MSC_VER)
#pragma warning(suppress: 6386)
#endif
			res[j++] = src[i];
		}
	}
	res[j] = 0;
	return res;
}

/* Label and Red Hat passes from iso.c's fix_config, in their original order. */
size_t GetIsoConfigPatches(const char* iso_label, const char* usb_label,
	bool rh8_derivative, const char* image_path, CONFIG_TEXT_PATCH patches[ISO_CONFIG_MAX_PATCHES])
{
	// GRUB linuxefi/linux, openSUSE $linux, Arch search, Artix for, and
	// isolinux/syslinux options/append all receive the same substitutions.
	static const char* cfg_token[] = { "options", "append", "linux", "linuxefi", "$linux", "search", "for" };
	size_t count = 0;
	for (size_t i = 0; i < ISO_CONFIG_LABEL_PASSES; i++) {
		patches[count++] = (CONFIG_TEXT_PATCH){ cfg_token[i], iso_label, usb_label };
	}
	// Keep Rufus's case-sensitive filename heuristic for Red Hat netinst media.
	if (rh8_derivative && strstr(image_path, "netinst") == NULL) {
		for (size_t i = 0; i < ISO_CONFIG_LABEL_PASSES; i++) {
			patches[count++] = (CONFIG_TEXT_PATCH){ cfg_token[i], "inst.stage2", "inst.repo" };
		}
	}
	return count;
}

CONFIG_TEXT_PATCH GetFreeNasConfigPatch(const char* iso_label, const char* usb_label,
	char source[CONFIG_VOLUME_PATH_CAPACITY], char replacement[CONFIG_VOLUME_PATH_CAPACITY])
{
	snprintf(source, CONFIG_VOLUME_PATH_CAPACITY, "cd9660:/dev/iso9660/%s", iso_label);
	snprintf(replacement, CONFIG_VOLUME_PATH_CAPACITY, "msdosfs:/dev/msdosfs/%s", usb_label);
	return (CONFIG_TEXT_PATCH){ "set", source, replacement };
}
