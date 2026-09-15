/*
 * Rufus: The Reliable USB Formatting Utility
 * ISO scan finalization extracted from iso.c, with a read-only coordinator
 * Copyright © 2011-2026 Pete Batard <pete@akeo.ie>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "iso_analyze.h"
#include "iso_bootloaders.h"
#include "embedded_efi.h"
#include "winpe_scan.h"
#include <cdio/udf.h>

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

extern void uprintf(const char* format, ...);

bool FinalizeIsoScan(RUFUS_IMG_REPORT* report, uint64_t total_blocks,
	int64_t image_size, int64_t volume_blocks)
{
	size_t length = strlen(report->label);
	double projected = (double)total_blocks * ISO_BLOCKSIZE * 1.01f;

	// Preserve the original first-character and float-constant conventions.
	while (length > 1 && isspace((unsigned char)report->label[length - 1]))
		report->label[--length] = 0;
	report->projected_size = 0;
	if (projected >= (double)UINT64_MAX || volume_blocks > INT64_MAX / ISO_BLOCKSIZE)
		return false;
	report->projected_size = (uint64_t)projected;
	if (image_size >= 0 && volume_blocks >= 0)
		report->mismatch_size = volume_blocks * ISO_BLOCKSIZE - image_size;
	return true;
}

static uint32_t PendingIsoChecks(const RUFUS_IMG_REPORT* report)
{
	uint32_t pending = 0;
	size_t i;

	if (HAS_EFI_IMG((*report)))
		pending |= ISO_PENDING_EMBEDDED_EFI;
	if (HAS_WINDOWS((*report)) || HAS_WININST((*report)))
		pending |= ISO_PENDING_WINDOWS;
	if (IS_EFI_BOOTABLE((*report)))
		pending |= ISO_PENDING_SECURE_BOOT;
	for (i = 0; i < sizeof(report->efi_boot_entry) / sizeof(report->efi_boot_entry[0]); i++) {
		if (report->efi_boot_entry[i].path[0])
			pending |= ISO_PENDING_SECURE_BOOT;
	}
	return pending;
}

int AnalyzeIsoImageWithSource(const char* image, uint64_t image_size,
	const ISO_ANALYSIS_OPTIONS* options, ISO_ANALYSIS* analysis, ISO_ANALYSIS_SOURCE* source)
{
	iso9660_t* iso = NULL;
	udf_t* udf = NULL;
	udf_dirent_t* root = NULL;
	iso9660_pvd_t pvd;
	iso_extension_mask_t mask = ISO_EXTENSION_ALL;
	StrArray configs = { 0 }, loaders = { 0 };
	ISO_SCAN_STATE state = { 0 };
	char* label = NULL;
	int64_t volume_blocks = -1;
	int result = ISO_SCAN_ERROR;
	bool found;

	if (source != NULL)
		memset(source, 0, sizeof(*source));
	if (analysis == NULL)
		return ISO_SCAN_ERROR;
	memset(analysis, 0, sizeof(*analysis));
	if (source == NULL || image == NULL || options == NULL || image_size == 0 || image_size > INT64_MAX)
		return ISO_SCAN_ERROR;
	if (IsIsoScanCancelled())
		return ISO_SCAN_CANCELLED;
	udf = udf_open(image);
	if (IsIsoScanCancelled()) {
		result = ISO_SCAN_CANCELLED;
		goto out;
	}
	// Rufus's initial scan prefers Rock Ridge over Joliet.
	if (!options->enable_joliet || options->enable_rockridge)
		mask &= ~ISO_EXTENSION_JOLIET;
	if (!options->enable_rockridge)
		mask &= ~ISO_EXTENSION_ROCK_RIDGE;
	analysis->report.image_size = image_size;
	StrArrayCreate(&configs, 8);
	StrArrayCreate(&loaders, 8);
	if (configs.String == NULL || loaders.String == NULL)
		goto out;
	state.report = &analysis->report;
	state.config_path = &configs;
	state.isolinux_path = &loaders;
	state.enable_rockridge = options->enable_rockridge;
	if (udf != NULL) {
		analysis->filesystem = ISO_FILESYSTEM_UDF;
		uprintf("Image is a UDF image");
		root = udf_get_root(udf, true, 0);
		if (root == NULL) {
			uprintf("Could not locate UDF root directory");
			goto out;
		}
		if (udf_get_logical_volume_id(udf, analysis->report.label,
			sizeof(analysis->report.label)) <= 0)
			analysis->report.label[0] = 0;
		// Rufus also opens the companion ISO view solely for PVD size checks.
		iso = iso9660_open_ext(image, ISO_EXTENSION_NONE);
		result = ScanUdfFiles(root, "", &state);
		root = NULL; // The scanner consumes the directory iterator.
	} else {
		analysis->filesystem = ISO_FILESYSTEM_ISO9660;
		iso = iso9660_open_ext(image, mask);
		if (iso == NULL || !iso9660_ifs_get_volume_id(iso, &label) || label == NULL)
			goto out;
		strncpy(analysis->report.label, label, sizeof(analysis->report.label) - 1);
		state.joliet_level = iso9660_ifs_get_joliet_level(iso);
		result = ScanIsoFiles(iso, "", &state);
	}
	if (result == ISO_SCAN_ERROR || result == ISO_SCAN_CANCELLED)
		goto out;
	analysis->total_blocks = state.total_blocks;
	analysis->extra_blocks = state.extra_blocks;
	analysis->files = state.files;
	analysis->pending = PendingIsoChecks(&analysis->report);
	if (result < 0 || analysis->report.has_deep_directories) {
		analysis->pending |= ISO_PENDING_DEEP_DIRECTORIES;
		result = ISO_SCAN_PARTIAL;
		goto out;
	}
	analysis->tree_complete = true;
	analysis->report.is_iso = true;
	if (iso != NULL && iso9660_ifs_read_pvd(iso, &pvd)) {
		volume_blocks = iso9660_get_pvd_space_size(&pvd);
		analysis->volume_size_known = true;
	} else if (udf == NULL) {
		result = ISO_SCAN_ERROR;
		goto out;
	}
	if (!FinalizeIsoScan(&analysis->report, state.total_blocks, (int64_t)image_size,
		volume_blocks)) {
		result = ISO_SCAN_ERROR;
		goto out;
	}
	if (analysis->report.mismatch_size > 0)
		analysis->pending |= ISO_PENDING_TRUNCATED;
	result = ScanSyslinuxBootloaders(image, mask, &state, options->embedded_sl_version);
	if (result == ISO_SCAN_ERROR || result == ISO_SCAN_CANCELLED)
		goto out;
	if (result == ISO_SCAN_PARTIAL)
		analysis->pending |= ISO_PENDING_CONTENT;
	if (HAS_EFI_IMG(analysis->report) && !IS_EFI_BOOTABLE(analysis->report)) {
		result = InspectEfiImage(udf == NULL ? iso : NULL, udf, analysis->report.efi_img_path, &found);
		if (result == ISO_SCAN_ERROR || result == ISO_SCAN_CANCELLED)
			goto out;
		if (result == ISO_SCAN_OK) {
			analysis->pending &= ~ISO_PENDING_EMBEDDED_EFI;
			if (found) {
				analysis->report.has_efi = 0x8000; // Original embedded-only marker.
				analysis->pending |= ISO_PENDING_SECURE_BOOT;
			}
		}
	}
	result = ScanWinPeOptions(image, mask, &analysis->report);
	if (result == ISO_SCAN_ERROR || result == ISO_SCAN_CANCELLED)
		goto out;
	if (analysis->report.winpe_options_known && !HAS_BOOTMGR(analysis->report) &&
		!HAS_WININST(analysis->report))
		analysis->pending &= ~ISO_PENDING_WINDOWS;
	result = ScanGrubAndEfiBootloaders(image, mask, &analysis->report);
	if (result == ISO_SCAN_ERROR || result == ISO_SCAN_CANCELLED)
		goto out;
	if (result == ISO_SCAN_PARTIAL)
		analysis->pending |= ISO_PENDING_CONTENT;
	result = analysis->pending ? ISO_SCAN_PARTIAL : ISO_SCAN_OK;
out:
	udf_dirent_free(root);
	free(label);
	StrArrayDestroy(&configs);
	StrArrayDestroy(&loaders);
	if (IsIsoScanCancelled())
		result = ISO_SCAN_CANCELLED;
	if (result == ISO_SCAN_OK || result == ISO_SCAN_PARTIAL) {
		if (udf != NULL) {
			source->udf = udf;
			udf = NULL;
		} else {
			source->iso = iso;
			iso = NULL;
		}
	}
	if (iso != NULL)
		iso9660_close(iso);
	if (udf != NULL)
		udf_close(udf);
	if (result == ISO_SCAN_ERROR || result == ISO_SCAN_CANCELLED)
		memset(analysis, 0, sizeof(*analysis));
	return result;
}

void CloseIsoAnalysisSource(ISO_ANALYSIS_SOURCE* source)
{
	if (source == NULL)
		return;
	if (source->iso != NULL)
		iso9660_close(source->iso);
	if (source->udf != NULL)
		udf_close(source->udf);
	memset(source, 0, sizeof(*source));
}

int AnalyzeIsoImage(const char* image, uint64_t image_size,
	const ISO_ANALYSIS_OPTIONS* options, ISO_ANALYSIS* analysis)
{
	ISO_ANALYSIS_SOURCE source = { 0 };
	int result = AnalyzeIsoImageWithSource(image, image_size, options, analysis, &source);

	CloseIsoAnalysisSource(&source);
	if (IsIsoScanCancelled()) {
		if (analysis != NULL)
			memset(analysis, 0, sizeof(*analysis));
		return ISO_SCAN_CANCELLED;
	}
	return result;
}
