/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "platform/wim_file.h"
#include "wim_probe_output.h"

int IsIsoScanCancelled(void)
{
	return 0;
}

int main(int argc, char** argv)
{
	WIM_FILE_ANALYSIS analysis;
	int result;

	if (argc != 2 || wimlib_global_init(WIMLIB_INIT_FLAG_ASSUME_UTF8) != 0)
		return 64;
	result = ScanWimFile(argv[1], 1, &analysis);
	if (result == ISO_SCAN_OK || result == ISO_SCAN_PARTIAL)
		PrintWimMetadata(&analysis.metadata);
	else
		fprintf(stderr, "ERROR=%d\n", analysis.wim_error);
	wimlib_global_cleanup();
	return result;
}
