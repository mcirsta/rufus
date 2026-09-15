/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "syslinux_scan.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static int cases, failures, conflicts, family_warnings;
static char last_log[512];
static const uint16_t embedded[] = { 0x0407, 0x0604 };

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
	if (strstr(last_log, "Found conflicting Isolinux") != NULL)
		conflicts++;
	if (strstr(last_log, "presence of ldlinux.c32") != NULL)
		family_warnings++;
}

static void CheckConfig(char** paths, uint32_t count, const char* expected, bool overwrite)
{
	RUFUS_IMG_REPORT report = { 0 };
	StrArray configs = { paths, count, count };
	bool result;

	strcpy(report.label, "Keep label");
	report.has_grub2 = 2;
	report.image_size = 12345;
	strcpy(report.cfg_path, "/stale.cfg");
	report.needs_syslinux_overwrite = 1;
	report.sl_version = report.sl_detected_version = 0x0603;
	report.sl_version_inferred = 1;
	strcpy(report.sl_version_ext, "/stale");
	strcpy(report.sl_detected_version_ext, "/stale");
	strcpy(report.sl_version_str, "6.03");
	cases++;
	result = SelectSyslinuxConfig(&report, &configs);
	CHECK(result == (expected != NULL));
	CHECK(strcmp(report.cfg_path, expected == NULL ? "" : expected) == 0);
	CHECK(HAS_SYSLINUX(report) == (count != 0));
	CHECK(report.needs_syslinux_overwrite == overwrite);
	CHECK(report.sl_version == 0 && report.sl_detected_version == 0 && !report.sl_version_inferred);
	CHECK(report.sl_version_ext[0] == 0 && report.sl_detected_version_ext[0] == 0 && report.sl_version_str[0] == 0);
	CHECK(strcmp(report.label, "Keep label") == 0 && report.has_grub2 == 2 && report.image_size == 12345);
}

static void CheckConfigurations(void)
{
	char* shortest[] = { "/deep/isolinux/isolinux.cfg", "/isolinux/isolinux.cfg", "/syslinux.cfg" };
	char* ties[] = { "/EFI/BOOT/isolinux.cfg", "/isolinux/isolinux.cfg" };
	char* suse[] = { "/syslinux.cfg", "/boot/x86_64/loader/isolinux.cfg", "/boot/i386/loader/isolinux.cfg" };
	char* mixed[] = { "/syslinux.cfg", "/BOOT/I386/LOADER/ISOLINUX.CFG" };
	char accepted[128], long_path[129];
	char* limits[] = { long_path, accepted };

	CheckConfig(NULL, 0, NULL, false);
	CheckConfig(shortest, 3, "/syslinux.cfg", false);
	CheckConfig(ties, 2, ties[1], false);
	CheckConfig(suse, 3, suse[1], true);
	CheckConfig(mixed, 2, mixed[1], true);
	memset(accepted, 'a', sizeof(accepted) - 1);
	accepted[0] = '/';
	accepted[sizeof(accepted) - 1] = 0;
	memset(long_path, 'b', sizeof(long_path) - 1);
	long_path[0] = '/';
	long_path[sizeof(long_path) - 1] = 0;
	CheckConfig(limits, 2, accepted, false);
	CheckConfig(limits, 1, NULL, false);
}

static void CheckCandidatePair(const char* config, const char* first_path, uint16_t first,
	const char* first_ext, const char* second_path, uint16_t second, const char* second_ext,
	bool choose_second, int expected_conflicts)
{
	RUFUS_IMG_REPORT report = { 0 };
	const char* selected = NULL;
	char* config_path[] = { (char*)config };
	StrArray configs = { config_path, 1, 1 };
	char expected_ext[32];

	cases++;
	CHECK(SelectSyslinuxConfig(&report, &configs));
	conflicts = 0;
	selected = SelectSyslinuxCandidate(&report, selected, first_path, first, first_ext);
	selected = SelectSyslinuxCandidate(&report, selected, second_path, second, second_ext);
	CHECK(selected == (choose_second ? second_path : first_path));
	CHECK(report.sl_detected_version == (choose_second ? second : first));
	snprintf(expected_ext, sizeof(expected_ext), "%s", choose_second ? second_ext : first_ext);
	CHECK(strcmp(report.sl_detected_version_ext, expected_ext) == 0);
	CHECK(conflicts == expected_conflicts);
	CHECK(report.sl_version == 0 && report.sl_version_str[0] == 0 && !report.sl_version_inferred);
}

