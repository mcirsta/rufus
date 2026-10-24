/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "iso_wim_split.h"
#include "native_input.h"
#include "rufus_reporting.h"
#include "progress_policy.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define WIM_SPLIT_PART_BYTES (4094ULL * 1024 * 1024)
#define FAT32_MAX_FILE_BYTES UINT32_MAX
#define WIM_SPLIT_HEADROOM_BYTES ((UINT64_C(1) << 32) - WIM_SPLIT_PART_BYTES)
enum { SPLIT_NAME_CAPACITY = 32, SPLIT_PATH_CAPACITY = 96 };

typedef struct {
	const IMAGE_FILE_SOURCE* source;
	const char* image;
	int stage, error;
	unsigned started, published;
	uint64_t previous_blocks, total_blocks, part_start;
	ISO_WIM_SPLIT_PLAN plan;
	ISO_WIM_SPLIT_RESULT result;
	ISO_WIM_PART_READY part_ready;
	void* part_context;
} WIM_SPLIT_CONTEXT;

static int WimError(int error)
{
	if (error == 0)
		return 0;
	if (error == WIMLIB_ERR_ABORTED_BY_PROGRESS || IsIsoScanCancelled())
		return ECANCELED;
	if (error == WIMLIB_ERR_NOMEM)
		return ENOMEM;
	if (error == WIMLIB_ERR_UNSUPPORTED)
		return ENOTSUP;
	uprintf("Could not split WIM: %s", wimlib_get_error_string(error));
	return EIO;
}

static int CheckResource(const struct wimlib_resource_entry* resource, void* data)
{
	int* error = data;
	if (IsIsoScanCancelled())
		*error = ECANCELED;
	else if (resource->packed || resource->is_missing || resource->is_spanned)
		*error = ENOTSUP;
	else if (resource->compressed_size >= FAT32_MAX_FILE_BYTES)
		*error = EFBIG;
	return *error == 0 ? 0 : WIMLIB_ERR_ABORTED_BY_PROGRESS;
}

static int OpenSplitWim(ISO_FILE_READER* reader, WIMStruct** wim)
{
	struct wimlib_wim_info info;
	int error = 0;
	int status = OpenWimFromIsoReader(reader, 0, wim);
	if (status != 0)
		return WimError(status);
	status = wimlib_get_wim_info(*wim, &info);
	if (status != 0)
		return WimError(status);
	if (info.total_parts != 1)
		return ENOTSUP;
	status = wimlib_iterate_lookup_table(*wim, 0, CheckResource, &error);
	return error != 0 ? error : WimError(status);
}

typedef struct {
	uint64_t resource_bytes;
	unsigned parts;
} WIM_SPLIT_LAYOUT;

static enum wimlib_progress_status ReadSplitLayout(enum wimlib_progress_msg message,
	union wimlib_progress_info* info, void* data)
{
	WIM_SPLIT_LAYOUT* layout = data;
	if (message == WIMLIB_PROGRESS_MSG_SPLIT_BEGIN_PART && !IsIsoScanCancelled()) {
		layout->resource_bytes = info->split.total_bytes;
		layout->parts = info->split.total_parts;
	}
	return WIMLIB_PROGRESS_STATUS_ABORT;
}

static int PlanSplitWim(WIMStruct* wim, uint64_t source_bytes, ISO_WIM_SPLIT_PLAN* plan)
{
	WIM_SPLIT_LAYOUT layout = { 0 };
	wimlib_register_progress_function(wim, ReadSplitLayout, &layout);
	// The callback stops before write_wim_part. Even if that contract changes,
	// this nonexistent descriptor path cannot create a file in the host's cwd.
	int status = wimlib_split(wim, "/proc/self/fd/-1/install.swm",
		WIM_SPLIT_PART_BYTES, WIMLIB_WRITE_FLAG_FSYNC);
	if (IsIsoScanCancelled())
		return ECANCELED;
	if (status != WIMLIB_ERR_ABORTED_BY_PROGRESS)
		return status == 0 ? EIO : WimError(status);
	if (layout.parts == 0 || layout.resource_bytes > source_bytes)
		return EBADMSG;
	uint64_t overhead = source_bytes - layout.resource_bytes;
	if (overhead < WIM_SPLIT_HEADROOM_BYTES)
		overhead = WIM_SPLIT_HEADROOM_BYTES;
	if (layout.parts > (UINT64_MAX - source_bytes) / overhead)
		return EOVERFLOW;
	plan->reserved_bytes = source_bytes + layout.parts * overhead;
	plan->parts = layout.parts;
	return 0;
}

