/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "platform/windows_image.h"
#include "wim_probe_output.h"

#include <string.h>
#include <sys/stat.h>

static int cancelled;

int IsIsoScanCancelled(void)
{
	return cancelled;
}

void uprintf(const char* format, ...)
{
	(void)format;
}

int main(int argc, char** argv)
{
	ISO_ANALYSIS_SOURCE source = { 0 };
	ISO_ANALYSIS_OPTIONS options = { true, true, { 0x0407, 0x0604 } };
	ISO_ANALYSIS analysis;
	WINDOWS_IMAGE_ANALYSIS windows = { 0 };
	struct stat info;
	int result, handles;

	if ((argc != 2 && argc != 3) || stat(argv[1], &info) != 0 || !S_ISREG(info.st_mode))
		return 64;
	if (wimlib_global_init(WIMLIB_INIT_FLAG_ASSUME_UTF8) != 0)
		return 65;
	result = AnalyzeIsoImageWithSource(argv[1], (uint64_t)info.st_size, &options, &analysis, &source);
	handles = (source.iso != NULL) + (source.udf != NULL);
	if (result == ISO_SCAN_OK || result == ISO_SCAN_PARTIAL)
	{
		if (handles != 1 || (analysis.filesystem == ISO_FILESYSTEM_UDF) != (source.udf != NULL))
			return 66;
		if (argc == 3 && strcmp(argv[2], "cancel") == 0)
			cancelled = 1;
		result = ScanWindowsImage(&source, &analysis, &windows);
	}
	printf("{\"status\":%d,\"state\":%d,\"error\":%d,\"pending\":%u,\"filesystem\":%d,"
		"\"format\":%u,\"major\":%u,\"paths\":%u,\"handles\":%d}\n",
		result, windows.state, windows.wim_error, analysis.pending, analysis.filesystem,
		analysis.report.wininst_version, analysis.report.win_version.major, analysis.report.wininst_index, handles);
	PrintWimMetadata(&windows.metadata);
	CloseIsoAnalysisSource(&source);
	if (source.iso != NULL || source.udf != NULL)
		return 67;
	wimlib_global_cleanup();
	return 0;
}
