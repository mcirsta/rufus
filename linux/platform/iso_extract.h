/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "iso_analyze.h"

/* Optional creation-job checksum collector. Called only during actual copying,
 * after preflight. Source MD5 is computed in the existing transfer loop; a
 * changed-file notification follows a successful Rufus config edit. Paths are
 * the sanitized output paths, not source lookup paths. Return errno to stop the
 * incomplete job. The caller owns final checksum publication and EFI wrapping. */
typedef struct {
	void* context;
	bool hash_md5;
	int (*begin)(void* context, int directory, unsigned manifest_kind);
	int (*copied)(void* context, const char* path, uint64_t size, const uint8_t* md5);
	int (*modified)(void* context, const char* path);
	int (*split_part)(void* context, const char* original, int stage, const char* name);
	int (*relocated)(void* context, const char* original, const char* destination);
	int (*linked)(void* context, const char* path, const char* target);
} ISO_EXTRACT_EVENTS;

typedef struct { const uint8_t* old_c32[2]; size_t old_c32_bytes[2]; } ISO_BOOT_RESOURCES;
typedef struct {
	const uint8_t* mboot;
	size_t mboot_bytes;
	const uint8_t* config;
	size_t config_bytes;
} ISO_PREPARED_REACTOS;

typedef struct {
	ISO_ANALYSIS_OPTIONS analysis;
	/* NULL label selects copy-only component use. Creation jobs supply the
	 * actual formatted label and original image path for Rufus's config rules. */
	const char* usb_label;
	const char* image_path;
	/* Explicit component operation, including small inputs. Creation must select
	 * this only for Rufus's large-WIM FAT32 case after its eligibility checks. */
	bool split_install_wim;
	uint64_t persistence_size; // Enables the original eligible Linux config passes.
	const ISO_EXTRACT_EVENTS* events;
	bool preserve_timestamps;
	bool ntfs_compression; // Effective NTFS compression; preserve Windows boot exceptions.
	bool windows_links; // NTFS Windows-compatible relative reparse links, resolved inside the ISO.
	const ISO_BOOT_RESOURCES* boot_resources;
	/* Original ReactOS Syslinux setup precedes extraction. The otherwise empty
	 * directory may contain exactly these two ordinary, byte-checked root files.
	 * Borrowed expected bytes are the helper's original embedded/generated set. */
	const ISO_PREPARED_REACTOS* prepared_reactos;
} ISO_EXTRACT_OPTIONS;

typedef struct {
	/* Processed source entries/bytes, including writes later replaced by a
	 * colliding entry. These are not the final output tree's unique size/count. */
	uint64_t files;
	uint64_t directories;
	uint64_t skipped;
	uint64_t bytes_written;
	uint64_t required_bytes; /* Planned source bytes plus the WIM split reservation. */
	uint64_t patched_files; /* Source configs changed, separate from copied byte totals. */
	uint64_t generated_bytes; /* Root and duplicated EFI config writes, separate from source totals. */
	uint64_t split_bytes; /* SWM output; bytes_written counts the processed source WIM. */
	unsigned split_parts;
	char path[ISO_SCAN_PATH_MAX]; /* Current/failed image entry; empty on success. */
} ISO_EXTRACT_RESULT;

#ifdef __cplusplus
extern "C" {
#endif

/* Run the extractor's source/tree checks without an output directory or writes.
 * max_file_bytes is the target filesystem's file-size limit (zero: no limit).
 * On success, files/bytes_written report the planned copy totals. Extraction
 * checks config text/size/expansion when a USB label is supplied, and
 * repeats preflight; retain and revalidate the source across subsequent steps.
 * Splitting requires exactly one conventional sources/install.wim and no source
 * install*.swm names there. Native WIM resources are checked read-only; final
 * part overhead/size is checked while writing. required_bytes includes a
 * conservative split reservation, not exact filesystem/customization overhead. */
int CheckIsoTree(const char* image, const ISO_EXTRACT_OPTIONS* options,
	uint64_t max_file_bytes, ISO_EXTRACT_RESULT* result);

/* Extract ordinary files/directories into a caller-owned, private, empty
 * directory. Does not mount, format or select devices.
 * Uses Rufus's UDF-first/Rock Ridge/Joliet selection and shared tree/readers.
 * Preflight plans source-only links/literal nonregular data and rejects unsafe paths and deep/incomplete trees.
 * Root ldlinux.sys is reserved for the native installer, as on Windows.
 * Uses Rufus's shared filename substitutions for output only; source paths and
 * result.path keep their original names. As with Rufus's CREATE_ALWAYS, later
 * files replace earlier files at the same output path; directories are reused.
 * Target filesystem rules decide aliases (e.g. FAT case folding). Reject links,
 * multiply linked files and file/directory conflicts; other naming rules can fail.
 * Classified .cfg/.conf files receive the shared label and Red Hat passes,
 * followed by FreeNAS volume-reference patches in GRUB configs,
 * after each file copy. EFI isolinux.cfg is copied after patching when the shared
 * rule requires it, keeping an existing regular syslinux.cfg. Config edits accept UTF-8/UTF-16LE and use bounded
 * whole-file memory (8 MiB input/output); source totals exclude rewrite bytes.
 * After copying, apply Rufus's shared root syslinux.cfg generation rules. An
 * overridden config is backed up as syslinux.org; a failed backup is an error.
 *
 * Returns 0 or errno (ECANCELED/ESTALE included). Keeps the source descriptor
 * and checks its identity before/during/after copying. Writes stay relative to
 * the retained target directory with no symlink traversal. Files/directories
 * are flushed. A copy failure removes the current output once created/truncated;
 * a patch failure preserves that completed copy. There is no rollback of an
 * overwritten file. Other completed entries remain. On
 * error, treat the whole output as incomplete and use a fresh target.
 * Fixed transfer buffer for ordinary file data; no complete-image or all-files
 * manifest buffer.
 * Reports OP_FILE_COPY through Rufus's existing progress adapter, in 2048-byte
 * ISO/UDF blocks with per-file tails rounded up. The shared scan denominator
 * includes skipped entries, matching Win32. Preflight emits no copy progress.
 *
 * This is file extraction, not a bootable-media operation. Optional timestamps,
 * Windows reparse links, original FAT link rules, embedded EFI and old COM32
 * replacements are part of copying; native boot installation remains separate. No installer
 * compatibility or boot claim is implied. Explicit WIM splitting uses upstream
 * wimlib, private staging, no-replace publication and actual FAT32 size checks.
 * It enforces FAT32's limit on other files too. Initialize wimlib before use.
 * Completed prior source entries remain on split failure. */
int ExtractIsoTree(const char* image, const char* directory,
	const ISO_EXTRACT_OPTIONS* options, ISO_EXTRACT_RESULT* result);

#ifdef __cplusplus
}
#endif
