/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "iso_extract.h"
#include "ntfs_attributes.h"
#include "target_io.h"
#include "iso_wim_split.h"
#include "config_patch.h"
#include "image_source.h"
#include "image_file.h"
#include "image_policy.h"
#include "iso_file.h"
#include "rufus_reporting.h"
#include "progress_policy.h"
#include "hash_native.h"
#include "embedded_efi_files.h"
#include "iso_links.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <cdio/udf_time.h>
#include <time.h>
#include <unistd.h>

#define EXTRACT_BUFFER_SIZE (1024 * 1024)
#define EXTRACT_CONFIG_MAX_PATCHES (ISO_CONFIG_MAX_PATCHES + 1)
#define INSTALL_WIM_PATH "/sources/install.wim"

extern void uprintf(const char* format, ...);

typedef struct iso_pending_link {
	char path[ISO_SCAN_PATH_MAX];
	ISO_LINK_PLAN plan;
	struct iso_pending_link* next;
} ISO_PENDING_LINK;

typedef struct {
	const char* image;
	IMAGE_FILE_SOURCE source;
	ISO_ANALYSIS_SOURCE view;
	RUFUS_IMG_REPORT report;
	CONFIG_TEXT_PATCH patches[ISO_CONFIG_MAX_PATCHES];
	CONFIG_TEXT_PATCH freenas;
	char volume_source[CONFIG_VOLUME_PATH_CAPACITY];
	char volume_replacement[CONFIG_VOLUME_PATH_CAPACITY];
	size_t patch_count;
	char* iso_label;
	char* usb_label;
	ISO_EXTRACT_RESULT* result;
	int root;
	int error;
	bool preflight;
	bool rockridge;
	bool split_install_wim;
	bool persistence;
	bool preserve_timestamps;
	bool ntfs_compression;
	bool windows_links;
	bool embedded_available;
	ISO_PENDING_LINK* links;
	unsigned pending_links;
	const ISO_BOOT_RESOURCES* boot_resources;
	bool has_source_swm;
	unsigned split_sources;
	unsigned planned_split_parts;
	uint64_t expected_bytes;
	uint64_t expected_files;
	uint64_t total_blocks;
	uint64_t copied_blocks;
	uint64_t max_file_bytes;
	uint8_t* buffer;
	const ISO_EXTRACT_EVENTS* events;
} ISO_EXTRACT_CONTEXT;

static void PreserveEntryTimes(const ISO_EXTRACT_CONTEXT* context, const ISO_TREE_ENTRY* entry, int fd)
{
	if (!context->preserve_timestamps)
		return;
	struct timespec times[2] = { { 0, 0 }, { 0, 0 } };
	if (entry->iso != NULL) {
		struct tm value = entry->iso->tm;
		times[0].tv_sec = times[1].tv_sec = mktime(&value);
	} else if (entry->udf != NULL) {
		times[0].tv_sec = udf_get_access_time(entry->udf);
		times[1].tv_sec = udf_get_modification_time(entry->udf);
	} else {
		return;
	}
	// As on Windows, optional metadata failure is logged. The subsequent sync
	// remains mandatory and propagates failure to the complete creation job.
	if (futimens(fd, times) != 0)
		uprintf("  Could not set timestamp for '%s': %s", entry->path, strerror(errno));
}

static bool ValidImagePath(const char* path)
{
	const char* end;
	size_t size;

	if (path == NULL || path[0] != '/')
		return false;
	bool valid_length = path[1] != 0 && strlen(path) < ISO_SCAN_PATH_MAX;
	if (!valid_length)
		return false;
	for (path++; *path; path = end + 1) {
		end = strchr(path, '/');
		size = end == NULL ? strlen(path) : (size_t)(end - path);
		bool dot_directory = size == 1 && path[0] == '.';
		bool parent_directory = size == 2 && path[0] == '.' && path[1] == '.';
		if (size == 0 || dot_directory || parent_directory)
			return false;
		if (end == NULL)
			return true;
	}
	return false;
}

static int SyncOutput(int fd);

/* Open each parent relative to the retained root. Existing symlinks can never
 * redirect creation. The caller owns this private directory during extraction. */
