/*
 * Rufus: The Reliable USB Formatting Utility
 * String array extracted from stdfn.c
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
#include "strarray.h"

#include <stdlib.h>
#include <string.h>

extern void uprintf(const char* format, ...);

void StrArrayCreate(StrArray* arr, uint32_t initial_size)
{
	if (arr == NULL) return;
	arr->Max = initial_size; arr->Index = 0;
	arr->String = (char**)calloc(arr->Max, sizeof(char*));
	if (arr->String == NULL)
		uprintf("Could not allocate string array");
}

int32_t StrArrayAdd(StrArray* arr, const char* str, int duplicate)
{
	char** resized;
	uint32_t capacity;
	if ((arr == NULL) || (arr->String == NULL) || (str == NULL))
		return -1;
	if (arr->Index == arr->Max) {
		if (arr->Max > INT32_MAX / 2)
			return -1;
		capacity = arr->Max == 0 ? 8 : arr->Max * 2;
		if ((uint64_t)capacity * sizeof(char*) > SIZE_MAX)
			return -1;
		resized = (char**)realloc(arr->String, capacity * sizeof(char*));
		if (resized == NULL) {
			uprintf("Could not reallocate string array");
			return -1;
		}
		arr->String = resized;
		arr->Max = capacity;
	}
	arr->String[arr->Index] = (duplicate)?_strdup(str):(char*)str;
	if (arr->String[arr->Index] == NULL) {
		uprintf("Could not store string in array");
		return -1;
	}
	return arr->Index++;
}

int32_t StrArrayFind(StrArray* arr, const char* str)
{
	uint32_t i;
	if ((str == NULL) || (arr == NULL) || (arr->String == NULL))
		return -1;
	for (i = 0; i < arr->Index; i++) {
		if (strcmp(arr->String[i], str) == 0)
			return (int32_t)i;
	}
	return -1;
}

int32_t StrArrayAddUnique(StrArray* arr, const char* str, int duplicate)
{
	int32_t i = StrArrayFind(arr, str);
	return (i < 0) ? StrArrayAdd(arr, str, duplicate) : i;
}

void StrArrayClear(StrArray* arr)
{
	uint32_t i;
	if ((arr == NULL) || (arr->String == NULL))
		return;
	for (i = 0; i < arr->Index; i++) {
		free(arr->String[i]);
		arr->String[i] = NULL;
	}
	arr->Index = 0;
}

void StrArrayDestroy(StrArray* arr)
{
	StrArrayClear(arr);
	if (arr != NULL) {
		free(arr->String);
		arr->String = NULL;
	}
}

