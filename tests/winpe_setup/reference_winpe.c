/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Unmodified loader loops from baseline wue.c:SetupWinPE.
 * Test-only oracle for well-formed, terminated buffers of at least 64 bytes.
 * Never used for short/truncated fuzz cases or linked into production. */
#include <assert.h>
#include <stdint.h>
#include <string.h>
#define DWORD uint32_t
#define ARRAYSIZE(a) (sizeof(a) / sizeof((a)[0]))
#define max(a, b) ((a) > (b) ? (a) : (b))
#define safe_strnicmp _strnicmp
/* Reference logging is intentionally inert; production logging is tested. */
#define uprintf(...) ((void)0)

void ReferencePatchWinPe(char* buffer, uint32_t size, unsigned int index, int uses_minint)
{
	const char* basedir[3] = { "i386", "amd64", "minint" };
	const char* patch_str_org[2] = { "\\minint\\txtsetup.sif", "\\minint\\system32\\" };
	const char* patch_str_rep[2][2] = { { "\\i386\\txtsetup.sif", "\\i386\\system32\\" },
		{ "\\amd64\\txtsetup.sif", "\\amd64\\system32\\" } };
	const char* win_nt_bt_org = "$win_nt$.~bt";
	const char* rdisk_zero = "rdisk(0)";
	DWORD i, j;
	// Remove CRC check for 32 bit part of setupldr.bin from Win2k3
	if ((size > 0x2061) && (buffer[0x2060] == 0x74) && (buffer[0x2061] == 0x03)) {
		buffer[0x2060] = 0xeb;
		buffer[0x2061] = 0x1a;
		uprintf("  0x00002060: 0x74 0x03 -> 0xEB 0x1A (disable Win2k3 CRC check)\n");
	}
	for (i = 1; i < size - 32; i++) {
		for (j = 0; j < ARRAYSIZE(patch_str_org); j++) {
			if (safe_strnicmp(&buffer[i], patch_str_org[j], strlen(patch_str_org[j]) - 1) == 0) {
				assert(index < 2);
				uprintf("  0x%08X: '%s' -> '%s'\n", i, &buffer[i], patch_str_rep[index][j]);
				strcpy(&buffer[i], patch_str_rep[index][j]);
				i += (DWORD)max(strlen(patch_str_org[j]), strlen(patch_str_rep[index][j]));	// in case org is a substring of rep
			}
		}
	}

	if (!uses_minint) {
		// Additional setupldr.bin/bootmgr patching
		for (i = 0; i < size - 32; i++) {
			// rdisk(0) -> rdisk(#) disk masquerading
			// NB: only the first one seems to be needed
			if (safe_strnicmp(&buffer[i], rdisk_zero, strlen(rdisk_zero) - 1) == 0) {
				buffer[i + 6] = 0x31;
				uprintf("  0x%08X: '%s' -> 'rdisk(%c)'\n", i, rdisk_zero, buffer[i + 6]);
			}
			// $WIN_NT$_~BT -> i386/amd64
			if (safe_strnicmp(&buffer[i], win_nt_bt_org, strlen(win_nt_bt_org) - 1) == 0) {
				uprintf("  0x%08X: '%s' -> '%s%s'\n", i, &buffer[i], basedir[index], &buffer[i + strlen(win_nt_bt_org)]);
				strcpy(&buffer[i], basedir[index]);
				// This ensures that we keep the terminator backslash
				buffer[i + strlen(basedir[index])] = buffer[i + strlen(win_nt_bt_org)];
				buffer[i + strlen(basedir[index]) + 1] = 0;
			}
		}
	}

}
