/*
 * Rufus: The Reliable USB Formatting Utility
 * WinPE options inspection separated from iso.c and parser.c
 * Copyright © 2011-2026 Pete Batard <pete@akeo.ie>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "winpe_scan.h"
#include "iso_read.h"

#include <stdlib.h>
#include <string.h>

extern void uprintf(const char* format, ...);

/* Rufus's get_token_data_line grammar, applied directly to SIF bytes. Only
 * ASCII syntax is interpreted; ANSI/UTF-8 value bytes need no wide conversion. */
static char* WinPeLineOptions(char* line)
{
	char* value;
	bool quoted = false, xml = false;

	line += strspn(line, " \t");
	if (*line == '<')
		line++;
	line += strspn(line, " \t");
	if (_strnicmp(line, "OsLoadOptions", 13) != 0)
		return NULL;
	line += 13;
	line += strspn(line, " \t");
	if (*line == '>')
		xml = true;
	else if (*line != '=')
		return NULL;
	line++;
	line += strspn(line, " \t");
	if (*line == '"') {
		quoted = true;
		line++;
	}
	value = line;
	while (*line != 0 && !(quoted && *line == '"') && !(xml && *line == '<'))
		line++;
	*line = 0;
	return *value != 0 ? value : NULL;
}

int InspectWinPeOptions(char* buffer, size_t size, RUFUS_IMG_REPORT* report)
{
	char *line, *next, *value;
	size_t i;

	if (report == NULL)
		return ISO_SCAN_ERROR;
	report->uses_minint = report->winpe_options_known = 0;
	if (buffer == NULL || size == 0 || size > WINPE_OPTIONS_MAX_SIZE)
		return ISO_SCAN_PARTIAL;
	if (buffer[size - 1] == 0)
		size--;
	if (size == 0 || memchr(buffer, 0, size) != NULL ||
		(size >= 2 && ((uint8_t)buffer[0] == 0xff || (uint8_t)buffer[0] == 0xfe)))
		return ISO_SCAN_PARTIAL;
	buffer[size] = 0;
	line = buffer;
	if (size >= 3 && memcmp(line, "\xef\xbb\xbf", 3) == 0)
		line += 3;
	while (*line != 0) {
		next = line + strcspn(line, "\r\n");
		if (*next != 0)
			*next++ = 0;
		value = WinPeLineOptions(line);
		if (value != NULL) {
			// Preserve Rufus's case-insensitive substring decision, including
			// suffixes such as /minint-extra; do not introduce new boot policy.
			for (i = 0; value[i] != 0; i++) {
				if (value[i] >= 'A' && value[i] <= 'Z')
					value[i] += 'a' - 'A';
			}
			uprintf("  Checking txtsetup.sif:\r\n  OsLoadOptions = %s", value);
			report->uses_minint = strstr(value, "/minint") != NULL;
			break;
		}
		line = next;
	}
	if (*line == 0)
		uprintf("  Checking txtsetup.sif: no OsLoadOptions value (/minint not requested)");
	report->winpe_options_known = 1;
	return ISO_SCAN_OK;
}

int ScanWinPeOptions(const char* image, iso_extension_mask_t mask, RUFUS_IMG_REPORT* report)
{
	static const char* paths[] = { "/i386/txtsetup.sif", "/amd64/txtsetup.sif", "/minint/txtsetup.sif" };
	const char* path;
	uint8_t* buffer = NULL;
	uint32_t size;
	int index, result;

	if (image == NULL || report == NULL)
		return ISO_SCAN_ERROR;
	report->uses_minint = report->winpe_options_known = 0;
	if (IsIsoScanCancelled())
		return ISO_SCAN_CANCELLED;
	if (!HAS_WINPE((*report)))
		return ISO_SCAN_OK;
	index = (report->winpe & WINPE_I386) == WINPE_I386 ? 0 :
		((report->winpe & WINPE_AMD64) == WINPE_AMD64 ? 1 : 2);
	path = report->winpe_path[index][0] ? report->winpe_path[index] : paths[index];
	size = ReadISOFileToBufferLimited(image, path, &buffer, mask, WINPE_OPTIONS_MAX_SIZE);
	result = InspectWinPeOptions((char*)buffer, size, report);
	free(buffer);
	if (IsIsoScanCancelled()) {
		report->uses_minint = report->winpe_options_known = 0;
		return ISO_SCAN_CANCELLED;
	}
	if (result == ISO_SCAN_PARTIAL)
		uprintf("  Could not inspect WinPE options in '%s'", path);
	return result;
}
