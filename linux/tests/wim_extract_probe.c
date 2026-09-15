/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "platform/wim_read_file.h"
#include "native_input.h"
#include "iso_file.h"
#include "pe_arch.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char* fault = "none";
static int cancelled, reads, max_blocks, iso_reads, extract_calls, point;
static int active, reading_output;
static char temporary[128];
static wimlib_progress_func_t progress;

int IsIsoScanCancelled(void) { return cancelled; }

long __real_iso9660_iso_seek_read(const iso9660_t*, void*, lsn_t, long);
ssize_t __real_udf_read_block(const udf_dirent_t*, void*, size_t);
char* __real_mkdtemp(char*);
ssize_t __real_read(int, void*, size_t);
void* __real_malloc(size_t);
int __real_wimlib_extract_paths(WIMStruct*, int, const char*, const char* const*, size_t, int);
void __real_wimlib_register_progress_function(WIMStruct*, wimlib_progress_func_t, void*);

static int ReadFault(long blocks)
{
	if (!active)
		return 0;
	reads++;
	if (blocks > max_blocks) max_blocks = (int)blocks;
	if (reads != point)
		return 0;
	if (strcmp(fault, "cancel-io") == 0) cancelled = 1;
	return strcmp(fault, "fail-io") == 0 || strcmp(fault, "short-io") == 0;
}

long __wrap_iso9660_iso_seek_read(const iso9660_t* iso, void* data, lsn_t lsn, long blocks)
{
	if (active) iso_reads++;
	if (ReadFault(blocks)) { errno = EIO; return strcmp(fault, "short-io") == 0 ? 0 : -1; }
	return __real_iso9660_iso_seek_read(iso, data, lsn, blocks);
}

ssize_t __wrap_udf_read_block(const udf_dirent_t* file, void* data, size_t blocks)
{
	if (ReadFault((long)blocks)) { errno = EIO; return strcmp(fault, "short-io") == 0 ? 0 : -1; }
	return __real_udf_read_block(file, data, blocks);
}

char* __wrap_mkdtemp(char* path)
{
	char* result;
	if (strcmp(fault, "mkdir") == 0) { errno = EACCES; return NULL; }
	result = __real_mkdtemp(path);
	if (result) snprintf(temporary, sizeof(temporary), "%s", result);
	return result;
}

void __wrap_wimlib_register_progress_function(WIMStruct* wim, wimlib_progress_func_t fn, void* ctx)
{
	progress = fn;
	__real_wimlib_register_progress_function(wim, fn, ctx);
}

int __wrap_wimlib_extract_paths(WIMStruct* wim, int index, const char* target,
	const char* const* paths, size_t count, int flags)
{
	struct stat info;
	int error;
	extract_calls++;
	if (stat(target, &info) != 0 || (info.st_mode & 0777) != 0700 || count != 1)
		abort();
	if (strcmp(fault, "cancel-extract") == 0) cancelled = 1;
	error = __real_wimlib_extract_paths(wim, index, target, paths, count, flags);
	if (error == 0 && strcmp(fault, "size") == 0) {
		char path[512];
		snprintf(path, sizeof(path), "%s/%s", target, strrchr(paths[0], '/') + 1);
		if (truncate(path, 1) != 0) abort();
	}
	reading_output = 1;
	if (strcmp(fault, "cancel-after") == 0) cancelled = 1;
	if (strcmp(fault, "write") == 0) return WIMLIB_ERR_WRITE;
	return error;
}

ssize_t __wrap_read(int fd, void* data, size_t count)
{
	if (reading_output) {
		if (strcmp(fault, "read") == 0) { errno = EIO; return -1; }
		if (strcmp(fault, "short") == 0) return 0;
		if (strcmp(fault, "cancel-read") == 0) cancelled = 1;
	}
	return __real_read(fd, data, count);
}

void* __wrap_malloc(size_t size)
{
	if (reading_output && strcmp(fault, "allocate") == 0) return NULL;
	return __real_malloc(size);
}

