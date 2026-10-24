/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "windows_wim.h"
#include "rufus_reporting.h"

#include <stdbool.h>
#include <sys/stat.h>

enum { WINDOWS_SETUP_IMAGE = 2, WINDOWS_SINGLE_IMAGE = 1 };

static enum wimlib_progress_status CheckCancellation(enum wimlib_progress_msg message,
	union wimlib_progress_info* info, void* context);

int OpenWindowsSetupWim(const char* path, WIMStruct** wim, int* image)
{
	struct stat info;
	WIMStruct* opened = NULL;
	int error;

	if (wim != NULL)
		*wim = NULL;
	if (image != NULL)
		*image = 0;
	const bool valid_path = path != NULL && *path != 0;
	const bool valid_outputs = wim != NULL && image != NULL;
	if (!valid_path || !valid_outputs)
		return WIMLIB_ERR_INVALID_PARAM;
	if (IsRufusJobCancelled())
		return WIMLIB_ERR_ABORTED_BY_PROGRESS;
	if (lstat(path, &info) != 0)
		return WIMLIB_ERR_OPEN;
	if (!S_ISREG(info.st_mode))
		return WIMLIB_ERR_NOT_A_REGULAR_FILE;
	if (info.st_nlink != 1)
		return WIMLIB_ERR_INVALID_PARAM;
	error = wimlib_open_wim(path, WIMLIB_OPEN_FLAG_WRITE_ACCESS, &opened);
	if (error == 0 && IsRufusJobCancelled())
		error = WIMLIB_ERR_ABORTED_BY_PROGRESS;
	if (error != 0) {
		wimlib_free(opened);
		return error;
	}
	int selected = wimlib_resolve_image(opened, "2");
	if (selected != WINDOWS_SETUP_IMAGE) {
		selected = wimlib_resolve_image(opened, "1");
		if (selected != WINDOWS_SINGLE_IMAGE) {
			wimlib_free(opened);
			return WIMLIB_ERR_INVALID_IMAGE;
		}
		uprintf("Windows Setup image 2 is absent; using image 1 as Rufus does.");
	}
	wimlib_register_progress_function(opened, CheckCancellation, NULL);
	*wim = opened;
	*image = selected;
	return 0;
}

static enum wimlib_progress_status CheckCancellation(enum wimlib_progress_msg message,
	union wimlib_progress_info* info, void* context)
{
	(void)info;
	(void)context;
	/* RENAME is a notification after commit, not a cancellable operation. */
	if (message == WIMLIB_PROGRESS_MSG_RENAME)
		return WIMLIB_PROGRESS_STATUS_CONTINUE;
	return IsRufusJobCancelled() ? WIMLIB_PROGRESS_STATUS_ABORT : WIMLIB_PROGRESS_STATUS_CONTINUE;
}

int UpdateWindowsSetupWim(WIMStruct* wim, int image,
	const struct wimlib_update_command* commands, size_t count)
{
	const bool valid_image = wim != NULL && image >= WINDOWS_SINGLE_IMAGE;
	const bool valid_commands = count == 0 || commands != NULL;
	if (!valid_image || !valid_commands)
		return WIMLIB_ERR_INVALID_PARAM;
	if (IsRufusJobCancelled())
		return WIMLIB_ERR_ABORTED_BY_PROGRESS;
	if (count == 0)
		return 0;
	wimlib_register_progress_function(wim, CheckCancellation, NULL);
	int error = wimlib_update_image(wim, image, commands, count, 0);
	if (error != 0)
		return error;
	if (IsRufusJobCancelled())
		return WIMLIB_ERR_ABORTED_BY_PROGRESS;
	const int write_flags = WIMLIB_WRITE_FLAG_RECOMPRESS | WIMLIB_WRITE_FLAG_FSYNC;
	return wimlib_overwrite(wim, write_flags, 0);
}