static void CheckCandidates(void)
{
	const char* cfg = "/isolinux/isolinux.cfg";
	CheckCandidatePair(cfg, "/other/isolinux.bin", 0, "", "/isolinux/isolinux.bin", 0x0604, "/pre1", true, 0);
	CheckCandidatePair(cfg, "/a/isolinux.bin", 0, "", "/b/isolinux.bin", 0, "", true, 0);
	CheckCandidatePair(cfg, "/a/isolinux.bin", 0x0604, "/pre1", "/isolinux/isolinux.bin", 0x0604, "/pre1", false, 0);
	CheckCandidatePair(cfg, "/a/isolinux.bin", 0x0407, "/date", "/isolinux/isolinux.bin", 0x0604, "/pre1", true, 1);
	CheckCandidatePair(cfg, "/isolinux/isolinux.bin", 0x0407, "/date", "/other/isolinux.bin", 0x0604, "/pre1", false, 1);
	CheckCandidatePair(cfg, "/a/isolinux.bin", 0x0604, "/pre1", "/isolinux/isolinux.bin", 0x0604, "/distro", true, 1);
	CheckCandidatePair(cfg, "/a/isolinux.bin", 0x0407, "", "/ISOLINUX/isolinux.bin", 0x0604, "", true, 1);
	// Retain the existing prefix behavior, including root and directory-name prefixes.
	CheckCandidatePair("/syslinux.cfg", "/a/isolinux.bin", 0x0407, "", "/b/isolinux.bin", 0x0604, "", true, 1);
	CheckCandidatePair(cfg, "/a/isolinux.bin", 0x0407, "", "/isolinux-extra/isolinux.bin", 0x0604, "", true, 1);
	CheckCandidatePair(cfg, "/a/isolinux.bin", 0x0604, "/pre1", "/isolinux/isolinux.bin", 0, "", true, 1);
	CheckCandidatePair(cfg, "/isolinux/isolinux.bin", 0x0604, "/pre1", "/a/isolinux.bin", 0, "", false, 1);
	CheckCandidatePair(cfg, "/a/isolinux.bin", 0x0604, "/abcdefghijklmnopqrstuvwxyz123456789", "/b/isolinux.bin", 0x0604,
		"/abcdefghijklmnopqrstuvwxyz123456789", false, 1);
}

static void CheckSelection(void)
{
	RUFUS_IMG_REPORT report = { 0 };
	char* path[] = { "/isolinux/isolinux.cfg" };
	StrArray configs = { path, 1, 1 };
	const uint16_t unavailable[] = { 0, 0 };
	const char* source;
	char ext[] = "/pre1";

	for (int family = 0; family < 2; family++) {
		cases++;
		CHECK(SelectSyslinuxConfig(&report, &configs));
		CHECK(HAS_SYSLINUX(report) && IS_BIOS_BOOTABLE(report) && HAS_PERSISTENCE(report));
		SelectSyslinuxVersion(&report, NULL, family != 0, embedded);
		CHECK(report.sl_version == embedded[family] && report.sl_version_inferred);
		CHECK(report.sl_detected_version == 0 && report.sl_detected_version_ext[0] == 0);
		CHECK(report.sl_version_ext[0] == 0);
		CHECK(strcmp(report.sl_version_str, family == 0 ? "4.07" : "6.04") == 0);
		cases++;
		SelectSyslinuxVersion(&report, NULL, family != 0, unavailable);
		CHECK(HAS_SYSLINUX(report) && report.sl_version == 0 && !report.sl_version_inferred);
		CHECK(report.sl_version_str[0] == 0 && report.sl_version_ext[0] == 0);
	}
	cases++;
	CHECK(SelectSyslinuxConfig(&report, &configs));
	source = SelectSyslinuxCandidate(&report, NULL, "/isolinux/isolinux.bin", 0x0604, ext);
	memset(ext, 'x', sizeof(ext) - 1);
	SelectSyslinuxVersion(&report, source, true, unavailable);
	CHECK(report.sl_version == 0x0604 && !report.sl_version_inferred);
	CHECK(report.sl_detected_version == 0x0604 && strcmp(report.sl_detected_version_ext, "/pre1") == 0);
	CHECK(strcmp(report.sl_version_ext, "/pre1") == 0);
	CHECK(strcmp(report.sl_version_str, "6.04") == 0);
	for (int family = 0; family < 2; family++) {
		for (int modern = 0; modern < 2; modern++) {
			cases++;
			CHECK(SelectSyslinuxConfig(&report, &configs));
			source = SelectSyslinuxCandidate(&report, NULL, "/isolinux/isolinux.bin", modern ? 0x0500 : 0x0407, "");
			family_warnings = 0;
			SelectSyslinuxVersion(&report, source, family != 0, embedded);
			CHECK(family_warnings == (family != modern));
		}
	}
	cases++;
	configs.Index = 0;
	CHECK(!SelectSyslinuxConfig(&report, &configs));
	SelectSyslinuxVersion(&report, NULL, true, embedded);
	CHECK(!HAS_SYSLINUX(report) && report.sl_version == 0 && report.sl_detected_version == 0);
	CHECK(!report.sl_version_inferred && report.cfg_path[0] == 0);
	// A stale installation value alone must not invent an image property.
	report.sl_version = 0x0604;
	CHECK(!HAS_SYSLINUX(report) && !IS_BIOS_BOOTABLE(report) && !HAS_PERSISTENCE(report));
}

int main(void)
{
	CheckConfigurations();
	CheckCandidates();
	CheckSelection();
	printf("Syslinux scan policy: %d cases, %d failures\n", cases, failures);
	return failures != 0;
}
