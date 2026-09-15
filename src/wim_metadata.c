/*
 * Rufus: The Reliable USB Formatting Utility
 * WIM metadata and Windows version reporting separated from vhd.c and wue.c
 * Copyright © 2013-2026 Pete Batard <pete@akeo.ie>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "wim_metadata.h"

#include <string.h>

void NormalizeWindowsVersion(winver_t* version)
{
	if (version == NULL)
		return;
	if (version->major <= 5)
	{
		version->major = 0;
		version->minor = 0;
	}
	else if (version->major == 6)
	{
		if (version->minor == 0)
			version->major = 0;
		else if (version->minor == 1)
		{
			version->major = 7;
			version->minor = 0;
		}
		else if (version->minor == 2)
		{
			version->major = 8;
			version->minor = 0;
		}
		else if (version->minor == 3)
		{
			version->major = 8;
			version->minor = 1;
		}
		else if (version->minor == 4)
		{
			version->major = 10;
			version->minor = 0;
		}
	}
	else if (version->major == 10 && version->build > 20000)
		version->major = 11;
}

int ReadWimArchiveInfo(WIMStruct* wim, RUFUS_WIM_INFO* info)
{
	struct wimlib_wim_info native;
	int result;

	if (info == NULL)
		return WIMLIB_ERR_INVALID_PARAM;
	memset(info, 0, sizeof(*info));
	if (wim == NULL)
		return WIMLIB_ERR_INVALID_PARAM;
	result = wimlib_get_wim_info(wim, &native);
	if (result != 0)
		return result;
	info->version = native.wim_version;
	info->image_count = native.image_count;
	info->boot_index = native.boot_index;
	info->part_number = native.part_number;
	info->total_parts = native.total_parts;
	return 0;
}

/* wimlib already decodes XML. Only decimal version fields need conversion;
 * wimlib_tchar handles both its native UTF-8 and Windows UTF-16 APIs. */
static int ParseWimVersionField(const wimlib_tchar* text, uint16_t* value)
{
	uint32_t number = 0;
	size_t i;

	if (text == NULL)
		return 0;
	for (i = 0; i < 32 && text[i] != 0; i++)
	{
		if (text[i] < '0' || text[i] > '9')
			return -1;
		number = number * 10 + (text[i] - '0');
		if (number > UINT16_MAX)
			return -1;
	}
	if (i == 0 || text[i] != 0)
		return -1;
	*value = (uint16_t)number;
	return 1;
}

int ReadWimMetadata(WIMStruct* wim, int image, RUFUS_WIM_METADATA* metadata)
{
	static const char* properties[] = {
		"WINDOWS/VERSION/MAJOR", "WINDOWS/VERSION/MINOR",
		"WINDOWS/VERSION/BUILD", "WINDOWS/VERSION/SPBUILD"
	};
	RUFUS_WIM_METADATA result = { 0 };
	uint16_t* fields[] = { &result.raw_version.major, &result.raw_version.minor,
		&result.raw_version.build, &result.raw_version.revision };
	wimlib_tchar property[32];
	size_t i, j;
	int error, state;

	if (metadata == NULL)
		return WIMLIB_ERR_INVALID_PARAM;
	memset(metadata, 0, sizeof(*metadata));
	if (image < 1)
		return WIMLIB_ERR_INVALID_IMAGE;
	error = ReadWimArchiveInfo(wim, &result.archive);
	if (error != 0)
		return error;
	if ((uint32_t)image > result.archive.image_count)
		return WIMLIB_ERR_INVALID_IMAGE;
	for (i = 0; i < sizeof(properties) / sizeof(properties[0]); i++)
	{
		// Property names are fixed ASCII, independent of wimlib's character width.
		for (j = 0; properties[i][j] != 0; j++)
			property[j] = properties[i][j];
		property[j] = 0;
		state = ParseWimVersionField(wimlib_get_image_property(wim, image, property), fields[i]);
		if (state > 0)
			result.valid_fields |= (uint8_t)(1U << i);
		else if (state < 0)
			result.invalid_fields |= (uint8_t)(1U << i);
	}
	if ((result.valid_fields & WIM_VERSION_REQUIRED) == WIM_VERSION_REQUIRED)
	{
		result.win_version = result.raw_version;
		NormalizeWindowsVersion(&result.win_version);
	}
	*metadata = result;
	return 0;
}
