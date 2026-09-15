/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "image_report.h"
#include <stdbool.h>
#include <stddef.h>

typedef struct {
	uint8_t is_cfg;
	uint8_t is_conf;
	uint8_t is_syslinux_cfg;
	uint8_t is_grub_cfg;
	uint8_t is_menu_cfg;
	uint8_t is_old_c32[NB_OLD_C32];
	uint8_t is_config_path;
	uint8_t is_isolinux;
	uint8_t is_ldlinux_c32;
	uint8_t is_root_ldlinux_sys;
} ISO_FILE_PROPS;

#ifdef __cplusplus
extern "C" {
#endif

/* Shared names also used by extraction and bootloader inspection. */
extern const char* bootmgr_efi_name;
extern const char* md5sum_name[2];
extern const char* efi_dirname;
extern const char* efi_bootname[3];
extern const char* efi_archname[ARCH_MAX];
extern const char* sources_str;
extern const char* wininst_name[3];
extern const char* grub_dirname[2];
extern const char* syslinux_cfg[5];
extern const char* old_c32_name[NB_OLD_C32];

/* Rufus's standalone Windows-image signature, independent of the filename.
 * Requires all eight bytes. This identifies a candidate, not a valid archive,
 * Windows installation or bootable image. wimlib's pipable signature differs. */
bool IsWimImageSignature(const void* data, size_t size);

/* The marker used by ms-sys is_br / Rufus AnalyzeMBR. A complete first sector
 * is required; this does not validate partitions, boot code or ISO-mode support. */
bool IsBootImageSignature(const void* data, size_t size);

/* Classify a file, with no I/O or report mutation. All flags are populated for
 * both scanning and extraction. dirname is "" for the root; NULL is accepted
 * for callers that do not have a directory. basename is non-NULL and terminated. */
void GetIsoFileProps(const char* dirname, int64_t file_length, const char* basename,
	ISO_FILE_PROPS* props);

/* Accumulate per-file scan findings into an initialized report. fullpath is a
 * non-NULL, terminated image path with its original case. Pass the properties
 * of this same file. Directory traversal, path lists, size accounting and
 * scan finalization belong to the caller; this is not a complete image scan. */
void CheckIsoScanFile(RUFUS_IMG_REPORT* report, const char* dirname, int64_t file_length,
	const char* basename, const char* fullpath, const ISO_FILE_PROPS* props);

#ifdef __cplusplus
}
#endif
