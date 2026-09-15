/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "image_policy.h"

#include <stdarg.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

static unsigned int failures;
static unsigned int cases;
static unsigned int diagnostics;

/* Satisfies the same logging symbol as Windows or the Qt reporting adapter. */
void uprintf(const char* format, ...)
{
	va_list args;

	diagnostics++;
	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);
	fputc('\n', stderr);
}

static void InitReport(RUFUS_IMG_REPORT* report)
{
	memset(report, 0, sizeof(*report));
	strcpy(report->label, "ordinary-image");
	report->is_iso = 1;
	report->is_bootable_img = 1;
	report->has_efi = 1 << ARCH_X86_64;
	// Unrelated scan results must survive applying policy.
	report->image_size = UINT64_C(9) << 30;
	report->projected_size = UINT64_C(10) << 30;
	report->mismatch_size = -123;
	report->has_symlinks = SYMLINKS_RR | SYMLINKS_UDF;
	strcpy(report->cfg_path, "/isolinux/isolinux.cfg");
	strcpy(report->efi_boot_entry[0].path, "/efi/boot/bootx64.efi");
	report->efi_boot_entry[0].type = EBT_MAIN;
}

static void CheckPolicy(const char* name, const RUFUS_IMG_REPORT* input,
	bool expected_dd, uint8_t expected_rh, uint8_t expected_disable)
{
	RUFUS_IMG_REPORT actual, expected;
	bool dd;

	memcpy(&actual, input, sizeof(actual));
	memcpy(&expected, input, sizeof(expected));
	expected.rh8_derivative = expected_rh;
	expected.disable_iso = expected_disable;
	dd = ApplyIsoPolicy(&actual);
	cases++;
	if (dd != expected_dd || memcmp(&actual, &expected, sizeof(actual)) != 0) {
		fprintf(stderr, "%s: DD=%d/%d, RH=%u/%u, disable=%u/%u or unrelated field changed\n",
			name, dd, expected_dd, (unsigned int)actual.rh8_derivative,
			(unsigned int)expected_rh, (unsigned int)actual.disable_iso,
			(unsigned int)expected_disable);
		failures++;
	}
	// Policy is repeatable and must leave is_iso transitions to the caller.
	dd = ApplyIsoPolicy(&actual);
	if (dd != expected_dd || memcmp(&actual, &expected, sizeof(actual)) != 0) {
		fprintf(stderr, "%s: repeated application changed the result\n", name);
		failures++;
	}
}

static void TestDistributionLabels(void)
{
	static const struct {
		const char* label;
		uint8_t rh;
		uint8_t disable;
	} labels[] = {
		{ "AlmaLinux-8.10-x86_64", 1, 0 },
		{ "AlmaLinux-9", 1, 0 },
		{ "AlmaLinux-7.9", 0, 0 },
		{ "AlmaLinux-10.0", 0, 0 },
		{ "Fedora-Workstation-Live-x86_64-32", 0, 0 },
		{ "Fedora-Workstation-Live-x86_64-33", 1, 0 },
		{ "Fedora-Workstation-Live-x86_64-39-1.5", 1, 0 },
		{ "Fedora-Workstation-Live-x86_64-40", 0, 0 },
		{ "CentOS-8-x86_64", 1, 0 },
		{ "CentOS-Stream-9-x86_64", 1, 0 },
		{ "Circle-9.4", 1, 0 },
		{ "OL-8.10", 1, 0 },
		{ "RHEL-9.4", 1, 0 },
		{ "Rocky-8.10", 1, 0 },
		{ "MIRACLE-LINUX-9.0", 1, 0 },
		{ "Install-SUSE", 0, 1 },
		{ "Install-SUSE-16.0-x86_64", 0, 1 },
		{ "Install-LEAP", 0, 1 },
		{ "Install-LEAP-16.0", 0, 1 },
		{ "openSUSE-Tumbleweed", 0, 1 },
		{ "openSUSE-Tumbleweed-DVD-x86_64", 0, 1 },
		{ "openSUSE-Leap-15.6", 0, 0 },
		{ "Install-LEA", 0, 0 },
		{ "prefix-Install-SUSE", 0, 0 },
		{ "install-SUSE", 0, 0 },
		{ "prefix-Rocky-9.4", 0, 0 },
		{ "rocky-9.4", 0, 0 },
		{ "", 0, 0 },
		{ "Rocky-9.4-\xc3\xa9", 1, 0 },
		{ "\xc3\xa9-Rocky-9.4", 0, 0 }
	};
	RUFUS_IMG_REPORT report;

	for (size_t i = 0; i < sizeof(labels) / sizeof(labels[0]); i++) {
		InitReport(&report);
		strcpy(report.label, labels[i].label);
		CheckPolicy(labels[i].label, &report, labels[i].disable != 0,
			labels[i].rh, labels[i].disable);
	}
	InitReport(&report);
	memset(report.label, 'x', sizeof(report.label) - 1);
	report.label[sizeof(report.label) - 1] = '\0';
	CheckPolicy("maximum label", &report, false, 0, 0);
}

