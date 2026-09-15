/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "iso_extract.h"
#include "image_source.h"
#include "image_file.h"
#include "iso_file.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define EXTRACT_BUFFER_SIZE (1024 * 1024)

extern void uprintf(const char* format, ...);

typedef struct {
	const char* image;
	IMAGE_FILE_SOURCE source;
	ISO_ANALYSIS_SOURCE view;
	ISO_EXTRACT_RESULT* result;
	int root;
	int error;
	bool preflight;
	bool rockridge;
	uint64_t expected_bytes;
	uint64_t expected_files;
	uint8_t* buffer;
} ISO_EXTRACT_CONTEXT;

static int CheckEmptyDirectory(int root)
{
	int fd = openat(root, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	int error = 0;
	DIR* directory;
	struct dirent* entry;

	if (fd < 0)
		return errno;
	directory = fdopendir(fd);
	if (directory == NULL) {
		error = errno;
		close(fd);
		return error;
	}
	errno = 0;
	while ((entry = readdir(directory)) != NULL) {
		if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
			error = ENOTEMPTY;
			break;
		}
	}
	if (entry == NULL && errno != 0)
		error = errno;
	if (closedir(directory) != 0 && error == 0)
		error = errno;
	return error;
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

static int WriteOutput(int fd, const uint8_t* buffer, size_t size, ISO_EXTRACT_RESULT* result)
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
		result->bytes_written += (uint64_t)count;
	}
	return 0;
}

static int CopyFileData(ISO_EXTRACT_CONTEXT* context, ISO_FILE_READER* reader, int output)
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
		error = WriteOutput(output, context->buffer, size, context->result);
		if (error != 0)
			return error;
		offset += size;
	}
	error = SyncOutput(output);
	return error != 0 ? error : CheckImageFileSource(&context->source, context->image);
}

