/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
/* Selected before src/ by Linux CMake. Names/tables are unchanged byte strings;
 * non-ASCII locale equivalence with the Windows APIs is not assumed. */
#include <string.h>
#include <strings.h>
#define HddPrefixCompare strncasecmp
#define HddFindString strcasestr
