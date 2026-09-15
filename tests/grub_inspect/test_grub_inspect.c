/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "grub_inspect.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int cases, failures;
static char last_log[512];

#ifdef RUFUS_TEST_ALLOCATION_FAILURE
static int duplicate_calls, fail_duplicate;
extern char* __real_strdup(const char* text);

char* __wrap_strdup(const char* text)
{
	if (++duplicate_calls == fail_duplicate)
		return NULL;
	return __real_strdup(text);
}
#endif

#define CHECK(expression) do { \
	if (!(expression)) { \
		fprintf(stderr, "case %d, line %d: %s\n", cases, __LINE__, #expression); \
		failures++; \
	} \
} while (0)

void uprintf(const char* format, ...)
{
	va_list args;
	va_start(args, format);
	vsnprintf(last_log, sizeof(last_log), format, args);
	va_end(args);
}

static void PutVersion(char* buf, size_t offset, const char* marker, const char* version)
{
	size_t length = strlen(marker);
	memcpy(buf + offset, marker, length + 1);
	memcpy(buf + offset + length + 1, version, strlen(version) + 1);
}

static void CheckVersion(const char* version, const char* label, uint8_t flags,
	bool debug, const char* expected)
{
	char buf[1024] = { 0 }, original[sizeof(buf)];
	RUFUS_IMG_REPORT report = { 0 };

	PutVersion(buf, 64, "GRUB  version %s", version);
	if (debug)
		memcpy(buf + 512, "grub_debug_is_enabled", 21);
	memcpy(original, buf, sizeof(buf));
	snprintf(report.label, sizeof(report.label), "%s", label);
	report.has_grub2 = flags;
	cases++;
	GetGrubVersion(buf, sizeof(buf), "normal.mod", &report);
	CHECK(strcmp(report.grub2_version, expected) == 0);
	CHECK(memcmp(buf, original, sizeof(buf)) == 0);
	CHECK(strcmp(report.label, label) == 0 && report.has_grub2 == flags);
	CHECK(strstr(last_log, "normal.mod") != NULL);
}

static void CheckVersionRules(void)
{
	static const struct {
		const char *version, *label, *expected;
		uint8_t flags;
		bool debug;
	} rules[] = {
		{ "2.12", "ignored", "2.12", 1, false },
		{ "2.12", "ignored", "2.12-nonstandard", 2, false },
		{ "2.12", "ignored", "2.12-gdie", 0x80, true },
		{ "2.12", "ignored", "2.12-nonstandard-gdie", 0x82, true },
		{ "2.06", "Fedora-WS-Live-x86_64-40", "2.06-fedora-40", 1, false },
		{ "2.06", "openSUSE-Leap-15.6-DVD-x86_64", "2.06-nonstandard-gdie-opensuse-15.6", 2, true },
		{ "2.06", "Ubuntu 22.04 LTS amd64", "2.06-ubuntu-22.04", 1, false },
		{ "2.06", "---TEST___LABEL.iso---", "2.06-test-label", 1, false },
		{ "2.06", "", "2.06", 1, false },
		{ "2.06-ubuntu", "ignored", "2.06-ubuntu", 1, false },
		{ "2.02-beta2", "ignored", "2.02-beta2", 1, false },
		{ "2.12 (Distro)", "ignored", "2.12-distro", 1, false },
		{ "0.97", "ignored", "", 1, false },
		{ "0", "ignored", "", 2, true },
		{ "", "ignored", "", 1, true },
	};
	for (size_t i = 0; i < sizeof(rules) / sizeof(rules[0]); i++)
		CheckVersion(rules[i].version, rules[i].label, rules[i].flags,
			rules[i].debug, rules[i].expected);
}

static void CheckSelectionAndBounds(void)
{
	char buf[1024] = { 0 }, long_version[301], expected[192];
	RUFUS_IMG_REPORT report = { 0 };
	const char* markers[] = { "GRUB  version %s", "GRUB version %s", "GRUB  version %s\n" };

	for (size_t i = 0; i < sizeof(markers) / sizeof(markers[0]); i++) {
		memset(buf, 0, sizeof(buf));
		PutVersion(buf, 0, markers[i], "2.12");
		report.grub2_version[0] = 0;
		cases++;
		GetGrubVersion(buf, sizeof(buf), "banner", &report);
		CHECK(strcmp(report.grub2_version, "2.12") == 0);
	}
	memset(buf, 0, sizeof(buf));
	PutVersion(buf, 64, markers[0], "2.04");
	PutVersion(buf, 256, markers[1], "2.12");
	report.grub2_version[0] = 0;
	cases++;
	GetGrubVersion(buf, sizeof(buf), "two-markers", &report);
	CHECK(strcmp(report.grub2_version, "2.12") == 0);
	memset(buf, 0, sizeof(buf));
	PutVersion(buf, 0, markers[0], "2.14");
	cases++;
	GetGrubVersion(buf, sizeof(buf), "second-file", &report);
	CHECK(strcmp(report.grub2_version, "2.12") == 0);
	CHECK(strstr(last_log, "2.14") != NULL);
	for (size_t size = 32; size <= 33; size++) {
		report.grub2_version[0] = 0;
		cases++;
		GetGrubVersion(buf, size, "minimum-span", &report);
		CHECK(strcmp(report.grub2_version, size == 33 ? "2.14" : "") == 0);
	}
	for (size_t offset = 63; offset <= 64; offset++) {
		memset(buf, 0, sizeof(buf));
		PutVersion(buf, offset, markers[0], "2.12");
		report.grub2_version[0] = 0;
		cases++;
		GetGrubVersion(buf, 96, "search-margin", &report);
		CHECK(strcmp(report.grub2_version, offset == 63 ? "2.12" : "") == 0);
	}
	// New bounded contract: a NUL outside the span does not terminate its text.
	memset(buf, 'Z', sizeof(buf));
	memcpy(buf, "GRUB  version %s", 16);
	buf[sizeof(buf) - 1] = 0;
	report.grub2_version[0] = 0;
	cases++;
	GetGrubVersion(buf, sizeof(buf) - 1, "unterminated", &report);
	CHECK(report.grub2_version[0] == 0);
	cases++;
	GetGrubVersion(NULL, 1024, "null", &report);
	CHECK(report.grub2_version[0] == 0);
	GetGrubVersion(buf, sizeof(buf), "null-report", NULL);
	memset(long_version, 'A', sizeof(long_version) - 1);
	long_version[sizeof(long_version) - 1] = 0;
	memset(expected, 'a', sizeof(expected) - 1);
	expected[sizeof(expected) - 1] = 0;
	CheckVersion(long_version, "ignored", 2, true, expected);
}

