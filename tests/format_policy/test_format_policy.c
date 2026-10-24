/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "format_policy.h"
#include <stdio.h>
#include <string.h>
#include <locale.h>

static unsigned int checks, failures;
#define CHECK(condition) do { checks++; if (!(condition)) { \
	fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #condition); failures++; } } while (0)
#define MIB UINT64_C(1048576)
#define GIB (1024 * MIB)
#define TIB (1024 * GIB)

enum { INPUT_CAPACITY = 128, GUARD_VALUE = 0x1234 };

static void CheckLabel(const uint16_t* source, bool fat, uint64_t size, const char* expected, bool fallback)
{
	struct { uint16_t before; uint16_t label[VOLUME_LABEL_CAPACITY]; uint16_t after; } output;
	output.before = output.after = GUARD_VALUE;
	CHECK(NormalizeVolumeLabel(source, output.label, fat, size) == fallback);
	for (size_t i = 0; i <= strlen(expected); i++)
		CHECK(output.label[i] == (unsigned char)expected[i]);
	CHECK(output.before == GUARD_VALUE && output.after == GUARD_VALUE);
}

static void TestLabels(void)
{
	static const struct { const char* text; bool fat; const char* expected; bool fallback; } cases[] = {
		{ "new label", true, "NEW LABEL", false },
		{ "a*?,;:/\\|+=<>[]\"b", true, "AB", false },
		{ "a.b\tc", true, "A_B_C", false },
		{ "ABCDEFGHIJK__________", true, "ABCDEFGHIJK", false },
		{ "_a", true, "_A", false },
		{ "_a_", true, "64 MB", true },
		{ "", true, "", false },
		{ "*?", true, "", false },
		{ " spaces ", true, " SPACES ", false },
		{ "ab\ncd", true, "AB\nCD", false },
		{ "a.b\tc*?:", false, "a_b_c*?:", false },
		{ "________________________________________", false, "________________________________", false },
	};
	uint16_t input[INPUT_CAPACITY];
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		for (size_t j = 0; j <= strlen(cases[i].text); j++)
			input[j] = (unsigned char)cases[i].text[j];
		CheckLabel(input, cases[i].fat, 64 * MIB, cases[i].expected, cases[i].fallback);
	}
	const uint16_t unicode[] = { 'A', 'B', 'C', 'D', 0xd83d, 0xde00, 0x219, 0 };
	CheckLabel(unicode, true, 64 * MIB, "ABCD___", false);
	uint16_t normalized[VOLUME_LABEL_CAPACITY];
	CHECK(!NormalizeVolumeLabel(unicode, normalized, false, 64 * MIB));
	CHECK(memcmp(unicode, normalized, sizeof(unicode)) == 0);
	const uint16_t emoji[] = { 0xd83d, 0xde00, 0 };
	CheckLabel(emoji, true, 64 * MIB, "64 MB", true);
	CheckLabel(emoji, true, 79 * GIB / 10, "7_9 GB", true);
	CheckLabel(emoji, true, GIB + GIB / 4, "1_2 GB", true);
	CheckLabel(emoji, true, 0, "0 bytes", true);
	CheckLabel(emoji, true, UINT64_MAX, "16 EB", true);
	// Decimal commas must not escape into an on-disk FAT label.
	const char* locales[] = { "de_DE.UTF-8", "ro_RO.UTF-8", "German_Germany.1252" };
	bool checked_decimal_comma = false;
	for (size_t i = 0; i < sizeof(locales) / sizeof(locales[0]); i++) {
		if (setlocale(LC_NUMERIC, locales[i]) == NULL)
			continue;
		CHECK(strcmp(localeconv()->decimal_point, ",") == 0);
		CheckLabel(emoji, true, 79 * GIB / 10, "7_9 GB", true);
		checked_decimal_comma = true;
		break;
	}
	if (!checked_decimal_comma)
		printf("Decimal-comma locale unavailable; locale case skipped\n");
	setlocale(LC_NUMERIC, "C");
}

static void TestClusters(void)
{
	/* Golden values from the original SetFileSystemAndClusterSize. They include
	 * its non-power-of-two FAT32 threshold and sector/default adjustment. */
	static const struct { uint64_t size; uint32_t sector, allowed, standard; } cases[] = {
		{ 0, 512, 0, 0 }, { 32 * MIB, 512, 0, 0 },
		{ 33 * MIB, 512, 0x200, 512 }, { 64 * MIB, 512, 0x200, 512 },
		{ 128 * MIB, 512, 0x600, 1024 }, { 256 * MIB, 512, 0xe00, 512 },
		{ 259 * MIB, 512, 0x1e00, 4096 }, { 512 * MIB, 512, 0x1e00, 4096 },
		{ GIB, 512, 0x3e00, 4096 }, { 2 * GIB, 512, 0x7e00, 4096 },
		{ 4 * GIB, 512, 0xfc00, 4096 }, { 8 * GIB, 512, 0x1f800, 4096 },
		{ 16 * GIB, 512, 0x1f000, 8192 }, { 32 * GIB, 512, 0x1c000, 32768 },
		{ 64 * GIB, 512, 0x1c000, 32768 }, { TIB, 512, 0x1c000, 32768 },
		{ 2 * TIB - 512, 512, 0x1c000, 32768 }, { 2 * TIB, 512, 0, 0 },
		{ 64 * MIB, 4096, 0, 0 }, { 512 * MIB, 4096, 0x1000, 4096 },
		{ 8 * GIB, 4096, 0x1f000, 4096 },
		{ GIB, 0, 0, 0 }, { GIB, 768, 0, 0 }, { GIB, 65536, 0, 0 }
	};
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		const RUFUS_CLUSTER_SIZES sizes = GetFat32ClusterSizes(cases[i].size, cases[i].sector);
		CHECK(sizes.Allowed == cases[i].allowed);
		CHECK(sizes.Default == cases[i].standard);
	}
}

