/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "platform/wim_file.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures, checks, opened, freed, info_calls, property_calls;
static int cancel_at, cancel_checks, open_error, info_error;
static const char* replace_property;
static const char* replace_value;
static const char* rename_source;
static const char* rename_target;

#define CHECK(expression) do { \
	checks++; \
	if (!(expression)) { \
		fprintf(stderr, "line %d: %s\n", __LINE__, #expression); \
		failures++; \
	} \
} while (0)

int IsIsoScanCancelled(void)
{
	cancel_checks++;
	return cancel_at > 0 && cancel_checks >= cancel_at;
}

int __real_wimlib_open_wim(const char* path, int flags, WIMStruct** wim);
void __real_wimlib_free(WIMStruct* wim);
int __real_wimlib_get_wim_info(WIMStruct* wim, struct wimlib_wim_info* info);
const char* __real_wimlib_get_image_property(const WIMStruct* wim, int image, const char* property);

int __wrap_wimlib_open_wim(const char* path, int flags, WIMStruct** wim)
{
	int error;

	CHECK(flags == 0);
	CHECK(strncmp(path, "/proc/self/fd/", 14) == 0);
	if (open_error != 0)
		return open_error;
	if (rename_source != NULL)
	{
		CHECK(rename(rename_source, rename_target) == 0);
		// The original pathname now names invalid data; the held file is a WIM.
		FILE* replacement = fopen(rename_source, "wb");
		if (replacement == NULL)
			exit(2);
		CHECK(fputs("replacement", replacement) >= 0 && fclose(replacement) == 0);
	}
	error = __real_wimlib_open_wim(path, flags, wim);
	if (error == 0)
		opened++;
	return error;
}

void __wrap_wimlib_free(WIMStruct* wim)
{
	if (wim != NULL)
		freed++;
	__real_wimlib_free(wim);
}

int __wrap_wimlib_get_wim_info(WIMStruct* wim, struct wimlib_wim_info* info)
{
	info_calls++;
	if (info_error != 0)
		return info_error;
	return __real_wimlib_get_wim_info(wim, info);
}

const char* __wrap_wimlib_get_image_property(const WIMStruct* wim, int image, const char* property)
{
	property_calls++;
	if (replace_property != NULL && strcmp(property, replace_property) == 0)
		return replace_value;
	return __real_wimlib_get_image_property(wim, image, property);
}

static void RequireWim(int error)
{
	if (error != 0)
	{
		fprintf(stderr, "fixture setup: %s\n", wimlib_get_error_string(error));
		exit(2);
	}
}

static void ResetFaults(void)
{
	opened = freed = info_calls = property_calls = 0;
	cancel_at = cancel_checks = open_error = info_error = 0;
	replace_property = replace_value = NULL;
	rename_source = rename_target = NULL;
}

static void CheckCancellation(const char* path)
{
	WIM_FILE_ANALYSIS analysis;
	WIM_FILE_ANALYSIS zero = { 0 };
	int point;

	for (point = 1; point <= 3; point++)
	{
		ResetFaults();
		cancel_at = point;
		memset(&analysis, 0x5a, sizeof(analysis));
		CHECK(ScanWimFile(path, 1, &analysis) == ISO_SCAN_CANCELLED);
		CHECK(memcmp(&analysis, &zero, sizeof(zero)) == 0);
		CHECK(opened == (point == 1 ? 0 : 1) && freed == opened);
		CHECK(property_calls == (point == 3 ? 4 : 0));
		CHECK(info_calls == (point == 3 ? 1 : 0));
	}
}

static void CheckFailures(const char* path)
{
	static const int errors[] = { WIMLIB_ERR_OPEN, WIMLIB_ERR_READ, WIMLIB_ERR_NOMEM,
		WIMLIB_ERR_UNKNOWN_VERSION, WIMLIB_ERR_XML };
	WIM_FILE_ANALYSIS analysis;
	RUFUS_WIM_METADATA zero = { 0 };
	size_t i;
	int stage;

	for (stage = 0; stage < 2; stage++)
		for (i = 0; i < sizeof(errors) / sizeof(errors[0]); i++)
		{
			ResetFaults();
			if (stage == 0)
				open_error = errors[i];
			else
				info_error = errors[i];
			memset(&analysis, 0x5a, sizeof(analysis));
			CHECK(ScanWimFile(path, 1, &analysis) == ISO_SCAN_ERROR);
			CHECK(analysis.wim_error == errors[i]);
			CHECK(memcmp(&analysis.metadata, &zero, sizeof(zero)) == 0);
			CHECK(opened == stage && freed == opened && property_calls == 0);
		}
	ResetFaults();
	open_error = WIMLIB_ERR_READ;
	cancel_at = 2;
	CHECK(ScanWimFile(path, 1, &analysis) == ISO_SCAN_CANCELLED);
	CHECK(analysis.wim_error == 0);
	ResetFaults();
	info_error = WIMLIB_ERR_READ;
	cancel_at = 3;
	CHECK(ScanWimFile(path, 1, &analysis) == ISO_SCAN_CANCELLED);
	CHECK(analysis.wim_error == 0 && opened == freed);
}

static int DescriptorCount(void)
{
	DIR* directory = opendir("/proc/self/fd");
	int count = 0;

	if (directory == NULL)
		exit(2);
	while (readdir(directory) != NULL)
		count++;
	closedir(directory);
	return count;
}

static void CheckReuse(const char* path)
{
	WIM_FILE_ANALYSIS analysis;
	int i, before = DescriptorCount();

	for (i = 0; i < 20; i++)
	{
		CheckCancellation(path);
		CheckFailures(path);
		ResetFaults();
		CHECK(ScanWimFile(path, 1, &analysis) == ISO_SCAN_OK);
		CHECK(analysis.metadata.win_version.major == 11 && opened == freed);
	}
	CHECK(DescriptorCount() == before);
}