static void TestGrubFilesystems(void)
{
	// FAT/exFAT/NTFS combinations; high bits are unrelated filesystem flags.
	static const uint8_t masks[] = { 0, 1, 2, 3, 4, 5, 6, 7, 0x80, 0x81, 0xff };
	// Keep Rufus's count/WIM/ESD/SWM encoding, including values beyond boolean 1.
	static const uint8_t large_files[] = { 0, 1, 0x11, 0x21, 0x41, 0xff };
	RUFUS_IMG_REPORT report;
	bool disable;
	char name[80];

	for (size_t i = 0; i < sizeof(masks) / sizeof(masks[0]); i++) {
		for (size_t j = 0; j < sizeof(large_files) / sizeof(large_files[0]); j++) {
			InitReport(&report);
			report.has_grub2 = 0x80;
			report.has_grub2_fs = masks[i];
			report.has_4GB_file = large_files[j];
			disable = masks[i] == 0 || masks[i] == 0x80 ||
				(large_files[j] != 0 && (masks[i] == 1 || masks[i] == 0x81));
			snprintf(name, sizeof(name), "EFI GRUB: filesystems=%u, large-files=%u",
				(unsigned int)masks[i], (unsigned int)large_files[j]);
			CheckPolicy(name, &report, disable, 0, disable);
			// The EFI-only restriction must not apply to BIOS GRUB.
			report.has_grub2 = 1;
			CheckPolicy("BIOS GRUB", &report, false, 0, 0);
		}
	}
}

static void TestBootMethods(void)
{
	RUFUS_IMG_REPORT report;

	InitReport(&report);
	CheckPolicy("EFI hybrid", &report, false, 0, 0);
	report.has_efi = 1;
	CheckPolicy("EFI image bit", &report, false, 0, 0);
	report.has_efi = 0;
	CheckPolicy("hybrid without supported boot method", &report, true, 0, 0);

	for (unsigned int method = 0; method < 10; method++) {
		InitReport(&report);
		report.has_efi = 0;
		switch (method) {
		case 0: report.has_bootmgr = 1; break;
		case 1: report.has_bootmgr_efi = 1; break;
		case 2: report.has_syslinux = 1; report.sl_version = 0x0604; break;
		case 3: report.winpe = WINPE_I386; break;
		case 4: report.winpe = WINPE_AMD64; break;
		case 5: report.winpe = WINPE_MININT; break;
		case 6: report.has_grub4dos = 1; break;
		case 7: report.has_grub2 = 1; break;
		case 8: strcpy(report.reactos_path, "/freeldr.sys"); break;
		case 9: report.has_kolibrios = 1; break;
		}
		CheckPolicy("supported BIOS method", &report, false, 0, 0);
		report.disable_iso = 1;
		CheckPolicy("explicitly disabled ISO", &report, true, 0, 1);
	}
	InitReport(&report);
	report.has_efi = 0;
	report.winpe = 1; // Incomplete WinPE flags are not a supported boot method.
	CheckPolicy("incomplete WinPE", &report, true, 0, 0);
	InitReport(&report);
	report.has_efi = 0;
	report.has_syslinux = 1;
	CheckPolicy("Syslinux presence with unknown version", &report, false, 0, 0);
	report.has_syslinux = 0;
	report.sl_version = 0x0604;
	CheckPolicy("selected version alone is not Syslinux presence", &report, true, 0, 0);
}

static void TestScanStates(void)
{
	RUFUS_IMG_REPORT report;

	for (int bootable = -1; bootable <= 2; bootable++) {
		InitReport(&report);
		report.is_bootable_img = (int8_t)bootable;
		report.has_efi = 0;
		CheckPolicy("DD detection states", &report, bootable > 0, 0, 0);
		strcpy(report.label, "Install-SUSE");
		CheckPolicy("DD-only label without/with DD support", &report, bootable > 0, 0, 1);
	}
	InitReport(&report);
	report.rh8_derivative = 1;
	report.disable_iso = 1;
	CheckPolicy("preserve previous findings", &report, true, 1, 1);

	InitReport(&report);
	strcpy(report.label, "Install-SUSE");
	report.is_iso = 0;
	CheckPolicy("non-ISO scan is unchanged", &report, false, 0, 0);
	strcpy(report.label, "Rocky-9.4");
	report.has_grub2 = 0x80;
	CheckPolicy("non-ISO skips label and GRUB policy", &report, false, 0, 0);

	// A previous report must not influence a newly scanned image.
	InitReport(&report);
	CheckPolicy("independent next scan", &report, false, 0, 0);
}

static void CheckChoices(const char* name, const RUFUS_IMG_REPORT* report,
	uint64_t persistence, bool gpt, bool fat, enum IMAGE_WRITE_MODE first,
	enum IMAGE_WRITE_MODE second, enum IMAGE_WRITE_MODE third)
{
	const enum IMAGE_WRITE_MODE expected[] = { first, second, third };
	static const int invalid[] = { -1, INT_MIN, 0, 3, 5, 6, 7, 8, INT_MAX };
	RUFUS_IMG_REPORT before;
	IMAGE_WRITE_CHOICES choices;
	int count = (first != IMAGE_WRITE_NONE) + (second != IMAGE_WRITE_NONE) + (third != IMAGE_WRITE_NONE);