static void TestFat16Clusters(void)
{
	/* Original Win32 FAT16 capacity steps, before native extent filtering. */
	static const struct { uint64_t size; uint32_t sector, allowed, standard; } cases[] = {
		{ 0, 512, 0x1e00, 512 }, { 16 * MIB, 512, 0x1e00, 512 },
		{ 32 * MIB - 512, 512, 0x1e00, 512 }, { 32 * MIB, 512, 0x3c00, 1024 },
		{ 64 * MIB - 512, 512, 0x3c00, 1024 }, { 64 * MIB, 512, 0x7800, 2048 },
		{ 128 * MIB - 512, 512, 0x7800, 2048 }, { 128 * MIB, 512, 0xf000, 4096 },
		{ 256 * MIB - 512, 512, 0xf000, 4096 }, { 256 * MIB, 512, 0x1e000, 8192 },
		{ 512 * MIB - 512, 512, 0x1e000, 8192 }, { 512 * MIB, 512, 0x1c000, 16384 },
		{ GIB - 512, 512, 0x1c000, 16384 }, { GIB, 512, 0x18000, 32768 },
		{ 2 * GIB - 512, 512, 0x18000, 32768 }, { 2 * GIB, 512, 0x10000, 65536 },
		{ 4 * GIB - 512, 512, 0x10000, 65536 }, { 4 * GIB, 512, 0, 0 },
		{ 16 * MIB, 4096, 0x1000, 4096 }, { 16 * MIB, 8192, 0, 0 },
		{ GIB, 65536, 0x10000, 65536 }, { GIB, 131072, 0, 0 },
		{ GIB, 0, 0, 0 }, { GIB, 768, 0, 0 }
	};
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		const RUFUS_CLUSTER_SIZES sizes = GetFat16ClusterSizes(cases[i].size, cases[i].sector);
		CHECK(sizes.Allowed == cases[i].allowed);
		CHECK(sizes.Default == cases[i].standard);
	}
}

static void TestNtfsClusters(void)
{
	static const struct { uint64_t size; uint32_t sector, allowed, standard; } ntfs[] = {
		{ 0, 512, 0x1f000, 4096 }, { 64 * MIB, 512, 0x1f000, 4096 },
		{ 16 * TIB - 512, 512, 0x1f000, 4096 }, { 16 * TIB, 512, 0x1f000, 8192 },
		{ 32 * TIB, 512, 0x1f000, 16384 }, { 64 * TIB, 512, 0x1f000, 32768 },
		{ 128 * TIB, 512, 0x1f000, 65536 }, { 256 * TIB - 512, 512, 0x1f000, 65536 },
		{ 256 * TIB, 512, 0, 0 }, { GIB, 8192, 0x1e000, 8192 },
		{ GIB, 65536, 0x10000, 65536 }, { GIB, 131072, 0, 0 },
		{ GIB, 0, 0, 0 }, { GIB, 768, 0, 0 }
	};
	for (size_t i = 0; i < sizeof(ntfs) / sizeof(ntfs[0]); i++) {
		const RUFUS_CLUSTER_SIZES sizes = GetNtfsClusterSizes(ntfs[i].size, ntfs[i].sector);
		CHECK(sizes.Allowed == ntfs[i].allowed);
		CHECK(sizes.Default == ntfs[i].standard);
	}
}

static void TestExfatClusters(void)
{
	/* Golden values from Win32, including strict capacity/default boundaries. */
	static const struct { uint64_t size; uint32_t sector, allowed, standard; } cases[] = {
		{ 0, 512, 0x03fffe00, 4096 }, { 16 * MIB, 512, 0x03fffe00, 4096 },
		{ 256 * MIB - 512, 512, 0x03fffe00, 4096 },
		{ 256 * MIB, 512, 0x03fffe00, 32768 },
		{ 32 * GIB - 512, 512, 0x03fffe00, 32768 },
		{ 32 * GIB, 512, 0x03fffe00, 131072 },
		{ 256 * TIB - 512, 512, 0x03fffe00, 131072 },
		{ 256 * TIB, 512, 0, 0 }, { UINT64_MAX, 512, 0, 0 },
		{ MIB, 4096, 0x03fff000, 4096 }, { MIB, 8192, 0x03ffe000, 8192 },
		{ GIB, 65536, 0x03ff0000, 65536 }, { 32 * GIB, 262144, 0x03fc0000, 262144 },
		{ GIB, 32 * MIB, 0x02000000, 32 * MIB }, { GIB, 64 * MIB, 0, 0 },
		{ GIB, 0, 0, 0 }, { GIB, 768, 0, 0 }
	};
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		const RUFUS_CLUSTER_SIZES sizes = GetExfatClusterSizes(cases[i].size, cases[i].sector);
		CHECK(sizes.Allowed == cases[i].allowed);
		CHECK(sizes.Default == cases[i].standard);
	}
}

