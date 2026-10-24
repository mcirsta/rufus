/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "image_file.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#define FOUR_GB (INT64_C(4) << 30)
#define CHECK(condition) do { checks++; if (!(condition)) { \
	fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #condition); failures++; } } while (0)

static unsigned int checks, failures;

static void TestSyslinuxConfig(void)
{
	static const struct {
		const char* path;
		const char* text;
	} cases[] = {
		{ "/isolinux.cfg", "DEFAULT loadconfig\n\nLABEL loadconfig\n  CONFIG /isolinux.cfg\n" },
		{ "isolinux.cfg", "DEFAULT loadconfig\n\nLABEL loadconfig\n  CONFIG isolinux.cfg\n" },
		{ "/boot/isolinux/isolinux.cfg", "DEFAULT loadconfig\n\nLABEL loadconfig\n"
			"  CONFIG /boot/isolinux/isolinux.cfg\n  APPEND /boot/isolinux/\n" },
		{ "/MiXeD/\xc8\x99 space/isolinux.cfg", "DEFAULT loadconfig\n\nLABEL loadconfig\n"
			"  CONFIG /MiXeD/\xc8\x99 space/isolinux.cfg\n  APPEND /MiXeD/\xc8\x99 space/\n" },
		{ "", "DEFAULT loadconfig\n\nLABEL loadconfig\n  CONFIG \n" },
		{ "/trailing/", "DEFAULT loadconfig\n\nLABEL loadconfig\n  CONFIG /trailing/\n  APPEND /trailing/\n" }
	};
	RUFUS_IMG_REPORT report = { 0 }, original;
	ISO_SYSLINUX_CONFIG config;

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		strcpy(report.cfg_path, cases[i].path);
		for (int state = 0; state < 8; state++) {
			bool exists = (state & 1) != 0;
			report.has_syslinux = (state & 2) != 0;
			report.needs_syslinux_overwrite = (state & 4) != 0;
			bool create = report.has_syslinux && (!exists || report.needs_syslinux_overwrite);
			original = report;
			memset(&config, 0xa5, sizeof(config));
			GetIsoSyslinuxConfig(&report, exists, &config);
			CHECK(config.create == create);
			CHECK(config.backup == (create && exists));
			CHECK(strcmp(config.text, create ? cases[i].text : "") == 0);
			CHECK(memcmp(&report, &original, sizeof(report)) == 0);
		}
	}
	// Longest terminated report path, with the longest possible APPEND directory.
	memset(report.cfg_path, 'x', sizeof(report.cfg_path) - 1);
	report.cfg_path[sizeof(report.cfg_path) - 2] = '/';
	report.cfg_path[sizeof(report.cfg_path) - 1] = 0;
	GetIsoSyslinuxConfig(&report, false, &config);
	CHECK(strlen(config.text) == 311);
	CHECK(strstr(config.text, report.cfg_path) != NULL);
	CHECK(strcmp(config.text + strlen(config.text) - 2, "/\n") == 0);
}

static void TestPathSubstitutions(void)
{
	static const struct {
		const char* source;
		const char* expected;
		size_t protected_prefix;
	} cases[] = {
		{ "", "", 0 },
		{ "D:", "D:", 2 },
		{ "D:/boot/grub.cfg", "D:/boot/grub.cfg", 2 },
		{ "D:/bad:name/*?<>|.txt", "D:/bad_name/_____.txt", 2 },
		{ "?:/dir:one/file?", "?:/dir_one/file_", 2 },
		{ "D:\\dir:one\\file?", "D:\\dir_one\\file_", 2 },
		{ "bad:name.txt", "bad_name.txt", 0 },
		{ "*?<>:|", "______", 0 },
		{ "/one:two/three?four/file|", "/one_two/three_four/file_", 0 },
		{ "/MiXeD/\xc8\x99-\xc3\xa9: name.txt", "/MiXeD/\xc8\x99-\xc3\xa9_ name.txt", 0 },
		// Preserve the exact legacy rule, including names it does not validate.
		{ "../CON/quote\"/trailing. /back\\slash", "../CON/quote\"/trailing. /back\\slash", 0 }
	};
	char path[128];

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		strcpy(path, cases[i].source);
		bool changed = SanitizeImagePath(path + cases[i].protected_prefix);
		bool expected_change = strcmp(cases[i].source, cases[i].expected) != 0;
		CHECK(changed == expected_change);
		CHECK(strcmp(path, cases[i].expected) == 0);
		CHECK(!SanitizeImagePath(path + cases[i].protected_prefix));
	}
	// Every other byte, including UTF-8 continuation bytes, must be preserved.
	for (unsigned int byte = 1; byte <= UCHAR_MAX; byte++) {
		char single[] = { (char)byte, 0 };
		bool replaced = strchr("*?<>:|", (int)byte) != NULL;
		CHECK(SanitizeImagePath(single) == replaced);
		CHECK((unsigned char)single[0] == (replaced ? '_' : byte));
		CHECK(single[1] == 0);
	}
}

