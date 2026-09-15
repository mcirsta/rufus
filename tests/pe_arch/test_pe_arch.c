/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pe_arch.h"
#include <stdio.h>
#include <string.h>

static int checks, failures;
#define CHECK(expression) do { checks++; if (!(expression)) { \
	fprintf(stderr, "line %d: %s\n", __LINE__, #expression); failures++; } } while (0)

static void SetOffset(uint8_t* bytes, uint32_t offset)
{
	unsigned i;
	for (i = 0; i < 4; i++)
		bytes[60 + i] = (uint8_t)(offset >> (8 * i));
}

int main(void)
{
	static const struct { uint16_t machine; enum ArchType arch; } cases[] = {
		{0x014c, ARCH_X86_32}, {0x8664, ARCH_X86_64}, {0x01c0, ARCH_ARM_32},
		{0xaa64, ARCH_ARM_64}, {0x0200, ARCH_IA_64}, {0x5064, ARCH_RISCV_64},
		{0x6264, ARCH_LOONGARCH_64}, {0x0ebc, ARCH_EBC}, {0, ARCH_UNKNOWN},
		{0xffff, ARCH_UNKNOWN}, {0x01c4, ARCH_UNKNOWN}
	};
	uint8_t buffer[257] = { 0 }, saved[256];
	uint8_t* bytes = buffer + 1; // Deliberately unaligned input.
	size_t i, size;

	bytes[0] = 'M'; bytes[1] = 'Z';
	SetOffset(bytes, 128);
	bytes[128] = 'P'; bytes[129] = 'E';
	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		bytes[132] = (uint8_t)cases[i].machine;
		bytes[133] = (uint8_t)(cases[i].machine >> 8);
		CHECK(MachineToArch(cases[i].machine) == cases[i].arch);
		CHECK(FindPeArch(bytes, 256) == cases[i].arch);
	}
	bytes[132] = 0x64; bytes[133] = 0x86;
	memcpy(saved, bytes, sizeof(saved));
	for (size = 0; size <= 256; size++)
		CHECK(FindPeArch(bytes, size) == (size < 152 ? ARCH_UNKNOWN : ARCH_X86_64));
	CHECK(memcmp(saved, bytes, sizeof(saved)) == 0);
	CHECK(FindPeArch(NULL, SIZE_MAX) == ARCH_UNKNOWN);
	bytes[0] = 0; CHECK(FindPeArch(bytes, 256) == ARCH_UNKNOWN); bytes[0] = 'M';
	bytes[130] = 1; CHECK(FindPeArch(bytes, 256) == ARCH_UNKNOWN); bytes[130] = 0;
	SetOffset(bytes, UINT32_MAX); CHECK(FindPeArch(bytes, 256) == ARCH_UNKNOWN);
	SetOffset(bytes, 0x80000000u); CHECK(FindPeArch(bytes, 256) == ARCH_UNKNOWN);
	SetOffset(bytes, 256); CHECK(FindPeArch(bytes, 256) == ARCH_UNKNOWN);
	SetOffset(bytes, 63); CHECK(FindPeArch(bytes, 256) == ARCH_UNKNOWN);
	SetOffset(bytes, 64);
	memcpy(bytes + 64, saved + 128, 24);
	CHECK(FindPeArch(bytes, 88) == ARCH_X86_64);
	CHECK(FindPeArch(bytes, 87) == ARCH_UNKNOWN);
	printf("PE architecture: %d checks, %d failures.\n", checks, failures);
	return failures != 0;
}
