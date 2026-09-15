/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "syslinux_version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, cases;

#define CHECK(expression) do { \
	if (!(expression)) { \
		fprintf(stderr, "case %d, line %d: %s\n", cases, __LINE__, #expression); \
		failures++; \
	} \
} while (0)

static void CheckBuffer(char* buf, size_t size, uint16_t expected_version, const char* expected_ext)
{
	char *ext = NULL, *original = malloc(size);
	uint16_t version;

	if (original == NULL)
		exit(2);
	memcpy(original, buf, size);
	cases++;
	version = GetSyslinuxVersion(buf, size, &ext);
	CHECK(version == expected_version);
	CHECK(ext != NULL);
	if (ext != NULL) {
		CHECK(strcmp(ext, expected_ext) == 0);
		if (*expected_ext != 0) {
			CHECK((uintptr_t)ext >= (uintptr_t)buf && (uintptr_t)ext < (uintptr_t)buf + size);
			CHECK(*ext == '/');
		}
	}
	if (version == 0)
		CHECK(memcmp(original, buf, size) == 0);
	CHECK(SL_MAJOR(version) == (expected_version >> 8));
	CHECK(SL_MINOR(version) == (expected_version & 255));
	free(original);
}

static void CheckBanner(const char* text, size_t offset, size_t size, uint16_t version, const char* ext)
{
	char* buf = malloc(size);
	size_t length = strlen(text);

	if (buf == NULL || offset + length >= size)
		exit(2);
	memset(buf, 0x5a, size);
	memcpy(buf + offset, text, length + 1);
	CheckBuffer(buf, size, version, ext);
	free(buf);
}

static void CheckBanners(void)
{
	static const struct {
		const char* text;
		uint16_t version;
		const char* ext;
	} banners[] = {
		{ "SYSLINUX 4.07 2013-07-25", 0x0407, "/2013-07-25" },
		{ "ISOLINUX 6.04 6.04-pre1", 0x0604, "/pre1" },
		{ "ISOLINUX 6.03 2014-10-06* ", 0x0603, "/2014-10-06" },
		{ "SYSLINUX 6.04 6.04-pre1 ***", 0x0604, "/pre1" },
		{ "ISOLINUX 6.03 6.03", 0x0603, "" },
		{ "SYSLINUX 6.04-pre1", 0x0604, "/pre1" },
		{ "ISOLINUX 6.04 6.04-pre1 build", 0x0604, "/pre1 build" },
		{ "SYSLINUX 4.07 ", 0x0407, "" },
		{ "SYSLINUX 4.07 *** ", 0x0407, "" },
		{ "SYSLINUX 4.07", 0x0407, "" },
		{ "SYSLINUX 6.03 <tag>:|*?\\/", 0x0603, "/_tag_______" },
		{ "SYSLINUX 006.003 suffix", 0x0603, "/suffix" },
		{ "SYSLINUX +6.03 date", 0x0603, "/date" },
		{ "SYSLINUX  6.03 date", 0x0603, "/date" },
		{ "SYSLINUX 6. 03 date", 0x0603, "/date" },
		{ "SYSLINUX 6.+3 date", 0x0603, "/date" },
		{ "SYSLINUX 0.01 date", 0x0001, "/date" },
		{ "ISOLINUX 255.255 suffix", 0xffff, "/suffix" },
		{ "SYSLINUX 0.00 ISOLINUX 6.04 6.04-pre1", 0x0604, "/pre1" },
		{ "No version here", 0, "" },
		{ "syslinux 6.03 date", 0, "" },
		{ "EXTLINUX 6.03 date", 0, "" },
		{ "LINUX 6.03 date", 0, "" },
		{ "SYSLINUX x.y date", 0, "" },
		{ "SYSLINUX 6x03 date", 0, "" },
		{ "SYSLINUX 6. date", 0, "" },
		{ "SYSLINUX .03 date", 0, "" },
		{ "SYSLINUX -6.03 date", 0, "" },
		{ "SYSLINUX 6.-3 date", 0, "" },
		{ "SYSLINUX 256.03 date", 0, "" },
		{ "SYSLINUX 6.256 date", 0, "" },
		{ "SYSLINUX 0.00 date", 0, "" },
	};
	for (size_t i = 0; i < sizeof(banners) / sizeof(banners[0]); i++)
		CheckBanner(banners[i].text, 128, 512, banners[i].version, banners[i].ext);
}

static void CheckBoundsAndSelection(void)
{
	char buf[512], *ext;
	const char first[] = "SYSLINUX 4.07 2013-07-25";
	const char second[] = "ISOLINUX 6.04 6.04-pre1";

	CheckBanner(first, 128, 255, 0, "");
	CheckBanner(first, 128, 256, 0x0407, "/2013-07-25");
	// Rufus searches for LINUX starting three bytes after the SYS/ISO prefix.
	CheckBanner(first, 60, 256, 0, "");
	CheckBanner(first, 61, 256, 0x0407, "/2013-07-25");
	CheckBanner(first, 188, 256, 0x0407, "/2013-07-25");
	CheckBanner(first, 189, 256, 0, "");
	memset(buf, 0, sizeof(buf));
	memcpy(buf, first, sizeof(first));
	memcpy(buf + 128, second, sizeof(second));
	CheckBuffer(buf, sizeof(buf), 0x0604, "/pre1");
	memset(buf, 0, sizeof(buf));
	memcpy(buf + 128, first, sizeof(first));
	memcpy(buf + 256, second, sizeof(second));
	CheckBuffer(buf, sizeof(buf), 0x0407, "/2013-07-25");
	memset(buf, 0, sizeof(buf));
	memcpy(buf + 128, "SYSLINUX 0.00", 14);
	memcpy(buf + 256, second, sizeof(second));
	CheckBuffer(buf, sizeof(buf), 0x0604, "/pre1");
	// A terminator outside the supplied span cannot make that span a string.
	memset(buf, 0x5a, sizeof(buf));
	memcpy(buf + 128, first, sizeof(first) - 1);
	CheckBuffer(buf, sizeof(buf), 0, "");
	buf[sizeof(buf) - 1] = 0;
	CheckBuffer(buf, sizeof(buf) - 1, 0, "");
	cases++;
	CHECK(GetSyslinuxVersion(NULL, sizeof(buf), &ext) == 0 && ext != NULL && *ext == 0);
	CHECK(GetSyslinuxVersion(buf, sizeof(buf), NULL) == 0);
}

static void CheckResource(const char* path, uint16_t version, const char* ext)
{
	FILE* file = fopen(path, "rb");
	char* buf;
	long size;

	if (file == NULL || fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) <= 0)
		exit(2);
	buf = malloc((size_t)size);
	if (buf == NULL || fseek(file, 0, SEEK_SET) != 0 || fread(buf, 1, (size_t)size, file) != (size_t)size)
		exit(2);
	fclose(file);
	CheckBuffer(buf, (size_t)size, version, ext);
	free(buf);
}

int main(int argc, char** argv)
{
	if (argc != 3)
		return 2;
	CheckBanners();
	CheckBoundsAndSelection();
	CheckResource(argv[1], 0x0407, "/2013-07-25");
	CheckResource(argv[2], 0x0604, "/pre1");
	printf("Syslinux version: %d cases, %d failures\n", cases, failures);
	return failures != 0;
}