static void TestBootFileSystems(void)
{
	/* Stable combobox ordinals: FAT16=0, FAT32=1, NTFS=2, exFAT=4,
	 * ext2/3/4=6/7/8. Expected masks are independent of FS_MASK. */
	static const struct { int boot; uint32_t allowed; } cases[] = {
		{ BT_NON_BOOTABLE, 0x1ff }, { BT_MSDOS, 0x003 }, { BT_FREEDOS, 0x003 },
		{ BT_IMAGE, 0x007 }, { BT_SYSLINUX_V4, 0x003 }, { BT_SYSLINUX_V6, 0x007 },
		{ BT_REACTOS, 0x003 }, { BT_GRUB4DOS, 0x007 }, { BT_GRUB2, 0x1c7 },
		{ BT_UEFI_NTFS, 0x014 }, { BT_MAX, 0 }, { -1, 0 }
	};
	RUFUS_IMG_REPORT report = { 0 };
	RUFUS_FS_OPTIONS options = { BT_IMAGE, TT_UEFI, false, false, false };
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		bool forced = true;
		options.boot_type = cases[i].boot;
		CHECK(GetAllowedFileSystems(&report, &options, &forced) == cases[i].allowed);
		CHECK(!forced);
	}
}

static void TestImageFileSystems(void)
{
	const uint32_t fat = 0x003, ntfs = 0x004, fat_and_ntfs = 0x007;
	RUFUS_IMG_REPORT report = { 0 };
	RUFUS_FS_OPTIONS options = { BT_IMAGE, TT_UEFI, true, false, false };
	bool forced;
	report.has_efi = 4;
	CHECK(GetAllowedFileSystems(&report, &options, &forced) == fat_and_ntfs);
	CHECK(!forced);

	// Modern Windows installation media: ordinary UEFI files vs a large WIM.
	report.has_bootmgr = report.has_bootmgr_efi = report.wininst_index = 1;
	CHECK(GetAllowedFileSystems(&report, &options, &forced) == fat_and_ntfs);
	report.has_4GB_file = 0x11;
	CHECK(!IsFat32Compatible(&report, false));
	CHECK(GetAllowedFileSystems(&report, &options, &forced) == ntfs);
	CHECK(!forced);
	options.allow_dual_uefi_bios = true;
	CHECK(IsFat32Compatible(&report, true));
	CHECK(GetAllowedFileSystems(&report, &options, &forced) == fat_and_ntfs);
	// The exception is exactly one large WIM, not every large-file report.
	report.has_4GB_file = 0x12;
	CHECK(!IsFat32Compatible(&report, true));
	report.has_4GB_file = 0x22;
	CHECK(!IsFat32Compatible(&report, true));
	report.has_4GB_file = 1;
	CHECK(GetAllowedFileSystems(&report, &options, &forced) == ntfs);
	report.has_4GB_file = 0x11;
	report.needs_ntfs = 1;
	CHECK(!IsFat32Compatible(&report, true));
	CHECK(GetAllowedFileSystems(&report, &options, &forced) == ntfs);
	report.has_4GB_file = report.needs_ntfs = 0;

	options.target_type = TT_BIOS;
	CHECK(GetAllowedFileSystems(&report, &options, &forced) == fat_and_ntfs);
	options.allow_dual_uefi_bios = false;
	CHECK(GetAllowedFileSystems(&report, &options, &forced) == ntfs);
	options.target_type = TT_UEFI;
	options.windows_to_go = true;
	CHECK(GetAllowedFileSystems(&report, &options, &forced) == ntfs);
	options.allow_dual_uefi_bios = true;
	CHECK(GetAllowedFileSystems(&report, &options, &forced) == ntfs);
	options.windows_to_go = options.allow_dual_uefi_bios = false;

	// GRUB advertises supported filesystems; a conflicting image keeps Rufus's
	// forced-FAT warning instead of silently offering NTFS that GRUB cannot read.
	report = (RUFUS_IMG_REPORT){ 0 };
	report.has_grub2 = 0x80;
	report.has_grub2_fs = 1;
	CHECK(GetAllowedFileSystems(&report, &options, &forced) == fat);
	CHECK(!forced);
	report.has_4GB_file = 1;
	CHECK(GetAllowedFileSystems(&report, &options, &forced) == fat);
	CHECK(forced);
	report.has_grub2_fs = 4;
	CHECK(GetAllowedFileSystems(&report, &options, &forced) == ntfs);
	CHECK(!forced);
	report.has_4GB_file = 0;
	CHECK(!IsFat32Compatible(&report, false));
	CHECK(GetAllowedFileSystems(&report, &options, &forced) == ntfs);
	report.has_grub2_fs = 5;
	CHECK(GetAllowedFileSystems(&report, &options, &forced) == fat_and_ntfs);

	// Before an image is selected its compatibility restrictions do not apply.
	report.has_grub2_fs = 0;
	report.needs_ntfs = 1;
	options.image_selected = false;
	CHECK(GetAllowedFileSystems(&report, &options, &forced) == fat_and_ntfs);
	CHECK(!forced);
	// Keep the original handling of a retained Windows report at this boundary.
	report.uses_minint = 1;
	options.target_type = TT_BIOS;
	CHECK(GetAllowedFileSystems(&report, &options, &forced) == ntfs);
}

