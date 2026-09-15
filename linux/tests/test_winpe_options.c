/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "winpe_scan.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned int checks, failures;
#define CHECK(test) do { checks++; if (!(test)) { \
	fprintf(stderr, "line %d: %s\n", __LINE__, #test); failures++; } } while (0)

int IsIsoScanCancelled(void)
{
	return 0;
}

void uprintf(const char* format, ...)
{
	(void)format;
}

static void CheckOptions(const char* source, size_t size, int status, bool minint)
{
	RUFUS_IMG_REPORT report = { 0 };
	char* data = malloc(size + 2);
	if (data == NULL)
		exit(1);
	memcpy(data, source, size);
	data[size] = 0;
	data[size + 1] = 0x55;
	report.uses_minint = report.winpe_options_known = 1;
	report.has_bootmgr = 42;
	CHECK(InspectWinPeOptions(data, size, &report) == status);
	CHECK(report.uses_minint == minint);
	CHECK(report.winpe_options_known == (status == ISO_SCAN_OK));
	CHECK(report.has_bootmgr == 42 && data[size + 1] == 0x55);
	free(data);
}

int main(void)
{
	static const struct {
		const char* text;
		bool minint;
	} cases[] = {
		{ "[SetupData]\r\nOsLoadOptions = \"/fastdetect /MININT\"\r\n", true },
		{ "\t osloadoptions\t=\t/minint", true },
		{ "OsLoadOptions=/minint-extra", true },
		{ "OsLoadOptions=/fastdetect ; /minint", true },
		{ "OsLoadOptions=\"/fastdetect\" /minint", false },
		{ "OsLoadOptions=\"/MININT", true },
		{ "OsLoadOptions=\"\"\nOsLoadOptions=/minint", true },
		{ "OsLoadOptions=/fastdetect\nOsLoadOptions=/minint", false },
		{ "; OsLoadOptions=/minint\nOther=/minint", false },
		{ "OsLoadOptionsExtra=/minint\nOsLoadOptions:/minint", false },
		{ "\xef\xbb\xbfOsLoadOptions=/minint", true },
		{ "< OsLoadOptions >/minint</OsLoadOptions>", true },
		{ "OsLoadOptions=\"/fastdetect\"\n", false },
		{ "OsLoadOptions=\n", false },
		{ "OsLoadOptions=\"\"", false },
		{ "\r\n\n[SetupData]\r\n", false },
		{ "Other=1", false },
		{ "OsLoadOptions=\"/minint /name=ș-é\"", true },
	};
	size_t i;
	char* large = malloc(WINPE_OPTIONS_MAX_SIZE + 2);
	RUFUS_IMG_REPORT report = { 0 };
	CHECK(large != NULL);
	if (large == NULL)
		return 1;
	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		CheckOptions(cases[i].text, strlen(cases[i].text), ISO_SCAN_OK, cases[i].minint);
		CheckOptions(cases[i].text, strlen(cases[i].text) + 1, ISO_SCAN_OK, cases[i].minint);
	}
	CheckOptions("", 0, ISO_SCAN_PARTIAL, false);
	CheckOptions("\0", 1, ISO_SCAN_PARTIAL, false);
	CheckOptions("x\0OsLoadOptions=/minint", 23, ISO_SCAN_PARTIAL, false);
	CheckOptions("\xff\xfeO\0", 4, ISO_SCAN_PARTIAL, false);
	memset(large, ' ', WINPE_OPTIONS_MAX_SIZE + 1);
	memcpy(large, "OsLoadOptions=/minint", sizeof("OsLoadOptions=/minint") - 1);
	CheckOptions(large, WINPE_OPTIONS_MAX_SIZE, ISO_SCAN_OK, true);
	CheckOptions(large, WINPE_OPTIONS_MAX_SIZE + 1, ISO_SCAN_PARTIAL, false);
	CHECK(InspectWinPeOptions(NULL, 10, &report) == ISO_SCAN_PARTIAL);
	CHECK(InspectWinPeOptions(large, 10, NULL) == ISO_SCAN_ERROR);
	free(large);
	printf("WinPE options: %u checks, %u failures\n", checks, failures);
	return failures != 0;
}