int PlanIsoWimSplit(ISO_FILE_READER* reader, ISO_WIM_SPLIT_PLAN* plan)
{
	WIMStruct* wim = NULL;
	if (plan == NULL)
		return EINVAL;
	memset(plan, 0, sizeof(*plan));
	if (reader == NULL)
		return EINVAL;
	int error = OpenSplitWim(reader, &wim);
	if (error == 0)
		error = PlanSplitWim(wim, reader->size, plan);
	wimlib_free(wim);
	return error;
}

static void PartName(char* name, unsigned part)
{
	if (part == 1)
		strcpy(name, "install.swm");
	else
		snprintf(name, SPLIT_NAME_CAPACITY, "install%u.swm", part);
}

static int CheckPart(WIM_SPLIT_CONTEXT* context, unsigned part)
{
	char name[SPLIT_NAME_CAPACITY];
	struct stat info;
	PartName(name, part);
	if (fstatat(context->stage, name, &info, AT_SYMLINK_NOFOLLOW) != 0)
		return errno;
	if (!S_ISREG(info.st_mode) || info.st_size < 0)
		return ENOTSUP;
	if (info.st_nlink != 1)
		return EMLINK;
	if ((uint64_t)info.st_size > FAT32_MAX_FILE_BYTES)
		return EFBIG;
	if ((uint64_t)info.st_size > context->plan.reserved_bytes - context->result.bytes)
		return ENOSPC;
	context->result.bytes += (uint64_t)info.st_size;
	context->result.parts++;
	return 0;
}

static enum wimlib_progress_status SplitProgress(enum wimlib_progress_msg message,
	union wimlib_progress_info* info, void* data)
{
	WIM_SPLIT_CONTEXT* context = data;
	context->error = CheckImageFileSource(context->source, context->image);
	if (context->error != 0)
		return WIMLIB_PROGRESS_STATUS_ABORT;
	if (message == WIMLIB_PROGRESS_MSG_SPLIT_BEGIN_PART) {
		context->started = info->split.cur_part_number;
		context->part_start = info->split.completed_bytes;
	} else if (message == WIMLIB_PROGRESS_MSG_WRITE_STREAMS) {
		// Same compressed-resource arithmetic as Win32's WimProgressFunc.
		uint64_t completed = context->part_start + info->write_streams.completed_compressed_bytes;
		context->result.copied_blocks = context->previous_blocks + completed / ISO_BLOCKSIZE;
		_UpdateProgressWithInfo(OP_FILE_COPY, 0, context->result.copied_blocks, context->total_blocks, 0);
	} else if (message == WIMLIB_PROGRESS_MSG_SPLIT_END_PART)
		context->error = CheckPart(context, info->split.cur_part_number);
	return context->error == 0 ? WIMLIB_PROGRESS_STATUS_CONTINUE : WIMLIB_PROGRESS_STATUS_ABORT;
}

static int SyncDirectory(int fd)
{
	int status;
	do {
		status = fsync(fd);
	} while (status != 0 && errno == EINTR);
	return status == 0 ? 0 : errno;
}

static int PublishParts(WIM_SPLIT_CONTEXT* context, int parent)
{
	char name[SPLIT_NAME_CAPACITY];
	for (unsigned part = 1; part <= context->result.parts; part++) {
		int error = CheckImageFileSource(context->source, context->image);
		if (error != 0)
			return error;
		PartName(name, part);
		if (context->part_ready != NULL) {
			error = context->part_ready(context->part_context, context->stage, name);
			if (error != 0)
				return error;
		}
		if (renameat2(context->stage, name, parent, name, RENAME_NOREPLACE) != 0)
			return errno;
		context->published = part;
	}
	return 0;
}