static void TestFileSystemPreferences(void)
{
	const uint32_t fat16 = 0x001, ntfs = 0x004, fat_and_ntfs = 0x007;
	RUFUS_IMG_REPORT report = { 0 };
	RUFUS_FS_OPTIONS options = { BT_IMAGE, TT_UEFI, true, false, false };
	// No recognized loader leaves the UI's existing/default selection alone.
	CHECK(GetPreferredFileSystem(&report, &options, fat_and_ntfs, FS_UNKNOWN) == FS_UNKNOWN);
	report.has_efi = 4;
	report.has_bootmgr_efi = 1;
	CHECK(GetPreferredFileSystem(&report, &options, fat_and_ntfs, FS_UNKNOWN) == FS_FAT32);
	CHECK(GetPreferredFileSystem(&report, &options, fat_and_ntfs, FS_NTFS) == FS_NTFS);
	CHECK(GetPreferredFileSystem(&report, &options, fat_and_ntfs, FS_EXT4) == FS_FAT32);
	CHECK(GetPreferredFileSystem(&report, &options, fat_and_ntfs, FS_MAX) == FS_FAT32);
	CHECK(GetPreferredFileSystem(&report, &options, fat_and_ntfs, -2) == FS_FAT32);
	CHECK(GetPreferredFileSystem(&report, &options, 0, FS_UNKNOWN) == FS_UNKNOWN);
	CHECK(GetPreferredFileSystem(&report, &options, fat16, FS_UNKNOWN) == FS_FAT16);
	// Rufus does not fall through to the Windows preference when EFI wins.
	CHECK(GetPreferredFileSystem(&report, &options, ntfs, FS_UNKNOWN) == FS_UNKNOWN);
	report.has_4GB_file = 0x11;
	CHECK(GetPreferredFileSystem(&report, &options, fat_and_ntfs, FS_UNKNOWN) == FS_NTFS);
	options.allow_dual_uefi_bios = true;
	CHECK(GetPreferredFileSystem(&report, &options, fat_and_ntfs, FS_UNKNOWN) == FS_NTFS);
	report.has_4GB_file = 0;
	options.target_type = TT_BIOS;
	CHECK(GetPreferredFileSystem(&report, &options, fat_and_ntfs, FS_UNKNOWN) == FS_FAT32);
	options.allow_dual_uefi_bios = false;
	CHECK(GetPreferredFileSystem(&report, &options, fat_and_ntfs, FS_UNKNOWN) == FS_NTFS);
	options.target_type = TT_UEFI;
	options.windows_to_go = true;
	CHECK(GetPreferredFileSystem(&report, &options, fat_and_ntfs, FS_UNKNOWN) == FS_NTFS);
	options.windows_to_go = false;
	options.image_selected = false;
	CHECK(GetPreferredFileSystem(&report, &options, fat_and_ntfs, FS_NTFS) == FS_UNKNOWN);
	options.image_selected = true;

	// Legacy FAT loaders retain precedence, including on a mixed Windows image.
	report.has_efi = 0;
	report.has_syslinux = 1;
	CHECK(GetPreferredFileSystem(&report, &options, fat_and_ntfs, FS_UNKNOWN) == FS_FAT32);
	report.has_syslinux = 0;
	strcpy(report.reactos_path, "/freeldr.sys");
	CHECK(GetPreferredFileSystem(&report, &options, fat16, FS_UNKNOWN) == FS_FAT16);
	report.reactos_path[0] = 0;
	report.has_kolibrios = 1;
	CHECK(GetPreferredFileSystem(&report, &options, fat_and_ntfs, FS_UNKNOWN) == FS_FAT32);
	CHECK(GetPreferredFileSystem(&report, &options, fat16, FS_UNKNOWN) == FS_UNKNOWN);
	report = (RUFUS_IMG_REPORT){ 0 };
	report.winpe = WINPE_AMD64;
	CHECK(GetPreferredFileSystem(&report, &options, fat_and_ntfs, FS_UNKNOWN) == FS_NTFS);

	// Characterize Win32's existing ordinal seed separately from offered rows:
	// it supplies FAT bits even when the dropdown contains only NTFS. Its wrapper
	// still selects only a matching row; the shared helper must not add bits.
	report.has_efi = 4;
	const uint32_t win32_seed = FS_NTFS | FS_FAT32;
	CHECK(win32_seed == 3);
	CHECK(GetPreferredFileSystem(&report, &options, ntfs | win32_seed, FS_UNKNOWN) == FS_FAT32);
	CHECK(GetPreferredFileSystem(&report, &options, ntfs, FS_UNKNOWN) == FS_UNKNOWN);
}

