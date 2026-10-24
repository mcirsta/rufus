/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
/* Windows build: retain the exact APIs previously used inside IsHDD. */
#include <windows.h>
#include <shlwapi.h>
#include <string.h>
#define HddPrefixCompare _strnicmp
#define HddFindString StrStrIA