static int RemoveParts(int directory, unsigned first, unsigned last)
{
	char name[SPLIT_NAME_CAPACITY];
	int error = 0;
	// Do not honor cancellation here: it must not interrupt removal of our files.
	for (unsigned part = first; part <= last; part++) {
		PartName(name, part);
		if (unlinkat(directory, name, 0) != 0 && errno != ENOENT) {
			error = errno;
			uprintf("Could not remove incomplete WIM part %s: %s", name, strerror(error));
		}
	}
	return error;
}

int SplitIsoWim(ISO_FILE_READER* reader, const IMAGE_FILE_SOURCE* source,
	const char* image, int parent, uint64_t copied_blocks, uint64_t total_blocks,
	ISO_WIM_SPLIT_RESULT* result, ISO_WIM_PART_READY part_ready, void* part_context)
{
	WIM_SPLIT_CONTEXT context = { .source = source, .image = image, .stage = -1,
		.previous_blocks = copied_blocks, .total_blocks = total_blocks,
		.part_ready = part_ready, .part_context = part_context };
	WIMStruct* wim = NULL;
	char stage_path[SPLIT_PATH_CAPACITY], output_path[SPLIT_PATH_CAPACITY];
	bool stage_created = false;
	int error, status;

	if (result == NULL)
		return EINVAL;
	memset(result, 0, sizeof(*result));
	bool valid_source = reader != NULL && source != NULL && image != NULL;
	if (!valid_source || parent < 0)
		return EINVAL;
	context.result.copied_blocks = copied_blocks;
	error = CheckImageFileSource(source, image);
	if (error == 0)
		error = OpenSplitWim(reader, &wim);
	if (error == 0)
		error = PlanSplitWim(wim, reader->size, &context.plan);
	if (error != 0)
		goto out;
	snprintf(stage_path, sizeof(stage_path), "/proc/self/fd/%d/.rufus-wim-XXXXXX", parent);
	if (mkdtemp(stage_path) == NULL) {
		error = errno;
		goto out;
	}
	stage_created = true;
	context.stage = open(stage_path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (context.stage < 0) {
		error = errno;
		goto out;
	}
	snprintf(output_path, sizeof(output_path), "/proc/self/fd/%d/install.swm", context.stage);
	wimlib_register_progress_function(wim, SplitProgress, &context);
	status = wimlib_split(wim, output_path, WIM_SPLIT_PART_BYTES, WIMLIB_WRITE_FLAG_FSYNC);
	error = context.error != 0 ? context.error : WimError(status);
	if (error == 0)
		error = CheckImageFileSource(source, image);
	if (error == 0)
		error = PublishParts(&context, parent);
out:
	wimlib_free(wim);
	if (context.stage >= 0) {
		status = RemoveParts(context.stage, context.published + 1, context.started);
		if (status != 0)
			error = status;
		if (close(context.stage) != 0 && error == 0)
			error = errno;
	}
	if (stage_created) {
		const char* name = strrchr(stage_path, '/') + 1;
		if (unlinkat(parent, name, AT_REMOVEDIR) != 0) {
			error = errno;
			uprintf("Could not remove WIM staging directory: %s", strerror(error));
		}
		status = SyncDirectory(parent);
		if (status != 0)
			error = status;
	}
	if (error == 0)
		error = CheckImageFileSource(source, image);
	if (error != 0 && context.published != 0) {
		status = RemoveParts(parent, 1, context.published);
		if (status != 0)
			error = status;
		status = SyncDirectory(parent);
		if (status != 0)
			error = status;
	}
	if (error == 0)
		*result = context.result;
	else
		result->copied_blocks = context.result.copied_blocks;
	return error;
}