static void TestWindowsPartitions(void)
{
	// Golden masks use the original drive.h values: MSR=1, ESP=2,
	// UEFI:NTFS=4, compatibility=8. Ordinary installers never request ESP/MSR.
	static const struct {
		const char* name;
		int fs, target;
		bool gpt, windows_to_go, dual;
		uint8_t expected;
	} cases[] = {
		{ "NTFS UEFI GPT installer", FS_NTFS, TT_UEFI, true, false, false, 4 },
		{ "NTFS UEFI MBR installer", FS_NTFS, TT_UEFI, false, false, false, 4 },
		{ "exFAT UEFI installer", FS_EXFAT, TT_UEFI, true, false, false, 4 },
		{ "FAT32 UEFI installer", FS_FAT32, TT_UEFI, true, false, false, 0 },
		{ "NTFS BIOS installer", FS_NTFS, TT_BIOS, false, false, false, 0 },
		{ "exFAT BIOS installer", FS_EXFAT, TT_BIOS, false, false, false, 0 },
		{ "NTFS dual installer", FS_NTFS, TT_BIOS, false, false, true, 4 },
		{ "NTFS GPT Windows To Go", FS_NTFS, TT_UEFI, true, true, false, 3 },
		{ "NTFS dual Windows To Go", FS_NTFS, TT_UEFI, true, true, true, 3 },
		{ "NTFS MBR Windows To Go", FS_NTFS, TT_UEFI, false, true, false, 4 },
		{ "NTFS BIOS Windows To Go", FS_NTFS, TT_BIOS, false, true, false, 4 },
		// Filesystem eligibility is separate; this decision retains its precedence.
		{ "FAT32 GPT Windows To Go", FS_FAT32, TT_UEFI, true, true, false, 3 }
	};
	RUFUS_IMG_REPORT report = { 0 };
	report.has_bootmgr = report.has_bootmgr_efi = report.wininst_index = 1;
	report.has_efi = 4;
	report.has_4GB_file = 0x11;
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		RUFUS_PARTITION_OPTIONS options = { BT_IMAGE, cases[i].fs, cases[i].target,
			cases[i].gpt, cases[i].windows_to_go, false, cases[i].dual, false, 0 };
		uint8_t actual = GetExtraPartitions(&report, &options);
		if (actual != cases[i].expected)
			fprintf(stderr, "%s: expected %u, got %u\n", cases[i].name, cases[i].expected, actual);
		CHECK(actual == cases[i].expected);
		options.old_bios_fixes = true;
		CHECK(GetExtraPartitions(&report, &options) == (cases[i].expected | 8));
	}
}

static void TestOtherPartitions(void)
{
	RUFUS_IMG_REPORT report = { 0 };
	RUFUS_PARTITION_OPTIONS options = { BT_IMAGE, FS_NTFS, TT_BIOS, false, false, false, false, false, 0 };
	CHECK(GetExtraPartitions(&report, &options) == 0);
	report.has_efi = 4;
	// Linux and EFI-only Windows loaders keep UEFI:NTFS even for a BIOS target.
	CHECK(GetExtraPartitions(&report, &options) == 4);
	report.has_bootmgr_efi = 1;
	CHECK(GetExtraPartitions(&report, &options) == 4);
	report.uses_minint = 1;
	report.winpe = WINPE_AMD64;
	CHECK(GetExtraPartitions(&report, &options) == 4);
	report.has_bootmgr = 1;
	CHECK(GetExtraPartitions(&report, &options) == 0);
	options.allow_dual_uefi_bios = true;
	CHECK(GetExtraPartitions(&report, &options) == 4);
	options.allow_dual_uefi_bios = false;

	report = (RUFUS_IMG_REPORT){ 0 };
	options.boot_type = BT_UEFI_NTFS;
	CHECK(GetExtraPartitions(&report, &options) == 4);
	options.boot_type = BT_NON_BOOTABLE;
	CHECK(GetExtraPartitions(&report, &options) == 0);
	options.old_bios_fixes = true;
	CHECK(GetExtraPartitions(&report, &options) == 8);
	options.old_bios_fixes = false;
	options.boot_type = BT_IMAGE;
	options.persistence_size = GIB;
	CHECK(GetExtraPartitions(&report, &options) == 0);
	report.has_syslinux = 1;
	CHECK(GetExtraPartitions(&report, &options) == 16);
	report.has_syslinux = 0;
	report.has_grub2 = 1;
	CHECK(GetExtraPartitions(&report, &options) == 16);
	report.has_grub2 = 0;
	report.has_grub4dos = 1;
	CHECK(GetExtraPartitions(&report, &options) == 16);
	report.has_efi = 4;
	CHECK(GetExtraPartitions(&report, &options) == 20);
	options.old_bios_fixes = true;
	CHECK(GetExtraPartitions(&report, &options) == 28);
	options.old_bios_fixes = false;
	// The original block disables persistence for DD; the caller later skips
	// partition creation altogether. It does not clear the UEFI:NTFS decision.
	options.write_as_image = true;
	CHECK(GetExtraPartitions(&report, &options) == 4);
	options.write_as_image = false;
	options.persistence_size = 0;
	CHECK(GetExtraPartitions(&report, &options) == 4);
	options.persistence_size = GIB;
	options.boot_type = BT_GRUB2;
	CHECK(GetExtraPartitions(&report, &options) == 0);
	options.boot_type = BT_IMAGE;
	options.fs_type = FS_FAT32;
	CHECK(GetExtraPartitions(&report, &options) == 16);
	report.has_bootmgr_efi = 1;
	CHECK(GetExtraPartitions(&report, &options) == 0);
	report.has_bootmgr_efi = 0;
	strcpy(report.reactos_path, "/freeldr.sys");
	CHECK(GetExtraPartitions(&report, &options) == 0);
	report.reactos_path[0] = 0;
	report.has_kolibrios = 1;
	CHECK(GetExtraPartitions(&report, &options) == 0);
}

