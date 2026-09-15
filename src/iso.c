/*
 * Rufus: The Reliable USB Formatting Utility
 * ISO file extraction
 * Copyright © 2011-2026 Pete Batard <pete@akeo.ie>
 * Based on libcdio's iso & udf samples:
 * Copyright © 2003-2014 Rocky Bernstein <rocky@gnu.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/* Memory leaks detection - define _CRTDBG_MAP_ALLOC as preprocessor macro */
#ifdef _CRTDBG_MAP_ALLOC
#include <stdlib.h>
#include <crtdbg.h>
#endif

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <malloc.h>
#include <errno.h>
#include <direct.h>
#include <ctype.h>
#include <assert.h>
#include <virtdisk.h>
#include <sys/stat.h>

#define DO_NOT_WANT_COMPATIBILITY
#include <cdio/cdio.h>
#include <cdio/logging.h>
#include <cdio/iso9660.h>
#include <cdio/udf.h>

#include "rufus.h"
#include "image_file.h"
#include "iso_scan.h"
#include "iso_read.h"
#include "iso_bootloaders.h"
#include "iso_analyze.h"
#include "embedded_efi.h"
#include "winpe_scan.h"
#include "ui.h"
#include "vhd.h"
#include "drive.h"
#include "libfat.h"
#include "missing.h"
#include "resource.h"
#include "msapi_utf8.h"
#include "localization.h"
#include "bled/bled.h"

// How often should we update the progress bar, as updating the
// progress bar too frequently will bring extraction to a crawl
_Static_assert(256 * KB >= ISO_BLOCKSIZE, "Can't set PROGRESS_THRESHOLD");
#define PROGRESS_THRESHOLD        ((256 * KB) / ISO_BLOCKSIZE)

// Needed for UDF symbolic link testing
#define S_IFLNK                   0xA000
#define S_ISLNK(m)                (((m) & S_IFMT) == S_IFLNK)

// Set the iso_open_ext() extension mask according to our global options
#define ISO_EXTENSION_MASK        (ISO_EXTENSION_ALL & (enable_joliet ? ISO_EXTENSION_ALL : ~ISO_EXTENSION_JOLIET) & \
                                  (enable_rockridge ? ISO_EXTENSION_ALL : ~ISO_EXTENSION_ROCK_RIDGE))

// Is an MBR partition type for a FAT12/FAT16/FAT32 partition?
#define IS_FAT_TYPE(x)            ((x) == 0x01 || (x) == 0x04 || (x) == 0x06 || (x) == 0x0b || (x) == 0x0c || (x) == 0x0e)

// Needed for UDF ISO access
CdIo_t* cdio_open (const char* psz_source, driver_id_t driver_id) {return NULL;}
void cdio_destroy (CdIo_t* p_cdio) {}

RUFUS_IMG_REPORT img_report;
FILE* fd_md5sum = NULL;
int64_t iso_blocking_status = -1;
uint64_t total_blocks, extra_blocks, nb_blocks, last_nb_blocks;

extern uint64_t md5sum_totalbytes;
extern BOOL preserve_timestamps, enable_ntfs_compression, validate_md5sum;
extern HANDLE format_thread;
extern StrArray modified_files;
BOOL enable_iso = TRUE, enable_joliet = TRUE, enable_rockridge = TRUE, has_ldlinux_c32;
#define ISO_BLOCKING(x) do {x; iso_blocking_status++; } while(0)
static const char* psz_extract_dir;
static const char* ldlinux_name = "ldlinux.sys";
_STATIC_ASSERT(ARRAYSIZE(wininst_name) < 4);	// Must fit as 4 bit position flag
static const char* autorun_name = "autorun.inf";
static const char* stupid_antivirus = "  NOTE: This is usually caused by a poorly designed security solution. "
	"See https://bit.ly/40qDtyF.\r\n  This file will be skipped for now, but you should really "
	"look into using a *SMARTER* antivirus solution.";
static uint8_t joliet_level = 0;
static uint32_t md5sum_size = 0;
static BOOL scan_only = FALSE;
static StrArray config_path, isolinux_path;
static char symlinked_syslinux[MAX_PATH], *md5sum_data = NULL, *md5sum_pos = NULL;

// Ensure filenames do not contain invalid FAT32 or NTFS characters
static __inline char* sanitize_filename(char* filename, BOOL* is_identical)
{
	size_t i, j;
	char* ret = NULL;
	char unauthorized[] = { '*', '?', '<', '>', ':', '|' };

	*is_identical = TRUE;
	ret = safe_strdup(filename);
	if (ret == NULL) {
		uprintf("Could not allocate string for sanitized path");
		return NULL;
	}

	// Must start after the drive part (D:\...) so that we don't eliminate the first column
	for (i = 2; i<safe_strlen(ret); i++) {
		for (j = 0; j<sizeof(unauthorized); j++) {
			if (ret[i] == unauthorized[j]) {
				ret[i] = '_';
				*is_identical = FALSE;
			}
		}
	}
	return ret;
}

static void log_handler (cdio_log_level_t level, const char *message)
{
	uprintf("libcdio: %s", message);
}

// Returns TRUE if a path appears in md5sum.txt
static BOOL is_in_md5sum(char* path)
{
	BOOL found = FALSE;
	char c[3], * p, * pos = md5sum_pos, * nul_pos;

	// If we are creating the md5sum file from scratch, every file is in it.
	if (fd_md5sum != NULL)
		return TRUE;

	// If we don't have an existing file at this stage, then no file is in it.
	if (md5sum_size == 0 || md5sum_data == NULL)
		return FALSE;

	// We should have a "X:/xyz" path
	assert(path[1] == ':' && path[2] == '/');

	// Modify the path to have " ./xyz"
	c[0] = path[0];
	c[1] = path[1];
	path[0] = ' ';
	path[1] = '.';

	// Search for the string in the remainder of the md5sum.txt
	// NB: md5sum_data is always NUL terminated.
	p = strstr(pos, path);
	// Cater for the case where we matched a partial string and look for the full one
	while (p != NULL && p[strlen(path)] != '\n' && p[strlen(path)] != '\r' && p[strlen(path)] != '\0') {
		pos = p + strlen(path);
		p = strstr(pos, path);
	}
	found = (p != NULL);
	// If not found in remainder and we have a remainder, loop to search from beginning
	if (!found && pos != md5sum_data) {
		nul_pos = pos;
		c[2] = *nul_pos;
		*nul_pos = 0;
		p = strstr(md5sum_data, path);
		while (p != NULL && p[strlen(path)] != '\n' && p[strlen(path)] != '\r' && p[strlen(path)] != '\0') {
			pos = p + strlen(path);
			p = strstr(pos, path);
		}
		*nul_pos = c[2];
		found = (p != NULL);
	}

	path[0] = c[0];
	path[1] = c[1];
	if (found)
		md5sum_pos = p + strlen(path);
	return found;
}

static void _print_extracted_file(char* psz_fullpath, uint64_t file_length, BOOL split)
{
	size_t nul_pos;

	if (psz_fullpath == NULL)
		return;
	// Replace slashes with backslashes and append the size to the path for UI display
	to_windows_path(psz_fullpath);
	nul_pos = strlen(psz_fullpath);
	safe_sprintf(&psz_fullpath[nul_pos], 24, " (%s)", SizeToHumanReadable(file_length, TRUE, FALSE));
	uprintf(split ? "Splitting: %s" : "Extracting: %s", psz_fullpath);
	safe_sprintf(&psz_fullpath[nul_pos], 24, " (%s)", SizeToHumanReadable(file_length, FALSE, FALSE));
	PrintStatus(0, MSG_000, psz_fullpath);	// MSG_000 is "%s"
	// Remove the appended size for extraction
	psz_fullpath[nul_pos] = 0;
	// ISO9660 cannot handle backslashes
	to_unix_path(psz_fullpath);
	// Update md5sum_totalbytes as needed
	if (is_in_md5sum(psz_fullpath))
		md5sum_totalbytes += file_length;
}
#define print_extracted_file(p, l) _print_extracted_file(p, l, FALSE)
#define print_split_file(p, l) _print_extracted_file(p, l, TRUE)

/* Returns TRUE when extraction handled/skipped this file. */
static BOOL check_iso_props(const char* psz_dirname, int64_t file_length, const char* psz_basename,
	const char* psz_fullpath, ISO_FILE_PROPS* props)
{
	size_t i;

	GetIsoFileProps(psz_dirname, file_length, psz_basename, props);
	// In case there's an ldlinux.sys on the ISO, prevent it from overwriting ours
	if (props->is_root_ldlinux_sys) {
		uprintf("Skipping '%s' file from ISO image", psz_basename);
		return TRUE;
	}

	// Split a >4GB install.wim if the target filesystem is FAT
	if (file_length >= 4 * GB && psz_dirname != NULL && IS_FAT(fs_type) && img_report.has_4GB_file == 0x11) {
		if (safe_stricmp(&psz_dirname[max(0, ((int)safe_strlen(psz_dirname)) -
			((int)strlen(sources_str)))], sources_str) == 0) {
			char wim_path[4 * MAX_PATH];
			for (i = 0; i < ARRAYSIZE(wininst_name) - 1; i++) {
				if (safe_stricmp(psz_basename, wininst_name[i]) == 0 && file_length >= 4 * GB) {
					print_split_file((char*)psz_fullpath, file_length);
					char* dst = safe_strdup(psz_fullpath);
					dst[strlen(dst) - 3] = 's';
					dst[strlen(dst) - 2] = 'w';
					dst[strlen(dst) - 1] = 'm';
					assert(safe_strlen(image_path) + safe_strlen(psz_dirname) + safe_strlen(psz_basename) + 2 < ARRAYSIZE(wim_path));
					static_sprintf(wim_path, "%s|%s/%s", image_path, psz_dirname, psz_basename);
					WimSplitFile(wim_path, dst);
					free(dst);
					return TRUE;
				}
			}
		}
	}
	return FALSE;
}