enum {
	CFG = 1, CONF = 2, SYSLINUX = 4, GRUB = 8, MENU = 16,
	OLD_MENU = 32, OLD_VESA = 64, CONFIG_PATH = 128, ISOLINUX = 256, LDLINUX = 512, ROOT_LOADER = 1024
};

static void TestFileProperties(void)
{
	static const struct {
		const char* dirname;
		const char* basename;
		int64_t size;
		unsigned int flags;
	} files[] = {
		{ "", "", 0, 0 },
		{ NULL, "readme", 1, 0 },
		{ "/isolinux", "IsOLinux.CfG", 1, CFG | SYSLINUX | CONFIG_PATH },
		{ "/EFI/BOOT", "syslinux.cfg", 1, CFG | SYSLINUX | CONFIG_PATH },
		{ "", "extlinux.conf", 1, CFG | SYSLINUX | CONFIG_PATH },
		{ "", "txt.cfg", 1, CFG | SYSLINUX },
		{ "", "live.cfg", 1, CFG | SYSLINUX },
		{ "", "grub.cfg", 1, CFG | GRUB },
		{ "", "loopback.cfg", 1, CFG | GRUB },
		{ "", "menu.cfg", 1, CFG | MENU },
		{ "", "other.CFG", 1, CFG },
		{ "", ".cfg", 1, CFG },
		{ "", "cfg", 1, 0 },
		{ "", "grub.cfg.backup", 1, 0 },
		{ "/loader/entries", "arch.CONF", 1, CONF },
		{ "/LOADER/ENTRIES", ".conf", 1, CONF },
		{ "/loader/entries", "extlinux.conf", 1, CFG | CONF | SYSLINUX | CONFIG_PATH },
		{ "/loader/entries/sub", "arch.conf", 1, 0 },
		{ "/loader/entries", "arch.conf.old", 1, 0 },
		{ "/loader/entries", "conf", 1, 0 },
		{ "", "menu.c32", 53499, OLD_MENU },
		{ "", "MENU.C32", 53500, OLD_MENU },
		{ "", "menu.c32", 53501, 0 },
		{ "", "vesamenu.c32", 148000, OLD_VESA },
		{ "", "vesamenu.c32", 148001, 0 },
		{ "", "isolinux.bin", 4096, ISOLINUX },
		{ "", "BOOT.BIN", 4096, ISOLINUX },
		{ "", "ldlinux.c32", 4096, LDLINUX },
		{ "", "ldlinux.sys", 4096, ROOT_LOADER },
		{ "", "LDLINUX.SYS", 0, ROOT_LOADER },
		{ "/boot", "ldlinux.sys", 4096, 0 },
		{ NULL, "ldlinux.sys", 4096, 0 },
		{ "", "ldlinux.sys.backup", 4096, 0 },
		{ "", "menu.c32.backup", 1, 0 },
		{ "", "\xc3\xa9.cfg", 1, CFG }
	};
	ISO_FILE_PROPS props;
	unsigned int flags;

	for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
		memset(&props, 0xff, sizeof(props));
		GetIsoFileProps(files[i].dirname, files[i].size, files[i].basename, &props);
		flags = props.is_cfg * CFG | props.is_conf * CONF | props.is_syslinux_cfg * SYSLINUX |
			props.is_grub_cfg * GRUB | props.is_menu_cfg * MENU | props.is_old_c32[0] * OLD_MENU |
			props.is_old_c32[1] * OLD_VESA | props.is_config_path * CONFIG_PATH |
			props.is_isolinux * ISOLINUX | props.is_ldlinux_c32 * LDLINUX |
			props.is_root_ldlinux_sys * ROOT_LOADER;
		if (flags != files[i].flags)
			fprintf(stderr, "Properties for %s: %u, expected %u\n",
				files[i].basename, flags, files[i].flags);
		CHECK(flags == files[i].flags);
	}
}