static void TestSecondaryBootSelection(void)
{
	// Windows keeps BT_IMAGE for Syslinux: WriteSBR then writes no GRUB record.
	const int images[] = { BT_IMAGE, BT_IMAGE, BT_GRUB4DOS, BT_IMAGE,
		BT_GRUB2, BT_IMAGE, BT_GRUB2, BT_IMAGE };
	for (unsigned int flags = 0; flags < 8; flags++) {
		RUFUS_IMG_REPORT report = { 0 };
		report.has_syslinux = (flags & 1) != 0;
		report.has_grub4dos = (flags & 2) != 0;
		report.has_grub2 = (flags & 4) != 0;
		for (int boot = BT_NON_BOOTABLE; boot < BT_MAX; boot++) {
			const int mbr = boot == BT_IMAGE ? images[flags] : boot;
			const int gpt = boot == BT_NON_BOOTABLE ? BT_NON_BOOTABLE : BT_MAX;
			CHECK(GetSecondaryBootType(boot, &report, false) == mbr);
			CHECK(GetSecondaryBootType(boot, &report, true) == gpt);
		}
	}
	CHECK(GetSecondaryBootType(BT_GRUB2, NULL, false) == BT_GRUB2);
	CHECK(GetSecondaryBootType(BT_NON_BOOTABLE, NULL, true) == BT_NON_BOOTABLE);
}

enum rufus_mbr_type ReferenceMasterBootRecord(const RUFUS_IMG_REPORT*, const RUFUS_BOOT_OPTIONS*);
enum rufus_boot_install_type ReferenceBootInstallType(const RUFUS_IMG_REPORT*, const RUFUS_BOOT_OPTIONS*);

enum dispatch_report_flags {
	DISPATCH_SYSLINUX = 1,
	DISPATCH_GRUB2 = 2,
	DISPATCH_GRUB4DOS = 4,
	DISPATCH_REACTOS = 8,
	DISPATCH_KOLIBRIOS = 16,
	DISPATCH_BOOTMGR = 32,
	DISPATCH_BOOTMGR_EFI = 64,
	DISPATCH_WINPE = 128,
	DISPATCH_MININT = 256,
	DISPATCH_REPORT_COMBINATIONS = 512
};

static void DispatchReport(unsigned int flags, RUFUS_IMG_REPORT* report)
{
	memset(report, 0, sizeof(*report));
	report->has_syslinux = (flags & DISPATCH_SYSLINUX) != 0;
	report->has_grub2 = (flags & DISPATCH_GRUB2) != 0;
	report->has_grub4dos = (flags & DISPATCH_GRUB4DOS) != 0;
	if (flags & DISPATCH_REACTOS) strcpy(report->reactos_path, "/freeldr.sys");
	report->has_kolibrios = (flags & DISPATCH_KOLIBRIOS) != 0;
	report->has_bootmgr = (flags & DISPATCH_BOOTMGR) != 0;
	report->has_bootmgr_efi = (flags & DISPATCH_BOOTMGR_EFI) != 0;
	report->winpe = flags & DISPATCH_WINPE ? WINPE_I386 : 0;
	report->uses_minint = (flags & DISPATCH_MININT) != 0;
}

enum dispatch_option_flags {
	DISPATCH_GPT = 1,
	DISPATCH_DUAL = 2,
	DISPATCH_RUFUS_MBR = 4,
	DISPATCH_MASQUERADE = 8,
	DISPATCH_OPTION_COMBINATIONS = 16
};

