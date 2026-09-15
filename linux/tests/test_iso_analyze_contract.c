/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "iso_analyze.h"
#include <cdio/iso9660.h>
#include <cdio/udf.h>

#include <stdio.h>
#include <string.h>

static int failures, checks, cancelled, opens, closes;
static uint64_t fake_udf;

#define CHECK(expression) do { \
	checks++; \
	if (!(expression)) { \
		fprintf(stderr, "line %d: %s\n", __LINE__, #expression); \
		failures++; \
	} \
} while (0)

void uprintf(const char* format, ...)
{
	(void)format;
}

int IsIsoScanCancelled(void)
{
	return cancelled == 1 || (cancelled == 2 && opens != 0);
}

udf_t* __wrap_udf_open(const char* path)
{
	CHECK(strcmp(path, "udf-fixture") == 0);
	opens++;
	return (udf_t*)&fake_udf;
}

bool __wrap_udf_close(udf_t* image)
{
	CHECK(image == (udf_t*)&fake_udf);
	closes++;
	return true;
}

udf_dirent_t* __wrap_udf_get_root(udf_t* image, bool any, partition_num_t partition)
{
	CHECK(image == (udf_t*)&fake_udf && any && partition == 0);
	return NULL;
}

iso9660_t* __wrap_iso9660_open_ext(const char* path, iso_extension_mask_t mask)
{
	(void)path;
	(void)mask;
	CHECK(false); // A recognized UDF must not silently scan its ISO view.
	return NULL;
}

static void CheckFinalization(void)
{
	RUFUS_IMG_REPORT report = { 0 };
	const char* labels[] = { "", "A", "A  \t\n", "  ", "\xc8\x99  " };
	const char* expected[] = { "", "A", "A", " ", "\xc8\x99" };
	size_t i;

	for (i = 0; i < sizeof(labels) / sizeof(labels[0]); i++) {
		strcpy(report.label, labels[i]);
		CHECK(FinalizeIsoScan(&report, 3, 204800, 100));
		CHECK(strcmp(report.label, expected[i]) == 0);
		CHECK(report.projected_size == 6205 && report.mismatch_size == 0);
	}
	CHECK(FinalizeIsoScan(&report, 0, 204801, 100));
	CHECK(report.projected_size == 0 && report.mismatch_size == -1);
	CHECK(FinalizeIsoScan(&report, 1, 204799, 100));
	CHECK(report.projected_size == 2068 && report.mismatch_size == 1);
	CHECK(FinalizeIsoScan(&report, 1, -1, 100));
	CHECK(report.mismatch_size == 1);
	CHECK(FinalizeIsoScan(&report, 1, 20, -1));
	CHECK(report.mismatch_size == 1);
	// Rufus's float constant, rather than a double 1.01, is observable here.
	CHECK(FinalizeIsoScan(&report, 1000000, 0, 1000000));
	CHECK(report.projected_size == UINT64_C(2068479980));
	CHECK(FinalizeIsoScan(&report, 0, INT64_MAX, 0));
	CHECK(report.mismatch_size == -INT64_MAX);
	CHECK(!FinalizeIsoScan(&report, UINT64_MAX, 0, 0));
	CHECK(report.projected_size == 0);
	CHECK(!FinalizeIsoScan(&report, 1, 0, INT64_MAX));
	CHECK(report.projected_size == 0);
}

static void CheckDispatch(void)
{
	ISO_ANALYSIS analysis, empty = { 0 };
	ISO_ANALYSIS_OPTIONS options = { true, true, { 0x0407, 0x0604 } };
	int result;

	memset(&analysis, 0xa5, sizeof(analysis));
	CHECK(AnalyzeIsoImage(NULL, 2048, &options, &analysis) == ISO_SCAN_ERROR);
	CHECK(memcmp(&analysis, &empty, sizeof(empty)) == 0 && opens == 0);
	CHECK(AnalyzeIsoImage("udf-fixture", 2048, NULL, &analysis) == ISO_SCAN_ERROR);
	CHECK(AnalyzeIsoImage("udf-fixture", 2048, &options, NULL) == ISO_SCAN_ERROR);
	CHECK(AnalyzeIsoImage("udf-fixture", 0, &options, &analysis) == ISO_SCAN_ERROR);
	CHECK(AnalyzeIsoImage("udf-fixture", UINT64_MAX, &options, &analysis) == ISO_SCAN_ERROR);
	CHECK(memcmp(&analysis, &empty, sizeof(empty)) == 0 && opens == 0);
	cancelled = 1;
	CHECK(AnalyzeIsoImage("udf-fixture", 2048, &options, &analysis) == ISO_SCAN_CANCELLED);
	CHECK(memcmp(&analysis, &empty, sizeof(empty)) == 0 && opens == 0);
	cancelled = 0;
	result = AnalyzeIsoImage("udf-fixture", 2048, &options, &analysis);
	CHECK(result == ISO_SCAN_ERROR); // No ISO fallback after UDF root failure.
	CHECK(memcmp(&analysis, &empty, sizeof(empty)) == 0 && opens == closes);
	cancelled = 2;
	opens = closes = 0;
	CHECK(AnalyzeIsoImage("udf-fixture", 2048, &options, &analysis) == ISO_SCAN_CANCELLED);
	CHECK(memcmp(&analysis, &empty, sizeof(empty)) == 0 && opens == 1 && closes == 1);
}

int main(void)
{
	CheckFinalization();
	CheckDispatch();
	printf("ISO finalization/dispatch: %d checks, %d failures\n", checks, failures);
	return failures != 0;
}