static void ScanFile(RUFUS_IMG_REPORT* report, const char* dirname, int64_t size, const char* basename)
{
	ISO_FILE_PROPS props;
	char path[512];

	snprintf(path, sizeof(path), "%s/%s", dirname == NULL ? "" : dirname, basename);
	GetIsoFileProps(dirname, size, basename, &props);
	CheckIsoScanFile(report, dirname, size, basename, path, &props);
}

static void TestEfiRecognition(void)
{
	RUFUS_IMG_REPORT report = { 0 };
	char name[32];

	ScanFile(&report, "", 512, "BoOtMgR.EfI");
	CHECK(report.has_bootmgr_efi == 1 && report.has_efi == 1);
	CHECK(report.efi_boot_entry[0].type == EBT_BOOTMGR);
	CHECK(strcmp(report.efi_boot_entry[0].path, "/BoOtMgR.EfI") == 0);
	for (unsigned int type = 0; type < 3; type++) {
		for (unsigned int arch = 0; arch < ARCH_MAX; arch++) {
			memset(&report, 0, sizeof(report));
			snprintf(name, sizeof(name), "%s%s.efi", efi_bootname[type], efi_archname[arch]);
			ScanFile(&report, "/EFI/Fedora", 512, name);
			CHECK(report.efi_boot_entry[0].type == type);
			CHECK(report.efi_boot_entry[0].path[0] == '/');
			CHECK(report.has_efi == (type == EBT_MAIN ? (2u << arch) : 0));
		}
	}
	memset(&report, 0, sizeof(report));
	ScanFile(&report, "/efi", 512, "bootx64.efi");
	CHECK(report.has_efi == 0); // The original directory rule requires /efi/.
	ScanFile(&report, "/images", 4096, "EfI-First.IMG");
	ScanFile(&report, "/images", 4096, "efi-second.img");
	CHECK(strcmp(report.efi_img_path, "/images/EfI-First.IMG") == 0);
	ScanFile(&report, "/efi/boot", 255, "BoOtX64.EfI");
	CHECK(report.has_efi == (0x4000 | (2 << ARCH_X86_64)));
	CHECK(strcmp(report.efi_img_path, "[BOOT]/1-Boot-NoEmul.img") == 0);
	memset(&report, 0, sizeof(report));
	ScanFile(&report, "/efi/boot", 256, "bootx64.efi");
	CHECK(report.has_efi == (2 << ARCH_X86_64) && report.efi_img_path[0] == 0);
	memset(&report, 0, sizeof(report));
	ScanFile(&report, "/[BOOT]", 4096, "0-Boot-HardDisk.img");
	CHECK(strcmp(report.efi_img_path, "/[BOOT]/0-Boot-HardDisk.img") == 0);
	for (unsigned int i = 0; i < 65; i++)
		ScanFile(&report, "/efi/boot", 512, "bootaa64.efi");
	CHECK(strcmp(report.efi_boot_entry[63].path, "/efi/boot/bootaa64.efi") == 0);
	CHECK(report.has_efi == (2 << ARCH_ARM_64));
}

