/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "iso_analyze.h"
#include "iso_read.h"
#include "winpe_scan.h"
#include "strarray.h"
#include <cdio/udf.h>
#include <cdio/logging.h>

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static unsigned int cancel_after, queries, reads, cancel_read, fail_read;
static unsigned int array_creates, fail_array;
static unsigned int dir_reads, fail_dir, dir_opens, fail_open_dir;
static bool fail_filesystem_name;

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
	queries++;
	bool query_limit_reached = cancel_after != 0 && queries >= cancel_after;
	bool read_limit_reached = cancel_read != 0 && reads >= cancel_read;
	return query_limit_reached || read_limit_reached;
}

uint32_t __real_ReadISOFileToBufferExt(const char*, const char*, uint8_t**, iso_extension_mask_t);

uint32_t __wrap_ReadISOFileToBufferExt(const char* image, const char* path, uint8_t** buf,
	iso_extension_mask_t mask)
{
	reads++;
	if (fail_read == reads) {
		*buf = NULL;
		return 0;
	}
	return __real_ReadISOFileToBufferExt(image, path, buf, mask);
}

uint32_t __real_ReadISOFileToBufferLimited(const char*, const char*, uint8_t**, iso_extension_mask_t, uint32_t);

uint32_t __wrap_ReadISOFileToBufferLimited(const char* image, const char* path, uint8_t** buf,
	iso_extension_mask_t mask, uint32_t maximum)
{
	if (maximum == WINPE_OPTIONS_MAX_SIZE) {
		reads++;
		if (reads == fail_read) {
			*buf = NULL;
			return 0;
		}
	}
	return __real_ReadISOFileToBufferLimited(image, path, buf, mask, maximum);
}

void __real_StrArrayCreate(StrArray*, uint32_t);

void __wrap_StrArrayCreate(StrArray* array, uint32_t size)
{
	array_creates++;
	if (fail_array == array_creates)
		memset(array, 0, sizeof(*array));
	else
		__real_StrArrayCreate(array, size);
}

char* __real_strdup(const char*);

char* __wrap_strdup(const char* value)
{
	if (fail_filesystem_name && strcmp(value, "fat") == 0)
		return NULL;
	return __real_strdup(value);
}

udf_dirent_t* __real_udf_readdir(udf_dirent_t*);
udf_dirent_t* __real_udf_opendir(const udf_dirent_t*);

udf_dirent_t* __wrap_udf_readdir(udf_dirent_t* entry)
{
	if (++dir_reads == fail_dir) {
		udf_dirent_free(entry);
		return NULL;
	}
	return __real_udf_readdir(entry);
}

udf_dirent_t* __wrap_udf_opendir(const udf_dirent_t* entry)
{
	if (++dir_opens == fail_open_dir)
		return NULL;
	return __real_udf_opendir(entry);
}

static void LogCdio(cdio_log_level_t level, const char* message)
{
	(void)level;
	uprintf("libcdio: %s", message);
}

int main(int argc, char** argv)
{
	ISO_ANALYSIS analysis, empty = { 0 };
	ISO_ANALYSIS_OPTIONS options = { true, true, { 0x0407, 0x0604 } };
	struct stat metadata;
	int result;

	if (argc != 4 || stat(argv[1], &metadata) != 0 || !S_ISREG(metadata.st_mode))
		return 64;
	if (strcmp(argv[2], "joliet") == 0)
		options.enable_rockridge = false;
	else if (strcmp(argv[2], "plain") == 0)
		options.enable_rockridge = options.enable_joliet = false;
	else if (strcmp(argv[2], "no-fallback") == 0)
		memset(options.embedded_sl_version, 0, sizeof(options.embedded_sl_version));
	else if (strcmp(argv[2], "rr") != 0)
		return 64;
	bool cancellation_fault = sscanf(argv[3], "cancel:%u", &cancel_after) == 1 ||
		sscanf(argv[3], "cancel-read:%u", &cancel_read) == 1;
	bool read_fault = false, setup_fault = false;
	if (!cancellation_fault)
		read_fault = sscanf(argv[3], "fail-read:%u", &fail_read) == 1 ||
			sscanf(argv[3], "fail-dir:%u", &fail_dir) == 1;
	if (!cancellation_fault && !read_fault)
		setup_fault = sscanf(argv[3], "fail-open-dir:%u", &fail_open_dir) == 1 ||
			sscanf(argv[3], "fail-array:%u", &fail_array) == 1;
	if (!cancellation_fault && !read_fault && !setup_fault) {
		if (strcmp(argv[3], "fail-fs") == 0)
			fail_filesystem_name = true;
		else if (strcmp(argv[3], "none") != 0)
			return 64;
	}
	cdio_log_set_handler(LogCdio);
	memset(&analysis, 0xa5, sizeof(analysis));
	result = AnalyzeIsoImage(argv[1], (uint64_t)metadata.st_size, &options, &analysis);
	printf("STATUS=%d\nPENDING=%u\nTREE=%u\nREADS=%u\n", result, analysis.pending,
		(unsigned int)analysis.tree_complete, reads);
	if (result == ISO_SCAN_ERROR || result == ISO_SCAN_CANCELLED) {
		if (memcmp(&analysis, &empty, sizeof(empty)) != 0)
			return 65;
		printf("CLEARED=1\n");
		return result;
	}
	printf("LABEL=%s\nSIZE=%" PRIu64 "\nPROJECTED=%" PRIu64 "\nMISMATCH=%" PRId64 "\n",
		analysis.report.label, analysis.report.image_size, analysis.report.projected_size,
		analysis.report.mismatch_size);
	printf("FILESYSTEM=%u\nVOLUME_SIZE_KNOWN=%u\nSYMLINKS=%u\n",
		(unsigned int)analysis.filesystem, (unsigned int)analysis.volume_size_known,
		(unsigned int)analysis.report.has_symlinks);
	printf("FILES=%" PRIu64 "\nBLOCKS=%" PRIu64 "\nEXTRA=%" PRIu64 "\nISO=%u\n",
		analysis.files, analysis.total_blocks, analysis.extra_blocks, (unsigned int)analysis.report.is_iso);
	printf("CONFIG=%s\nPRESENT=%u\nDETECTED=%u\nDETECTED_EXT=%s\nSELECTED=%u\nINFERRED=%u\n",
		analysis.report.cfg_path, (unsigned int)analysis.report.has_syslinux,
		(unsigned int)analysis.report.sl_detected_version, analysis.report.sl_detected_version_ext,
		(unsigned int)analysis.report.sl_version, (unsigned int)analysis.report.sl_version_inferred);
	printf("GRUB=%u\nGRUB_VERSION=%s\nGRUB_FS=%u\nEFI=%u\n",
		(unsigned int)analysis.report.has_grub2, analysis.report.grub2_version,
		(unsigned int)analysis.report.has_grub2_fs, (unsigned int)analysis.report.has_efi);
	printf("WINPE=%u\nMININT=%u\nWINPE_KNOWN=%u\n", (unsigned int)analysis.report.winpe,
		(unsigned int)analysis.report.uses_minint, (unsigned int)analysis.report.winpe_options_known);
	return result;
}