static int OpenOutputParent(int root, const char* path)
{
	char parent[ISO_SCAN_PATH_MAX];
	char* name;
	char* next;
	int fd, child, error;

	strcpy(parent, path + 1);
	next = strrchr(parent, '/');
	if (next != NULL)
		*next = 0;
	else
		parent[0] = 0;
	fd = openat(root, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	for (name = parent; *name; name = next + 1) {
		next = strchr(name, '/');
		if (next != NULL)
			*next = 0;
		child = openat(fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		if (child < 0 && errno == ENOENT) {
			if (mkdirat(fd, name, 0755) != 0 && errno != EEXIST) { error = errno; close(fd); errno = error; return -1; }
			error = SyncOutput(fd);
			if (error != 0) { close(fd); errno = error; return -1; }
			child = openat(fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		}
		error = errno;
		close(fd);
		if (child < 0) {
			errno = error;
			return -1;
		}
		fd = child;
		if (next == NULL)
			break;
	}
	return fd;
}

static int SyncOutput(int fd)
{
	while (fsync(fd) != 0) {
		if (errno != EINTR)
			return errno;
		if (IsIsoScanCancelled())
			return ECANCELED;
	}
	return 0;
}

static int WriteOutput(int fd, const uint8_t* buffer, size_t size, uint64_t* bytes_written)
{
	ssize_t count;
	size_t done = 0;

	while (done < size) {
		if (IsIsoScanCancelled())
			return ECANCELED;
		count = write(fd, buffer + done, size - done);
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			return count == 0 ? EIO : errno;
		done += (size_t)count;
		*bytes_written += (uint64_t)count;
	}
	return 0;
}

static int CopyFileData(ISO_EXTRACT_CONTEXT* context, ISO_FILE_READER* reader, int output, HASH_CONTEXT* md5)
{
	uint64_t offset = 0;
	size_t size;
	int error, status;

	while (offset < reader->size) {
		error = CheckImageFileSource(&context->source, context->image);
		if (error != 0)
			return error;
		size = reader->size - offset > EXTRACT_BUFFER_SIZE ? EXTRACT_BUFFER_SIZE : (size_t)(reader->size - offset);
		status = ReadIsoFileRange(reader, offset, context->buffer, size);
		if (status != ISO_SCAN_OK)
			return status == ISO_SCAN_CANCELLED ? ECANCELED : EIO;
		if (md5 != NULL)
			hash_write[HASH_MD5](md5, context->buffer, size);
		error = WriteOutput(output, context->buffer, size, &context->result->bytes_written);
		if (error != 0)
			return error;
		offset += size;
		/* Rufus counts 2048-byte ISO/UDF blocks, rounding each file's tail. */
		context->copied_blocks += size / ISO_BLOCKSIZE + (size % ISO_BLOCKSIZE != 0);
		_UpdateProgressWithInfo(OP_FILE_COPY, 0, context->copied_blocks, context->total_blocks, 0);
	}
	return 0;
}

/* Match Rufus's CREATE_ALWAYS for ordinary output files. Delay truncation of
 * an existing inode until it is checked, so a link or special entry is untouched. */
static int OpenOutputFile(int parent, const char* name)
{
	const int flags = O_WRONLY | O_NOFOLLOW | O_CLOEXEC;
	struct stat info;
	int error = 0;
	int fd = openat(parent, name, flags | O_CREAT | O_EXCL, 0644);

	if (fd >= 0 || errno != EEXIST)
		return fd;
	fd = openat(parent, name, flags | O_NONBLOCK);
	if (fd < 0)
		return -1;
	if (fstat(fd, &info) != 0)
		error = errno;
	else if (!S_ISREG(info.st_mode))
		error = ENOTSUP;
	else if (info.st_nlink != 1)
		error = EMLINK;
	else if (IsIsoScanCancelled())
		error = ECANCELED;
	else if (ftruncate(fd, 0) != 0)
		error = errno;
	if (error != 0) {
		close(fd);
		errno = error;
		return -1;
	}
	return fd;
}

static int PrepareOutputDirectory(int parent, const char* name)
{
	int directory;

	if (mkdirat(parent, name, 0755) == 0)
		return 0;
	if (errno != EEXIST)
		return errno;
	// Windows reuses an existing directory. Verify that it is a directory here,
	// including for an empty source directory with no child that would open it.
	directory = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (directory < 0)
		return errno;
	return close(directory) == 0 ? 0 : errno;
}

static int CreateRootSyslinuxConfig(ISO_EXTRACT_CONTEXT* context)
{
	ISO_SYSLINUX_CONFIG config;
	struct stat info;
	int output, error;
	bool exists;
	const char* name = "syslinux.cfg";
	const char* backup = "syslinux.org";

	if (!HAS_SYSLINUX(context->report))
		return 0;
	strcpy(context->result->path, "/syslinux.cfg");
	error = CheckImageFileSource(&context->source, context->image);
	if (error != 0 || IsIsoScanCancelled())
		return error != 0 ? error : ECANCELED;
	exists = fstatat(context->root, name, &info, AT_SYMLINK_NOFOLLOW) == 0;
	if (!exists && errno != ENOENT)
		return errno;
	if (exists) {
		if (!S_ISREG(info.st_mode))
			return ENOTSUP;
		if (info.st_nlink != 1)
			return EMLINK;
	}
	GetIsoSyslinuxConfig(&context->report, exists, &config);
	if (!config.create)
		return 0;
	error = CheckImageFileSource(&context->source, context->image);
	if (error != 0 || IsIsoScanCancelled())
		return error != 0 ? error : ECANCELED;
	if (config.backup) {
		// Windows rename does not replace an existing backup. On Linux, report
		// failure instead of continuing and losing the config we could not save.
		if (renameat2(context->root, name, context->root, backup, RENAME_NOREPLACE) != 0) {
			error = errno;
			const bool unsupported = error == ENOSYS || error == EINVAL || error == EOPNOTSUPP;
			if (!unsupported)
				return error;
			// NTFS-3G lacks RENAME_NOREPLACE. A hard link also creates the
			// backup without replacing anything. Keep both names if unlink
			// fails; this private extraction is then incomplete, not resumed.
			if (linkat(context->root, name, context->root, backup, 0) != 0)
				return errno;
			if (unlinkat(context->root, name, 0) != 0)
				return errno;
		}
		uprintf("Renamed: /%s to /%s", name, backup);
	}
	output = openat(context->root, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
	if (output < 0)
		return errno;
	error = WriteOutput(output, (const uint8_t*)config.text, strlen(config.text), &context->result->generated_bytes);
	if (error == 0)
		error = SyncOutput(output);
	if (close(output) != 0 && error == 0)
		error = errno;
	if (error != 0) {
		if (unlinkat(context->root, name, 0) != 0)
			uprintf("Could not remove incomplete /%s: %s", name, strerror(errno));
	} else {
		uprintf("Created: /%s -> %s", name, context->report.cfg_path);
		if (context->events != NULL)
			error = context->events->modified(context->events->context, "/syslinux.cfg");
	}
	return error;
}

static size_t GetFileConfigPatches(const ISO_EXTRACT_CONTEXT* context, const ISO_FILE_PROPS* props,
	CONFIG_TEXT_PATCH patches[EXTRACT_CONFIG_MAX_PATCHES])
{
	size_t count = 0;
	if (props->is_cfg || props->is_conf) {
		count = context->patch_count;
		memcpy(patches, context->patches, count * sizeof(*patches));
	}
	if (props->is_grub_cfg && context->freenas.token != NULL)
		patches[count++] = context->freenas;
	return count;
}

/* Validate the same ordered transformations before any destination writes.
 * Only config contents are buffered; the ordinary tree remains streaming. */
static bool HasPersistenceConfig(const ISO_EXTRACT_CONTEXT* context, const ISO_FILE_PROPS* props)
{
    return context->persistence && HAS_PERSISTENCE(context->report) &&
        (props->is_grub_cfg || props->is_menu_cfg || props->is_syslinux_cfg);
}

static int CheckConfigEntry(ISO_EXTRACT_CONTEXT* context, const ISO_TREE_ENTRY* entry, uint64_t size,
	const CONFIG_TEXT_PATCH* patches, size_t patch_count, bool persistence, bool grub)
{
	ISO_FILE_READER reader;
	uint8_t* bytes;
	uint64_t offset = 0;
	size_t output_size = 0;
	int status, error;

	if (size > CONFIG_PATCH_MAX_BYTES)
		return EFBIG;
	status = OpenIsoFileReader(&reader, context->view.iso, context->view.udf, entry->path);
	error = status == ISO_SCAN_OK ? 0 : (status == ISO_SCAN_CANCELLED ? ECANCELED : EIO);
	if (error == 0) {
		bool same_size = reader.size == size;
		bool same_location = entry->iso == NULL || reader.lsn == entry->iso->lsn;
		if (!same_size || !same_location)
			error = EIO;
	}
	bytes = error == 0 ? malloc(size == 0 ? 1 : (size_t)size) : NULL;
	if (error == 0 && bytes == NULL)
		error = ENOMEM;
	while (error == 0 && offset < size) {
		error = CheckImageFileSource(&context->source, context->image);
		if (error != 0)
			break;
		size_t count = size - offset > EXTRACT_BUFFER_SIZE ? EXTRACT_BUFFER_SIZE : (size_t)(size - offset);
		status = ReadIsoFileRange(&reader, offset, bytes + offset, count);
		if (status != ISO_SCAN_OK)
			error = status == ISO_SCAN_CANCELLED ? ECANCELED : EIO;
		offset += count;
	}
	if (error == 0)
		error = CheckIsoConfigText(bytes, (size_t)size, patches, patch_count, persistence, grub, &output_size);
	if (error == 0 && context->max_file_bytes != 0 && output_size > context->max_file_bytes)
		error = EFBIG;
	free(bytes);
	CloseIsoFileReader(&reader);
	return error;
}

static int CopyConfigData(ISO_EXTRACT_CONTEXT* context, int input, int output, uint64_t size)
{
	uint64_t offset = 0;
	ssize_t count;
	int error;

	while (offset < size) {
		error = CheckImageFileSource(&context->source, context->image);
		if (error != 0 || IsIsoScanCancelled())
			return error != 0 ? error : ECANCELED;
		size_t available = size - offset > EXTRACT_BUFFER_SIZE ? EXTRACT_BUFFER_SIZE : (size_t)(size - offset);
		count = pread(input, context->buffer, available, (off_t)offset);
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			return count == 0 ? EIO : errno;
		error = WriteOutput(output, context->buffer, (size_t)count, &context->result->generated_bytes);
		if (error != 0)
			return error;
		offset += (uint64_t)count;
	}
	return SyncOutput(output);
}

/* Copy the already-patched file, as CopyFileA(..., TRUE) does. A pre-existing
 * regular destination is kept. Other failures stop the job and remove only a
 * new incomplete copy; no source config or existing destination is truncated. */
static int DuplicateEfiConfig(ISO_EXTRACT_CONTEXT* context, int parent, const char* source, const char* name)
{
	struct stat info;
	int input = -1, output = -1;
	int error = CheckImageFileSource(&context->source, context->image);

	if (error != 0 || IsIsoScanCancelled())
		return error != 0 ? error : ECANCELED;
	input = openat(parent, source, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
	if (input < 0)
		return errno;
	if (fstat(input, &info) != 0)
		error = errno;
	else if (!S_ISREG(info.st_mode) || info.st_size < 0)
		error = ENOTSUP;
	else if (info.st_nlink != 1)
		error = EMLINK;
	if (error != 0)
		goto out;
	output = openat(parent, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, info.st_mode & 0777);
	if (output < 0) {
		error = errno;
		if (error != EEXIST)
			goto out;
		if (fstatat(parent, name, &info, AT_SYMLINK_NOFOLLOW) != 0)
			error = errno;
		else if (!S_ISREG(info.st_mode))
			error = ENOTSUP;
		else if (info.st_nlink != 1)
			error = EMLINK;
		else {
			error = 0;
			uprintf("  Keeping existing EFI config: %s", name);
		}
		goto out;
	}
	error = CopyConfigData(context, input, output, (uint64_t)info.st_size);
	if (error == 0)
		error = CheckImageFileSource(&context->source, context->image);
out:
	if (close(input) != 0 && error == 0)
		error = errno;
	if (output >= 0) {
		if (close(output) != 0 && error == 0)
			error = errno;
		if (error != 0) {
			if (unlinkat(parent, name, 0) != 0)
				uprintf("Could not remove incomplete EFI config %s: %s", name, strerror(errno));
		} else
			uprintf("  Duplicated EFI config: %s to %s", source, name);
	}
	return error;
}

static int FixExtractedConfig(ISO_EXTRACT_CONTEXT* context, const ISO_TREE_ENTRY* entry,
	uint64_t size, int parent, const char* name, const char* output_path)
{
	ISO_FILE_PROPS props;
	CONFIG_TEXT_PATCH patches[EXTRACT_CONFIG_MAX_PATCHES];
	char duplicate[ISO_SYSLINUX_CONFIG_NAME_CAPACITY];
	bool modified = false;
	int error = 0;

	GetIsoFileProps(entry->parent, (int64_t)size, entry->name, &props);
	size_t count = GetFileConfigPatches(context, &props, patches);
	if (count != 0 || HasPersistenceConfig(context, &props))
		error = ReplaceIsoConfigTokensAt(parent, name, patches, count, HasPersistenceConfig(context, &props), props.is_grub_cfg, &modified);
	if (modified) {
		context->result->patched_files++;
		uprintf("  Patched config: %s", entry->path);
		if (error == 0 && context->events != NULL)
			error = context->events->modified(context->events->context, output_path);
	}
	// GRUB configs and isolinux.cfg are disjoint classifications. The FreeNAS
	// pass is last for GRUB; EFI duplication sees the completed label passes.
	if (error == 0 && GetEfiSyslinuxConfigName(&context->report, &props, entry->parent, entry->name, duplicate))
		error = DuplicateEfiConfig(context, parent, name, duplicate);
	return error;
}

typedef struct {
	ISO_EXTRACT_CONTEXT* extraction;
	const char* original;
} SPLIT_INTEGRITY_CONTEXT;

static int RecordSplitPart(void* opaque, int stage, const char* name)
{
	SPLIT_INTEGRITY_CONTEXT* context = opaque;
	const ISO_EXTRACT_EVENTS* events = context->extraction->events;
	return events->split_part(events->context, context->original, stage, name);
}

static int ReplaceObsoleteModule(ISO_EXTRACT_CONTEXT* context, const ISO_TREE_ENTRY* entry, int parent, const char* name, const char* path)
{
	if (!context->boot_resources || (context->report.sl_version >> 8) >= 5) return 0;
	uint64_t size = entry->iso ? entry->iso->total_size : udf_get_file_length(entry->udf);
	ISO_FILE_PROPS props; GetIsoFileProps(entry->parent,(int64_t)size,entry->name,&props);
	for (int i = 0; i < 2; ++i) {
		size_t count = context->boot_resources->old_c32_bytes[i];
		if (!props.is_old_c32[i] || !count) continue;
		if (!context->boot_resources->old_c32[i] || count > 512 * 1024) return EINVAL;
		int output = OpenOutputFile(parent,name); if (output < 0) return errno;
		int error = WriteOutput(output,context->boot_resources->old_c32[i],count,&context->result->generated_bytes);
		if (!error) error = SyncOutput(output);
		if (close(output) != 0 && !error) error = errno;
		if (!error && context->events) error = context->events->modified(context->events->context,path);
		return error;
	}
	return 0;
}

static int ExtractFile(ISO_EXTRACT_CONTEXT* context, const ISO_TREE_ENTRY* entry,
	int parent, const char* output_name, const char* output_path)
{
	ISO_FILE_READER reader = { 0 };
	HASH_CONTEXT md5 = { 0 };
	const bool hashing = context->events != NULL && context->events->hash_md5;
	if (hashing)
		hash_init[HASH_MD5](&md5);
	uint64_t size = entry->iso != NULL ? entry->iso->total_size : udf_get_file_length(entry->udf);
	int output = -1;
	int status;
	if (entry->iso != NULL) {
		// Joliet can give distinct records the same translated name. Read the
		// visited record's extent, as Windows does, rather than looking it up
		// again by an ambiguous path. The shared range reader bounds the I/O.
		const bool valid_extent = entry->iso->lsn >= 0 || size == 0;
		status = valid_extent && size <= INT64_MAX ? ISO_SCAN_OK : ISO_SCAN_ERROR;
		reader.iso = context->view.iso;
		reader.lsn = entry->iso->lsn;
		reader.size = size;
	} else {
		status = OpenIsoFileReader(&reader, NULL, context->view.udf, entry->path);
		if (status == ISO_SCAN_PARTIAL && entry->udf->fe.icb_tag.file_type != ICBTAG_FILE_TYPE_DIRECTORY) {
			udf_dirent_t* root = udf_get_root(context->view.udf,true,0);
			reader.udf_file = root ? udf_fopen(root,entry->path) : NULL;
			udf_dirent_free(root);
			if (reader.udf_file) { reader.size = udf_get_file_length(reader.udf_file); status = ISO_SCAN_OK; }
		}
	}
	if (IsIsoScanCancelled())
		status = ISO_SCAN_CANCELLED;
	int error = status == ISO_SCAN_OK ? 0 : (status == ISO_SCAN_CANCELLED ? ECANCELED : EIO);

	if (error == 0) {
		bool same_size = reader.size == size;
		bool same_location = entry->iso == NULL || reader.lsn == entry->iso->lsn;
		if (!same_size || !same_location)
			error = EIO;
	}
	if (error == 0) {
		bool split = context->split_install_wim && strcasecmp(entry->path, INSTALL_WIM_PATH) == 0;
		if (split) {
			ISO_WIM_SPLIT_RESULT result;
			SPLIT_INTEGRITY_CONTEXT integrity = { context, output_path };
			uprintf("Splitting: %s", entry->path);
			error = SplitIsoWim(&reader, &context->source, context->image, parent,
				context->copied_blocks, context->total_blocks, &result,
				context->events != NULL ? RecordSplitPart : NULL, &integrity);
			context->copied_blocks = result.copied_blocks;
			CloseIsoFileReader(&reader);
			if (error == 0) {
				context->result->files++;
				context->result->bytes_written += size;
				context->result->split_bytes += result.bytes;
				context->result->split_parts += result.parts;
			}
			return error;
		}
		bool restore = false;
		const bool windows = HAS_BOOTMGR(context->report) && !HAS_SYSLINUX(context->report);
		const bool root_file = entry->parent[0] == 0 || strcmp(entry->parent, "/") == 0;
		const size_t length = strlen(entry->name);
		const bool manager = root_file && strncasecmp(entry->name, "bootmgr", 7) == 0;
		const bool efi = strcasecmp(entry->parent, "/efi/boot") == 0 && length >= 4
			&& strcasecmp(entry->name + length - 4, ".efi") == 0;
		const bool syslinux = HAS_SYSLINUX(context->report) && root_file && strcasecmp(entry->name, "ldlinux.c32") == 0;
		const bool grub4dos = root_file && strcasecmp(entry->name, "grldr") == 0;
		const bool uncompressed = context->ntfs_compression && ((windows && (manager || efi)) || syslinux || grub4dos);
		if (uncompressed)
			error = PrepareNtfsUncompressedCreation(parent, &restore);
		if (error == 0) {
			output = OpenOutputFile(parent, output_name);
			if (output < 0)
				error = errno;
		}
		if (restore) {
			const int restored = SetNtfsDirectoryCompression(parent, true);
			if (error == 0)
				error = restored;
		}
	}
	if (error == 0) {
		uprintf("Extracting: %s", entry->path);
		error = CopyFileData(context, &reader, output, hashing ? &md5 : NULL);
	}
	CloseIsoFileReader(&reader);
	if (error == 0 && output >= 0) {
		PreserveEntryTimes(context, entry, output);
		error = SyncOutput(output);
		if (error == 0)
			error = CheckImageFileSource(&context->source, context->image);
	}
	if (output >= 0) {
		if (close(output) != 0 && error == 0)
			error = errno;
		if (error != 0 && unlinkat(parent, output_name, 0) != 0)
			uprintf("Could not remove incomplete file %s: %s", entry->path, strerror(errno));
	}
	if (error == 0) {
		context->result->files++;
		if (hashing)
			hash_final[HASH_MD5](&md5);
		if (context->events != NULL)
			error = context->events->copied(context->events->context, output_path, size, hashing ? md5.buf : NULL);
		if (error == 0) error = ReplaceObsoleteModule(context,entry,parent,output_name,output_path);
		if (error == 0)
			error = FixExtractedConfig(context, entry, size, parent, output_name, output_path);
	}
	return error;
}

static bool IsSplitPartName(const char* name)
{
	static const char prefix[] = "install";
	const size_t length = sizeof(prefix) - 1;
	if (strncasecmp(name, prefix, length) != 0)
		return false;
	const char* suffix = name + length;
	while (*suffix >= '0' && *suffix <= '9')
		suffix++;
	return strcasecmp(suffix, ".swm") == 0;
}

static int CheckSplitEntry(ISO_EXTRACT_CONTEXT* context, const ISO_TREE_ENTRY* entry, uint64_t* required_bytes)
{
	ISO_FILE_READER reader;
	ISO_WIM_SPLIT_PLAN plan;
	int status = OpenIsoFileReader(&reader, context->view.iso, context->view.udf, entry->path);
	int error = status == ISO_SCAN_OK ? PlanIsoWimSplit(&reader, &plan) : EIO;
	CloseIsoFileReader(&reader);
	if (error == 0) {
		*required_bytes = plan.reserved_bytes;
		context->planned_split_parts = plan.parts;
	}
	if (IsIsoScanCancelled())
		error = ECANCELED;
	return error;
}

static int CheckEntry(ISO_EXTRACT_CONTEXT* context, const ISO_TREE_ENTRY* entry, bool* skip)
{
	uint64_t size;
	ISO_FILE_PROPS props;
	mode_t mode;

	*skip = false;
	if (entry->kind == ISO_TREE_DEEP)
		return ENOTSUP;
	bool valid_name = entry->name[0] != 0 && (strchr(entry->name, '/') == NULL || entry->kind == ISO_TREE_FILE);
	if (!ValidImagePath(entry->path) || !valid_name)
		return EINVAL;
	bool has_posix_metadata = entry->iso != NULL && context->rockridge && entry->iso->rr.b3_rock == yep;
	if (has_posix_metadata) {
		mode = iso9660_get_posix_filemode(entry->iso);
		(void)mode; // Nonregular ISO entries become ordinary output, as on Windows.
		if (entry->iso->rr.psz_symlink != NULL) return EINVAL; // Handled by the link planner.
	}
	bool sources_directory = strcasecmp(entry->parent, "/sources") == 0;
	bool reserved_split_name = sources_directory && IsSplitPartName(entry->name);
	if (context->preflight && context->split_install_wim && reserved_split_name)
		context->has_source_swm = true;
	if (entry->kind != ISO_TREE_FILE)
		return 0;
	size = entry->iso != NULL ? entry->iso->total_size : udf_get_file_length(entry->udf);
	bool total_overflow = context->preflight && size > UINT64_MAX - context->expected_bytes;
	if (size > INT64_MAX || total_overflow)
		return EOVERFLOW;
	GetIsoFileProps(entry->parent, (int64_t)size, entry->name, &props);
	*skip = props.is_root_ldlinux_sys;
	bool split = context->split_install_wim && strcasecmp(entry->path, INSTALL_WIM_PATH) == 0;
	bool exceeds_limit = context->max_file_bytes != 0 && size > context->max_file_bytes;
	if (!*skip && !split && exceeds_limit)
		return EFBIG;
	if (context->preflight && !*skip) {
		uint64_t required_bytes = size;
		if (split) {
			context->split_sources++;
			int error = CheckSplitEntry(context, entry, &required_bytes);
			if (error != 0)
				return error;
		}
		CONFIG_TEXT_PATCH patches[EXTRACT_CONFIG_MAX_PATCHES];
		size_t count = GetFileConfigPatches(context, &props, patches);
		if (count != 0 || HasPersistenceConfig(context, &props)) {
			int error = CheckConfigEntry(context, entry, size, patches, count, HasPersistenceConfig(context, &props), props.is_grub_cfg);
			if (error != 0)
				return error;
		}
		context->expected_bytes += size;
		context->expected_files++;
		if (required_bytes > UINT64_MAX - context->result->required_bytes)
			return EOVERFLOW;
		context->result->required_bytes += required_bytes;
		if (context->boot_resources && (context->report.sl_version >> 8) < 5) for (int i = 0; i < 2; ++i) if (props.is_old_c32[i]) {
			uint64_t replacement = context->boot_resources->old_c32_bytes[i];
			if (replacement > UINT64_MAX - context->result->required_bytes) return EOVERFLOW;
			context->result->required_bytes += replacement;
		}
	}
	return 0;
}

static int HandleIsoLink(ISO_EXTRACT_CONTEXT* context, const ISO_TREE_ENTRY* entry)
{
	if (!ValidImagePath(entry->path) || strchr(entry->name,'/')) return EINVAL;
	if ((context->report.has_efi & 0x4000) && context->embedded_available && strcasecmp(entry->path,"/efi/boot/bootx64.efi") == 0) {
		if (!context->preflight) ++context->result->skipped;
		return 0;
	}
	ISO_LINK_PLAN plan;
	int error = PlanIsoLink(context->view.iso,entry,context->windows_links,HAS_SYSLINUX(context->report),&plan);
	if (error) return error;
	if (plan.action == ISO_LINK_SKIP) { if (!context->preflight) ++context->result->skipped; return 0; }
	uint64_t size = plan.action == ISO_LINK_REPARSE || plan.action == ISO_LINK_SYSLINUX ? 0 : plan.size;
	if (context->preflight) {
		if ((context->max_file_bytes && size > context->max_file_bytes) || size > UINT64_MAX - context->expected_bytes
			|| size > UINT64_MAX - context->result->required_bytes) return EFBIG;
		context->expected_bytes += size; ++context->expected_files; context->result->required_bytes += size + 2048;
		if (plan.action == ISO_LINK_DUPLICATE) context->total_blocks += (size + ISO_BLOCKSIZE - 1) / ISO_BLOCKSIZE;
		return 0;
	}
	error = CheckImageFileSource(&context->source,context->image);
	if (error) return error;
	char path[ISO_SCAN_PATH_MAX]; strcpy(path,entry->path); if (SanitizeImagePath(path)) return EINVAL;
	if (plan.action == ISO_LINK_REPARSE || plan.action == ISO_LINK_SYSLINUX) {
		if (++context->pending_links > 65536) return E2BIG;
		ISO_PENDING_LINK* pending = calloc(1,sizeof(*pending));
		if (!pending) return ENOMEM;
		strcpy(pending->path,path); pending->plan = plan; pending->next = context->links; context->links = pending;
		++context->result->files;
		return 0;
	}
	int parent = OpenOutputParent(context->root,path); if (parent < 0) return errno;
	const char* name = strrchr(path,'/')+1;
	if (plan.action == ISO_LINK_DUPLICATE) {
		iso9660_stat_t* source = iso9660_ifs_stat_translate(context->view.iso,plan.source);
		if (!source) error = EIO;
		else { ISO_TREE_ENTRY duplicate = *entry; duplicate.iso = source; error = ExtractFile(context,&duplicate,parent,name,path); iso9660_stat_free(source); }
	} else {
		int output = OpenOutputFile(parent,name);
		error = output < 0 ? errno : 0;
		const uint8_t* text = (const uint8_t*)entry->iso->rr.psz_symlink;
		if (!error) error = WriteOutput(output,text,(size_t)size,&context->result->bytes_written);
		if (!error) error = SyncOutput(output);
		if (output >= 0 && close(output) != 0 && !error) error = errno;
		if (!error) {
			++context->result->files;
			if (context->events) {
				HASH_CONTEXT md5 = {0}; const bool hashing = context->events->hash_md5;
				if (hashing) { hash_init[HASH_MD5](&md5); hash_write[HASH_MD5](&md5,text,(size_t)size); hash_final[HASH_MD5](&md5); }
				error = context->events->copied(context->events->context,path,size,hashing ? md5.buf : NULL);
				if (!error) error = context->events->modified(context->events->context,path);
			}
			if (!error) error = FixExtractedConfig(context,entry,size,parent,name,path);
		} else if (output >= 0) unlinkat(parent,name,0);
	}
	if (close(parent) != 0 && !error) error = errno;
	return error;
}

static int FinishIsoLinks(ISO_EXTRACT_CONTEXT* context)
{
	for (ISO_PENDING_LINK* link = context->links; link; link = link->next) {
		if (IsIsoScanCancelled()) return ECANCELED;
		int parent = OpenOutputParent(context->root,link->path); if (parent < 0) return errno;
		const char* name = strrchr(link->path,'/')+1;
		int error = 0;
		if (link->plan.action == ISO_LINK_REPARSE) {
            error = CreateNtfsIsoLink(parent,name,&link->plan);
            if (!error && !link->plan.directory && context->events && context->events->linked)
                error = context->events->linked(context->events->context,link->path,link->plan.source);
        }
		else {
			error = PrepareOutputDirectory(parent,name);
			int directory = !error ? openat(parent,name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC) : -1;
			if (!error && directory < 0) error = errno;
			for (const char** config = (const char*[]){"syslnx32.cfg","syslnx64.cfg",NULL}; !error && *config; ++config) {
				char source[ISO_SCAN_PATH_MAX];
				if (snprintf(source,sizeof(source),"%s/%s",link->plan.source,*config) >= (int)sizeof(source)) { error = ENAMETOOLONG; break; }
				int input_parent = OpenOutputParent(context->root,source);
				if (input_parent < 0) { error = errno; break; }
				struct stat info; bool exists = fstatat(input_parent,*config,&info,AT_SYMLINK_NOFOLLOW) == 0;
				if (!exists && errno != ENOENT) error = errno;
				if (exists && !S_ISREG(info.st_mode)) error = EINVAL;
				close(input_parent);
				if (!exists || error) continue;
				char text[2 * ISO_SCAN_PATH_MAX + 128], output_path[ISO_SCAN_PATH_MAX];
				int length = snprintf(text,sizeof(text),"DEFAULT loadconfig\n\nLABEL loadconfig\n  CONFIG %s\n  APPEND %s\n",source,link->plan.source);
				if (snprintf(output_path,sizeof(output_path),"%s/%s",link->path,*config) >= (int)sizeof(output_path)) { error = ENAMETOOLONG; break; }
				int output = OpenOutputFile(directory,*config); if (output < 0) { error = errno; break; }
				error = WriteOutput(output,(const uint8_t*)text,(size_t)length,&context->result->generated_bytes);
				if (!error) error = SyncOutput(output);
				if (close(output) != 0 && !error) error = errno;
				if (!error && context->events) {
                    HASH_CONTEXT md5 = {0}; const bool hashing = context->events->hash_md5;
                    if (hashing) { hash_init[HASH_MD5](&md5); hash_write[HASH_MD5](&md5,(const uint8_t*)text,(size_t)length); hash_final[HASH_MD5](&md5); }
                    error = context->events->copied(context->events->context,output_path,(uint64_t)length,hashing ? md5.buf : NULL);
                    if (!error) error = context->events->modified(context->events->context,output_path);
                }
			}
			if (directory >= 0) { if (!error) error = SyncOutput(directory); if (close(directory) != 0 && !error) error = errno; }
		}
		if (!error) error = SyncOutput(parent);
		if (close(parent) != 0 && !error) error = errno;
		if (error) return error;
	}
	return 0;
}

static int VisitExtractEntry(const ISO_TREE_ENTRY* entry, void* data)
{
	ISO_EXTRACT_CONTEXT* context = data;
	char output_path[ISO_SCAN_PATH_MAX];
	const char* output_name;
	int parent, directory, error;
	bool skip;

	snprintf(context->result->path, sizeof(context->result->path), "%s", entry->path);
	if (entry->iso && context->rockridge && entry->iso->rr.psz_symlink && entry->kind == ISO_TREE_FILE) {
		error = HandleIsoLink(context,entry); goto out;
	}
	error = CheckEntry(context, entry, &skip);
	if (error != 0 || context->preflight)
		goto out;
	if (skip) {
		context->result->skipped++;
		uprintf("Skipping '%s' file from ISO image", entry->name);
		goto out;
	}
	error = CheckImageFileSource(&context->source, context->image);
	if (error != 0)
		goto out;
	// CheckEntry validated the length. Only the output copy is renamed; image
	// readers and file classification continue to use the original source path.
	strcpy(output_path, entry->path);
	bool renamed = SanitizeImagePath(output_path);
	output_name = strrchr(output_path, '/') + 1;
	if (renamed && entry->kind != ISO_TREE_DIRECTORY_END)
		uprintf("  File name sanitized to '%s'", output_path);
	parent = OpenOutputParent(context->root, output_path);
	if (parent < 0) {
		error = errno;
		goto out;
	}
	if (entry->kind == ISO_TREE_DIRECTORY) {
		error = PrepareOutputDirectory(parent, output_name);
		if (error == 0)
			context->result->directories++;
	} else if (entry->kind == ISO_TREE_DIRECTORY_END) {
		directory = openat(parent, output_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		error = directory < 0 ? errno : 0;
		if (error == 0) {
			PreserveEntryTimes(context, entry, directory);
			error = SyncOutput(directory);
		}
		if (directory >= 0 && close(directory) != 0 && error == 0)
			error = errno;
	} else
		error = ExtractFile(context, entry, parent, output_name, output_path);
	if (close(parent) != 0 && error == 0)
		error = errno;
out:
	context->error = error;
	return error == 0 ? ISO_SCAN_OK : (error == ECANCELED ? ISO_SCAN_CANCELLED : ISO_SCAN_ERROR);
}

static int ExtractEmbeddedEntry(const EMBEDDED_EFI_FILE* file, void* opaque)
{
	ISO_EXTRACT_CONTEXT* context = opaque;
	bool copy = false;
	int error = EmbeddedEfiPrecedence(context->view.iso, context->view.udf, &context->report, file, &copy);
	if (error != 0 || !copy) return error;
	if (!ValidImagePath(file->path)) return EINVAL;
	strcpy(context->result->path, file->path);
	char path[ISO_SCAN_PATH_MAX]; strcpy(path, file->path);
	if (SanitizeImagePath(path)) return EINVAL; // Embedded EFI aliases must remain exact.
	if (context->preflight) {
		if (file->directory) return 0;
		if ((context->max_file_bytes && file->size > context->max_file_bytes)
			|| file->size > UINT64_MAX - context->expected_bytes
			|| file->size > UINT64_MAX - context->result->required_bytes) return EFBIG;
		context->expected_bytes += file->size; ++context->expected_files;
		context->total_blocks += (file->size + ISO_BLOCKSIZE - 1) / ISO_BLOCKSIZE;
		context->result->required_bytes += file->size + strlen(path) + 35;
		return 0;
	}
	error = CheckImageFileSource(&context->source, context->image);
	if (error != 0) return error;
	int parent = OpenOutputParent(context->root,path);
	if (parent < 0) return errno;
	const char* name = strrchr(path,'/') + 1;
	if (file->directory) { error = PrepareOutputDirectory(parent,name); if (!error) ++context->result->directories; }
	else {
		// A proven broken fallback is the only regular-tree replacement allowed.
		struct stat old;
		if ((context->report.has_efi & 0x4000) && strcasecmp(path,"/efi/boot/bootx64.efi") == 0
			&& fstatat(parent,name,&old,AT_SYMLINK_NOFOLLOW) == 0) {
			if ((!S_ISREG(old.st_mode) && !S_ISLNK(old.st_mode)) || old.st_size >= 256) error = ESTALE;
			else if (unlinkat(parent,name,0) != 0) error = errno;
		}
		int output = error == 0 ? openat(parent,name,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0644) : -1;
		if (!error && output < 0) error = errno;
		HASH_CONTEXT md5 = {0}; const bool hashing = context->events && context->events->hash_md5;
		if (hashing) hash_init[HASH_MD5](&md5);
		for (uint64_t offset = 0; !error && offset < file->size;) {
			size_t count = file->size - offset > EXTRACT_BUFFER_SIZE ? EXTRACT_BUFFER_SIZE : (size_t)(file->size - offset);
			error = ReadEmbeddedEfiRange(file,offset,context->buffer,count);
			if (!error && hashing) hash_write[HASH_MD5](&md5,context->buffer,count);
			if (!error) error = WriteOutput(output,context->buffer,count,&context->result->bytes_written);
			offset += count;
			context->copied_blocks += (count + ISO_BLOCKSIZE - 1) / ISO_BLOCKSIZE;
		}
		if (!error) error = SyncOutput(output);
		if (output >= 0 && close(output) != 0 && !error) error = errno;
		if (!error) {
			++context->result->files;
			if (hashing) hash_final[HASH_MD5](&md5);
			if (context->events) error = context->events->copied(context->events->context,path,file->size,hashing ? md5.buf : NULL);
			char directory[ISO_SCAN_PATH_MAX]; strcpy(directory,path); *strrchr(directory,'/') = 0;
			ISO_TREE_ENTRY entry = {ISO_TREE_FILE,directory,name,path,NULL,NULL};
			if (!error) error = FixExtractedConfig(context,&entry,file->size,parent,name,path);
			if (!error && context->events) error = context->events->modified(context->events->context,path);
		} else if (output >= 0) unlinkat(parent,name,0);
	}
	if (!error) error = SyncOutput(parent);
	if (close(parent) != 0 && !error) error = errno;
	return error;
}

static int RelocateSlaxEfi(ISO_EXTRACT_CONTEXT* context)
{
	struct stat info;
	if (fstatat(context->root,"EFI",&info,AT_SYMLINK_NOFOLLOW) == 0) return 0;
	if (errno != ENOENT) return errno;
	int slax = openat(context->root,"slax",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
	if (slax < 0) return errno == ENOENT ? 0 : errno;
	int boot = openat(slax,"boot",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
	int error = boot < 0 && errno != ENOENT ? errno : 0;
	if (boot >= 0) {
		if (fstatat(boot,"EFI",&info,AT_SYMLINK_NOFOLLOW) != 0) error = errno == ENOENT ? 0 : errno;
		else if (!S_ISDIR(info.st_mode)) error = ENOTDIR;
		else if (renameat2(boot,"EFI",context->root,"EFI",RENAME_NOREPLACE) != 0) error = errno;
		else {
			for (ISO_PENDING_LINK* link = context->links; !error && link; link = link->next)
                error = RelocateIsoLink(link->path,&link->plan,"/slax/boot/EFI","/EFI");
            if (!error && context->events && context->events->relocated) error = context->events->relocated(context->events->context,"/slax/boot/EFI","/EFI");
			if (!error) error = SyncOutput(boot);
		}
		if (close(boot) != 0 && !error) error = errno;
	}
	if (close(slax) != 0 && !error) error = errno;
	return error;
}

static int OpenExtractionSource(ISO_EXTRACT_CONTEXT* context, const ISO_ANALYSIS_OPTIONS* options)
{
	ISO_ANALYSIS analysis;
	char retained_path[64];
	int status;

	// All library opens use the retained inode, even if its original path is
	// replaced during analysis. The source checks still reject such changes.
	snprintf(retained_path, sizeof(retained_path), "/proc/self/fd/%d", context->source.fd);
	status = AnalyzeIsoImageWithSource(retained_path, (uint64_t)context->source.info.st_size,
		options, &analysis, &context->view);
	if (status != ISO_SCAN_OK && status != ISO_SCAN_PARTIAL)
		return status == ISO_SCAN_CANCELLED ? ECANCELED : EIO;
	bool incomplete_tree = (analysis.pending & (ISO_PENDING_DEEP_DIRECTORIES | ISO_PENDING_TRUNCATED)) != 0;
	if (!analysis.tree_complete || incomplete_tree)
		return ENOTSUP;
	context->report = analysis.report;
	context->total_blocks = analysis.total_blocks;
	// GUI scanning applies these same shared image decisions after analysis.
	ApplyIsoPolicy(&context->report);
	return 0;
}

static int PrepareConfigPatches(ISO_EXTRACT_CONTEXT* context, const ISO_EXTRACT_OPTIONS* options)
{
	context->persistence = options->persistence_size != 0;
	if (options->usb_label == NULL)
		return 0;
	context->freenas = GetFreeNasConfigPatch(context->report.label, options->usb_label,
		context->volume_source, context->volume_replacement);
	context->iso_label = replace_char(context->report.label, ' ', "\\x20");
	context->usb_label = replace_char(options->usb_label, ' ', "\\x20");
	if (context->iso_label == NULL || context->usb_label == NULL)
		return ENOMEM;
	size_t count = GetIsoConfigPatches(context->iso_label, context->usb_label,
		context->report.rh8_derivative, options->image_path, context->patches);
	bool label_changes = context->iso_label[0] != 0 && strcmp(context->iso_label, context->usb_label) != 0;
	if (label_changes || count > ISO_CONFIG_LABEL_PASSES)
		context->patch_count = count;
	return 0;
}

static int CheckExtractionSpace(int root, uint64_t required_bytes)
{
	struct statvfs space;
	if (fstatvfs(root, &space) != 0)
		return errno;
	if (space.f_frsize == 0)
		return EIO;
	// Divide instead of multiplying f_bavail: even enormous filesystem reports
	// cannot wrap the comparison. Allocation/metadata can still fail during I/O.
	uint64_t blocks = required_bytes / space.f_frsize + (required_bytes % space.f_frsize != 0);
	return blocks > space.f_bavail ? ENOSPC : 0;
}

static int CheckPreparedReactOsFile(int root, const char* name, const uint8_t* expected,
	size_t size, dev_t device)
{
	int file = openat(root,name,O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
	if (file < 0) return errno;
	struct stat info;
	int error = fstat(file,&info) == 0 ? 0 : errno;
	if (!error && (!S_ISREG(info.st_mode) || info.st_nlink != 1 || info.st_dev != device ||
		info.st_size < 0 || (uint64_t)info.st_size != size)) error = ESTALE;
	uint8_t buffer[4096];
	for (size_t done = 0; !error && done < size;) {
		if (IsIsoScanCancelled()) { error = ECANCELED; break; }
		size_t count = size - done > sizeof(buffer) ? sizeof(buffer) : size - done;
		ssize_t read_bytes = read(file,buffer,count);
		if (read_bytes < 0 && errno == EINTR) continue;
		if (read_bytes <= 0) { error = read_bytes == 0 ? EIO : errno; break; }
		if (memcmp(buffer,expected+done,(size_t)read_bytes) != 0) error = EBADMSG;
		done += (size_t)read_bytes;
	}
	if (close(file) != 0 && !error) error = errno;
	return error;
}

static int CheckPreparedReactOsDirectory(int root, const ISO_PREPARED_REACTOS* files)
{
	if (files == NULL) return CheckEmptyTargetDirectory(root);
	const bool valid_files = files->mboot != NULL && files->config != NULL &&
		files->mboot_bytes != 0 && files->mboot_bytes <= 512 * 1024 &&
		files->config_bytes != 0 && files->config_bytes <= 512;
	if (!valid_files) return EINVAL;
	struct stat info;
	if (fstat(root,&info) != 0) return errno;
	int enumerate = openat(root,".",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
	if (enumerate < 0) return errno;
	DIR* directory = fdopendir(enumerate);
	if (directory == NULL) { int error = errno; close(enumerate); return error; }
	unsigned found = 0;
	int error = 0;
	for (;;) {
		errno = 0;
		struct dirent* entry = readdir(directory);
		if (entry == NULL) { error = errno; break; }
		if (strcmp(entry->d_name,".") == 0 || strcmp(entry->d_name,"..") == 0) continue;
		unsigned file = strcmp(entry->d_name,"mboot.c32") == 0 ? 1 :
			strcmp(entry->d_name,"syslinux.cfg") == 0 ? 2 : 0;
		if (file == 0 || (found & file)) { error = ENOTEMPTY; break; }
		found |= file;
	}
	if (closedir(directory) != 0 && !error) error = errno;
	if (!error && found != 3) error = ENOENT;
	if (!error) error = CheckPreparedReactOsFile(root,"mboot.c32",files->mboot,files->mboot_bytes,info.st_dev);
	if (!error) error = CheckPreparedReactOsFile(root,"syslinux.cfg",files->config,files->config_bytes,info.st_dev);
	return error;
}

static int ProcessIsoTree(const char* image, const char* directory,
	const ISO_EXTRACT_OPTIONS* options, uint64_t max_file_bytes, ISO_EXTRACT_RESULT* result)
{
	ISO_EXTRACT_CONTEXT context = { 0 };
	ISO_TREE_WALK walk = { VisitExtractEntry, &context, false, 0 };
	int error, status;

	if (result == NULL)
		return EINVAL;
	memset(result, 0, sizeof(*result));
	if (image == NULL || options == NULL)
		return EINVAL;
	if (options->usb_label != NULL && options->image_path == NULL)
		return EINVAL;
	if (options->events != NULL) {
		const bool complete_events = options->events->begin != NULL && options->events->copied != NULL
			&& options->events->modified != NULL && (!options->split_install_wim || options->events->split_part != NULL);
		if (!complete_events)
			return EINVAL;
		if (options->events->hash_md5) {
			const int initialized = InitializeNativeHashes();
			if (initialized != 0)
				return initialized;
		}
	}
	context.preflight = true;
	context.image = image;
	context.result = result;
	context.root = -1;
	context.rockridge = options->analysis.enable_rockridge;
	context.max_file_bytes = max_file_bytes;
	context.split_install_wim = options->split_install_wim;
	context.events = options->events;
	context.preserve_timestamps = options->preserve_timestamps;
	context.ntfs_compression = options->ntfs_compression;
	context.windows_links = options->windows_links;
	context.boot_resources = options->boot_resources;
	if (context.split_install_wim) {
		if (max_file_bytes != 0 && max_file_bytes < UINT32_MAX)
			return EINVAL;
		context.max_file_bytes = UINT32_MAX;
	}
	error = OpenImageFileSource(image, &context.source);
	if (error != 0)
		return error;
	if (directory != NULL) {
		context.root = open(directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		error = context.root < 0 ? errno : CheckPreparedReactOsDirectory(context.root,options->prepared_reactos);
		if (error != 0)
			goto out;
	}
	error = OpenExtractionSource(&context, &options->analysis);
	if (!error && options->prepared_reactos != NULL && !HAS_REACTOS(context.report))
		error = EINVAL;
    if (!error && (context.report.has_efi & 0xc000) && HAS_EFI_IMG(context.report)) {
        ISO_FILE_READER embedded = {0};
        const int opened = OpenIsoFileReader(&embedded,context.view.iso,context.view.udf,context.report.efi_img_path);
        context.embedded_available = opened == ISO_SCAN_OK;
        if (opened == ISO_SCAN_CANCELLED) error = ECANCELED;
        CloseIsoFileReader(&embedded);
    }
	if (error == 0)
		error = PrepareConfigPatches(&context, options);
	if (error != 0)
		goto out;
	walk.enable_rockridge = options->analysis.enable_rockridge;
	if (context.view.iso != NULL)
		walk.joliet_level = iso9660_ifs_get_joliet_level(context.view.iso);
	if (directory != NULL) {
		context.buffer = malloc(EXTRACT_BUFFER_SIZE);
		error = ENOMEM;
		if (context.buffer == NULL)
			goto out;
	}
	const int passes = directory != NULL ? 2 : 1;
	for (int pass = 0; pass < passes; pass++) {
		context.preflight = pass == 0;
		if (!context.preflight)
			_UpdateProgressWithInfo(OP_FILE_COPY, 0, 0, context.total_blocks, 1);
		error = CheckImageFileSource(&context.source, image);
		if (error != 0)
			goto out;
		if (!context.preflight && context.events != NULL) {
			error = context.events->begin(context.events->context, context.root, context.report.has_md5sum);
			if (error != 0)
				goto out;
		}
		status = context.view.udf != NULL ? WalkUdfFiles(udf_get_root(context.view.udf, true, 0), "", &walk)
			: WalkIsoFiles(context.view.iso, "", &walk);
		error = context.error != 0 ? context.error : (status == ISO_SCAN_CANCELLED ? ECANCELED : EIO);
		if (status != ISO_SCAN_OK)
			goto out;
		if (context.embedded_available) {
			error = WalkEmbeddedEfiFiles(context.view.iso, context.view.udf, context.report.efi_img_path, ExtractEmbeddedEntry, &context);
			if (error != 0) goto out;
		}
		if (context.preflight && context.split_install_wim) {
			bool conflicting = context.has_source_swm || context.split_sources > 1;
			if (conflicting || context.split_sources == 0) {
				error = conflicting ? EEXIST : ENOENT;
				goto out;
			}
			if (directory != NULL) {
				error = CheckExtractionSpace(context.root, result->required_bytes);
				if (error != 0)
					goto out;
			}
		}
	}
	if (directory == NULL) {
		result->split_parts = context.planned_split_parts;
		result->files = context.expected_files;
		result->bytes_written = context.expected_bytes;
		error = CheckImageFileSource(&context.source, image);
		goto out;
	}
	error = EIO;
	if (result->bytes_written != context.expected_bytes || result->files != context.expected_files)
		goto out;
	error = CreateRootSyslinuxConfig(&context);
	if (error == 0) error = RelocateSlaxEfi(&context);
	if (error == 0) error = FinishIsoLinks(&context);
	if (error != 0)
		goto out;
	strcpy(result->path, "/");
	error = SyncOutput(context.root);
	if (error == 0)
		error = CheckImageFileSource(&context.source, image);
out:
	if (!context.preflight)
		_UpdateProgressWithInfo(OP_FILE_COPY, 0, context.copied_blocks, context.total_blocks, 1);
	while (context.links) { ISO_PENDING_LINK* next = context.links->next; free(context.links); context.links = next; }
	free(context.iso_label);
	free(context.usb_label);
	free(context.buffer);
	CloseIsoAnalysisSource(&context.view);
	CloseImageFileSource(&context.source);
	if (context.root >= 0 && close(context.root) != 0 && error == 0)
		error = errno;
	if (error == 0)
		result->path[0] = 0;
	return error;
}

int ExtractIsoTree(const char* image, const char* directory,
	const ISO_EXTRACT_OPTIONS* options, ISO_EXTRACT_RESULT* result)
{
	if (directory == NULL) {
		if (result != NULL)
			memset(result, 0, sizeof(*result));
		return EINVAL;
	}
	return ProcessIsoTree(image, directory, options, 0, result);
}

int CheckIsoTree(const char* image, const ISO_EXTRACT_OPTIONS* options,
	uint64_t max_file_bytes, ISO_EXTRACT_RESULT* result)
{
	return ProcessIsoTree(image, NULL, options, max_file_bytes, result);
}
