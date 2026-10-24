/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Host/CRT stand-ins for the frozen DOS text oracle, not a Windows runtime. */
#pragma once
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint32_t ULONG;
typedef uint32_t UINT;
typedef int BOOL;
#define TRUE 1
#define FALSE 0
#define MAX_PATH 260
#define ARRAYSIZE(A) (sizeof(A) / sizeof((A)[0]))
#define safe_strcmp(a, b) strcmp((a) == NULL ? "<NULL>" : (a), (b) == NULL ? "<NULL>" : (b))
#define static_strcpy(destination, source) snprintf(destination, sizeof(destination), "%s", source)
#define static_strcat(destination, source) strncat(destination, source, sizeof(destination) - strlen(destination) - 1)
#define uprintf(...) ((void)0)
#define WindowsErrorString() "reference error"
#define LOCALE_IDEFAULTCODEPAGE 0
#define LOCALE_RETURN_NUMBER 0
#define GetUserDefaultUILanguage() 0
#define GetLocaleInfoA(...) 0

static const char* test_keyboard;
static ULONG test_codepage;
static const char* get_kb(void) { return test_keyboard; }
static ULONG GetOEMCP(void) { return test_codepage; }
BOOL SetDOSLocale(const char* path, BOOL freedos);

int main(int argc, char** argv)
{
	if (argc != 5)
		return 64;
	test_keyboard = argv[2];
	test_codepage = (ULONG)strtoul(argv[3], NULL, 10);
	return SetDOSLocale(argv[1], atoi(argv[4])) ? 0 : 1;
}
