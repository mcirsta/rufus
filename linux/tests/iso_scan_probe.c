/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Test driver for the shared tree scan. No mounts, devices or content analysis. */
#include "iso_scan.h"

#include <cdio/logging.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static unsigned int cancel_after, cancel_checks;

int IsIsoScanCancelled(void)
{
	cancel_checks++;
	return cancel_after != 0 && cancel_checks >= cancel_after;
}

void uprintf(const char* format, ...)
{
	va_list args;
	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);
	fputc('\n', stderr);
}

static void LogCdio(cdio_log_level_t level, const char* message)
{
	(void)level;
	uprintf("libcdio: %s", message);
}

static void PrintScan(const ISO_SCAN_STATE* state)
{
	const RUFUS_IMG_REPORT* report = state->report;
	uint32_t i;

	printf("STATUS=tree-complete\nANALYSIS=partial\n");
	printf("FILES=%" PRIu64 "\nBLOCKS=%" PRIu64 "\nEXTRA_BLOCKS=%" PRIu64 "\n",
		state->files, state->total_blocks, state->extra_blocks);
	printf("JOLIET=%u\nLDLINUX=%u\nLONG_NAME=%u\nSYMLINKS=%u\nNEEDS_NTFS=%u\n",
		(unsigned int)state->joliet_level, (unsigned int)state->has_ldlinux_c32,
		(unsigned int)report->has_long_filename, (unsigned int)report->has_symlinks,
		(unsigned int)report->needs_ntfs);
	printf("EFI=%u\nBOOTMGR=%u\nBOOTMGR_EFI=%u\nGRUB=%u\nCASPER=%u\nDISABLE_ISO=%u\n",
		(unsigned int)report->has_efi, (unsigned int)report->has_bootmgr,
		(unsigned int)report->has_bootmgr_efi, (unsigned int)report->has_grub2,
		(unsigned int)report->uses_casper, (unsigned int)report->disable_iso);
	printf("OLD_MENU=%u\nOLD_VESAMENU=%u\nWININST=%u\nWIM0=%s\nREACTOS=%s\n",
		(unsigned int)report->has_old_c32[0], (unsigned int)report->has_old_c32[1],
		(unsigned int)report->wininst_index, report->wininst_path[0], report->reactos_path);
	for (i = 0; i < state->config_path->Index; i++)
		printf("CONFIG=%s\n", state->config_path->String[i]);
	for (i = 0; i < state->isolinux_path->Index; i++)
		printf("ISOLINUX=%s\n", state->isolinux_path->String[i]);
	for (i = 0; i < sizeof(report->efi_boot_entry) / sizeof(report->efi_boot_entry[0]); i++) {
		if (report->efi_boot_entry[i].path[0] != 0)
			printf("BOOT=%u\t%s\n", (unsigned int)report->efi_boot_entry[i].type,
				report->efi_boot_entry[i].path);
	}
}

int main(int argc, char** argv)
{
	RUFUS_IMG_REPORT report = { 0 };
	ISO_SCAN_STATE state = { 0 };
	StrArray configs = { 0 }, isolinux = { 0 };
	struct stat metadata;
	iso9660_t* iso;
	iso_extension_mask_t mask = ISO_EXTENSION_NONE;
	int result;

	if (argc < 3 || argc > 4)
		return 64;
	if (strcmp(argv[2], "rr") == 0) {
		state.enable_rockridge = true;
		mask = ISO_EXTENSION_ALL & ~ISO_EXTENSION_JOLIET;
	} else if (strcmp(argv[2], "joliet") == 0) {
		mask = ISO_EXTENSION_ALL & ~ISO_EXTENSION_ROCK_RIDGE;
	} else if (strcmp(argv[2], "plain") != 0) {
		return 64;
	}
	if (argc == 4)
		cancel_after = (unsigned int)strtoul(argv[3], NULL, 10);
	if (stat(argv[1], &metadata) != 0 || !S_ISREG(metadata.st_mode) || metadata.st_size == 0)
		return ISO_SCAN_ERROR;
	cdio_log_set_handler(LogCdio);
	iso = iso9660_open_ext(argv[1], mask);
	if (iso == NULL)
		return ISO_SCAN_ERROR;
	StrArrayCreate(&configs, 8);
	StrArrayCreate(&isolinux, 8);
	state.report = &report;
	state.config_path = &configs;
	state.isolinux_path = &isolinux;
	state.joliet_level = iso9660_ifs_get_joliet_level(iso);
	result = ScanIsoFiles(iso, "", &state);
	if (result == ISO_SCAN_CANCELLED)
		printf("STATUS=cancelled\n");
	else if (result < 0 || (result == ISO_SCAN_OK && report.has_deep_directories)) {
		printf("STATUS=tree-partial\nDEEP_DIRECTORIES=1\n");
		result = 3;
	} else if (result == ISO_SCAN_OK) {
		PrintScan(&state);
	}
	StrArrayDestroy(&isolinux);
	StrArrayDestroy(&configs);
	iso9660_close(iso);
	return result;
}