static int ExtractFile(ISO_EXTRACT_CONTEXT* context, const ISO_TREE_ENTRY* entry, int parent)
{
	ISO_FILE_READER reader;
	uint64_t size = entry->iso != NULL ? entry->iso->total_size : udf_get_file_length(entry->udf);
	int output = -1;
	int status = OpenIsoFileReader(&reader, context->view.iso, context->view.udf, entry->path);
	int error = status == ISO_SCAN_OK ? 0 : (status == ISO_SCAN_CANCELLED ? ECANCELED : EIO);

	if (error == 0) {
		bool same_size = reader.size == size;
		bool same_location = entry->iso == NULL || reader.lsn == entry->iso->lsn;
		if (!same_size || !same_location)
			error = EIO;
	}
	if (error == 0) {
		output = openat(parent, entry->name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
		if (output < 0)
			error = errno;
	}
	if (error == 0) {
		uprintf("Extracting: %s", entry->path);
		error = CopyFileData(context, &reader, output);
	}
	CloseIsoFileReader(&reader);
	if (output >= 0) {
		if (close(output) != 0 && error == 0)
			error = errno;
		if (error != 0 && unlinkat(parent, entry->name, 0) != 0)
			uprintf("Could not remove incomplete file %s: %s", entry->path, strerror(errno));
	}
	if (error == 0)
		context->result->files++;
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
	bool valid_name = entry->name[0] != 0 && strchr(entry->name, '/') == NULL;
	if (!ValidImagePath(entry->path) || !valid_name)
		return EINVAL;
	bool has_posix_metadata = entry->iso != NULL && context->rockridge && entry->iso->rr.b3_rock == yep;
	if (has_posix_metadata) {
		mode = iso9660_get_posix_filemode(entry->iso);
		bool supported_type = S_ISREG(mode) || S_ISDIR(mode);
		if (entry->iso->rr.psz_symlink != NULL || !supported_type)
			return ENOTSUP;
	}
	if (entry->udf != NULL) {
		uint8_t type = entry->udf->fe.icb_tag.file_type;
		bool supported_type = type == ICBTAG_FILE_TYPE_REGULAR || type == ICBTAG_FILE_TYPE_DIRECTORY;
		if (!supported_type)
			return ENOTSUP;
	}
	if (entry->kind != ISO_TREE_FILE)
		return 0;
	size = entry->iso != NULL ? entry->iso->total_size : udf_get_file_length(entry->udf);
	bool total_overflow = context->preflight && size > UINT64_MAX - context->expected_bytes;
	if (size > INT64_MAX || total_overflow)
		return EOVERFLOW;
	GetIsoFileProps(entry->parent, (int64_t)size, entry->name, &props);
	*skip = props.is_root_ldlinux_sys;
	if (context->preflight && !*skip) {
		context->expected_bytes += size;
		context->expected_files++;
	}
	return 0;
}

static int VisitExtractEntry(const ISO_TREE_ENTRY* entry, void* data)
{
	ISO_EXTRACT_CONTEXT* context = data;
	int parent, directory, error;
	bool skip;

	snprintf(context->result->path, sizeof(context->result->path), "%s", entry->path);
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
	parent = OpenOutputParent(context->root, entry->path);
	if (parent < 0) {
		error = errno;
		goto out;
	}
	if (entry->kind == ISO_TREE_DIRECTORY) {
		if (mkdirat(parent, entry->name, 0755) != 0)
			error = errno;
		else
			context->result->directories++;
	} else if (entry->kind == ISO_TREE_DIRECTORY_END) {
		directory = openat(parent, entry->name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		error = directory < 0 ? errno : SyncOutput(directory);
		if (directory >= 0 && close(directory) != 0 && error == 0)
			error = errno;
	} else
		error = ExtractFile(context, entry, parent);
	if (close(parent) != 0 && error == 0)
		error = errno;
out:
	context->error = error;
	return error == 0 ? ISO_SCAN_OK : (error == ECANCELED ? ISO_SCAN_CANCELLED : ISO_SCAN_ERROR);
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
	return 0;
}

int ExtractIsoTree(const char* image, const char* directory,
	const ISO_ANALYSIS_OPTIONS* options, ISO_EXTRACT_RESULT* result)
{
	ISO_EXTRACT_CONTEXT context = { 0 };
	ISO_TREE_WALK walk = { VisitExtractEntry, &context, false, 0 };
	int error, status;

	if (result == NULL)
		return EINVAL;
	memset(result, 0, sizeof(*result));
	if (image == NULL || directory == NULL || options == NULL)
		return EINVAL;
	context.image = image;
	context.result = result;
	context.root = -1;
	context.rockridge = options->enable_rockridge;
	error = OpenImageFileSource(image, &context.source);
	if (error != 0)
		return error;
	context.root = open(directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	error = context.root < 0 ? errno : CheckEmptyDirectory(context.root);
	if (error != 0)
		goto out;
	error = OpenExtractionSource(&context, options);
	if (error != 0)
		goto out;
	walk.enable_rockridge = options->enable_rockridge;
	if (context.view.iso != NULL)
		walk.joliet_level = iso9660_ifs_get_joliet_level(context.view.iso);
	context.buffer = malloc(EXTRACT_BUFFER_SIZE);
	error = ENOMEM;
	if (context.buffer == NULL)
		goto out;
	for (int pass = 0; pass < 2; pass++) {
		context.preflight = pass == 0;
		error = CheckImageFileSource(&context.source, image);
		if (error != 0)
			goto out;
		status = context.view.udf != NULL ? WalkUdfFiles(udf_get_root(context.view.udf, true, 0), "", &walk)
			: WalkIsoFiles(context.view.iso, "", &walk);
		error = context.error != 0 ? context.error : (status == ISO_SCAN_CANCELLED ? ECANCELED : EIO);
		if (status != ISO_SCAN_OK)
			goto out;
	}
	error = EIO;
	if (result->bytes_written != context.expected_bytes || result->files != context.expected_files)
		goto out;
	strcpy(result->path, "/");
	error = SyncOutput(context.root);
	if (error == 0)
		error = CheckImageFileSource(&context.source, image);
out:
	free(context.buffer);
	CloseIsoAnalysisSource(&context.view);
	CloseImageFileSource(&context.source);
	if (context.root >= 0 && close(context.root) != 0 && error == 0)
		error = errno;
	if (error == 0)
		result->path[0] = 0;
	return error;
}
