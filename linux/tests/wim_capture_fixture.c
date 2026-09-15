/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Capture generated test files with the real upstream library. */
#include <wimlib.h>
#include <stdio.h>
#include <string.h>

int IsIsoScanCancelled(void) { return 0; }

int main(int argc, char** argv)
{
	WIMStruct* wim = NULL;
	struct wimlib_wim_info info = { 0 };
	int error, flags = 0, compression = WIMLIB_COMPRESSION_TYPE_NONE;

	if (argc != 4 && argc != 5)
		return 64;
	if (strcmp(argv[3], "lzx") == 0)
		compression = WIMLIB_COMPRESSION_TYPE_LZX;
	else if (strcmp(argv[3], "xpress") == 0)
		compression = WIMLIB_COMPRESSION_TYPE_XPRESS;
	else if (strcmp(argv[3], "solid") == 0) {
		compression = WIMLIB_COMPRESSION_TYPE_LZMS;
		flags = WIMLIB_WRITE_FLAG_SOLID;
	} else if (strcmp(argv[3], "pipable") == 0) {
		compression = WIMLIB_COMPRESSION_TYPE_LZX;
		flags = WIMLIB_WRITE_FLAG_PIPABLE;
	} else if (strcmp(argv[3], "integrity") == 0)
		flags = WIMLIB_WRITE_FLAG_CHECK_INTEGRITY;
	else if (strcmp(argv[3], "none") != 0)
		return 64;
	error = wimlib_global_init(WIMLIB_INIT_FLAG_ASSUME_UTF8);
	if (!error) error = wimlib_create_new_wim(compression, &wim);
	if (!error) error = wimlib_add_image(wim, argv[1], "Generated image", NULL, 0);
	if (!error && argc == 5) {
		error = wimlib_add_image(wim, argv[4], "Second image", NULL, 0);
		info.boot_index = 2;
		if (!error) error = wimlib_set_wim_info(wim, &info, WIMLIB_CHANGE_BOOT_INDEX);
	}
	if (!error) error = wimlib_write(wim, argv[2], WIMLIB_ALL_IMAGES, flags, 1);
	if (error) fprintf(stderr, "Fixture: %s\n", wimlib_get_error_string(error));
	wimlib_free(wim);
	wimlib_global_cleanup();
	return error != 0;
}