// Apply various workarounds to Linux config files
static void fix_config(const char* psz_fullpath, const char* psz_path, const char* psz_basename, ISO_FILE_PROPS* props)
{
	BOOL modified = FALSE, patched;
	size_t nul_pos;
	char *iso_label = NULL, *usb_label = NULL, *src, *dst;

	src = safe_strdup(psz_fullpath);
	if (src == NULL)
		return;
	nul_pos = strlen(src);
	to_windows_path(src);

	// Add persistence to the kernel options
	if ((boot_type == BT_IMAGE) && HAS_PERSISTENCE(img_report) && persistence_size) {
		if ((props->is_grub_cfg) || (props->is_menu_cfg) || (props->is_syslinux_cfg)) {
			if (replace_in_token_data(src, props->is_grub_cfg ? "linux" : "append",
				"file=/cdrom/preseed", "persistent file=/cdrom/preseed", TRUE) != NULL) {
				// Ubuntu & derivatives are assumed to use 'file=/cdrom/preseed/...'
				// or 'layerfs-path=minimal.standard.live.squashfs' (see below)
				// somewhere in their kernel options and use 'persistent' as keyword.
				uprintf("  Added 'persistent' kernel option");
				modified = TRUE;
				// Also remove Ubuntu's "maybe-ubiquity" to avoid splash screen (GRUB only)
				if ((props->is_grub_cfg) && replace_in_token_data(src, "linux",
					"maybe-ubiquity", "", TRUE))
					uprintf("  Removed 'maybe-ubiquity' kernel option");
			} else if (replace_in_token_data(src, props->is_grub_cfg ? "linux" : "append",
				"boot=casper", "boot=casper persistent", TRUE) != NULL) {
				// Linux Mint uses "boot=casper". Oh and we want this replacement to happen BEFORE
				// the "linux /casper/vmlinuz" one, because Mint (Why is it ALWAYS them?) also use
				// "linux /casper/vmlinuz" and "kernel /casper/vmlinuz" in their config, and even
				// do so in a SUPER INCONSISTENT manner in their Syslinux' live.cfg, so we want to
				// make sure we don't have to do extra work to fix their inconsistency.
				uprintf("  Added 'persistent' kernel option");
				modified = TRUE;
			} else if (replace_in_token_data(src, "linux", "/casper/vmlinuz",
				"/casper/vmlinuz persistent", TRUE) != NULL) {
				// Ubuntu 23.04 and 24.04 use GRUB only with the above and don't use "maybe-ubiquity"
				uprintf("  Added 'persistent' kernel option");
				modified = TRUE;
			} else if (replace_in_token_data(src, "kernel", "/casper/vmlinuz",
				"/casper/vmlinuz persistent", TRUE) != NULL) {
				// Some people might use "kernel" in their Syslinux config instead of "linux"
				uprintf("  Added 'persistent' kernel option");
				modified = TRUE;
			} else if (replace_in_token_data(src, props->is_grub_cfg ? "linux" : "append",
				"boot=live", "boot=live persistence", TRUE) != NULL) {
				// Debian & derivatives are assumed to use 'boot=live' in
				// their kernel options and use 'persistence' as keyword.
				uprintf("  Added 'persistence' kernel option");
				modified = TRUE;
			}
			// Other distros can go to hell. Seriously, just check all partitions for
			// an ext volume with the right label and use persistence *THEN*. I mean,
			// why on earth do you need a bloody *NONSTANDARD* kernel option and/or a
			// "persistence.conf" file. This is SO INCREDIBLY RETARDED that it makes
			// Windows look smart in comparison. Great job there, Linux people!
		}
	}

	// Workaround for config files requiring an ISO label for kernel append that may be
	// different from our USB label. Oh, and these labels must have spaces converted to \x20.
	if ((props->is_cfg) || (props->is_conf)) {
		// Older versions of GRUB EFI used "linuxefi", newer just use "linux".
		// Also, in their great wisdom, the openSUSE maintainers added a 'set linux=linux'
		// line to their grub.cfg, which means that their kernel option cfg_token is no longer
		//'linux' but '$linux'... and we have to add a workaround for that.
		// Then, newer Arch and derivatives added an extra "search --label ..." command
		// in their GRUB conf, which we need to cater for in supplement of the kernel line.
		// Then Artix called in and decided they would use a "for kopt ..." loop.
		// Finally, we're just shoving the known isolinux/syslinux tokens in there to process
		// all config files equally.
		static const char* cfg_token[] = { "options", "append", "linux", "linuxefi", "$linux", "search", "for"};
		iso_label = replace_char(img_report.label, ' ', "\\x20");
		usb_label = replace_char(img_report.usb_label, ' ', "\\x20");
		if ((iso_label != NULL) && (usb_label != NULL)) {
			patched = FALSE;
			for (int i = 0; i < ARRAYSIZE(cfg_token); i++) {
				if (replace_in_token_data(src, cfg_token[i], iso_label, usb_label, TRUE) != NULL) {
					modified = TRUE;
					patched = TRUE;
				}
			}
			if (patched)
				uprintf("  Patched %s: '%s' ➔ '%s'", src, iso_label, usb_label);
			// Since version 8.2, and https://github.com/rhinstaller/anaconda/commit/a7661019546ec1d8b0935f9cb0f151015f2e1d95,
			// Red Hat derivatives have changed their CD-ROM detection policy which leads to the installation source
			// not being found. So we need to use 'inst.repo' instead of 'inst.stage2' in the kernel options.
			// *EXCEPT* this should not be done for netinst media such as Fedora 37 netinstall and trying to differentiate
			// netinst from regular is a pain. So, because I don't have all day to fix the mess that Red-Hat created when
			// they introduced a kernel option to decide where the source packages should be picked from we're just going
			// to *hope* that users didn't rename their ISOs and check whether it contains 'netinst' or not. Oh well...
			patched = FALSE;
			if (img_report.rh8_derivative && (strstr(image_path, "netinst") == NULL)) {
				for (int i = 0; i < ARRAYSIZE(cfg_token); i++) {
					if (replace_in_token_data(src, cfg_token[i], "inst.stage2", "inst.repo", TRUE) != NULL) {
						modified = TRUE;
						patched = TRUE;
					}
				}
				if (patched)
					uprintf("  Patched %s: '%s' ➔ '%s'", src, "inst.stage2", "inst.repo");
			}
		}
		safe_free(iso_label);
		safe_free(usb_label);
	}

	// Fix dual BIOS + EFI support for tails and other ISOs
	if ( (props->is_syslinux_cfg) && (safe_stricmp(psz_path, efi_dirname) == 0) &&
		 (safe_stricmp(psz_basename, syslinux_cfg[0]) == 0) &&
		 (!img_report.has_efi_syslinux) && (dst = safe_strdup(src)) ) {
		dst[nul_pos-12] = 's'; dst[nul_pos-11] = 'y'; dst[nul_pos-10] = 's';
		CopyFileA(src, dst, TRUE);
		uprintf("Duplicated %s to %s", src, dst);
		free(dst);
	}

	// Workaround for FreeNAS
	if (props->is_grub_cfg) {
		iso_label = malloc(MAX_PATH);
		usb_label = malloc(MAX_PATH);
		if ((iso_label != NULL) && (usb_label != NULL)) {
			safe_sprintf(iso_label, MAX_PATH, "cd9660:/dev/iso9660/%s", img_report.label);
			safe_sprintf(usb_label, MAX_PATH, "msdosfs:/dev/msdosfs/%s", img_report.usb_label);
			if (replace_in_token_data(src, "set", iso_label, usb_label, TRUE) != NULL) {
				uprintf("  Patched %s: '%s' ➔ '%s'", src, iso_label, usb_label);
				modified = TRUE;
			}
		}
		safe_free(iso_label);
		safe_free(usb_label);
	}

	if (modified)
		StrArrayAdd(&modified_files, psz_fullpath, TRUE);

	free(src);
}

// Convert from time_t to FILETIME
// Uses 3 static entries so that we can convert 3 concurrent values at the same time
static LPFILETIME __inline to_filetime(time_t t)
{
	static int i = 0;
	static FILETIME ft[3], *r;
	LONGLONG ll = (t * 10000000LL) + 116444736000000000LL;

	r = &ft[i];
	r->dwLowDateTime = (DWORD)ll;
	r->dwHighDateTime = (DWORD)(ll >> 32);
	i = (i + 1) % ARRAYSIZE(ft);
	return r;
}

