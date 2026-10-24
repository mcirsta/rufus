/* SPDX-License-Identifier: GPL-3.0-or-later */
// Separate system-wimlib executable: no Rufus reader or split adapter linked.
#include "wim_split_inventory.h"

static enum wimlib_progress_status VerifyProgress(enum wimlib_progress_msg message,
	union wimlib_progress_info* info, void* context)
{
	unsigned* reported = context;
	if (message == WIMLIB_PROGRESS_MSG_VERIFY_STREAMS && info->verify_streams.total_bytes != 0) {
		unsigned percent = 100 * (double)info->verify_streams.completed_bytes / info->verify_streams.total_bytes;
		if (percent >= *reported + 10) {
			fprintf(stderr, "Verified %u%% of uncompressed data\n", percent);
			*reported = percent;
		}
	}
	return WIMLIB_PROGRESS_STATUS_CONTINUE;
}

int main(int argc, char** argv)
{
	WIMStruct* wim = NULL;
	struct wimlib_wim_info info;
	unsigned reported = 0;
	if (argc < 3 || wimlib_global_init(WIMLIB_INIT_FLAG_ASSUME_UTF8) != 0)
		return 2;
	wimlib_set_print_errors(true);
	int error = wimlib_open_wim(argv[1], 0, &wim);
	if (error == 0)
		error = wimlib_get_wim_info(wim, &info);
	if (error == 0 && (info.part_number != 1 || info.total_parts != (unsigned)(argc - 1)))
		error = WIMLIB_ERR_INVALID_PARAM;
	if (error == 0)
		error = wimlib_reference_resource_files(wim, (const char* const*)&argv[2], argc - 2, 0, 0);
	if (error == 0) {
		fprintf(stderr, "Verifying all %u images and %u parts with system wimlib %s\n",
			info.image_count, info.total_parts, wimlib_get_version_string());
		wimlib_register_progress_function(wim, VerifyProgress, &reported);
		error = wimlib_verify_wim(wim, 0);
	}
	if (error == 0)
		error = PrintWimInventory(wim);
	wimlib_free(wim);
	wimlib_global_cleanup();
	fprintf(stderr, "WIM_SPLIT_VERIFY_ERROR=%d\n", error);
	return error == 0 ? 0 : 1;
}