static void TestWindowsRecognition(void)
{
	RUFUS_IMG_REPORT report = { 0 };

	ScanFile(&report, "/x/SOURCES", FOUR_GB - 1, "Install.WIM");
	CHECK(report.wininst_index == 1 && report.has_4GB_file == 0);
	CHECK(strcmp(report.wininst_path[0], "?:/x/SOURCES/Install.WIM") == 0);
	ScanFile(&report, "/sources", FOUR_GB, "install.wim");
	CHECK(report.has_4GB_file == 0x11);
	ScanFile(&report, "/sources", FOUR_GB + 1, "install.esd");
	CHECK(report.has_4GB_file == 0x32);
	ScanFile(&report, "/sources", FOUR_GB, "install.swm");
	CHECK(report.has_4GB_file == 0x73 && report.wininst_index == MAX_WININST);
	for (unsigned int i = 0; i < 20; i++)
		ScanFile(&report, "/sources", FOUR_GB, "install.wim");
	CHECK(report.has_4GB_file == 0x7f && report.wininst_index == MAX_WININST);
	memset(&report, 0, sizeof(report));
	report.wininst_index = MAX_WININST;
	ScanFile(&report, "/sources", FOUR_GB, "install.esd");
	CHECK(report.has_4GB_file == 1); // No WIM-type bit once the path list is full.
	memset(&report, 0, sizeof(report));
	ScanFile(&report, "/resources", 1, "install.wim");
	ScanFile(&report, "sources", 1, "install.wim");
	ScanFile(&report, NULL, 1, "install.wim");
	CHECK(report.wininst_index == 0);
	ScanFile(&report, "/sources/$OEM$/$$/Panther", 1, "UNATTEND.XML");
	CHECK(report.has_panther_unattend == 1);
	ScanFile(&report, "/i386", 1, "ntdetect.com");
	ScanFile(&report, "/i386", 1, "setupldr.bin");
	ScanFile(&report, "/i386", 1, "txtsetup.sif");
	CHECK(report.winpe == 0x0007);
	ScanFile(&report, "/amd64", 1, "ntdetect.com");
	ScanFile(&report, "/amd64", 1, "setupldr.bin");
	ScanFile(&report, "/amd64", 1, "txtsetup.sif");
	CHECK(report.winpe == 0x003f);
	ScanFile(&report, "/minint", 1, "ntdetect.com");
	ScanFile(&report, "/minint", 1, "setupldr.bin");
	ScanFile(&report, "/minint", 1, "txtsetup.sif");
	CHECK(report.winpe == 0x01ff);
}

static void TestOtherMediaRules(void)
{
	RUFUS_IMG_REPORT report = { 0 };
	char long_dir[200];

	report.image_size = 12345;
	report.projected_size = 67890;
	report.sl_version = 0x0604;
	strcpy(report.label, "Keep label");
	ScanFile(&report, "/nested", 1, "bootmgr");
	ScanFile(&report, "/nested", 1, "grldr");
	CHECK(!report.has_bootmgr && !report.has_grub4dos);
	ScanFile(&report, "", 1, "BOOTMGR");
	ScanFile(&report, "", 1, "GRLDR");
	ScanFile(&report, "", 1, "KOLIBRI.IMG");
	CHECK(report.has_bootmgr && report.has_grub4dos && report.has_kolibrios);
	ScanFile(&report, "", 1, "md5sum.txt");
	CHECK(report.has_md5sum == 1);
	ScanFile(&report, "", 1, "MD5SUMS");
	CHECK(report.has_md5sum == 2);
	ScanFile(&report, "/casper-pop-os", 1, "vmlinuz");
	CHECK(report.uses_casper && report.disable_iso);
	report.disable_iso = 0;
	ScanFile(&report, "/casper-POP-OS", 1, "vmlinuz");
	CHECK(report.uses_casper && !report.disable_iso);
	ScanFile(&report, "/proxmox", 1, "kernel");
	CHECK(report.disable_iso);
	report.disable_iso = 0;
	ScanFile(&report, "", 1, ".miso");
	CHECK(report.disable_iso);
	ScanFile(&report, "/BOOT/GRUB/I386-PC", 1, "normal.mod");
	CHECK(report.has_grub2 == 1);
	ScanFile(&report, "/boot/grub2/i386-pc", 1, "normal.mod");
	CHECK(report.has_grub2 == 2);
	ScanFile(&report, "/efi/boot", 1, "syslinux.cfg");
	CHECK(report.has_efi_syslinux);
	ScanFile(&report, "/somewhere", 1, "menu.c32");
	ScanFile(&report, "/somewhere", 148001, "vesamenu.c32");
	CHECK(report.has_old_c32[0] && !report.has_old_c32[1]);
	ScanFile(&report, "/ReactOS", 1, "FreeLdr.SYS");
	ScanFile(&report, "/later", 1, "setupldr.sys");
	CHECK(strcmp(report.reactos_path, "/ReactOS/FreeLdr.SYS") == 0);
	CHECK(report.image_size == 12345 && report.projected_size == 67890);
	CHECK(report.sl_version == 0x0604 && strcmp(report.label, "Keep label") == 0);

	memset(&report, 0, sizeof(report));
	memset(long_dir, 'x', sizeof(long_dir) - 1);
	long_dir[0] = '/';
	long_dir[sizeof(long_dir) - 1] = 0;
	ScanFile(&report, long_dir, 1, "freeldr.sys");
	CHECK(strlen(report.reactos_path) == sizeof(report.reactos_path) - 1);
	CHECK(memcmp(report.reactos_path, long_dir, sizeof(report.reactos_path) - 1) == 0);
}

