/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

/* Selected by the Linux build for shared C files using these CRT calls.
 * Both implementations compare byte strings in the current C locale. Paths
 * retain their original bytes; these calls only match known media markers. */
#include <strings.h>
#include <wchar.h>
#define _stricmp strcasecmp
#define _strnicmp strncasecmp
#define _wcsnicmp wcsncasecmp

#define _strdup strdup
