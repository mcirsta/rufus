/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Create an ordinary metadata WIM for container/transport tests. */
#include "wim_metadata.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int IsIsoScanCancelled(void)
{
	return 0;
}

static void RequireWim(int error)
{
	if (error != 0)
	{
		fprintf(stderr, "%s\n", wimlib_get_error_string(error));
		exit(2);
	}
}

static void SetVersion(WIMStruct* wim, int image, const char* kind)
{
	bool windows7 = strcmp(kind, "win7") == 0 || strcmp(kind, "multi") == 0;
	bool windows8 = strcmp(kind, "win8") == 0;
	bool windows81 = strcmp(kind, "win8.1") == 0;
	bool vista = strcmp(kind, "vista") == 0;
	const char* major = "10";
	const char* minor = "0";
	const char* build = "26100";
	if (windows7 || windows8 || windows81 || vista)
		major = "6";
	if (vista)
		build = "6002";
	if (windows7)
	{
		minor = "1";
		build = "7601";
	}
	if (windows8)
	{
		minor = "2";
		build = "9200";
	}
	if (windows81)
	{
		minor = "3";
		build = "9600";
	}
	if (strcmp(kind, "win11-25h2") == 0)
		build = "26200";
	if (strcmp(kind, "win10") == 0)
		build = "19045";

	if (strcmp(kind, "unknown") == 0)
		return;
	RequireWim(wimlib_set_image_property(wim, image, "WINDOWS/VERSION/MAJOR",
		strcmp(kind, "invalid") == 0 ? "10?" : major));
	RequireWim(wimlib_set_image_property(wim, image, "WINDOWS/VERSION/MINOR", minor));
	RequireWim(wimlib_set_image_property(wim, image, "WINDOWS/VERSION/BUILD", build));
	if (strcmp(kind, "no-revision") != 0)
		RequireWim(wimlib_set_image_property(wim, image, "WINDOWS/VERSION/SPBUILD", "0"));
}

int main(int argc, char** argv)
{
	WIMStruct* wim = NULL;
	RUFUS_WIM_METADATA metadata;
	char description[40001];
	size_t i;
	int image, flags = 0;
	int expected_major;

	if (argc != 3 && argc != 4 && argc != 5)
		return 64;
	if (argc == 5 && strcmp(argv[3], "efi") != 0)
		return 64;
	if (strcmp(argv[2], "win7") == 0 || strcmp(argv[2], "multi") == 0)
		expected_major = 7;
	else if (strcmp(argv[2], "win8") == 0 || strcmp(argv[2], "win8.1") == 0)
		expected_major = 8;
	else if (strcmp(argv[2], "win11") == 0 || strcmp(argv[2], "win11-25h2") == 0 || strcmp(argv[2], "no-revision") == 0)
		expected_major = 11;
	else if (strcmp(argv[2], "win10") == 0)
		expected_major = 10;
	else if (strcmp(argv[2], "unknown") == 0 || strcmp(argv[2], "invalid") == 0 || strcmp(argv[2], "vista") == 0)
		expected_major = 0;
	else
		return 64;
	if (argc == 4)
	{
		if (strcmp(argv[3], "pipable") == 0)
			flags = WIMLIB_WRITE_FLAG_PIPABLE;
		else if (strcmp(argv[3], "solid") == 0)
			flags = WIMLIB_WRITE_FLAG_SOLID;
		else if (strcmp(argv[3], "integrity") == 0)
			flags = WIMLIB_WRITE_FLAG_CHECK_INTEGRITY;
		else
			return 64;
	}
	// Large XML exercises cache boundaries; optional EFI bytes are generated test code.
	for (i = 0; i < sizeof(description) - 1; i++)
		description[i] = (char)('a' + i % 26);
	description[i] = 0;
	RequireWim(wimlib_global_init(WIMLIB_INIT_FLAG_ASSUME_UTF8));
	RequireWim(wimlib_create_new_wim(WIMLIB_COMPRESSION_TYPE_NONE, &wim));
	RequireWim(wimlib_add_empty_image(wim, argv[2], &image));
	RequireWim(wimlib_set_image_property(wim, image, "DESCRIPTION", description));
	SetVersion(wim, image, argv[2]);
	if (argc == 5)
	{
		struct wimlib_update_command command = { 0 };
		command.op = WIMLIB_UPDATE_OP_ADD;
		command.add.fs_source_path = argv[4];
		command.add.wim_target_path = "Windows/Boot/EFI/bootmgfw.efi";
		RequireWim(wimlib_update_image(wim, 1, &command, 1, 0));
	}
	if (strcmp(argv[2], "multi") == 0)
	{
		struct wimlib_wim_info info = { 0 };
		RequireWim(wimlib_add_empty_image(wim, "Second image", &image));
		SetVersion(wim, image, "win11");
		info.boot_index = 2;
		RequireWim(wimlib_set_wim_info(wim, &info, WIMLIB_CHANGE_BOOT_INDEX));
	}
	RequireWim(wimlib_write(wim, argv[1], WIMLIB_ALL_IMAGES, flags, 1));
	wimlib_free(wim);
	RequireWim(wimlib_open_wim(argv[1], 0, &wim));
	RequireWim(ReadWimMetadata(wim, 1, &metadata));
	if (metadata.win_version.major != expected_major)
		return 3;
	if (metadata.win_version.minor != (strcmp(argv[2], "win8.1") == 0 ? 1 : 0))
		return 3;
	wimlib_free(wim);
	wimlib_global_cleanup();
	return 0;
}
