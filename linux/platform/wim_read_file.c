/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "wim_read_file.h"
#include "iso_scan.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum { WIM_READ_CHUNK_BYTES = 32 * 1024 };

typedef struct {
	char name[256];
	size_t size;
} WIM_FILE_ENTRY;

static int CheckFile(const struct wimlib_dir_entry* entry, void* context)
{
	WIM_FILE_ENTRY* file = context;
	uint64_t size = entry->streams[0].resource.uncompressed_size;
	const char* name = entry->filename;

	if (IsIsoScanCancelled())
		return WIMLIB_ERR_ABORTED_BY_PROGRESS;
	const unsigned unsupported_attributes = WIMLIB_FILE_ATTRIBUTE_DIRECTORY | WIMLIB_FILE_ATTRIBUTE_DEVICE |
		WIMLIB_FILE_ATTRIBUTE_REPARSE_POINT | WIMLIB_FILE_ATTRIBUTE_ENCRYPTED;
	if (entry->attributes & unsupported_attributes)
		return WIMLIB_ERR_NOT_A_REGULAR_FILE;
	if (entry->num_named_streams != 0 || size > WIM_READ_FILE_MAX_SIZE)
		return WIMLIB_ERR_UNSUPPORTED;
	if (name == NULL)
		return WIMLIB_ERR_INVALID_PARAM;
	bool valid_length = *name != 0 && strlen(name) < sizeof(file->name);
	bool has_separator = strchr(name, '/') != NULL || strchr(name, '\\') != NULL;
	bool dot_directory = strcmp(name, ".") == 0 || strcmp(name, "..") == 0;
	if (!valid_length || has_separator || dot_directory)
		return WIMLIB_ERR_INVALID_PARAM;
	strcpy(file->name, name);
	file->size = (size_t)size;
	return 0;
}

static enum wimlib_progress_status CheckCancellation(enum wimlib_progress_msg message,
	union wimlib_progress_info* info, void* context)
{
	(void)message;
	(void)info;
	(void)context;
	return IsIsoScanCancelled() ? WIMLIB_PROGRESS_STATUS_ABORT : WIMLIB_PROGRESS_STATUS_CONTINUE;
}

static int ReadExtractedFile(const char* path, size_t size, uint8_t** data)
{
	struct stat info;
	uint8_t* bytes = NULL;
	size_t position = 0;
	ssize_t count;
	int error = WIMLIB_ERR_READ;
	int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);

	if (fd < 0)
		return WIMLIB_ERR_OPEN;
	if (fstat(fd, &info) != 0)
		goto out;
	bool expected_size = info.st_size >= 0 && (uint64_t)info.st_size == size;
	if (!S_ISREG(info.st_mode) || !expected_size)
		goto out;
	if (size != 0) {
		bytes = malloc(size);
		if (bytes == NULL) {
			error = WIMLIB_ERR_NOMEM;
			goto out;
		}
	}
	while (position < size) {
		if (IsIsoScanCancelled()) {
			error = WIMLIB_ERR_ABORTED_BY_PROGRESS;
			goto out;
		}
		size_t remaining = size - position;
		size_t chunk = remaining > WIM_READ_CHUNK_BYTES ? WIM_READ_CHUNK_BYTES : remaining;
		count = read(fd, bytes + position, chunk);
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			goto out;
		position += (size_t)count;
	}
	error = 0;
	*data = bytes;
	bytes = NULL;
out:
	free(bytes);
	close(fd);
	return error;
}

int ReadWimFile(WIMStruct* wim, int image, const char* path, uint8_t** data, size_t* size)
{
	WIM_FILE_ENTRY entry = { 0 };
	char directory[] = "/tmp/rufus-wim-file-XXXXXX";
	char extracted[sizeof(directory) + sizeof(entry.name)];
	uint8_t* bytes = NULL;
	int error;

	if (data != NULL)
		*data = NULL;
	if (size != NULL)
		*size = 0;
	bool valid_image = wim != NULL && image >= 1;
	bool valid_path = path != NULL && *path != 0;
	bool valid_output = data != NULL && size != NULL;
	if (!valid_image || !valid_path || !valid_output)
		return WIMLIB_ERR_INVALID_PARAM;
	if (IsIsoScanCancelled())
		return WIMLIB_ERR_ABORTED_BY_PROGRESS;
	error = wimlib_iterate_dir_tree(wim, image, path, WIMLIB_ITERATE_DIR_TREE_FLAG_RESOURCES_NEEDED,
		CheckFile, &entry);
	if (IsIsoScanCancelled())
		return WIMLIB_ERR_ABORTED_BY_PROGRESS;
	if (error != 0)
		return error;
	if (mkdtemp(directory) == NULL)
		return WIMLIB_ERR_MKDIR;
	snprintf(extracted, sizeof(extracted), "%s/%s", directory, entry.name);
	wimlib_register_progress_function(wim, CheckCancellation, NULL);
	error = wimlib_extract_paths(wim, image, directory, &path, 1,
		WIMLIB_EXTRACT_FLAG_NO_PRESERVE_DIR_STRUCTURE | WIMLIB_EXTRACT_FLAG_NO_ACLS |
		WIMLIB_EXTRACT_FLAG_NO_ATTRIBUTES | WIMLIB_EXTRACT_FLAG_NORPFIX);
	wimlib_register_progress_function(wim, NULL, NULL);
	if (error == 0)
		error = ReadExtractedFile(extracted, entry.size, &bytes);
	if (unlink(extracted) != 0 && errno != ENOENT && error == 0)
		error = WIMLIB_ERR_WRITE;
	if (rmdir(directory) != 0 && error == 0)
		error = WIMLIB_ERR_WRITE;
	if (IsIsoScanCancelled())
		error = WIMLIB_ERR_ABORTED_BY_PROGRESS;
	if (error != 0) {
		free(bytes);
		return error;
	}
	*data = bytes;
	*size = entry.size;
	return 0;
}
