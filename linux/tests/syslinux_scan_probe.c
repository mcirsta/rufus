/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "iso_scan.h"
#include "iso_read.h"
#include "syslinux_scan.h"
#include "syslinux_version.h"

#include <cdio/logging.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

void uprintf(const char* format, ...)
{
	va_list args;
	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);
	fputc('\n', stderr);
}

int IsIsoScanCancelled(void)
{
	return 0;
}

static void LogCdio(cdio_log_level_t level, const char* message)
{
	(void)level;
	uprintf("libcdio: %s", message);
}

static bool ScanVersions(const char* image, const ISO_SCAN_STATE* state,
	const uint16_t embedded[2])
{
	RUFUS_IMG_REPORT* report = state->report;
	const char* source = NULL;
	uint8_t* buf = NULL;
	uint32_t size, i;
	uint16_t version;
	char* ext;

	if (!SelectSyslinuxConfig(report, state->config_path))
		return !report->has_syslinux;
	for (i = 0; i < state->isolinux_path->Index; i++) {
		size = ReadISOFileToBufferExt(image, state->isolinux_path->String[i], &buf,
			ISO_EXTENSION_ALL & ~ISO_EXTENSION_JOLIET);
		if (size == 0) {
			uprintf("  Could not access %s", state->isolinux_path->String[i]);
			continue;
		}
		version = GetSyslinuxVersion((char*)buf, (size_t)size + 1, &ext);
		source = SelectSyslinuxCandidate(report, source, state->isolinux_path->String[i], version, ext);
		free(buf);
		buf = NULL;
	}
	SelectSyslinuxVersion(report, source, state->has_ldlinux_c32, embedded);
	return true;
}

static void PrintReport(const RUFUS_IMG_REPORT* report)
{
	printf("PRESENT=%u\nCONFIG=%s\nOVERWRITE=%u\n", (unsigned int)report->has_syslinux,
		report->cfg_path, (unsigned int)report->needs_syslinux_overwrite);
	printf("DETECTED=%u\nDETECTED_EXT=%s\n", (unsigned int)report->sl_detected_version,
		report->sl_detected_version_ext);
	printf("SELECTED=%u\nSELECTED_TEXT=%s\nSELECTED_EXT=%s\nINFERRED=%u\n",
		(unsigned int)report->sl_version, report->sl_version_str,
		report->sl_version_ext, (unsigned int)report->sl_version_inferred);
}

int main(int argc, char** argv)
{
	RUFUS_IMG_REPORT report = { 0 };
	ISO_SCAN_STATE state = { 0 };
	StrArray configs = { 0 }, loaders = { 0 };
	struct stat metadata;
	iso9660_t* iso;
	uint16_t embedded[2];
	unsigned long value;
	char* end;
	int result = 1;

	if (argc != 4 || stat(argv[1], &metadata) != 0 || !S_ISREG(metadata.st_mode))
		return 1;
	for (int i = 0; i < 2; i++) {
		value = strtoul(argv[i + 2], &end, 0);
		if (*end != 0 || end == argv[i + 2] || value > UINT16_MAX)
			return 64;
		embedded[i] = (uint16_t)value;
	}
	cdio_log_set_handler(LogCdio);
	iso = iso9660_open_ext(argv[1], ISO_EXTENSION_ALL & ~ISO_EXTENSION_JOLIET);
	if (iso == NULL)
		return 1;
	StrArrayCreate(&configs, 8);
	StrArrayCreate(&loaders, 8);
	state.report = &report;
	state.config_path = &configs;
	state.isolinux_path = &loaders;
	state.enable_rockridge = true;
	state.joliet_level = iso9660_ifs_get_joliet_level(iso);
	if (configs.String == NULL || loaders.String == NULL)
		goto out;
	if (ScanIsoFiles(iso, "", &state) != 0 || report.has_deep_directories)
		goto out;
	if (!ScanVersions(argv[1], &state, embedded))
		goto out;
	PrintReport(&report);
	result = 0;
out:
	StrArrayDestroy(&configs);
	StrArrayDestroy(&loaders);
	iso9660_close(iso);
	return result;
}
