/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "wim_file.h"
#include "rufus_reporting.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int OpenWimFileDescriptor(int fd, WIMStruct** wim)
{
	struct stat info;
	char descriptor_path[64];

	if (wim == NULL)
		return WIMLIB_ERR_INVALID_PARAM;
	*wim = NULL;
	if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode))
		return WIMLIB_ERR_INVALID_PARAM;
	snprintf(descriptor_path, sizeof(descriptor_path), "/proc/self/fd/%d", fd);
	return wimlib_open_wim(descriptor_path, 0, wim);
}

/* The legacy metadata-only wrapper pins the path for opening. Callers needing
 * classification and content queries retain an IMAGE_FILE_SOURCE instead. */
static int OpenWimFile(const char* path, WIMStruct** wim)
{
	int result;
	int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);

	if (fd < 0)
		return WIMLIB_ERR_OPEN;
	result = OpenWimFileDescriptor(fd, wim);
	close(fd);
	return result;
}

int ScanWimFile(const char* path, int image, WIM_FILE_ANALYSIS* analysis)
{
	WIMStruct* wim = NULL;
	int error, status = ISO_SCAN_ERROR;

	if (analysis == NULL)
		return ISO_SCAN_ERROR;
	memset(analysis, 0, sizeof(*analysis));
	if (path == NULL || *path == 0 || image < 1)
	{
		analysis->wim_error = WIMLIB_ERR_INVALID_PARAM;
		return ISO_SCAN_ERROR;
	}
	if (IsIsoScanCancelled())
		return ISO_SCAN_CANCELLED;
	error = OpenWimFile(path, &wim);
	if (IsIsoScanCancelled())
		status = ISO_SCAN_CANCELLED;
	else if (error == 0)
	{
		error = ReadWimMetadata(wim, image, &analysis->metadata);
		if (IsIsoScanCancelled())
			status = ISO_SCAN_CANCELLED;
		else if (error == 0)
		{
			status = ISO_SCAN_PARTIAL;
			bool complete_version = analysis->metadata.valid_fields == WIM_VERSION_ALL;
			bool known_version = analysis->metadata.win_version.major != 0 && analysis->metadata.win_version.build != 0;
			if (complete_version && known_version)
				status = ISO_SCAN_OK;
		}
	}
	wimlib_free(wim);
	if (status == ISO_SCAN_ERROR || status == ISO_SCAN_CANCELLED)
		memset(&analysis->metadata, 0, sizeof(analysis->metadata));
	if (status == ISO_SCAN_ERROR)
		analysis->wim_error = error;
	return status;
}
