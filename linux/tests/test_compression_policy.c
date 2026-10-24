/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "image_compression.h"
#include <stdio.h>

static int checks, failures;
#define CHECK(value) do { checks++; if (!(value)) { fprintf(stderr, "line %d: %s\n", __LINE__, #value); failures++; } } while (0)

int main(void)
{
	static const struct { const char* name; int expected; } cases[] = {
		{"/image.zip", 1}, {"/image.Z", 2}, {"/image.gz", 3}, {"/image.lzma", 4},
		{"/image.bz2", 5}, {"/image.xz", 6}, {"/image.vtsi", 8}, {"/image.zst", 9},
		{"/image.ffu", 10}, {"/image.vhd", 11}, {"/image.vhdx", 12},
		{"C:\\directory.with.dot\\IMAGE.IMG.GZ", 3}, {"/name ü.IMG.XZ", 6},
		{"/image.7z", 0}, {"/image.gzip", 0}, {"/image.bzip2", 5}, {"/image.img", 0},
		{"/folder.gz/image", 0}, {"/image.gz.bak", 0}, {"/image.gz/", 0},
		{".gz", 0}, {"a.gz", 0}, {"a.gZ", 0}, {"/a.gz", 3}, {"", 0}, {NULL, 0}
	};
	CHECK(BLED_COMPRESSION_NONE == 0 && BLED_COMPRESSION_MAX == 10);
	CHECK(BLED_COMPRESSION_7ZIP == 7 && IMG_COMPRESSION_VHDX == 12);
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
		CHECK(GetImageCompressionType(cases[i].name) == cases[i].expected);
	static const char* suffix[] = {"zip", "z", "gz", "lzma", "bz2", "bzip2", "xz", "vtsi", "zst", "ffu", "vhd", "vhdx"};
	static const int type[] = {1, 2, 3, 4, 5, 5, 6, 8, 9, 10, 11, 12};
	for (size_t i = 0; i < sizeof(type) / sizeof(type[0]); i++) {
		const size_t length = strlen(suffix[i]);
		for (unsigned mask = 0; mask < (1u << length); mask++) {
			char path[64] = "/test/image.";
			const size_t start = strlen(path);
			for (size_t j = 0; j < length; j++) {
				const bool letter = suffix[i][j] >= 'a' && suffix[i][j] <= 'z';
				path[start + j] = suffix[i][j] - ((letter && (mask & (1u << j))) ? 'a' - 'A' : 0);
			}
			path[start + length] = 0;
			CHECK(GetImageCompressionType(path) == type[i]);
		}
	}
	uint8_t sector[512] = {0};
	CHECK(GetCompressedBootMarker(NULL, 512, true) == 0);
	CHECK(GetCompressedBootMarker(sector, 511, true) == 0);
	CHECK(GetCompressedBootMarker(sector, 513, true) == 0);
	CHECK(GetCompressedBootMarker(sector, 512, false) == 0);
	CHECK(GetCompressedBootMarker(sector, 512, true) == 2);
	sector[510] = 0x55; sector[511] = 0xaa;
	CHECK(GetCompressedBootMarker(sector, 512, false) == 1);
	CHECK(GetCompressedBootMarker(sector, 512, true) == 1);
	sector[511] = 0xab;
	CHECK(GetCompressedBootMarker(sector, 512, false) == 0);
	printf("Compression policy: %d checks, %d failures\n", checks, failures);
	return failures != 0;
}