static void CheckArgumentsAndEmptyProperty(const char* path)
{
	WIM_FILE_ANALYSIS analysis;
	RUFUS_WIM_METADATA metadata, zero = { 0 };
	RUFUS_WIM_INFO info, empty = { 0 };

	ResetFaults();
	CHECK(ScanWimFile(path, 1, NULL) == ISO_SCAN_ERROR);
	CHECK(ScanWimFile(NULL, 1, &analysis) == ISO_SCAN_ERROR);
	CHECK(ScanWimFile("", 1, &analysis) == ISO_SCAN_ERROR);
	CHECK(ScanWimFile(path, 0, &analysis) == ISO_SCAN_ERROR);
	CHECK(ScanWimFile(path, -1, &analysis) == ISO_SCAN_ERROR);
	CHECK(opened == 0 && cancel_checks == 0);
	memset(&metadata, 0x5a, sizeof(metadata));
	CHECK(ReadWimMetadata(NULL, 1, &metadata) == WIMLIB_ERR_INVALID_PARAM);
	CHECK(memcmp(&metadata, &zero, sizeof(zero)) == 0);
	CHECK(ReadWimMetadata(NULL, 1, NULL) == WIMLIB_ERR_INVALID_PARAM);
	memset(&info, 0x5a, sizeof(info));
	CHECK(ReadWimArchiveInfo(NULL, &info) == WIMLIB_ERR_INVALID_PARAM);
	CHECK(memcmp(&info, &empty, sizeof(empty)) == 0);
	CHECK(ReadWimArchiveInfo(NULL, NULL) == WIMLIB_ERR_INVALID_PARAM);
	CHECK(info_calls == 0);
	replace_property = "WINDOWS/VERSION/SPBUILD";
	replace_value = "";
	CHECK(ScanWimFile(path, 1, &analysis) == ISO_SCAN_PARTIAL);
	CHECK(analysis.metadata.invalid_fields == WIM_VERSION_REVISION);
	CHECK(analysis.metadata.valid_fields == WIM_VERSION_REQUIRED);
	CHECK(analysis.metadata.win_version.major == 11);
}

static void CheckFileIdentity(const char* path, const char* directory)
{
	char moved[512];
	WIM_FILE_ANALYSIS analysis;

	snprintf(moved, sizeof(moved), "%s/moved.wim", directory);
	ResetFaults();
	rename_source = path;
	rename_target = moved;
	CHECK(ScanWimFile(path, 1, &analysis) == ISO_SCAN_OK);
	CHECK(analysis.metadata.win_version.major == 11 && opened == freed);
	CHECK(unlink(path) == 0);
	CHECK(rename(moved, path) == 0);
	ResetFaults();
}

static void CheckDescriptorArguments(const char* directory)
{
	WIMStruct* wim = (WIMStruct*)1;
	int fd, pipes[2], before = DescriptorCount();

	ResetFaults();
	CHECK(OpenWimFileDescriptor(-1, NULL) == WIMLIB_ERR_INVALID_PARAM);
	CHECK(OpenWimFileDescriptor(-1, &wim) == WIMLIB_ERR_INVALID_PARAM && wim == NULL);
	fd = open(directory, O_RDONLY | O_DIRECTORY);
	if (fd < 0 || pipe(pipes) != 0)
		exit(2);
	wim = (WIMStruct*)1;
	CHECK(OpenWimFileDescriptor(fd, &wim) == WIMLIB_ERR_INVALID_PARAM && wim == NULL);
	CHECK(fcntl(fd, F_GETFD) >= 0);
	wim = (WIMStruct*)1;
	CHECK(OpenWimFileDescriptor(pipes[0], &wim) == WIMLIB_ERR_INVALID_PARAM && wim == NULL);
	CHECK(fcntl(pipes[0], F_GETFD) >= 0);
	close(fd);
	close(pipes[0]);
	close(pipes[1]);
	CHECK(opened == 0 && info_calls == 0 && property_calls == 0);
	CHECK(DescriptorCount() == before);
}

int main(void)
{
	char directory[] = "/tmp/rufus-wim-contract-XXXXXX", path[512];
	WIMStruct* wim = NULL;
	int image;

	if (mkdtemp(directory) == NULL)
		return 2;
	snprintf(path, sizeof(path), "%s/test.wim", directory);
	RequireWim(wimlib_global_init(WIMLIB_INIT_FLAG_ASSUME_UTF8));
	RequireWim(wimlib_create_new_wim(WIMLIB_COMPRESSION_TYPE_NONE, &wim));
	RequireWim(wimlib_add_empty_image(wim, "Test", &image));
	RequireWim(wimlib_set_image_property(wim, image, "WINDOWS/VERSION/MAJOR", "10"));
	RequireWim(wimlib_set_image_property(wim, image, "WINDOWS/VERSION/MINOR", "0"));
	RequireWim(wimlib_set_image_property(wim, image, "WINDOWS/VERSION/BUILD", "26100"));
	RequireWim(wimlib_set_image_property(wim, image, "WINDOWS/VERSION/SPBUILD", "0"));
	RequireWim(wimlib_write(wim, path, WIMLIB_ALL_IMAGES, 0, 1));
	__real_wimlib_free(wim);
	CheckReuse(path);
	CheckArgumentsAndEmptyProperty(path);
	CheckFileIdentity(path, directory);
	CheckDescriptorArguments(directory);
	wimlib_global_cleanup();
	CHECK(unlink(path) == 0 && rmdir(directory) == 0);
	printf("%d WIM ownership, cancellation and error checks, %d failures\n", checks, failures);
	return failures != 0;
}
