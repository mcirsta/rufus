/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Fixture driver: classify explicitly listed entries, not a production walker. */
#include "image_file.h"
#include <cdio/iso9660.h>

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int ClassifyEntry(iso9660_t* iso, const char* path, RUFUS_IMG_REPORT* report)
{
	iso9660_stat_t* entry;
	ISO_FILE_PROPS props;
	char dirname[512], fullpath[1024];
	const char* slash = strrchr(path, '/');
	size_t length;
	unsigned int flags;

	if (slash == NULL || path[0] != '/' || (size_t)(slash - path) >= sizeof(dirname))
		return 2;
	entry = iso9660_ifs_stat_translate(iso, path);
	if (entry == NULL)
		return 3;
	if (entry->type != _STAT_FILE || entry->total_size > INT64_MAX) {
		iso9660_stat_free(entry);
		return 4;
	}
	length = (size_t)(slash - path);
	memcpy(dirname, path, length);
	dirname[length] = 0;
	// The fixture uses Rock Ridge and known parent paths; read the actual file name.
	if (snprintf(fullpath, sizeof(fullpath), "%s/%s", dirname, entry->filename) >= (int)sizeof(fullpath)) {
		iso9660_stat_free(entry);
		return 2;
	}
	GetIsoFileProps(dirname, (int64_t)entry->total_size, entry->filename, &props);
	CheckIsoScanFile(report, dirname, (int64_t)entry->total_size, entry->filename, fullpath, &props);
	flags = props.is_config_path | props.is_isolinux << 1 | props.is_ldlinux_c32 << 2 |
		props.is_cfg << 3 | props.is_conf << 4 | props.is_old_c32[0] << 5 | props.is_old_c32[1] << 6;
	printf("FILE=%s\t%" PRIu64 "\t%u\n", fullpath, entry->total_size, flags);
	if (entry->rr.psz_symlink != NULL)
		printf("LINK=%s\n", entry->rr.psz_symlink);
	iso9660_stat_free(entry);
	return 0;
}

int main(int argc, char** argv)
{
	struct stat status;
	RUFUS_IMG_REPORT report = { 0 };
	iso9660_t* iso;
	int result = 0;

	if (argc < 3 || stat(argv[1], &status) != 0 || !S_ISREG(status.st_mode))
		return 2;
	iso = iso9660_open_ext(argv[1], ISO_EXTENSION_ROCK_RIDGE);
	if (iso == NULL)
		return 1;
	for (int i = 2; i < argc && result == 0; i++)
		result = ClassifyEntry(iso, argv[i], &report);
	iso9660_close(iso);
	if (result != 0)
		return result;
	printf("EFI=%u\nBOOTMGR=%u\nBOOTMGR_EFI=%u\nGRUB=%u\nCASPER=%u\nDISABLE_ISO=%u\n",
		(unsigned int)report.has_efi, (unsigned int)report.has_bootmgr,
		(unsigned int)report.has_bootmgr_efi, (unsigned int)report.has_grub2,
		(unsigned int)report.uses_casper, (unsigned int)report.disable_iso);
	printf("WININST=%u\nWIM0=%s\nREACTOS=%s\nEFI_IMG=%s\n",
		(unsigned int)report.wininst_index, report.wininst_path[0],
		report.reactos_path, report.efi_img_path);
	for (size_t i = 0; i < sizeof(report.efi_boot_entry) / sizeof(report.efi_boot_entry[0]); i++) {
		if (report.efi_boot_entry[i].path[0] != 0)
			printf("BOOT=%u\t%s\n", (unsigned int)report.efi_boot_entry[i].type,
				report.efi_boot_entry[i].path);
	}
	return 0;
}