	memcpy(&before, report, sizeof(before));
	choices = GetImageWriteChoices(report, persistence, gpt, fat);
	cases++;
	if (choices.count != count || memcmp(&before, report, sizeof(before)) != 0) {
		fprintf(stderr, "%s: wrong choice count or report modified\n", name);
		failures++;
		return;
	}
	for (int i = 0; i < 3; i++) {
		if (choices.mode[i] != expected[i] || GetImageWriteMode(&choices, 1 << i) != expected[i]) {
			fprintf(stderr, "%s: wrong mode for row %d\n", name, i);
			failures++;
		}
	}
	for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
		if (GetImageWriteMode(&choices, invalid[i]) != IMAGE_WRITE_NONE) {
			fprintf(stderr, "%s: invalid/cancelled selection accepted\n", name);
			failures++;
		}
	}
}

static void TestWriteChoices(void)
{
	RUFUS_IMG_REPORT report;
	static const uint64_t sizes[] = { MAX_ISO_TO_ESP_SIZE - 1, MAX_ISO_TO_ESP_SIZE, MAX_ISO_TO_ESP_SIZE + 1 };
	// Bit 0 is an embedded EFI image, bit 15 is outside HAS_REGULAR_EFI.
	static const uint16_t efi[] = { 0, 1, 2, 4, 0x4000, 0x8000, 0xffff };

	InitReport(&report);
	CheckChoices("ISO default, DD optional", &report, 0, false, true,
		IMAGE_WRITE_ISO, IMAGE_WRITE_DD, IMAGE_WRITE_NONE);
	CheckChoices("persistence skips hybrid choice", &report, 1, true, true,
		IMAGE_WRITE_ISO, IMAGE_WRITE_NONE, IMAGE_WRITE_NONE);
	report.has_efi = 0;
	report.has_syslinux = 1;
	CheckChoices("unknown Syslinux version keeps ISO default", &report, 0, false, true,
		IMAGE_WRITE_ISO, IMAGE_WRITE_DD, IMAGE_WRITE_NONE);
	report.has_syslinux = 0;
	CheckChoices("unsupported extraction boot method", &report, 0, false, true,
		IMAGE_WRITE_DD, IMAGE_WRITE_NONE, IMAGE_WRITE_NONE);
	InitReport(&report);
	strcpy(report.label, "Install-SUSE");
	ApplyIsoPolicy(&report);
	CheckChoices("DD-required policy before is_iso transition", &report, 0, true, true,
		IMAGE_WRITE_DD, IMAGE_WRITE_NONE, IMAGE_WRITE_NONE);
	CheckChoices("DD-required policy precedes persistence", &report, 4096, true, true,
		IMAGE_WRITE_DD, IMAGE_WRITE_NONE, IMAGE_WRITE_NONE);
	report.is_iso = 0;
	CheckChoices("Windows DD-required is_iso transition", &report, 0, true, true,
		IMAGE_WRITE_DD, IMAGE_WRITE_NONE, IMAGE_WRITE_NONE);
	for (int bootable = -1; bootable <= 2; bootable++) {
		InitReport(&report);
		report.is_bootable_img = (int8_t)bootable;
		CheckChoices("DD detection states", &report, 0, false, true, IMAGE_WRITE_ISO,
			bootable > 0 ? IMAGE_WRITE_DD : IMAGE_WRITE_NONE, IMAGE_WRITE_NONE);
		report.is_iso = 0;
		CheckChoices("non-ISO detection states", &report, 0, true, true,
			bootable > 0 ? IMAGE_WRITE_DD : IMAGE_WRITE_NONE, IMAGE_WRITE_NONE, IMAGE_WRITE_NONE);
	}
	for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
		for (size_t j = 0; j < sizeof(efi) / sizeof(efi[0]); j++) {
			for (int options = 0; options < 8; options++) {
				bool esp = i == 0 && (efi[j] & 0x7ffe) && (options & 3) == 3;
				bool persistence = (options & 4) != 0;
				InitReport(&report);
				report.has_syslinux = 1; // ISO mode remains viable even without EFI.
				report.has_efi = efi[j];
				report.projected_size = sizes[i];
				CheckChoices("ESP size/EFI/partition/filesystem/persistence", &report, persistence,
					(options & 1) != 0, (options & 2) != 0, IMAGE_WRITE_ISO,
					persistence ? IMAGE_WRITE_NONE : (esp ? IMAGE_WRITE_ESP : IMAGE_WRITE_DD),
					!persistence && esp ? IMAGE_WRITE_DD : IMAGE_WRITE_NONE);
			}
		}
	}
}

int main(void)
{
	TestDistributionLabels();
	TestGrubFilesystems();
	TestBootMethods();
	TestScanStates();
	TestWriteChoices();
	if (diagnostics != 0) {
		fprintf(stderr, "%u unexpected policy diagnostics\n", diagnostics);
		failures++;
	}
	printf("Image policy: %u cases, %u failures\n", cases, failures);
	return failures == 0 ? 0 : 1;
}