static void TestWimSignature(void)
{
	uint8_t data[16] = { 'M', 'S', 'W', 'I', 'M', 0, 0, 0 };
	size_t i;

	for (i = 0; i <= sizeof(data); i++)
		CHECK(IsWimImageSignature(data, i) == (i >= 8));
	CHECK(!IsWimImageSignature(NULL, SIZE_MAX));
	for (i = 0; i < 8; i++) {
		data[i] ^= 1;
		CHECK(!IsWimImageSignature(data, sizeof(data)));
		data[i] ^= 1;
	}
	CHECK(!IsWimImageSignature("WLPWM\0\0\0", 8));
	memmove(data + 1, data, 8);
	CHECK(IsWimImageSignature(data + 1, 8));
}

static void TestBootSignature(void)
{
	uint8_t data[513] = { 0 };
	const uint8_t empty[510] = { 0 };

	data[510] = 0x55;
	data[511] = 0xaa;
	for (size_t size = 0; size <= sizeof(data); size++)
		CHECK(IsBootImageSignature(data, size) == (size >= 512));
	CHECK(!IsBootImageSignature(NULL, SIZE_MAX));
	data[510] = 0xaa;
	data[511] = 0x55;
	CHECK(!IsBootImageSignature(data, sizeof(data)));
	data[510] = 0x55;
	CHECK(!IsBootImageSignature(data, sizeof(data)));
	data[511] = 0xaa;
	memmove(data + 1, data, 512);
	CHECK(IsBootImageSignature(data + 1, 512));
	/* Boot code and partition tables can be absent: this remains only a marker. */
	CHECK(memcmp(data + 1, empty, sizeof(empty)) == 0);
}

static void TestEfiSyslinuxConfigName(void)
{
	static const struct { const char* directory; const char* source; const char* output; } cases[] = {
		{ "/EFI/BOOT", "isolinux.cfg", "syslinux.cfg" },
		{ "/efi/Boot", "ISOLINUX.CFG", "sysLINUX.CFG" },
		{ "/EFI/BOOT", "syslinux.cfg", "" },
		{ "/EFI/BOOT", "grub.cfg", "" },
		{ "/EFI/BOOT", "loopback.cfg", "" },
		{ "/EFI/BOOT", "extlinux.conf", "" },
		{ "/EFI/BOOT/nested", "isolinux.cfg", "" },
		{ "/boot", "isolinux.cfg", "" },
		{ NULL, "isolinux.cfg", "" }
	};
	RUFUS_IMG_REPORT report = { 0 };
	ISO_FILE_PROPS props;
	char name[ISO_SYSLINUX_CONFIG_NAME_CAPACITY];
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		GetIsoFileProps(cases[i].directory, 1, cases[i].source, &props);
		// The text-pass ordering relies on these real classifications being disjoint.
		CHECK(!props.is_grub_cfg || !props.is_syslinux_cfg);
		report.has_efi_syslinux = false;
		bool duplicate = GetEfiSyslinuxConfigName(&report, &props, cases[i].directory, cases[i].source, name);
		CHECK(duplicate == (cases[i].output[0] != 0));
		CHECK(strcmp(name, cases[i].output) == 0);
		report.has_efi_syslinux = true;
		CHECK(!GetEfiSyslinuxConfigName(&report, &props, cases[i].directory, cases[i].source, name));
		CHECK(name[0] == 0);
	}
}

int main(void)
{
	TestPathSubstitutions();
	TestSyslinuxConfig();
	TestEfiSyslinuxConfigName();
	TestWimSignature();
	TestBootSignature();
	TestFileProperties();
	TestEfiRecognition();
	TestWindowsRecognition();
	TestOtherMediaRules();
	printf("Image file classification: %u checks, %u failures\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