// Helper function to restore the timestamp on a directory
static void __inline set_directory_timestamp(char* path, LPFILETIME creation, LPFILETIME last_access, LPFILETIME modify)
{
	HANDLE dir_handle = CreateFileU(path, GENERIC_READ | GENERIC_WRITE,
		FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
	if ((dir_handle == INVALID_HANDLE_VALUE) || (!SetFileTime(dir_handle, creation, last_access, modify)))
		uprintf("  Could not set timestamp for directory '%s': %s", path, WindowsErrorString());
	safe_closehandle(dir_handle);
}

// Returns 0 on success, nonzero on error
static int udf_extract_files(udf_t *p_udf, udf_dirent_t *p_udf_dirent, const char *psz_path)
{
	HANDLE file_handle = NULL;
	DWORD buf_size, wr_size, err;
	ISO_FILE_PROPS props;
	HASH_CONTEXT ctx;
	BOOL r, is_identical;
	int length;
	size_t i, j, nb;
	char tmp[128], *psz_fullpath = NULL, *psz_sanpath = NULL;
	const char* psz_basename;
	udf_dirent_t *p_udf_dirent2;
	_Static_assert(ISO_BUFFER_SIZE % UDF_BLOCKSIZE == 0,
		"ISO_BUFFER_SIZE is not a multiple of UDF_BLOCKSIZE");
	uint8_t* buf = malloc(ISO_BUFFER_SIZE);
	int64_t read, file_length;

	if ((p_udf_dirent == NULL) || (psz_path == NULL) || (buf == NULL)) {
		safe_free(buf);
		return 1;
	}

	if (psz_path[0] == 0)
		UpdateProgressWithInfoInit(NULL, TRUE);
	while ((p_udf_dirent = udf_readdir(p_udf_dirent)) != NULL) {
		if (ErrorStatus) goto out;
		psz_basename = udf_get_filename(p_udf_dirent);
		if (strlen(psz_basename) == 0)
			continue;
		length = (int)(3 + strlen(psz_path) + strlen(psz_basename) + strlen(psz_extract_dir) + 24);
		psz_fullpath = (char*)calloc(sizeof(char), length);
		if (psz_fullpath == NULL) {
			uprintf("Error allocating file name");
			goto out;
		}
		length = _snprintf_s(psz_fullpath, length, _TRUNCATE, "%s%s/%s", psz_extract_dir, psz_path, psz_basename);
		if (length < 0)
			goto out;
		if (S_ISLNK(udf_get_posix_filemode(p_udf_dirent)))
			img_report.has_symlinks = SYMLINKS_UDF;
		if (udf_is_dir(p_udf_dirent)) {
			psz_sanpath = sanitize_filename(psz_fullpath, &is_identical);
			IGNORE_RETVAL(_mkdirU(psz_sanpath));
			if (preserve_timestamps) {
				set_directory_timestamp(psz_sanpath, to_filetime(udf_get_attribute_time(p_udf_dirent)),
					to_filetime(udf_get_access_time(p_udf_dirent)), to_filetime(udf_get_modification_time(p_udf_dirent)));
			}
			safe_free(psz_sanpath);
			p_udf_dirent2 = udf_opendir(p_udf_dirent);
			if (p_udf_dirent2 != NULL) {
				if (udf_extract_files(p_udf, p_udf_dirent2, &psz_fullpath[strlen(psz_extract_dir)]))
					goto out;
			}
		} else {
			file_length = udf_get_file_length(p_udf_dirent);
			if (check_iso_props(psz_path, file_length, psz_basename, psz_fullpath, &props)) {
				safe_free(psz_fullpath);
				continue;
			}
			print_extracted_file(psz_fullpath, file_length);
			for (i = 0; i < NB_OLD_C32; i++) {
				if (props.is_old_c32[i] && use_own_c32[i]) {
					static_sprintf(tmp, "%s/syslinux-%s/%s", FILES_DIR, embedded_sl_version_str[0], old_c32_name[i]);
					if (CopyFileU(tmp, psz_fullpath, FALSE)) {
						uprintf("  Replaced with local version %s", IsFileInDB(tmp)?"✓":"✗");
						break;
					}
					uprintf("  Could not replace file: %s", WindowsErrorString());
				}
			}
			if (i < NB_OLD_C32)
				continue;
			psz_sanpath = sanitize_filename(psz_fullpath, &is_identical);
			if (!is_identical)
				uprintf("  File name sanitized to '%s'", psz_sanpath);
			file_handle = CreatePreallocatedFile(psz_sanpath, GENERIC_READ | GENERIC_WRITE,
				FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, file_length);
			if (file_handle == INVALID_HANDLE_VALUE) {
				err = GetLastError();
				uprintf("  Unable to create file: %s", WindowsErrorString());
				if (((err == ERROR_ACCESS_DENIED) || (err == ERROR_INVALID_HANDLE)) &&
					(safe_strcmp(&psz_sanpath[3], autorun_name) == 0))
					uprintf(stupid_antivirus);
				else
					goto out;
			} else {
				if (fd_md5sum != NULL)
					hash_init[HASH_MD5](&ctx);
				while (file_length > 0) {
					if (ErrorStatus)
						goto out;
					nb = (size_t)MIN(ISO_BUFFER_SIZE / UDF_BLOCKSIZE, (file_length + UDF_BLOCKSIZE - 1) / UDF_BLOCKSIZE);
					read = udf_read_block(p_udf_dirent, buf, nb);
					if (read < 0) {
						uprintf("  Error reading UDF file %s", &psz_fullpath[strlen(psz_extract_dir)]);
						goto out;
					}
					buf_size = (DWORD)MIN(file_length, read);
					if (fd_md5sum != NULL)
						hash_write[HASH_MD5](&ctx, buf, buf_size);
					ISO_BLOCKING(r = WriteFileWithRetry(file_handle, buf, buf_size, &wr_size, WRITE_RETRIES));
					if (!r || (wr_size != buf_size)) {
						if (r)
							SetLastError(ERROR_WRITE_FAULT);
						uprintf("  Error writing file: %s", WindowsErrorString());
						goto out;
					}
					file_length -= wr_size;
					nb_blocks += nb;
					if (nb_blocks - last_nb_blocks >= PROGRESS_THRESHOLD) {
						UpdateProgressWithInfo(OP_FILE_COPY, MSG_231, nb_blocks, total_blocks);
						last_nb_blocks = nb_blocks;
					}
				}
				if (fd_md5sum != NULL) {
					hash_final[HASH_MD5](&ctx);
					for (j = 0; j < MD5_HASHSIZE; j++)
						fprintf(fd_md5sum, "%02x", ctx.buf[j]);
					fprintf(fd_md5sum, "  ./%s\n", &psz_fullpath[3]);
				}
			}
			if ((preserve_timestamps) && (!SetFileTime(file_handle, to_filetime(udf_get_attribute_time(p_udf_dirent)),
				to_filetime(udf_get_access_time(p_udf_dirent)), to_filetime(udf_get_modification_time(p_udf_dirent)))))
				uprintf("  Could not set timestamp: %s", WindowsErrorString());

			// If you have a fast USB 3.0 device, the default Windows buffering does an
			// excellent job at compensating for our small blocks read/writes to max out the
			// device's bandwidth.
			// The drawback however is with cancellation. With a large file, CloseHandle()
			// may take forever to complete and is not interruptible. We try to detect this.
			ISO_BLOCKING(safe_closehandle(file_handle));
			if (props.is_cfg || props.is_conf)
				fix_config(psz_sanpath, psz_path, psz_basename, &props);
			safe_free(psz_sanpath);
		}
		safe_free(psz_fullpath);
	}
	safe_free(buf);
	return 0;

out:
	if (GetLastError() != ERROR_SUCCESS)
		ErrorStatus = RUFUS_ERROR(GetLastError());
	udf_dirent_free(p_udf_dirent);
	ISO_BLOCKING(safe_closehandle(file_handle));
	safe_free(psz_sanpath);
	safe_free(psz_fullpath);
	safe_free(buf);
	return 1;
}

// Returns 0 on success, >0 on error, <0 to ignore current dir
static int iso_extract_files(iso9660_t* p_iso, const char *psz_path)
{
	HANDLE file_handle = NULL;
	DWORD buf_size, wr_size, err;
	ISO_FILE_PROPS props;
	HASH_CONTEXT ctx;
	BOOL is_symlink, is_identical, create_file, free_p_statbuf = FALSE;
	int length, r = 1;
	char psz_fullpath[MAX_PATH], *psz_basename = NULL, *psz_sanpath = NULL;
	char tmp[128], target_path[256], *last_slash;
	const char *psz_iso_name = &psz_fullpath[strlen(psz_extract_dir)];
	_Static_assert(ISO_BUFFER_SIZE % ISO_BLOCKSIZE == 0,
		"ISO_BUFFER_SIZE is not a multiple of ISO_BLOCKSIZE");
	uint8_t* buf = malloc(ISO_BUFFER_SIZE);
	CdioListNode_t* p_entnode;
	iso9660_stat_t *p_statbuf;
	CdioISO9660FileList_t* p_entlist = NULL;
	size_t i, j, nb;
	lsn_t lsn;
	int64_t file_length;

	if ((p_iso == NULL) || (psz_path == NULL) || (buf == NULL)) {
		safe_free(buf);
		return 1;
	}

	length = _snprintf_s(psz_fullpath, sizeof(psz_fullpath), _TRUNCATE, "%s%s/", psz_extract_dir, psz_path);
	if (length < 0)
		goto out;
	psz_basename = &psz_fullpath[length];

	p_entlist = iso9660_ifs_readdir(p_iso, psz_path);
	if (!p_entlist) {
		uprintf("Could not access directory %s", psz_path);
		goto out;
	}

	if (psz_path[0] == 0)
		UpdateProgressWithInfoInit(NULL, TRUE);
	_CDIO_LIST_FOREACH(p_entnode, p_entlist) {
		if (ErrorStatus) goto out;
		p_statbuf = (iso9660_stat_t*) _cdio_list_node_data(p_entnode);
		free_p_statbuf = FALSE;
		// Eliminate . and .. entries
		if ( (strcmp(p_statbuf->filename, ".") == 0)
			|| (strcmp(p_statbuf->filename, "..") == 0) )
			continue;
		// Rock Ridge requires an exception
		is_symlink = FALSE;
		if ((p_statbuf->rr.b3_rock == yep) && enable_rockridge) {
			safe_strcpy(psz_basename, sizeof(psz_fullpath) - length - 1, p_statbuf->filename);
			if (safe_strlen(p_statbuf->filename) > 64)
				img_report.has_long_filename = TRUE;
			is_symlink = (p_statbuf->rr.psz_symlink != NULL);
			if (is_symlink)
				img_report.has_symlinks = SYMLINKS_RR;
		} else {
			iso9660_name_translate_ext(p_statbuf->filename, psz_basename, joliet_level);
		}
		if (p_statbuf->type == _STAT_DIR) {
			if (!scan_only) {
				psz_sanpath = sanitize_filename(psz_fullpath, &is_identical);
				IGNORE_RETVAL(_mkdirU(psz_sanpath));
				if (preserve_timestamps) {
					LPFILETIME ft = to_filetime(mktime(&p_statbuf->tm));
					set_directory_timestamp(psz_sanpath, ft, ft, ft);
				}
				safe_free(psz_sanpath);
			}
			r = iso_extract_files(p_iso, psz_iso_name);
			if (r > 0)
				goto out;
			if (r < 0)	// Stop processing current dir
				break;
		} else {
			file_length = p_statbuf->total_size;
			if (check_iso_props(psz_path, file_length, psz_basename, psz_fullpath, &props))
				continue;
			if (!is_symlink)
				print_extracted_file(psz_fullpath, file_length);
			for (i = 0; i < NB_OLD_C32; i++) {
				if (props.is_old_c32[i] && use_own_c32[i]) {
					static_sprintf(tmp, "%s/syslinux-%s/%s", FILES_DIR, embedded_sl_version_str[0], old_c32_name[i]);
					if (CopyFileU(tmp, psz_fullpath, FALSE)) {
						uprintf("  Replaced with local version %s", IsFileInDB(tmp)?"✓":"✗");
						break;
					}
					uprintf("  Could not replace file: %s", WindowsErrorString());
				}
			}
			if (i < NB_OLD_C32)
				continue;
			psz_sanpath = sanitize_filename(psz_fullpath, &is_identical);
			if (!is_identical)
				uprintf("  File name sanitized to '%s'", psz_sanpath);
			create_file = TRUE;
			if (is_symlink) {
				if (fs_type == FS_NTFS) {
					// Replicate symlinks if NTFS is being used
					static_sprintf(target_path, "%s/%s", psz_path, p_statbuf->rr.psz_symlink);
					iso9660_stat_t* p_statbuf2 = iso9660_ifs_stat_translate(p_iso, target_path);
					if (p_statbuf2 != NULL) {
						to_windows_path(psz_fullpath);
						to_windows_path(p_statbuf->rr.psz_symlink);
						uprintf("Symlinking: %s%s ➔ %s", psz_fullpath,
							(p_statbuf2->type == _STAT_DIR) ? "\\" : "", p_statbuf->rr.psz_symlink);
						if (!CreateSymbolicLinkU(psz_fullpath, p_statbuf->rr.psz_symlink,
							(p_statbuf2->type == _STAT_DIR) ? SYMBOLIC_LINK_FLAG_DIRECTORY : 0))
							uprintf("  Could not create symlink: %s", WindowsErrorString());
						to_unix_path(p_statbuf->rr.psz_symlink);
						to_unix_path(psz_fullpath);
						iso9660_stat_free(p_statbuf2);
						create_file = FALSE;
					}
				} else if (file_length == 0) {
					if ((safe_stricmp(p_statbuf->filename, "syslinux") == 0) &&
						// Special handling for ISOs that have a syslinux → isolinux symbolic link (e.g. Knoppix)
						(safe_stricmp(p_statbuf->rr.psz_symlink, "isolinux") == 0)) {
						static_strcpy(symlinked_syslinux, psz_fullpath);
						print_extracted_file(psz_fullpath, file_length);
						uprintf("  Found Rock Ridge symbolic link to '%s'", p_statbuf->rr.psz_symlink);
					} else if (strcmp(psz_path, "/firmware") == 0) {
						// Special handling for ISOs that use symlinks for /firmware/ (e.g. Debian non-free)
						// TODO: Do we want to do this for all file symlinks?
						static_sprintf(target_path, "%s/%s", psz_path, p_statbuf->rr.psz_symlink);
						p_statbuf = iso9660_ifs_stat_translate(p_iso, target_path);
						if (p_statbuf != NULL) {
							// The original p_statbuf will be freed automatically, but not
							// the new one so we need to force an explicit free.
							free_p_statbuf = TRUE;
							file_length = p_statbuf->total_size;
							print_extracted_file(psz_fullpath, file_length);
							uprintf("  Duplicated from '%s'", target_path);
						} else {
							uprintf("Could not resolve Rock Ridge Symlink - ABORTING!");
							goto out;
						}
					} else {
						print_extracted_file(psz_fullpath, safe_strlen(p_statbuf->rr.psz_symlink));
						uprintf("  Ignoring Rock Ridge symbolic link to '%s'", p_statbuf->rr.psz_symlink);
					}
				} else {
					uuprintf("Unexpected symlink length: %d", file_length);
					create_file = FALSE;
				}
			}
			if (create_file) {
				file_handle = CreatePreallocatedFile(psz_sanpath, GENERIC_READ | GENERIC_WRITE,
					FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, file_length);
				if (file_handle == INVALID_HANDLE_VALUE && GetLastError() == ERROR_PATH_NOT_FOUND) {
					// Some folks (umbrelos) managed to master their ISOs in a manner where some
					// directories don't exist (or don't have _STAT_DIR) but still have files,
					// in which case our approach, that expects a sane layout with directories
					// properly declared before the files they contain, breaks. Therefore:
					last_slash = strrchr(psz_sanpath, '/');
					if (last_slash != NULL) {
						*last_slash = '\0';
						uprintf("WARNING: Directory '%s/' was improperly mastered on the source image!", &psz_sanpath[2]);
						_mkdirExU(psz_sanpath);
						*last_slash = '/';
						file_handle = CreatePreallocatedFile(psz_sanpath, GENERIC_READ | GENERIC_WRITE,
							FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, file_length);
					}
				}
				if (file_handle == INVALID_HANDLE_VALUE) {
					err = GetLastError();
					uprintf("  Unable to create file: %s", WindowsErrorString());
					if (((err == ERROR_ACCESS_DENIED) || (err == ERROR_INVALID_HANDLE)) &&
						(safe_strcmp(&psz_sanpath[3], autorun_name) == 0))
						uprintf(stupid_antivirus);
					else
						goto out;
				} else if (is_symlink) {
					// Create a text file that contains the target link
					ISO_BLOCKING(r = WriteFileWithRetry(file_handle, p_statbuf->rr.psz_symlink,
						(DWORD)safe_strlen(p_statbuf->rr.psz_symlink), &wr_size, WRITE_RETRIES));
					if (!r) {
						uprintf("  Error writing file: %s", WindowsErrorString());
						goto out;
					}
				} else {
					if (fd_md5sum != NULL)
						hash_init[HASH_MD5](&ctx);
					for (i = 0; file_length > 0; i += nb) {
						if (ErrorStatus)
							goto out;
						lsn = p_statbuf->lsn + (lsn_t)i;
						nb = (size_t)MIN(ISO_BUFFER_SIZE / ISO_BLOCKSIZE, (file_length + ISO_BLOCKSIZE - 1) / ISO_BLOCKSIZE);
						if (iso9660_iso_seek_read(p_iso, buf, lsn, (long)nb) != (nb * ISO_BLOCKSIZE)) {
							uprintf("  Error reading ISO9660 file %s at LSN %lu",
								psz_iso_name, (long unsigned int)lsn);
							goto out;
						}
						buf_size = (DWORD)MIN(file_length, ISO_BUFFER_SIZE);
						if (fd_md5sum != NULL)
							hash_write[HASH_MD5](&ctx, buf, buf_size);
						ISO_BLOCKING(r = WriteFileWithRetry(file_handle, buf, buf_size, &wr_size, WRITE_RETRIES));
						if (!r || wr_size != buf_size) {
							if (r)
								SetLastError(ERROR_WRITE_FAULT);
							uprintf("  Error writing file: %s", WindowsErrorString());
							goto out;
						}
						file_length -= wr_size;
						nb_blocks += nb;
						if (nb_blocks - last_nb_blocks >= PROGRESS_THRESHOLD) {
							UpdateProgressWithInfo(OP_FILE_COPY, MSG_231, nb_blocks, total_blocks +
								((fs_type != FS_NTFS) ? extra_blocks : 0));
							last_nb_blocks = nb_blocks;
						}
					}
					if (fd_md5sum != NULL) {
						hash_final[HASH_MD5](&ctx);
						for (j = 0; j < MD5_HASHSIZE; j++)
							fprintf(fd_md5sum, "%02x", ctx.buf[j]);
						fprintf(fd_md5sum, "  ./%s\n", &psz_fullpath[3]);
					}
				}
				if (preserve_timestamps) {
					LPFILETIME ft = to_filetime(mktime(&p_statbuf->tm));
					if (!SetFileTime(file_handle, ft, ft, ft))
						uprintf("  Could not set timestamp: %s", WindowsErrorString());
				}
			}
			if (free_p_statbuf)
				iso9660_stat_free(p_statbuf);
			ISO_BLOCKING(safe_closehandle(file_handle));
			if (props.is_cfg || props.is_conf)
				fix_config(psz_sanpath, psz_path, psz_basename, &props);
			safe_free(psz_sanpath);
		}
	}
	r = 0;

out:
	if (r != 0 && GetLastError() != ERROR_SUCCESS)
		ErrorStatus = RUFUS_ERROR(GetLastError());
	ISO_BLOCKING(safe_closehandle(file_handle));
	if (p_entlist != NULL)
		iso9660_filelist_free(p_entlist);
	safe_free(psz_sanpath);
	safe_free(buf);
	return r;
}

int IsIsoScanCancelled(void)
{
	return ErrorStatus != 0;
}

BOOL ExtractISO(const char* src_iso, const char* dest_dir, BOOL scan)
{
	int r = 1, content_result;
	char *tmp, *spacing = "  ";
	char path[MAX_PATH], path2[16];
	size_t i;
	FILE* fd;
	iso9660_t* p_iso = NULL;
	iso9660_pvd_t pvd;
	udf_t* p_udf = NULL;
	udf_dirent_t* p_udf_root;
	iso_extension_mask_t iso_extension_mask = ISO_EXTENSION_ALL;

	if ((!enable_iso) || (src_iso == NULL) || (dest_dir == NULL))
		return FALSE;

	scan_only = scan;
	if (!scan_only)
		spacing = "";
	cdio_log_set_handler((scan_only && !usb_debug) ? NULL : log_handler);
	psz_extract_dir = dest_dir;
	// Change progress style to marquee for scanning
	if (scan_only) {
		uprintf("ISO analysis:");
		SendMessage(hMainDialog, UM_PROGRESS_INIT, PBS_MARQUEE, 0);
		total_blocks = 0;
		extra_blocks = 0;
		has_ldlinux_c32 = FALSE;
		// String array of all isolinux/syslinux locations
		StrArrayCreate(&config_path, 8);
		StrArrayCreate(&isolinux_path, 8);
		PrintInfo(0, MSG_202);
	} else {
		uprintf("Extracting files...");
		IGNORE_RETVAL(_chdirU(app_data_dir));
		if (total_blocks == 0) {
			uprintf("Error: ISO has not been properly scanned.");
			ErrorStatus = RUFUS_ERROR(APPERR(ERROR_ISO_SCAN));
			goto out;
		}
		nb_blocks = 0;
		last_nb_blocks = 0;
		iso_blocking_status = 0;
		symlinked_syslinux[0] = 0;
		StrArrayClear(&modified_files);
		if (validate_md5sum) {
			md5sum_totalbytes = 0;
			// If there isn't an already existing md5sum.txt create one
			if (img_report.has_md5sum != 1) {
				static_sprintf(path, "%s\\%s", dest_dir, md5sum_name[0]);
				fd_md5sum = fopenU(path, "wb");
				if (fd_md5sum == NULL)
					uprintf("WARNING: Could not create '%s'", md5sum_name[0]);
			} else {
				md5sum_size = ReadISOFileToBuffer(src_iso, md5sum_name[0], (uint8_t**)&md5sum_data);
				md5sum_pos = md5sum_data;
			}
		}
	}

	// First try to open as UDF - fallback to ISO if it failed
	p_udf = udf_open(src_iso);
	if (p_udf == NULL)
		goto try_iso;
	uprintf("%sImage is a UDF image", spacing);

	p_udf_root = udf_get_root(p_udf, true, 0);
	if (p_udf_root == NULL) {
		uprintf("%sCould not locate UDF root directory", spacing);
		if (scan_only) {
			r = 1;
			goto out;
		}
		goto try_iso;
	}
	if (scan_only) {
		ISO_SCAN_STATE state = { 0 };
		if (udf_get_logical_volume_id(p_udf, img_report.label, sizeof(img_report.label)) <= 0)
			img_report.label[0] = 0;
		// Open the UDF as ISO so that we can perform size checks
		p_iso = iso9660_open(src_iso);
		state.report = &img_report;
		state.config_path = &config_path;
		state.isolinux_path = &isolinux_path;
		UpdateProgressWithInfoInit(NULL, TRUE);
		r = ScanUdfFiles(p_udf_root, "", &state);
		total_blocks = state.total_blocks;
		has_ldlinux_c32 = state.has_ldlinux_c32;
	} else {
		r = udf_extract_files(p_udf, p_udf_root, "");
	}
	p_udf_root = NULL; // Both traversal paths consume the iterator.
	goto out;

try_iso:
	// Perform our first scan with Joliet disabled (if Rock Ridge is enabled), so that we can find if
	// there exists a Rock Ridge file with a name > 64 chars or if there are symlinks. If that is the
	// case then we also disable Joliet during the extract phase.
	if ((!enable_joliet) || (enable_rockridge && (scan_only || img_report.has_long_filename ||
		(img_report.has_symlinks == SYMLINKS_RR)))) {
		iso_extension_mask &= ~ISO_EXTENSION_JOLIET;
	}
	if (!enable_rockridge) {
		iso_extension_mask &= ~ISO_EXTENSION_ROCK_RIDGE;
	}

	p_iso = iso9660_open_ext(src_iso, iso_extension_mask);
	if (p_iso == NULL) {
		uprintf("%s'%s' doesn't look like an ISO image", spacing, src_iso);
		r = 1;
		goto out;
	}
	uprintf("%sImage is an ISO9660 image", spacing);
	joliet_level = iso9660_ifs_get_joliet_level(p_iso);
	if (scan_only) {
		if (iso9660_ifs_get_volume_id(p_iso, &tmp)) {
			static_strcpy(img_report.label, tmp);
			safe_free(tmp);
		} else
			img_report.label[0] = 0;
	} else {
		if (iso_extension_mask & (ISO_EXTENSION_JOLIET|ISO_EXTENSION_ROCK_RIDGE))
			uprintf("%sThis image will be extracted using %s extensions (if present)", spacing,
				(iso_extension_mask & ISO_EXTENSION_JOLIET)?"Joliet":"Rock Ridge");
		else
			uprintf("%sThis image will not be extracted using any ISO extensions", spacing);
	}
	if (scan_only) {
		ISO_SCAN_STATE state = { 0 };
		state.report = &img_report;
		state.config_path = &config_path;
		state.isolinux_path = &isolinux_path;
		state.enable_rockridge = enable_rockridge;
		state.joliet_level = joliet_level;
		UpdateProgressWithInfoInit(NULL, TRUE);
		r = ScanIsoFiles(p_iso, "", &state);
		total_blocks = state.total_blocks;
		extra_blocks = state.extra_blocks;
		has_ldlinux_c32 = state.has_ldlinux_c32;
	} else {
		r = iso_extract_files(p_iso, "");
	}

out:
	iso_blocking_status = -1;
	if (scan_only) {
		struct __stat64 stat;
		ISO_SCAN_STATE state = { 0 };
		int64_t image_size = -1, volume_blocks = -1;
		if (p_iso != NULL && iso9660_ifs_read_pvd(p_iso, &pvd))
			volume_blocks = iso9660_get_pvd_space_size(&pvd);
		if (_stat64U(src_iso, &stat) == 0)
			image_size = stat.st_size;
		if (!FinalizeIsoScan(&img_report, total_blocks, image_size, volume_blocks)) {
			r = 1;
			ErrorStatus = RUFUS_ERROR(APPERR(ERROR_ISO_SCAN));
		}
		state.report = &img_report;
		state.config_path = &config_path;
		state.isolinux_path = &isolinux_path;
		state.has_ldlinux_c32 = has_ldlinux_c32;
		cdio_loglevel_default = CDIO_LOG_WARN;
		content_result = ScanSyslinuxBootloaders(src_iso, ISO_EXTENSION_MASK, &state, embedded_sl_version);
		cdio_loglevel_default = usb_debug ? CDIO_LOG_INFO : CDIO_LOG_WARN;
		if (content_result == ISO_SCAN_ERROR || content_result == ISO_SCAN_CANCELLED) {
			r = 1;
			if (ErrorStatus == 0)
				ErrorStatus = RUFUS_ERROR(APPERR(ERROR_ISO_SCAN));
		}
		if (!IS_EFI_BOOTABLE(img_report) && HAS_EFI_IMG(img_report) && HasEfiImgBootLoaders(p_iso, p_udf)) {
			img_report.has_efi = 0x8000;
		}
		content_result = ScanWinPeOptions(src_iso, ISO_EXTENSION_MASK, &img_report);
		if (content_result == ISO_SCAN_ERROR || content_result == ISO_SCAN_CANCELLED) {
			r = 1;
			if (ErrorStatus == 0)
				ErrorStatus = RUFUS_ERROR(APPERR(ERROR_ISO_SCAN));
		}
		if (HAS_WININST(img_report)) {
			char wim_path[4 * MAX_PATH];
			assert(safe_strlen(image_path) + safe_strlen(&img_report.wininst_path[0][2]) + 2 < ARRAYSIZE(wim_path));
			static_sprintf(wim_path, "%s|%s", image_path, &img_report.wininst_path[0][2]);
			img_report.wininst_version = GetWimVersion(wim_path);
		}
		cdio_loglevel_default = CDIO_LOG_WARN;
		content_result = ScanGrubAndEfiBootloaders(src_iso, ISO_EXTENSION_MASK, &img_report);
		cdio_loglevel_default = usb_debug ? CDIO_LOG_INFO : CDIO_LOG_WARN;
		if (content_result == ISO_SCAN_ERROR || content_result == ISO_SCAN_CANCELLED) {
			r = 1;
			if (ErrorStatus == 0)
				ErrorStatus = RUFUS_ERROR(APPERR(ERROR_ISO_SCAN));
		}

		StrArrayDestroy(&config_path);
		StrArrayDestroy(&isolinux_path);
		SendMessage(hMainDialog, UM_PROGRESS_EXIT, 0, 0);
	} else {
		// Solus and other ISOs only provide EFI boot files in a FAT efi.img
		// Also work around ISOs that have a borked symbolic link for bootx64.efi.
		// See https://github.com/linuxmint/linuxmint/issues/622.
		if (img_report.has_efi & 0xc000) {
			if (img_report.has_efi & 0x4000) {
				uprintf("Broken UEFI bootloader detected - Applying workaround:");
				static_sprintf(path, "%s\\EFI\\boot\\bootx64.efi", dest_dir);
				DeleteFileU(path);
			}
			DumpFatDir(p_iso, dest_dir, 0);
		}
		if (HAS_SYSLINUX(img_report)) {
			static_sprintf(path, "%s\\syslinux.cfg", dest_dir);
			// Create a /syslinux.cfg (if none exists) that points to the existing isolinux cfg
			fd = fopen(path, "r");
			if (fd != NULL && img_report.needs_syslinux_overwrite) {
				fclose(fd);
				fd = NULL;
				static_sprintf(path2, "%s\\syslinux.org", dest_dir);
				uprintf("Renaming: %s ➔ %s", path, path2);
				IGNORE_RETVAL(rename(path, path2));
			}
			if (fd == NULL) {
				fd = fopen(path, "w");	// No "/syslinux.cfg" => create a new one
				if (fd == NULL) {
					uprintf("Unable to create %s - booting from USB will not work", path);
					r = 1;
				} else {
					fprintf(fd, "DEFAULT loadconfig\n\nLABEL loadconfig\n  CONFIG %s\n", img_report.cfg_path);
					for (i = safe_strlen(img_report.cfg_path); (i > 0) && (img_report.cfg_path[i] != '/'); i--);
					if (i > 0) {
						img_report.cfg_path[i] = 0;
						fprintf(fd, "  APPEND %s/\n", img_report.cfg_path);
						img_report.cfg_path[i] = '/';
					}
					uprintf("Created: %s → %s", path, img_report.cfg_path);
				}
			}
			if (fd != NULL)
				fclose(fd);
			// Workaround needed for Knoppix that has a /boot/syslinux that links to /boot/isolinux/
			// with EFI Syslinux trying to read /boot/syslinux/syslnx[32|64].cfg as the config file.
			if (symlinked_syslinux[0] != 0) {
				static const char* efi_cfg_name[] = { "syslnx32.cfg", "syslnx64.cfg" };
				size_t len = strlen(symlinked_syslinux);
				char isolinux_dir[MAX_PATH];
				static_strcpy(isolinux_dir, symlinked_syslinux);
				assert(len > 8);
				// ".../syslinux" -> ".../isolinux"
				isolinux_dir[len - 8] = 'i';
				isolinux_dir[len - 7] = 's';
				isolinux_dir[len - 6] = 'o';
				// Delete the empty syslinux symbolic link remnant and replace it with a syslinux/ dir
				DeleteFileA(symlinked_syslinux);
				CreateDirectoryA(symlinked_syslinux, NULL);
				// Now add the relevant config files that link back to the ones in isolinux/
				for (i = 0; i < ARRAYSIZE(efi_cfg_name); i++) {
					static_sprintf(path, "%s/%s", isolinux_dir, efi_cfg_name[i]);
					if (!PathFileExistsA(path))
						continue;
					static_sprintf(path, "%s/%s", symlinked_syslinux, efi_cfg_name[i]);
					fd = fopen(path, "w");
					if (fd == NULL) {
						uprintf("Unable to create %s - booting from USB may not work", path);
						r = 1;
						continue;
					}
					static_sprintf(path, "%s/%s", isolinux_dir, efi_cfg_name[i]);
					fprintf(fd, "DEFAULT loadconfig\n\nLABEL loadconfig\n  CONFIG %s\n  APPEND %s\n", &path[2], &isolinux_dir[2]);
					fclose(fd);
					to_windows_path(symlinked_syslinux);
					uprintf("Created: %s\\%s → %s", symlinked_syslinux, efi_cfg_name[i], &path[2]);
					to_unix_path(symlinked_syslinux);
				}
			}
		} else if (HAS_BOOTMGR(img_report) && enable_ntfs_compression) {
			// bootmgr might need to be uncompressed: https://github.com/pbatard/rufus/issues/1381
			RunCommand("compact /u bootmgr* efi/boot/*.efi", dest_dir, TRUE);
		}
		// Exception for Slax Syslinux UEFI bootloaders...
		// ...that don't appear to work anyway as of slax-64bit-slackware-15.0.3.iso
		static_sprintf(path, "%s\\slax\\boot\\EFI", dest_dir);
		if (PathFileExistsA(path)) {
			char dst_path[16];
			static_sprintf(dst_path, "%s\\EFI", dest_dir);
			if (!PathFileExistsA(dst_path)) {
				if (MoveFileA(path, dst_path))
					uprintf("Moved: %s → %s", path, dst_path);
				else
					uprintf("Could not move %s → %s: %s", path, dst_path, WindowsErrorString());
			}
		}
		if (fd_md5sum != NULL) {
			uprintf("Created: %s\\%s (%s)", dest_dir, md5sum_name[0], SizeToHumanReadable(ftell(fd_md5sum), FALSE, FALSE));
			fclose(fd_md5sum);
		} else if (md5sum_data != NULL) {
			safe_free(md5sum_data);
			md5sum_size = 0;
		}
	}
	iso9660_close(p_iso);
	udf_close(p_udf);
	if ((r != 0) && (ErrorStatus == 0))
		ErrorStatus = RUFUS_ERROR(APPERR(scan_only ? ERROR_ISO_SCAN : ERROR_ISO_EXTRACT));
	return (r == 0);
}

int64_t ExtractISOFile(const char* iso, const char* iso_file, const char* dest_file, DWORD attributes)
{
	size_t i;
	ssize_t read_size;
	int64_t file_length, r = 0;
	char buf[UDF_BLOCKSIZE];
	DWORD buf_size, wr_size;
	iso9660_t* p_iso = NULL;
	udf_t* p_udf = NULL;
	udf_dirent_t *p_udf_root = NULL, *p_udf_file = NULL;
	iso9660_stat_t *p_statbuf = NULL;
	lsn_t lsn;
	HANDLE file_handle = INVALID_HANDLE_VALUE;

	file_handle = CreateFileU(dest_file, GENERIC_READ | GENERIC_WRITE,
		FILE_SHARE_READ, NULL, CREATE_ALWAYS, attributes, NULL);
	if (file_handle == INVALID_HANDLE_VALUE) {
		uprintf("Could not create file %s: %s", dest_file, WindowsErrorString());
		goto out;
	}

	// First try to open as UDF - fallback to ISO if it failed
	p_udf = udf_open(iso);
	if (p_udf == NULL)
		goto try_iso;

	p_udf_root = udf_get_root(p_udf, true, 0);
	if (p_udf_root == NULL) {
		uprintf("Could not locate UDF root directory");
		goto out;
	}
	p_udf_file = udf_fopen(p_udf_root, iso_file);
	if (!p_udf_file) {
		uprintf("Could not locate file %s in ISO image", iso_file);
		goto out;
	}
	file_length = udf_get_file_length(p_udf_file);
	while (file_length > 0) {
		memset(buf, 0, UDF_BLOCKSIZE);
		read_size = udf_read_block(p_udf_file, buf, 1);
		if (read_size < 0) {
			uprintf("Error reading UDF file %s", iso_file);
			goto out;
		}
		buf_size = (DWORD)MIN(file_length, (int64_t)read_size);
		if (!WriteFileWithRetry(file_handle, buf, buf_size, &wr_size, WRITE_RETRIES)) {
			uprintf("Error writing file %s: %s", dest_file, WindowsErrorString());
			goto out;
		}
		file_length -= buf_size;
		r += buf_size;
	}
	goto out;

try_iso:
	// Make sure to enable extensions, else we may not match the name of the file we are looking
	// for since Rock Ridge may be needed to translate something like 'I386_PC' into 'i386-pc'...
	p_iso = iso9660_open_ext(iso, ISO_EXTENSION_MASK);
	if (p_iso == NULL) {
		uprintf("Unable to open image '%s'", iso);
		goto out;
	}

	p_statbuf = iso9660_ifs_stat_translate(p_iso, iso_file);
	if (p_statbuf == NULL) {
		uprintf("Could not get ISO-9660 file information for file %s", iso_file);
		goto out;
	}

	file_length = p_statbuf->total_size;
	for (i = 0; file_length > 0; i++) {
		memset(buf, 0, ISO_BLOCKSIZE);
		lsn = p_statbuf->lsn + (lsn_t)i;
		if (iso9660_iso_seek_read(p_iso, buf, lsn, 1) != ISO_BLOCKSIZE) {
			uprintf("Error reading ISO9660 file %s at LSN %lu", iso_file, (long unsigned int)lsn);
			goto out;
		}
		buf_size = (DWORD)MIN(file_length, ISO_BLOCKSIZE);
		if (!WriteFileWithRetry(file_handle, buf, buf_size, &wr_size, WRITE_RETRIES)) {
			uprintf("Error writing file %s: %s", dest_file, WindowsErrorString());
			goto out;
		}
		file_length -= buf_size;
		r += buf_size;
	}

out:
	safe_closehandle(file_handle);
	if (r == 0)
		DeleteFileU(dest_file);
	iso9660_stat_free(p_statbuf);
	udf_dirent_free(p_udf_root);
	udf_dirent_free(p_udf_file);
	iso9660_close(p_iso);
	udf_close(p_udf);
	return r;
}

/*
 * Extract a file to a buffer. Buffer must be freed by the caller.
 */
uint32_t ReadISOFileToBuffer(const char* iso, const char* iso_file, uint8_t** buf)
{
	uint32_t ret;
	cdio_loglevel_default = CDIO_LOG_WARN;
	ret = ReadISOFileToBufferExt(iso, iso_file, buf, ISO_EXTENSION_MASK);
	cdio_loglevel_default = usb_debug ? CDIO_LOG_INFO : CDIO_LOG_WARN;
	return ret;
}

#define ISO_NB_BLOCKS 16
typedef struct {
	iso9660_t*      p_iso;
	lsn_t           lsn;
	libfat_sector_t sec_start;
	// Use a multi block buffer, to improve sector reads
	uint8_t         buf[ISO_BLOCKSIZE * ISO_NB_BLOCKS];
} iso9660_readfat_private;

/*
 * Read sectors from a FAT img file residing on an ISO-9660 filesystem.
 * NB: This assumes that the img file sectors are contiguous on the ISO.
  */
int iso9660_readfat(intptr_t pp, void *buf, size_t secsize, libfat_sector_t sec)
{
	iso9660_readfat_private* p_private = (iso9660_readfat_private*)pp;

	if (sizeof(p_private->buf) % secsize != 0) {
		uprintf("iso9660_readfat: Sector size %zu is not a divisor of %zu", secsize, sizeof(p_private->buf));
		return 0;
	}

	if ((sec < p_private->sec_start) || (sec >= p_private->sec_start + sizeof(p_private->buf) / secsize)) {
		// Sector being queried is not in our multi block buffer -> Update it
		p_private->sec_start = (((sec * secsize) / ISO_BLOCKSIZE) * ISO_BLOCKSIZE) / secsize;
		if (iso9660_iso_seek_read(p_private->p_iso, p_private->buf,
			p_private->lsn + (lsn_t)((p_private->sec_start * secsize) / ISO_BLOCKSIZE), ISO_NB_BLOCKS)
			!= ISO_NB_BLOCKS * ISO_BLOCKSIZE) {
			uprintf("Error reading ISO-9660 file %s at LSN %lu", img_report.efi_img_path,
				(long unsigned int)(p_private->lsn + (p_private->sec_start * secsize) / ISO_BLOCKSIZE));
			return 0;
		}
	}
	memcpy(buf, &p_private->buf[(sec - p_private->sec_start)*secsize], secsize);
	return (int)secsize;
}

/*
 * Returns TRUE if an EFI bootloader exists in the img.
 */
BOOL HasEfiImgBootLoaders(void* iso, void* udf)
{
	bool found = false;
	return InspectEfiImage(udf == NULL ? (iso9660_t*)iso : NULL, (udf_t*)udf,
		img_report.efi_img_path, &found) == ISO_SCAN_OK && found;
}

BOOL DumpFatDir(void* iso, const char* path, int32_t cluster)
{
	// We don't have concurrent calls to this function, so a static lf_fs is fine
	static struct libfat_filesystem *lf_fs = NULL;
	void* buf;
	char *target = NULL, *name = NULL;
	BOOL ret = FALSE;
	HANDLE handle = NULL;
	DWORD size, written;
	libfat_diritem_t diritem = { 0 };
	libfat_dirpos_t dirpos = { cluster, -1, 0 };
	libfat_sector_t s;
	iso9660_t* p_iso = (iso9660_t*)iso;
	iso9660_stat_t* p_statbuf = NULL;
	iso9660_readfat_private* p_private = NULL;

	if (path == NULL)
		return -1;

	if (cluster == 0) {
		// Root dir => Perform init stuff
		if (iso == NULL || image_path == NULL)
			return FALSE;
		p_statbuf = iso9660_ifs_stat_translate(p_iso, img_report.efi_img_path);
		if (p_statbuf == NULL) {
			uprintf("Could not get ISO-9660 file information for file %s", img_report.efi_img_path);
			goto out;
		}
		p_private = malloc(sizeof(iso9660_readfat_private));
		if (p_private == NULL)
			goto out;
		p_private->p_iso = p_iso;
		p_private->lsn = p_statbuf->lsn;
		p_private->sec_start = 0;
		// Populate our initial buffer
		if (iso9660_iso_seek_read(p_private->p_iso, p_private->buf, p_private->lsn, ISO_NB_BLOCKS) != ISO_NB_BLOCKS * ISO_BLOCKSIZE) {
			uprintf("Error reading ISO-9660 file %s at LSN %lu", img_report.efi_img_path, (long unsigned int)p_private->lsn);
			goto out;
		}
		// Try to skip to first FAT partition, if working with an MBR partitioned image
		if (p_private->buf[0x1fe] == 0x55 && p_private->buf[0x1ff] == 0xaa &&
			p_private->buf[0x1be] == 0x80 && IS_FAT_TYPE(p_private->buf[0x1c2])) {
			p_private->lsn += *((uint32_t*)&p_private->buf[0x1c6]) / 4;
			if (iso9660_iso_seek_read(p_private->p_iso, p_private->buf, p_private->lsn, ISO_NB_BLOCKS) != ISO_NB_BLOCKS * ISO_BLOCKSIZE) {
				uprintf("Error reading ISO-9660 file %s at LSN %lu", img_report.efi_img_path, (long unsigned int)p_private->lsn);
				goto out;
			}
		}
		lf_fs = libfat_open(iso9660_readfat, (intptr_t)p_private);
		if (lf_fs == NULL) {
			uprintf("FAT access error");
			goto out;
		}
	}

	do {
		dirpos.cluster = libfat_dumpdir(lf_fs, &dirpos, &diritem);
		if (dirpos.cluster >= 0) {
			name = wchar_to_utf8(diritem.name);
			target = malloc(strlen(path) + safe_strlen(name) + 2);
			if ((name == NULL) || (target == NULL)) {
				uprintf("Could not allocate buffer");
				goto out;
			}
			strcpy(target, path);
			strcat(target, "\\");
			strcat(target, name);
			if (diritem.attributes & 0x10) {
				// Directory => Create directory
				if (!CreateDirectoryU(target, 0) && (GetLastError() != ERROR_ALREADY_EXISTS)) {
					uprintf("Could not create directory '%s': %s", target, WindowsErrorString());
					continue;
				}
				if (!DumpFatDir(p_iso, target, dirpos.cluster))
					goto out;
			} else if (!PathFileExistsU(target)) {
				// Need to figure out if it's a .conf file (Damn you Solus!!)
				ISO_FILE_PROPS props = { 0 };
				size_t len = strlen(name);
				props.is_conf = ((len > 4) && (stricmp(&name[len - 5], ".conf") == 0));
				uprintf("Extracting: %s (from '%s', %s)", target, img_report.efi_img_path,
					SizeToHumanReadable(diritem.size, FALSE, FALSE));
				handle = CreateFileU(target, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
					NULL, CREATE_ALWAYS, diritem.attributes, NULL);
				if (handle == INVALID_HANDLE_VALUE) {
					uprintf("Could not create '%s': %s", target, WindowsErrorString());
					continue;
				}

				written = 0;
				s = libfat_clustertosector(lf_fs, dirpos.cluster);
				while ((s != 0) && (s < 0xFFFFFFFFULL) && (written < diritem.size)) {
					buf = libfat_get_sector(lf_fs, s);
					if (buf == NULL)
						ErrorStatus = RUFUS_ERROR(ERROR_SECTOR_NOT_FOUND);
					if (IS_ERROR(ErrorStatus))
						goto out;
					size = MIN(LIBFAT_SECTOR_SIZE, diritem.size - written);
					if (!WriteFileWithRetry(handle, buf, size, &size, WRITE_RETRIES) ||
						(size != MIN(LIBFAT_SECTOR_SIZE, diritem.size - written))) {
						uprintf("Could not write '%s': %s", target, WindowsErrorString());
						break;
					}
					written += size;
					s = libfat_nextsector(lf_fs, s);
					// Trust me, you *REALLY* want to invoke libfat_flush() here
					libfat_flush(lf_fs);
				}
				safe_closehandle(handle);
				if (props.is_conf)
					fix_config(target, NULL, NULL, &props);
			}
			safe_free(target);
			safe_free(name);
		}
	// coverity[tainted_data]
	} while (dirpos.cluster >= 0);
	ret = TRUE;

out:
	if (cluster == 0) {
		if (lf_fs != NULL) {
			libfat_close(lf_fs);
			lf_fs = NULL;
		}
		iso9660_stat_free(p_statbuf);
		safe_free(p_private);
	}
	safe_closehandle(handle);
	safe_free(name);
	safe_free(target);
	return ret;
}

static DWORD WINAPI OpticalDiscSaveImageThread(void* param)
{
	BOOL s;
	DWORD rSize, wSize;
	IMG_SAVE* img_save = (IMG_SAVE*)param;
	HANDLE hPhysicalDrive = INVALID_HANDLE_VALUE;
	HANDLE hDestImage = INVALID_HANDLE_VALUE;
	LARGE_INTEGER li;
	uint8_t* buffer = NULL;
	uint64_t wb;
	int i;

	assert(img_save->Type == VIRTUAL_STORAGE_TYPE_DEVICE_ISO);

	PrintInfoDebug(0, MSG_225);
	hPhysicalDrive = CreateFileA(img_save->DevicePath, GENERIC_READ, FILE_SHARE_READ,
		NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
	if (hPhysicalDrive == INVALID_HANDLE_VALUE) {
		ErrorStatus = RUFUS_ERROR(ERROR_OPEN_FAILED);
		goto out;
	}

	// In case someone poked the disc before us
	li.QuadPart = 0;
	if (!SetFilePointerEx(hPhysicalDrive, li, NULL, FILE_BEGIN))
		uprintf("WARNING: Unable to rewind device position - wrong data might be copied!");
	hDestImage = CreateFileU(img_save->ImagePath, GENERIC_WRITE, FILE_SHARE_WRITE, NULL,
		CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hDestImage == INVALID_HANDLE_VALUE) {
		uprintf("Could not open image '%s': %s", img_save->ImagePath, WindowsErrorString());
		ErrorStatus = RUFUS_ERROR(ERROR_OPEN_FAILED);
		goto out;
	}

	buffer = (uint8_t*)_mm_malloc(img_save->BufSize, 16);
	if (buffer == NULL) {
		ErrorStatus = RUFUS_ERROR(ERROR_NOT_ENOUGH_MEMORY);
		uprintf("Could not allocate buffer");
		goto out;
	}

	uprintf("Will use a buffer size of %s", SizeToHumanReadable(img_save->BufSize, FALSE, FALSE));
	uprintf("Saving to image '%s'...", img_save->ImagePath);

	// Don't bother trying for something clever, using double buffering overlapped and whatnot:
	// With Windows' default optimizations, sync read + sync write for sequential operations
	// will be as fast, if not faster, than whatever async scheme you can come up with.
	UpdateProgressWithInfoInit(NULL, FALSE);
	for (wb = 0; ; wb += wSize) {
		// Optical drives do not appear to increment the sectors to read automatically
		li.QuadPart = wb;
		if (!SetFilePointerEx(hPhysicalDrive, li, NULL, FILE_BEGIN))
			uprintf("WARNING: Unable to set device position - wrong data might be copied!");
		s = ReadFile(hPhysicalDrive, buffer,
			(DWORD)MIN(img_save->BufSize, img_save->DeviceSize - wb), &rSize, NULL);
		if (!s) {
			ErrorStatus = RUFUS_ERROR(ERROR_READ_FAULT);
			uprintf("Read error: %s", WindowsErrorString());
			goto out;
		}
		if (rSize == 0)
			break;
		UpdateProgressWithInfo(OP_FORMAT, MSG_261, wb, img_save->DeviceSize);
		for (i = 1; i <= WRITE_RETRIES; i++) {
			CHECK_FOR_USER_CANCEL;
			s = WriteFile(hDestImage, buffer, rSize, &wSize, NULL);
			if ((s) && (wSize == rSize))
				break;
			if (s)
				uprintf("Write error: Wrote %d bytes, expected %d bytes", wSize, rSize);
			else
				uprintf("Write error: %s", WindowsErrorString());
			if (i < WRITE_RETRIES) {
				li.QuadPart = wb;
				uprintf("Retrying in %d seconds...", WRITE_TIMEOUT / 1000);
				Sleep(WRITE_TIMEOUT);
				if (!SetFilePointerEx(hDestImage, li, NULL, FILE_BEGIN)) {
					uprintf("Write error: Could not reset position - %s", WindowsErrorString());
					goto out;
				}
			} else {
				ErrorStatus = RUFUS_ERROR(ERROR_WRITE_FAULT);
				goto out;
			}
			Sleep(200);
		}
		if (i > WRITE_RETRIES)
			goto out;
	}
	if (wb != img_save->DeviceSize) {
		uprintf("Error: wrote %s, expected %s", SizeToHumanReadable(wb, FALSE, FALSE),
			SizeToHumanReadable(img_save->DeviceSize, FALSE, FALSE));
		ErrorStatus = RUFUS_ERROR(ERROR_WRITE_FAULT);
		goto out;
	}
	uprintf("Operation complete (Wrote %s).", SizeToHumanReadable(wb, FALSE, FALSE));

out:
	safe_free(img_save->ImagePath);
	safe_mm_free(buffer);
	safe_closehandle(hDestImage);
	safe_unlockclose(hPhysicalDrive);
	PostMessage(hMainDialog, UM_FORMAT_COMPLETED, (WPARAM)TRUE, 0);
	ExitThread(0);
}

void OpticalDiscSaveImage(void)
{
	static IMG_SAVE img_save = { 0 };
	char filename[33] = "disc_image.iso";
	EXT_DECL(img_ext, filename, __VA_GROUP__("*.iso"), __VA_GROUP__(lmprintf(MSG_036)));

	if (op_in_progress || (format_thread != NULL))
		return;

	img_save.Type = VIRTUAL_STORAGE_TYPE_DEVICE_ISO;
	if (!GetOpticalMedia(&img_save)) {
		uprintf("No dumpable optical media found.");
		return;
	}
	// Adjust the buffer size according to the disc size so that we get a decent speed.
	for (img_save.BufSize = 32 * MB;
		(img_save.BufSize > 8 * MB) && (img_save.DeviceSize <= img_save.BufSize * 64);
		img_save.BufSize /= 2);
	if ((img_save.Label != NULL) && (img_save.Label[0] != 0))
		static_sprintf(filename, "%s.iso", img_save.Label);

	img_save.ImagePath = FileDialog(TRUE, NULL, &img_ext, 0);
	if (img_save.ImagePath == NULL)
		return;

	uprintf("ISO media size %s", SizeToHumanReadable(img_save.DeviceSize, FALSE, FALSE));
	SendMessage(hMainDialog, UM_PROGRESS_INIT, 0, 0);
	ErrorStatus = 0;
	// Disable all controls except cancel
	EnableControls(FALSE, FALSE);
	InitProgress(TRUE);
	format_thread = CreateThread(NULL, 0, OpticalDiscSaveImageThread, &img_save, 0, NULL);
	if (format_thread != NULL) {
		uprintf("\r\nSave to ISO operation started");
		PrintInfo(0, -1);
		SendMessage(hMainDialog, UM_TIMER_START, 0, 0);
	} else {
		uprintf("Unable to start ISO save thread");
		ErrorStatus = RUFUS_ERROR(APPERR(ERROR_CANT_START_THREAD));
		safe_free(img_save.ImagePath);
		PostMessage(hMainDialog, UM_FORMAT_COMPLETED, (WPARAM)FALSE, 0);
	}
}

// Create an ISO image from the currently selected drive, using oscdimg.exe
DWORD WINAPI IsoSaveImageThread(void* param)
{
	DWORD r = ERROR_NOT_FOUND;
	HANDLE exe = INVALID_HANDLE_VALUE;
	IMG_SAVE* img_save = (IMG_SAVE*)param;
	char cmd[2 * KB], letters[27], *label;

	if (!GetDriveLabel(SelectedDrive.DeviceNumber, letters, &label, TRUE) || letters[0] == '\0')
		goto out;

	// Get a lock and validate that the oscdimg.exe has not been tampered with
	static_sprintf(cmd, "%s\\%s\\oscdimg_%s.exe", app_data_dir, FILES_DIR, APPLICATION_ARCH);
	exe = CreateFileU(cmd, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (exe == INVALID_HANDLE_VALUE) {
		uprintf("ERROR: Could not lock 'oscdimg.exe'");
		r = GetLastError();
		goto out;
	}
	if (!FileMatchesHash(cmd, OSCDIMG_HASH)) {
		uprintf("ERROR: Existing 'oscdimg.exe' hash does not match expected value!");
		r = ERROR_INVALID_IMAGE_HASH;
		goto out;
	}

	// UDF labels cannot be more than 32 characters (and if we have more than 32 chars we are
	// using the static modifiable char buffer from GetDriveLabel(), so we can alter it).
	if (strlen(label) > 32)
		label[32] = '\0';

	// Save to UDF only, as Microsoft's implementation of ISO-9660 doesn't support multiextent
	// and produces BROKEN images if you try to add files larger than 4 GB.
	// Plus ISO-9660/Joliet limits labels to 16 characters and has issues with long paths.
	static_sprintf(cmd, "\"%s\\%s\\oscdimg_%s.exe\" -g -h -k -l\"%s\" -m -u2 -udfver102 %c:\\ \"%s\"",
		app_data_dir, FILES_DIR, APPLICATION_ARCH, label, letters[0], img_save->ImagePath);
	uprintf("Running command: '%s'", cmd);
	// For detecting typical oscdimg commandline progress report of type: "\r15.5% complete"
	r = RunCommandWithProgress(cmd, sysnative_dir, FALSE, MSG_261, ".*\r([0-9\\.]+)% complete.*");

out:
	safe_closehandle(exe);
	if (r != 0 && !IS_ERROR(ErrorStatus)) {
		SetLastError(r);
		uprintf("Failed to write ISO image: %s", WindowsErrorString());
		ErrorStatus = RUFUS_ERROR(SCODE_CODE(r));
	}
	PostMessage(hMainDialog, UM_FORMAT_COMPLETED, (WPARAM)TRUE, 0);
	if (!IS_ERROR(ErrorStatus))
		uprintf("Saved '%s'", img_save->ImagePath);
	safe_free(img_save->DevicePath);
	safe_free(img_save->ImagePath);
	ExitThread(r);
}
