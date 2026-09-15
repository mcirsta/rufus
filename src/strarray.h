/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Basic String Array */
typedef struct {
	char** String;
	uint32_t Index;		// Current array size
	uint32_t Max;		// Maximum array size
} StrArray;
extern void StrArrayCreate(StrArray* arr, uint32_t initial_size);
extern int32_t StrArrayAdd(StrArray* arr, const char* str, int duplicate);
extern int32_t StrArrayAddUnique(StrArray* arr, const char* str, int duplicate);
extern int32_t StrArrayFind(StrArray* arr, const char* str);
extern void StrArrayClear(StrArray* arr);
extern void StrArrayDestroy(StrArray* arr);
#define IsStrArrayEmpty(arr) (arr.Index == 0)

#ifdef __cplusplus
}
#endif
