/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "platform/wim_file.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures, checks, fixtures;
static char directory[] = "/tmp/rufus-wim-metadata-XXXXXX";
static const char* properties[] = {
	"WINDOWS/VERSION/MAJOR", "WINDOWS/VERSION/MINOR",
	"WINDOWS/VERSION/BUILD", "WINDOWS/VERSION/SPBUILD"
};

#define CHECK(expression) do { \
	checks++; \
	if (!(expression)) { \
		fprintf(stderr, "fixture %d, line %d: %s\n", fixtures, __LINE__, #expression); \
		failures++; \
	} \
} while (0)

int IsIsoScanCancelled(void)
{
	return 0;
}

static void RequireWim(int error)
{
	if (error != 0)
	{
		fprintf(stderr, "fixture setup: %s (%d)\n", wimlib_get_error_string(error), error);
		exit(2);
	}
}

static void SetVersion(WIMStruct* wim, int image, const char* const* values)
{
	size_t i;

	for (i = 0; i < 4; i++)
		if (values[i] != NULL)
			RequireWim(wimlib_set_image_property(wim, image, properties[i], values[i]));
}

static void CreateFixture(const char* path, const char* const* values, int solid)
{
	WIMStruct* wim = NULL;
	int image;

	RequireWim(wimlib_create_new_wim(WIMLIB_COMPRESSION_TYPE_LZX, &wim));
	RequireWim(wimlib_add_empty_image(wim, "Test image", &image));
	SetVersion(wim, image, values);
	RequireWim(wimlib_write(wim, path, WIMLIB_ALL_IMAGES, solid ? WIMLIB_WRITE_FLAG_SOLID : 0, 1));
	wimlib_free(wim);
	fixtures++;
}

static void CheckNormalization(void)
{
	static const struct {
		winver_t input;
		uint16_t major, minor;
	} cases[] = {
		{ { 0, 9, 0, 0 }, 0, 0 }, { { 5, 2, 3790, 1 }, 0, 0 },
		{ { 6, 0, 6002, 2 }, 0, 0 }, { { 6, 1, 7601, 1 }, 7, 0 },
		{ { 6, 2, 9200, 0 }, 8, 0 }, { { 6, 3, 9600, 42 }, 8, 1 },
		{ { 6, 4, 9841, 0 }, 10, 0 }, { { 6, 5, 9999, 3 }, 6, 5 },
		{ { 7, 2, 10000, 4 }, 7, 2 }, { { 10, 0, 19045, 1 }, 10, 0 },
		{ { 10, 0, 20000, 0 }, 10, 0 }, { { 10, 0, 20001, 0 }, 11, 0 },
		{ { 10, 2, 26100, 100 }, 11, 2 }, { { 11, 0, 30000, 0 }, 11, 0 },
		{ { 12, 3, 65535, 65535 }, 12, 3 }
	};
	winver_t version;
	size_t i;

	NormalizeWindowsVersion(NULL);
	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
	{
		version = cases[i].input;
		NormalizeWindowsVersion(&version);
		CHECK(version.major == cases[i].major && version.minor == cases[i].minor);
		CHECK(version.build == cases[i].input.build && version.revision == cases[i].input.revision);
	}
}

