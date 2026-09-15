/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "windows_image.h"
#include "native_input.h"
#include "iso_file.h"
#include "rufus_reporting.h"

#include <string.h>

int ScanWindowsImage(const ISO_ANALYSIS_SOURCE* source, ISO_ANALYSIS* analysis,
	WINDOWS_IMAGE_ANALYSIS* windows)
{
	ISO_FILE_READER reader = { 0 };
	WIMStruct* wim = NULL;
	const char* path;
	int status, error = 0;

	if (windows == NULL || analysis == NULL)
		return ISO_SCAN_ERROR;
	memset(windows, 0, sizeof(*windows));
	if (IsIsoScanCancelled())
		goto cancelled;
	if (!analysis->tree_complete || !HAS_WININST(analysis->report))
		return analysis->pending ? ISO_SCAN_PARTIAL : ISO_SCAN_OK;
	analysis->report.wininst_version = 0;
	memset(&analysis->report.win_version, 0, sizeof(analysis->report.win_version));
	analysis->pending |= ISO_PENDING_WINDOWS;
	path = analysis->report.wininst_path[0];
	if (source == NULL || memchr(path, 0, sizeof(analysis->report.wininst_path[0])) == NULL ||
		strncmp(path, "?:/", 3) != 0)
	{
		error = WIMLIB_ERR_INVALID_PARAM;
		goto out;
	}
	uprintf("Windows installation image: %s (image 1)", path + 2);
	status = OpenIsoFileReader(&reader, source->iso, source->udf, path + 2);
	if (status != ISO_SCAN_OK)
	{
		error = WIMLIB_ERR_OPEN;
		goto out;
	}
	error = OpenWimFromIsoReader(&reader, 0, &wim);
	if (error == 0)
		error = ReadWimMetadata(wim, 1, &windows->metadata);
out:
	wimlib_free(wim);
	CloseIsoFileReader(&reader);
	if (IsIsoScanCancelled() || error == WIMLIB_ERR_ABORTED_BY_PROGRESS)
		goto cancelled;
	if (error != 0)
	{
		memset(&windows->metadata, 0, sizeof(windows->metadata));
		windows->state = WINDOWS_IMAGE_FAILED;
		windows->wim_error = error;
		uprintf("Cannot read Windows image metadata: %s", wimlib_get_error_string(error));
		return ISO_SCAN_PARTIAL;
	}
	analysis->report.wininst_version = windows->metadata.archive.version;
	analysis->report.win_version = windows->metadata.win_version;
	windows->state = WINDOWS_IMAGE_UNKNOWN;
	if (windows->metadata.valid_fields == WIM_VERSION_ALL &&
		windows->metadata.win_version.major != 0 && windows->metadata.win_version.build != 0)
	{
		windows->state = WINDOWS_IMAGE_KNOWN;
		if (!HAS_WINPE(analysis->report) || analysis->report.winpe_options_known)
			analysis->pending &= ~ISO_PENDING_WINDOWS;
	}
	return analysis->pending ? ISO_SCAN_PARTIAL : ISO_SCAN_OK;
cancelled:
	memset(windows, 0, sizeof(*windows));
	memset(analysis, 0, sizeof(*analysis));
	return ISO_SCAN_CANCELLED;
}
