/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define WINPE_SETUP_SECTION "[SetupData]"
#define WINPE_SETUP_SOURCE_DEVICE "SetupSourceDevice = \"\\device\\harddisk1\\partition1\""

typedef enum {
	WINPE_PATCH_LOADER,
	WINPE_KEEP_LOADER,
	WINPE_UNSUPPORTED_LAYOUT
} WINPE_SETUP_ACTION;

typedef struct {
	unsigned int index; /* Original i386/amd64/minint ordering. */
	const char* setup_directory;
	const char* loader_directory; /* AMD64 also loads from i386. */
	bool copy_sif;
	bool masquerade_disk; /* WriteMBR uses 0x81 rather than 0x80 for legacy WinPE. */
	WINPE_SETUP_ACTION action;
} WINPE_SETUP_PLAN;

#ifdef __cplusplus
extern "C" {
#endif

/* Original SetupWinPE selection, including partial minint bits and early exit,
 * plus WriteMBR's full-family masquerading predicate. Describes copy/patch
 * order; does not validate files or perform I/O. */
WINPE_SETUP_PLAN GetWinPeSetupPlan(uint16_t winpe, bool uses_minint);

/* Original SETUPLDR transformations for index 0 (i386) or 1 (amd64).
 * No allocation/I/O. Inputs of at most 32 bytes are unchanged; comparisons, writes and logs
 * stay within size bytes. NULL is accepted only for size zero. Invalid arguments
 * return false without mutation. Matching retains Rufus's omitted final pattern
 * character, scan offsets/tail, pass order, NUL writes and following character.
 * Success does not establish a valid executable or bootability. */
bool PatchWinPeLoader(char* buffer, uint32_t size, unsigned int index, bool uses_minint);

#ifdef __cplusplus
}
#endif