static void TestOriginalBootDispatch(void)
{
	const int filesystems[] = { FS_FAT16, FS_FAT32, FS_NTFS, FS_EXFAT };
	for (unsigned int flags = 0; flags < DISPATCH_REPORT_COMBINATIONS; flags++) {
		RUFUS_IMG_REPORT report;
		DispatchReport(flags, &report);
		const RUFUS_IMG_REPORT original = report;
		for (int boot = BT_NON_BOOTABLE; boot < BT_MAX; boot++)
		for (size_t fs = 0; fs < sizeof(filesystems) / sizeof(filesystems[0]); fs++)
		for (int target = TT_BIOS; target < TT_MAX; target++)
		for (unsigned int choices = 0; choices < DISPATCH_OPTION_COMBINATIONS; choices++) {
			const RUFUS_BOOT_OPTIONS options = { boot, filesystems[fs], target,
				(choices & DISPATCH_GPT) != 0, (choices & DISPATCH_DUAL) != 0,
				(choices & DISPATCH_RUFUS_MBR) != 0, (choices & DISPATCH_MASQUERADE) != 0 };
			CHECK(GetMasterBootRecord(&report, &options) == ReferenceMasterBootRecord(&report, &options));
			CHECK(GetBootInstallType(&report, &options) == ReferenceBootInstallType(&report, &options));
		}
		CHECK(memcmp(&report, &original, sizeof(report)) == 0);
	}
}

static void TestMixedBootSequence(void)
{
	/* Explicit expectations for the complete decision tuple, independent of
	 * the frozen oracle. Selecting a provider does not validate its payload. */
	static const struct {
		unsigned int flags;
		bool dual;
		enum rufus_mbr_type mbr;
		int sbr;
		enum rufus_boot_install_type install;
	} cases[] = {
		{ DISPATCH_WINPE | DISPATCH_SYSLINUX, false, RUFUS_MBR_SYSLINUX, BT_IMAGE, RUFUS_BOOT_INSTALL_SYSLINUX },
		{ DISPATCH_WINPE | DISPATCH_SYSLINUX, true, RUFUS_MBR_WIN7, BT_IMAGE, RUFUS_BOOT_INSTALL_PBR },
		{ DISPATCH_WINPE | DISPATCH_GRUB2, false, RUFUS_MBR_GRUB2, BT_GRUB2, RUFUS_BOOT_INSTALL_PBR },
		{ DISPATCH_WINPE | DISPATCH_GRUB2, true, RUFUS_MBR_WIN7, BT_GRUB2, RUFUS_BOOT_INSTALL_PBR },
		{ DISPATCH_WINPE | DISPATCH_GRUB2 | DISPATCH_SYSLINUX, true, RUFUS_MBR_WIN7, BT_IMAGE, RUFUS_BOOT_INSTALL_PBR },
		{ DISPATCH_REACTOS, false, RUFUS_MBR_WIN7, BT_IMAGE, RUFUS_BOOT_INSTALL_SYSLINUX },
		{ DISPATCH_REACTOS | DISPATCH_BOOTMGR_EFI, true, RUFUS_MBR_WIN7, BT_IMAGE, RUFUS_BOOT_INSTALL_PBR },
		{ DISPATCH_GRUB4DOS | DISPATCH_GRUB2, false, RUFUS_MBR_GRUB2, BT_GRUB2, RUFUS_BOOT_INSTALL_PBR },
		{ DISPATCH_KOLIBRIOS, false, RUFUS_MBR_KOLIBRIOS, BT_IMAGE, RUFUS_BOOT_INSTALL_PBR },
		{ DISPATCH_MININT | DISPATCH_SYSLINUX, true, RUFUS_MBR_WIN7, BT_IMAGE, RUFUS_BOOT_INSTALL_PBR }
	};
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		RUFUS_IMG_REPORT report;
		DispatchReport(cases[i].flags, &report);
		RUFUS_BOOT_OPTIONS options = { BT_IMAGE, FS_FAT32, TT_BIOS, false, cases[i].dual, false, false };
		CHECK(GetMasterBootRecord(&report, &options) == cases[i].mbr);
		CHECK(GetSecondaryBootType(options.boot_type, &report, options.gpt) == cases[i].sbr);
		CHECK(GetBootInstallType(&report, &options) == cases[i].install);
		options.gpt = true;
		CHECK(GetMasterBootRecord(&report, &options) == RUFUS_MBR_PROTECTIVE);
		CHECK(GetSecondaryBootType(options.boot_type, &report, options.gpt) == BT_MAX);
		// GPT alone does not change the later target/provider installation test.
		CHECK(GetBootInstallType(&report, &options) == cases[i].install);
		options.target_type = TT_UEFI;
		CHECK(GetBootInstallType(&report, &options) == RUFUS_BOOT_INSTALL_EFI_CHECK);
		options.gpt = false;
		CHECK(GetMasterBootRecord(&report, &options) == RUFUS_MBR_ZERO);
	}
}

enum rufus_pbr_type ReferencePartitionBootRecord(int, int, const RUFUS_IMG_REPORT*);

static void TestOriginalPartitionBootSelection(void)
{
	for (unsigned int flags = 0; flags < DISPATCH_REPORT_COMBINATIONS; flags++) {
		RUFUS_IMG_REPORT report;
		DispatchReport(flags, &report);
		const RUFUS_IMG_REPORT original = report;
		for (int boot = BT_NON_BOOTABLE; boot < BT_MAX; boot++)
		for (int fs = FS_UNKNOWN; fs <= FS_MAX; fs++)
			CHECK(GetPartitionBootRecord(boot, fs, &report) == ReferencePartitionBootRecord(boot, fs, &report));
		CHECK(memcmp(&report, &original, sizeof(report)) == 0);
	}
}

