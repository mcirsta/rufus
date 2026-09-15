/*
 * Rufus: The Reliable USB Formatting Utility
 * ISO/UDF traversal and read-only scan accounting extracted from iso.c
 * Copyright © 2011-2026 Pete Batard <pete@akeo.ie>
 * Based on libcdio's ISO samples:
 * Copyright © 2003-2014 Rocky Bernstein <rocky@gnu.org>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "iso_scan.h"
#include "image_file.h"

#include <stdio.h>
#include <string.h>

extern void uprintf(const char* format, ...);

static int GetScanPath(const char* path, const iso9660_stat_t* entry,
	const ISO_TREE_WALK* walk, char* fullpath, size_t capacity, const char** basename)
{
	char name[ISO_SCAN_PATH_MAX];
	int length, prefix;

	if (strlen(entry->filename) >= sizeof(name))
		return ISO_SCAN_ERROR;
	if (entry->rr.b3_rock == yep && walk->enable_rockridge)
		strcpy(name, entry->filename);
	else
		iso9660_name_translate_ext(entry->filename, name, walk->joliet_level);
	prefix = snprintf(fullpath, capacity, "%s/", path);
	if (prefix < 0 || (size_t)prefix >= capacity)
		return ISO_SCAN_ERROR;
	length = snprintf(fullpath + prefix, capacity - (size_t)prefix, "%s", name);
	if (length < 0 || (size_t)length >= capacity - (size_t)prefix) {
		uprintf("ISO path exceeds the supported scan length: %s/%s", path, name);
		return ISO_SCAN_ERROR;
	}
	*basename = fullpath + prefix;
	return ISO_SCAN_OK;
}

static void CheckScanSymlink(iso9660_t* iso, const char* path,
	const iso9660_stat_t* entry, ISO_SCAN_STATE* state)
{
	char target_path[ISO_SCAN_PATH_MAX];
	iso9660_stat_t* target;
	int length;

	if (entry->total_size != 0 || entry->rr.psz_symlink == NULL)
		return;
	// Preserve Rufus's extra space allowance for duplicated firmware symlinks.
	if (strcmp(path, "/firmware") == 0) {
		length = snprintf(target_path, sizeof(target_path), "%s/%s", path, entry->rr.psz_symlink);
		if (length < 0 || (size_t)length >= sizeof(target_path))
			return;
		target = iso9660_ifs_stat_translate(iso, target_path);
		if (target != NULL) {
			state->extra_blocks += target->total_size / ISO_BLOCKSIZE +
				(target->total_size % ISO_BLOCKSIZE != 0);
			iso9660_stat_free(target);
		}
	} else if (strcmp(entry->filename, "live") == 0 &&
		strcmp(entry->rr.psz_symlink, "casper") == 0) {
		// Mint LMDE needs working symbolic links and therefore NTFS.
		state->report->needs_ntfs = true;
	}
}

static int RecordScanFile(const char* path, const char* basename,
	const char* fullpath, uint64_t size, ISO_SCAN_STATE* state)
{
	ISO_FILE_PROPS props;
	uint64_t blocks;

	if (size > INT64_MAX)
		return ISO_SCAN_ERROR;
	GetIsoFileProps(path, (int64_t)size, basename, &props);
	if (props.is_config_path && StrArrayAdd(state->config_path, fullpath, true) < 0)
		return ISO_SCAN_ERROR;
	CheckIsoScanFile(state->report, path, (int64_t)size, basename, fullpath, &props);
	if (props.is_ldlinux_c32)
		state->has_ldlinux_c32 = true;
	if (props.is_isolinux && StrArrayAdd(state->isolinux_path, fullpath, true) < 0)
		return ISO_SCAN_ERROR;
	blocks = size / ISO_BLOCKSIZE + (size % ISO_BLOCKSIZE != 0);
	if (blocks > UINT64_MAX - state->total_blocks)
		return ISO_SCAN_ERROR;
	state->total_blocks += blocks;
	state->files++;
	return ISO_SCAN_OK;
}

int WalkIsoFiles(iso9660_t* iso, const char* path, const ISO_TREE_WALK* walk)
{
	CdioISO9660FileList_t* entries;
	CdioListNode_t* node;
	const iso9660_stat_t* entry;
	char fullpath[ISO_SCAN_PATH_MAX];
	ISO_TREE_ENTRY item = { 0 };
	int result = ISO_SCAN_OK;

	if (iso == NULL || path == NULL || walk == NULL || walk->visit == NULL)
		return ISO_SCAN_ERROR;
	if (IsIsoScanCancelled())
		return ISO_SCAN_CANCELLED;
	entries = iso9660_ifs_readdir(iso, path);
	if (entries == NULL) {
		uprintf("Could not access directory %s", path);
		return ISO_SCAN_ERROR;
	}
	_CDIO_LIST_FOREACH(node, entries) {
		if (IsIsoScanCancelled()) {
			result = ISO_SCAN_CANCELLED;
			break;
		}
		entry = (const iso9660_stat_t*)_cdio_list_node_data(node);
		item.iso = entry;
		item.parent = path;
		if (entry->rr.b3_rock == yep && walk->enable_rockridge &&
			(entry->rr.u_su_fields & ISO_ROCK_SUF_PL)) {
			item.kind = ISO_TREE_DEEP;
			item.name = entry->filename;
			item.path = path;
			result = walk->visit(&item, walk->context);
			break;
		}
		if (strcmp(entry->filename, ".") == 0 || strcmp(entry->filename, "..") == 0)
			continue;
		result = GetScanPath(path, entry, walk, fullpath, sizeof(fullpath), &item.name);
		if (result != ISO_SCAN_OK)
			break;
		item.path = fullpath;
		item.kind = entry->type == _STAT_DIR ? ISO_TREE_DIRECTORY : ISO_TREE_FILE;
		result = walk->visit(&item, walk->context);
		if (result != ISO_SCAN_OK)
			break;
		if (entry->type == _STAT_DIR) {
			result = WalkIsoFiles(iso, fullpath, walk);
			if (result > 0)
				break;
			if (result < 0) {
				result = ISO_SCAN_OK; // Preserve Rufus's stop of this parent directory.
				break;
			}
			item.kind = ISO_TREE_DIRECTORY_END;
			result = walk->visit(&item, walk->context);
			if (result != ISO_SCAN_OK)
				break;
		}
	}
	iso9660_filelist_free(entries);
	return result;
}

int WalkUdfFiles(udf_dirent_t* directory, const char* path, const ISO_TREE_WALK* walk)
{
	udf_dirent_t* child;
	char fullpath[ISO_SCAN_PATH_MAX];
	ISO_TREE_ENTRY item = { 0 };
	int length, result = ISO_SCAN_ERROR;

	if (directory == NULL || path == NULL || walk == NULL || walk->visit == NULL)
		goto out;
	for (;;) {
		if (IsIsoScanCancelled()) {
			result = ISO_SCAN_CANCELLED;
			break;
		}
		// libcdio consumes the iterator on EOF/error. Zero remaining bytes
		// alone means EOF; NULL while bytes remain is an unreadable directory.
		if (directory->dir_left == 0) {
			result = ISO_SCAN_OK;
			break;
		}
		directory = udf_readdir(directory);
		if (directory == NULL)
			break;
		if (directory->b_parent)
			continue;
		item.name = udf_get_filename(directory);
		if (item.name == NULL || item.name[0] == 0 || strchr(item.name, '/') != NULL ||
			strcmp(item.name, ".") == 0 || strcmp(item.name, "..") == 0)
			break;
		length = snprintf(fullpath, sizeof(fullpath), "%s/%s", path, item.name);
		if (length < 0 || (size_t)length >= sizeof(fullpath)) {
			uprintf("UDF path exceeds the supported scan length: %s/%s", path, item.name);
			break;
		}
		item.path = fullpath;
		item.parent = path;
		item.udf = directory;
		item.kind = udf_is_dir(directory) ? ISO_TREE_DIRECTORY : ISO_TREE_FILE;
		result = walk->visit(&item, walk->context);
		if (result != ISO_SCAN_OK)
			break;
		if (item.kind == ISO_TREE_DIRECTORY) {
			child = udf_opendir(directory);
			if (child == NULL) {
				result = ISO_SCAN_ERROR;
				break;
			}
			result = WalkUdfFiles(child, fullpath, walk);
			if (result == ISO_SCAN_OK) {
				item.kind = ISO_TREE_DIRECTORY_END;
				result = walk->visit(&item, walk->context);
			}
		}
		if (result != ISO_SCAN_OK)
			break;
		result = ISO_SCAN_ERROR;
	}
out:
	udf_dirent_free(directory);
	if (result == ISO_SCAN_ERROR)
		uprintf("Could not traverse UDF directory %s", path == NULL ? "" : path);
	return result;
}

typedef struct {
	ISO_SCAN_STATE* state;
	iso9660_t* iso;
} ISO_SCAN_CONTEXT;

static int VisitScanEntry(const ISO_TREE_ENTRY* entry, void* context)
{
	ISO_SCAN_CONTEXT* scan = context;
	ISO_SCAN_STATE* state = scan->state;
	int result;
	uint64_t size;

	if (entry->kind == ISO_TREE_DIRECTORY_END)
		return ISO_SCAN_OK;
	if (entry->kind == ISO_TREE_DEEP) {
		if (!state->report->has_deep_directories)
			uprintf("  Note: The selected ISO uses Rock Ridge 'deep directories'.\r\n"
				"  Because of this, it may take a very long time to scan or extract...");
		state->report->has_deep_directories = true;
		state->total_blocks++;
		return -1;
	}
	if (entry->iso != NULL && entry->iso->rr.b3_rock == yep && state->enable_rockridge) {
		if (strlen(entry->iso->filename) > 64)
			state->report->has_long_filename = true;
		if (entry->iso->rr.psz_symlink != NULL)
			state->report->has_symlinks = SYMLINKS_RR;
	}
	if (entry->udf != NULL && entry->udf->fe.icb_tag.file_type == ICBTAG_FILE_TYPE_SYMLINK)
		state->report->has_symlinks = SYMLINKS_UDF;
	if (entry->kind == ISO_TREE_DIRECTORY)
		return ISO_SCAN_OK;
	size = entry->iso != NULL ? entry->iso->total_size : udf_get_file_length(entry->udf);
	result = RecordScanFile(entry->parent, entry->name, entry->path, size, state);
	if (result == ISO_SCAN_OK && entry->iso != NULL && entry->iso->rr.b3_rock == yep && state->enable_rockridge)
		CheckScanSymlink(scan->iso, entry->parent, entry->iso, state);
	return result;
}

static bool ValidScanState(const ISO_SCAN_STATE* state)
{
	return state != NULL && state->report != NULL && state->config_path != NULL &&
		state->isolinux_path != NULL && state->config_path->String != NULL && state->isolinux_path->String != NULL;
}

int ScanIsoFiles(iso9660_t* iso, const char* path, ISO_SCAN_STATE* state)
{
	ISO_SCAN_CONTEXT context = { state, iso };
	ISO_TREE_WALK walk = { VisitScanEntry, &context, false, 0 };

	if (!ValidScanState(state))
		return ISO_SCAN_ERROR;
	walk.enable_rockridge = state->enable_rockridge;
	walk.joliet_level = state->joliet_level;
	return WalkIsoFiles(iso, path, &walk);
}

int ScanUdfFiles(udf_dirent_t* directory, const char* path, ISO_SCAN_STATE* state)
{
	ISO_SCAN_CONTEXT context = { state, NULL };
	ISO_TREE_WALK walk = { VisitScanEntry, &context, false, 0 };

	if (!ValidScanState(state)) {
		udf_dirent_free(directory);
		return ISO_SCAN_ERROR;
	}
	return WalkUdfFiles(directory, path, &walk);
}
