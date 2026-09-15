/*
 * Rufus: The Reliable USB Formatting Utility
 * PE architecture recognition extracted from rufus.c
 * Copyright © 2011-2026 Pete Batard <pete@akeo.ie>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "pe_arch.h"

enum ArchType MachineToArch(uint16_t machine)
{
	switch (machine) {
	case 0x014c: return ARCH_X86_32;       // IMAGE_FILE_MACHINE_I386
	case 0x8664: return ARCH_X86_64;       // IMAGE_FILE_MACHINE_AMD64
	case 0x01c0: return ARCH_ARM_32;       // IMAGE_FILE_MACHINE_ARM
	case 0xaa64: return ARCH_ARM_64;       // IMAGE_FILE_MACHINE_ARM64
	case 0x0200: return ARCH_IA_64;        // IMAGE_FILE_MACHINE_IA64
	case 0x5064: return ARCH_RISCV_64;     // IMAGE_FILE_MACHINE_RISCV64
	case 0x6264: return ARCH_LOONGARCH_64; // IMAGE_FILE_MACHINE_LOONGARCH64
	case 0x0ebc: return ARCH_EBC;          // IMAGE_FILE_MACHINE_EBC
	default: return ARCH_UNKNOWN;
	}
}

enum ArchType FindPeArch(const void* buffer, size_t size)
{
	const uint8_t* bytes = buffer;
	uint32_t offset;

	if (bytes == NULL || size < 64 || bytes[0] != 'M' || bytes[1] != 'Z')
		return ARCH_UNKNOWN;
	offset = (uint32_t)bytes[60] | ((uint32_t)bytes[61] << 8) |
		((uint32_t)bytes[62] << 16) | ((uint32_t)bytes[63] << 24);
	// e_lfanew is signed. Require the complete PE signature and COFF header.
	if (offset < 64 || offset > INT32_MAX || offset > size || size - offset < 24)
		return ARCH_UNKNOWN;
	bytes += offset;
	if (bytes[0] != 'P' || bytes[1] != 'E' || bytes[2] != 0 || bytes[3] != 0)
		return ARCH_UNKNOWN;
	return MachineToArch((uint16_t)(bytes[4] | ((uint16_t)bytes[5] << 8)));
}