static void CheckVersions(const char* path)
{
	static const struct {
		const char* values[4];
		winver_t expected;
		int status;
	} cases[] = {
		{ { "6", "1", "7601", "1" }, { 7, 0, 7601, 1 }, ISO_SCAN_OK },
		{ { "6", "2", "9200", "0" }, { 8, 0, 9200, 0 }, ISO_SCAN_OK },
		{ { "6", "3", "9600", "42" }, { 8, 1, 9600, 42 }, ISO_SCAN_OK },
		{ { "6", "4", "9841", "0" }, { 10, 0, 9841, 0 }, ISO_SCAN_OK },
		{ { "10", "0", "20000", "0" }, { 10, 0, 20000, 0 }, ISO_SCAN_OK },
		{ { "10", "0", "20001", "0" }, { 11, 0, 20001, 0 }, ISO_SCAN_OK },
		{ { "10", "0", "26100", "100" }, { 11, 0, 26100, 100 }, ISO_SCAN_OK },
		{ { "00010", "00", "019045", "000" }, { 10, 0, 19045, 0 }, ISO_SCAN_OK },
		{ { "10", "0", "26100", "00000000000000000000000000000100" }, { 11, 0, 26100, 100 }, ISO_SCAN_OK },
		{ { "10", "0", "65535", "65535" }, { 11, 0, 65535, 65535 }, ISO_SCAN_OK },
		{ { "5", "2", "3790", "1" }, { 0, 0, 3790, 1 }, ISO_SCAN_PARTIAL },
		{ { "6", "0", "6002", "2" }, { 0, 0, 6002, 2 }, ISO_SCAN_PARTIAL },
		{ { "10", "0", "0", "0" }, { 10, 0, 0, 0 }, ISO_SCAN_PARTIAL }
	};
	WIM_FILE_ANALYSIS result;
	size_t i;
	int solid;

	for (solid = 0; solid <= 1; solid++)
		for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
		{
			CreateFixture(path, cases[i].values, solid);
			memset(&result, 0x5a, sizeof(result));
			CHECK(ScanWimFile(path, 1, &result) == cases[i].status);
			CHECK(result.wim_error == 0);
			CHECK(result.metadata.archive.version == (solid ? 0x00000e00 : 0x00010d00));
			CHECK(result.metadata.archive.image_count == 1 && result.metadata.archive.boot_index == 0);
			CHECK(result.metadata.archive.part_number == 1 && result.metadata.archive.total_parts == 1);
			CHECK(result.metadata.valid_fields == WIM_VERSION_ALL && result.metadata.invalid_fields == 0);
			CHECK(memcmp(&result.metadata.win_version, &cases[i].expected, sizeof(winver_t)) == 0);
			CHECK(unlink(path) == 0);
		}
}

static void CheckUnknownFields(const char* path)
{
	static const char* unknown[] = { NULL, "", "-1", "+1", " 1", "1 ", "0x10", "1.0",
		"ten", "65536", "4294967296", "999999999999999999999999999999999999",
		"00000000000000000000000000000000100", "\xef\xbc\x91" };
	const char* values[] = { "10", "0", "26100", "100" };
	WIM_FILE_ANALYSIS result;
	uint8_t bit;
	size_t i, field;
	const char* original;
	winver_t zero = { 0 };

	for (field = 0; field < 4; field++)
	{
		original = values[field];
		bit = (uint8_t)(1U << field);
		for (i = 0; i < sizeof(unknown) / sizeof(unknown[0]); i++)
		{
			values[field] = unknown[i];
			CreateFixture(path, values, 0);
			CHECK(ScanWimFile(path, 1, &result) == ISO_SCAN_PARTIAL);
			CHECK(result.wim_error == 0 && result.metadata.archive.image_count == 1);
			CHECK(result.metadata.valid_fields == (WIM_VERSION_ALL & ~bit));
			// Setting an empty property removes it in wimlib's public API.
			CHECK(result.metadata.invalid_fields == (i < 2 ? 0 : bit));
			if (field < 3)
				CHECK(memcmp(&result.metadata.win_version, &zero, sizeof(zero)) == 0);
			else
				CHECK(result.metadata.win_version.major == 11 && result.metadata.win_version.revision == 0);
			CHECK(unlink(path) == 0);
		}
		values[field] = original;
	}
	for (i = 0; i < 4; i++)
		values[i] = NULL;
	CreateFixture(path, values, 0);
	CHECK(ScanWimFile(path, 1, &result) == ISO_SCAN_PARTIAL);
	CHECK(result.metadata.valid_fields == 0 && result.metadata.invalid_fields == 0);
	CHECK(memcmp(&result.metadata.raw_version, &zero, sizeof(zero)) == 0);
	CHECK(unlink(path) == 0);

}