static void CheckFilesystems(void)
{
	char buf[1024] = { 0 };
	const char* names[] = { "fat", "ntfs", "fat", "exfat", "FAT", "ext2", "12345678901", "123456789012", "" };
	const char* expected[] = { "fat", "ntfs", "exfat", "FAT", "ext2", "12345678901" };
	StrArray filesystems;

	StrArrayCreate(&filesystems, 1);
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
		PutVersion(buf, i * 64, "fshelp", names[i]);
	cases++;
	CHECK(GetGrubFs(buf, sizeof(buf), &filesystems));
	CHECK(filesystems.Index == sizeof(expected) / sizeof(expected[0]));
	for (size_t i = 0; i < filesystems.Index; i++)
		CHECK(strcmp(filesystems.String[i], expected[i]) == 0);
	CHECK(GetGrubFsMask(&filesystems) == 7);
	memset(buf, 0, sizeof(buf));
	CHECK(strcmp(filesystems.String[0], "fat") == 0); // Names own their bytes.
	StrArrayClear(&filesystems);
	for (size_t i = 0; i < 3; i++) {
		PutVersion(buf, 0, "fshelp", i == 0 ? "FAT" : i == 1 ? "exFAT" : "NTFS");
		cases++;
		CHECK(GetGrubFs(buf, sizeof(buf), &filesystems));
		CHECK(GetGrubFsMask(&filesystems) == (1 << i));
		StrArrayClear(&filesystems);
	}
	for (size_t offset = 63; offset <= 64; offset++) {
		memset(buf, 0, sizeof(buf));
		PutVersion(buf, offset, "fshelp", "fat");
		cases++;
		CHECK(GetGrubFs(buf, 96, &filesystems));
		CHECK(filesystems.Index == (offset == 63 ? 1u : 0u));
		StrArrayClear(&filesystems);
	}
	memset(buf, 'Z', sizeof(buf));
	memcpy(buf, "fshelp", 7);
	cases++;
	CHECK(GetGrubFs(buf, sizeof(buf), &filesystems) && filesystems.Index == 0);
	memset(buf, 0, sizeof(buf));
	PutVersion(buf, 0, "FsHelp", "fat");
	PutVersion(buf, 64, "fshelpX", "ntfs");
	cases++;
	CHECK(GetGrubFs(buf, sizeof(buf), &filesystems) && filesystems.Index == 0);
	CHECK(GetGrubFs(NULL, 1024, &filesystems));
	CHECK(!GetGrubFs(buf, sizeof(buf), NULL));
	CHECK(GetGrubFsMask(NULL) == 0);
	StrArrayDestroy(&filesystems);
	// An unavailable array cannot silently discard a discovered filesystem.
	PutVersion(buf, 0, "fshelp", "fat");
	cases++;
	CHECK(!GetGrubFs(buf, sizeof(buf), &filesystems));
}

#ifdef RUFUS_TEST_ALLOCATION_FAILURE
static void CheckAllocationFailure(void)
{
	char buf[256] = { 0 };
	StrArray filesystems;

	StrArrayCreate(&filesystems, 8);
	PutVersion(buf, 0, "fshelp", "fat");
	PutVersion(buf, 64, "fshelp", "ntfs");
	duplicate_calls = 0;
	fail_duplicate = 2;
	cases++;
	CHECK(!GetGrubFs(buf, sizeof(buf), &filesystems));
	CHECK(filesystems.Index == 1 && GetGrubFsMask(&filesystems) == 1);
	fail_duplicate = 0;
	cases++;
	CHECK(GetGrubFs(buf, sizeof(buf), &filesystems));
	CHECK(filesystems.Index == 2 && GetGrubFsMask(&filesystems) == 5);
	StrArrayDestroy(&filesystems);
}
#endif

int main(void)
{
	CheckVersionRules();
	CheckSelectionAndBounds();
	CheckFilesystems();
#ifdef RUFUS_TEST_ALLOCATION_FAILURE
	CheckAllocationFailure();
#endif
	printf("GRUB inspection: %d cases, %d failures\n", cases, failures);
	return failures != 0;
}