static void TestPartitionBootPriorities(void)
{
	RUFUS_IMG_REPORT report = { 0 };
	report.has_kolibrios = report.has_bootmgr = report.has_bootmgr_efi = 1;
	report.winpe = WINPE_I386;
	CHECK(GetPartitionBootRecord(BT_IMAGE, FS_FAT16, &report) == RUFUS_PBR_UNSUPPORTED);
	CHECK(GetPartitionBootRecord(BT_IMAGE, FS_FAT32, &report) == RUFUS_PBR_FAT32_KOLIBRIOS);
	CHECK(GetPartitionBootRecord(BT_FREEDOS, FS_FAT16, &report) == RUFUS_PBR_FAT16_FREEDOS);
	CHECK(GetPartitionBootRecord(BT_FREEDOS, FS_FAT32, &report) == RUFUS_PBR_FAT32_FREEDOS);
	CHECK(GetPartitionBootRecord(BT_REACTOS, FS_FAT16, &report) == RUFUS_PBR_FAT16_REACTOS);
	CHECK(GetPartitionBootRecord(BT_REACTOS, FS_FAT32, &report) == RUFUS_PBR_FAT32_REACTOS);
	report.has_kolibrios = 0;
	CHECK(GetPartitionBootRecord(BT_IMAGE, FS_FAT16, &report) == RUFUS_PBR_FAT16_STANDARD);
	CHECK(GetPartitionBootRecord(BT_IMAGE, FS_FAT32, &report) == RUFUS_PBR_FAT32_PE);
	report.has_bootmgr = 0; // EFI BOOTMGR alone still selects PE, as on Windows.
	CHECK(GetPartitionBootRecord(BT_IMAGE, FS_FAT32, &report) == RUFUS_PBR_FAT32_PE);
	report.has_bootmgr_efi = 0;
	const uint16_t complete_layouts[] = { WINPE_I386, WINPE_AMD64, WINPE_MININT };
	for (size_t i = 0; i < sizeof(complete_layouts) / sizeof(complete_layouts[0]); i++) {
		report.winpe = complete_layouts[i];
		CHECK(GetPartitionBootRecord(BT_IMAGE, FS_FAT16, &report) == RUFUS_PBR_FAT16_STANDARD);
		CHECK(GetPartitionBootRecord(BT_IMAGE, FS_FAT32, &report) == RUFUS_PBR_FAT32_NT);
	}
	report.winpe = 1; // A partial layout is not HAS_WINPE, even with /minint.
	report.uses_minint = 1;
	CHECK(GetPartitionBootRecord(BT_IMAGE, FS_FAT32, &report) == RUFUS_PBR_FAT32_STANDARD);
	CHECK(GetPartitionBootRecord(BT_IMAGE, FS_NTFS, &report) == RUFUS_PBR_NTFS);
	CHECK(GetPartitionBootRecord(BT_IMAGE, FS_EXT4, &report) == RUFUS_PBR_NONE);
	CHECK(GetPartitionBootRecord(BT_IMAGE, FS_EXFAT, &report) == RUFUS_PBR_UNSUPPORTED);
	// Choosing a record does not gate the operation: Syslinux/UEFI/nonbootable
	// callers use the independent GetBootInstallType decision before writing it.
	CHECK(GetPartitionBootRecord(BT_NON_BOOTABLE, FS_FAT32, NULL) == RUFUS_PBR_FAT32_STANDARD);
	CHECK(GetPartitionBootRecord(BT_SYSLINUX_V6, FS_NTFS, NULL) == RUFUS_PBR_NTFS);
}

int main(void)
{
    const struct { uint64_t size; uint32_t cluster; } esp_defaults[] = {
        {64 * MIB, 1024}, {128 * MIB, 2048}, {256 * MIB, 4096}, {8 * GIB, 8192},
        {16 * GIB, 16384}, {32 * GIB, 32768}, {2 * TIB, 65536}
    };
    for (size_t i = 0; i < sizeof(esp_defaults) / sizeof(esp_defaults[0]); i++) {
        CHECK(GetLargeFat32DefaultClusterSize(esp_defaults[i].size) == esp_defaults[i].cluster);
        CHECK(GetLargeFat32DefaultClusterSize(esp_defaults[i].size - 1) == esp_defaults[i].cluster / 2);
    }
	TestLabels();
	TestClusters();
	TestFat16Clusters();
	TestNtfsClusters();
	TestExfatClusters();
	TestBootFileSystems();
	TestImageFileSystems();
	TestFileSystemPreferences();
	TestWindowsPartitions();
	TestOtherPartitions();
	TestSecondaryBootSelection();
	TestOriginalBootDispatch();
	TestMixedBootSequence();
	TestOriginalPartitionBootSelection();
	TestPartitionBootPriorities();
	printf("%u checks, %u failures\n", checks, failures);
	return failures ? 1 : 0;
}