static void CheckImages(const char* path)
{
	static const char* first[] = { "6", "1", "7601", "1" };
	static const char* second[] = { "10", "0", "26100", "100" };
	WIMStruct* wim = NULL;
	struct wimlib_wim_info info = { 0 };
	WIM_FILE_ANALYSIS result;
	RUFUS_WIM_METADATA zero = { 0 };
	int image;

	RequireWim(wimlib_create_new_wim(WIMLIB_COMPRESSION_TYPE_NONE, &wim));
	RequireWim(wimlib_add_empty_image(wim, "First edition", &image));
	SetVersion(wim, image, first);
	RequireWim(wimlib_add_empty_image(wim, "Second edition", &image));
	SetVersion(wim, image, second);
	info.boot_index = 2;
	RequireWim(wimlib_set_wim_info(wim, &info, WIMLIB_CHANGE_BOOT_INDEX));
	RequireWim(wimlib_write(wim, path, WIMLIB_ALL_IMAGES, 0, 1));
	wimlib_free(wim);
	fixtures++;
	CHECK(ScanWimFile(path, 1, &result) == ISO_SCAN_OK);
	CHECK(result.metadata.archive.image_count == 2 && result.metadata.archive.boot_index == 2);
	CHECK(result.metadata.win_version.major == 7);
	CHECK(ScanWimFile(path, 2, &result) == ISO_SCAN_OK);
	CHECK(result.metadata.win_version.major == 11);
	CHECK(result.metadata.raw_version.major == 10);
	CHECK(ScanWimFile(path, 3, &result) == ISO_SCAN_ERROR);
	CHECK(result.wim_error == WIMLIB_ERR_INVALID_IMAGE);
	CHECK(memcmp(&result.metadata, &zero, sizeof(zero)) == 0);
	CHECK(unlink(path) == 0);
	RequireWim(wimlib_create_new_wim(WIMLIB_COMPRESSION_TYPE_NONE, &wim));
	RequireWim(wimlib_write(wim, path, WIMLIB_ALL_IMAGES, 0, 1));
	wimlib_free(wim);
	fixtures++;
	RequireWim(wimlib_open_wim(path, 0, &wim));
	RequireWim(wimlib_get_wim_info(wim, &info));
	CHECK(info.image_count == 0);
	wimlib_free(wim);
	CHECK(ScanWimFile(path, 1, &result) == ISO_SCAN_ERROR);
	CHECK(result.wim_error == WIMLIB_ERR_INVALID_IMAGE);
	CHECK(memcmp(&result.metadata, &zero, sizeof(zero)) == 0);
	CHECK(unlink(path) == 0);
}

static void CheckBadFiles(const char* path)
{
	static const unsigned char data[] = { 'M', 'S', 'W', 'I', 'M', 0, 0, 0 };
	WIM_FILE_ANALYSIS result;
	RUFUS_WIM_METADATA zero = { 0 };
	FILE* file;
	size_t size;

	for (size = 0; size <= sizeof(data); size++)
	{
		file = fopen(path, "wb");
		if (file == NULL)
			exit(2);
		CHECK(fwrite(data, 1, size, file) == size);
		CHECK(fclose(file) == 0);
		fixtures++;
		memset(&result, 0x5a, sizeof(result));
		CHECK(ScanWimFile(path, 1, &result) == ISO_SCAN_ERROR);
		CHECK(result.wim_error != 0 && memcmp(&result.metadata, &zero, sizeof(zero)) == 0);
		CHECK(unlink(path) == 0);
	}
	CHECK(ScanWimFile(path, 1, &result) == ISO_SCAN_ERROR);
	CHECK(result.wim_error == WIMLIB_ERR_OPEN);
	CHECK(ScanWimFile(directory, 1, &result) == ISO_SCAN_ERROR);
	CHECK(result.wim_error == WIMLIB_ERR_INVALID_PARAM);
	CHECK(mkfifo(path, 0600) == 0);
	CHECK(ScanWimFile(path, 1, &result) == ISO_SCAN_ERROR);
	CHECK(result.wim_error == WIMLIB_ERR_INVALID_PARAM);
	CHECK(unlink(path) == 0);
}

int main(void)
{
	char path[512];

	if (mkdtemp(directory) == NULL)
		return 2;
	snprintf(path, sizeof(path), "%s/instalare \xc8\x99i test.wim", directory);
	RequireWim(wimlib_global_init(WIMLIB_INIT_FLAG_ASSUME_UTF8));
	CheckNormalization();
	CheckVersions(path);
	CheckUnknownFields(path);
	CheckImages(path);
	CheckBadFiles(path);
	wimlib_global_cleanup();
	CHECK(rmdir(directory) == 0);
	printf("%d real WIM/file fixtures, %d checks, %d failures\n", fixtures, checks, failures);
	return failures != 0;
}