static void CheckParameters(WIMStruct* wim)
{
	uint8_t* data = (uint8_t*)&data;
	size_t size = 123;
	if (ReadWimFile(NULL, 1, "/file", &data, &size) != WIMLIB_ERR_INVALID_PARAM || data || size) abort();
	if (ReadWimFile(wim, 1, NULL, &data, &size) != WIMLIB_ERR_INVALID_PARAM || data || size) abort();
	size = 123;
	if (ReadWimFile(wim, 1, "/file", NULL, &size) != WIMLIB_ERR_INVALID_PARAM || size) abort();
	data = (uint8_t*)&data;
	if (ReadWimFile(wim, 1, "/file", &data, NULL) != WIMLIB_ERR_INVALID_PARAM || data) abort();
}

static int DescriptorCount(void)
{
	DIR* directory = opendir("/proc/self/fd");
	int count = 0;
	if (!directory) abort();
	while (readdir(directory)) count++;
	closedir(directory);
	return count;
}

int main(int argc, char** argv)
{
	ISO_FILE_READER reader = { 0 };
	iso9660_t* iso = NULL;
	udf_t* udf = NULL;
	WIMStruct* wim = NULL;
	uint8_t* data = NULL;
	size_t size = 0;
	int error, before, index, status = 0;
	uint8_t signature[8];

	if (argc != 6 && argc != 7 && argc != 8) return 64;
	if (argc >= 7) fault = argv[6];
	if (argc == 8) point = atoi(argv[7]);
	index = atoi(argv[5]);
	if (wimlib_global_init(WIMLIB_INIT_FLAG_ASSUME_UTF8) != 0) return 65;
	before = DescriptorCount();
	if (strcmp(argv[2], "file") == 0)
		error = wimlib_open_wim(argv[1], 0, &wim);
	else {
		if (strcmp(argv[2], "udf") == 0) udf = udf_open(argv[1]);
		else iso = iso9660_open_ext(argv[1], strcmp(argv[2], "joliet") == 0 ?
			ISO_EXTENSION_JOLIET : ISO_EXTENSION_ALL & ~ISO_EXTENSION_JOLIET);
		error = OpenIsoFileReader(&reader, iso, udf, argv[3]);
		if (error == 0) error = OpenWimFromIsoReader(&reader, 0, &wim);
	}
	active = 1; // Inject faults in file metadata/data reads after the WIM opens.
	CheckParameters(wim);
	if (strcmp(fault, "cancel-pre") == 0) cancelled = 1;
	if (error == 0) error = ReadWimFile(wim, index, argv[4], &data, &size);
	active = 0;
	if (error != 0 && (data != NULL || size != 0)) abort();
	if (progress != NULL) abort();
	if (temporary[0] && access(temporary, F_OK) == 0) abort();
	if (error == 0 && size != 0 && fwrite(data, 1, size, stdout) != size) abort();
	fprintf(stderr, "ERROR=%d SIZE=%zu ARCH=%d READS=%d MAX_BLOCKS=%d ISO_READS=%d EXTRACTS=%d\n",
		error, size, FindPeArch(data, size), reads, max_blocks, iso_reads, extract_calls);
	free(data);
	// A cancelled/failed extraction must not poison this WIM's next operation.
	if (error != 0 && strcmp(fault, "none") != 0 && wim != NULL) {
		fault = "none"; cancelled = 0; reading_output = 0;
		if (ReadWimFile(wim, index, argv[4], &data, &size) != 0) status = 66;
		free(data);
		if (temporary[0] && access(temporary, F_OK) == 0) abort();
	}
	wimlib_free(wim);
	cancelled = 0;
	if (reader.size >= sizeof(signature) && ReadIsoFileRange(&reader, 0, signature, sizeof(signature)) != 0)
		status = 67;
	CloseIsoFileReader(&reader);
	iso9660_close(iso);
	if (udf != NULL) udf_close(udf);
	if (DescriptorCount() != before) abort();
	wimlib_global_cleanup();
	return status;
}
