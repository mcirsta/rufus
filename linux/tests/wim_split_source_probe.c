/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "native_input.h"
#include "iso_file.h"
#include "iso_analyze.h"
#include "wim_split_inventory.h"

int IsIsoScanCancelled(void) { return 0; }
void uprintf(const char* format, ...) { (void)format; }

int main(int argc, char** argv)
{
	ISO_ANALYSIS_SOURCE view = { 0 };
	ISO_FILE_READER reader = { 0 };
	WIMStruct* wim = NULL;
	if (argc != 2 || wimlib_global_init(WIMLIB_INIT_FLAG_ASSUME_UTF8) != 0)
		return 2;
	view.udf = udf_open(argv[1]);
	if (view.udf == NULL)
		view.iso = iso9660_open_ext(argv[1], ISO_EXTENSION_ALL);
	int error = OpenIsoFileReader(&reader, view.iso, view.udf, "/sources/install.wim");
	if (error == ISO_SCAN_OK) {
		fprintf(stderr, "WIM_BYTES=%" PRIu64 "\n", reader.size);
		error = OpenWimFromIsoReader(&reader, 0, &wim);
		if (error == 0)
			error = PrintWimInventory(wim);
	}
	wimlib_free(wim);
	CloseIsoFileReader(&reader);
	CloseIsoAnalysisSource(&view);
	wimlib_global_cleanup();
	return error == 0 ? 0 : 1;
}
