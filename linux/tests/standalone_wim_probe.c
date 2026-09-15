/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Native classification/ownership integration probe; mutation modes are tests. */
#include "platform/image_source.h"
#include "platform/wim_file.h"
#include "platform/wim_read_file.h"
#include "pe_arch.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int cancelled, opened;
int IsIsoScanCancelled(void) { return cancelled; }
int __real_wimlib_open_wim(const char*, int, WIMStruct**);

int __wrap_wimlib_open_wim(const char* path, int flags, WIMStruct** wim)
{
	if (strncmp(path, "/proc/self/fd/", 14) != 0 || flags != 0)
		abort();
	opened++;
	return __real_wimlib_open_wim(path, flags, wim);
}

static int DescriptorCount(void)
{
	DIR* directory = opendir("/proc/self/fd");
	int count = 0;
	if (!directory)
		abort();
	while (readdir(directory)) count++;
	closedir(directory);
	return count;
}

static void ReplacePath(const char* path, const char* replacement, const struct stat* original)
{
	struct timespec times[2] = { original->st_atim, original->st_mtim };
	if (rename(replacement, path) != 0 || utimensat(AT_FDCWD, path, times, 0) != 0)
		abort();
}

static void ChangeInPlace(const char* path, const struct stat* original)
{
	struct timespec times[2] = { original->st_atim, original->st_mtim };
	uint8_t byte;
	int fd = open(path, O_RDWR);
	if (fd < 0 || pread(fd, &byte, 1, 0) != 1)
		abort();
	byte ^= 1;
	if (pwrite(fd, &byte, 1, 0) != 1 || futimens(fd, times) != 0)
		abort();
	close(fd);
}

int main(int argc, char** argv)
{
	IMAGE_FILE_SOURCE source = { .fd = -1 };
	WIMStruct* wim = NULL;
	RUFUS_WIM_METADATA metadata = { 0 };
	uint8_t* data = NULL;
	size_t size = 0;
	const char* mode = argc >= 3 ? argv[2] : "none";
	int source_error, check_error = 0, wim_error = 0, metadata_error = 0, efi_error = 0;
	int arch = ARCH_UNKNOWN, before, is_wim, source_current;
	char prefix[8];

	if (argc < 2 || argc > 4)
		return 64;
	if (wimlib_global_init(WIMLIB_INIT_FLAG_ASSUME_UTF8 | WIMLIB_INIT_FLAG_DEFAULT_CASE_INSENSITIVE) != 0)
		return 65;
	before = DescriptorCount();
	if (strcmp(mode, "cancel-before") == 0)
		cancelled = 1;
	source_error = OpenImageFileSource(argv[1], &source);
	is_wim = source.is_wim;
	if (source_error == 0) {
		if (argc == 4 && strcmp(mode, "replace-before-open") == 0)
			ReplacePath(argv[1], argv[3], &source.info);
		if (strcmp(mode, "unlink-before-open") == 0 && unlink(argv[1]) != 0)
			abort();
		if (source.is_wim) {
			if (lseek(source.fd, 3, SEEK_SET) != 3)
				abort();
			wim_error = OpenWimFileDescriptor(source.fd, &wim);
			if (lseek(source.fd, 0, SEEK_CUR) != 3)
				abort();
			if (argc == 4 && strcmp(mode, "replace-after-open") == 0)
				ReplacePath(argv[1], argv[3], &source.info);
			if (wim_error == 0) {
				metadata_error = ReadWimMetadata(wim, 1, &metadata);
				efi_error = ReadWimFile(wim, 1, "Windows/Boot/EFI/bootmgr.efi", &data, &size);
				if (efi_error != 0 && (data != NULL || size != 0))
					abort();
				arch = FindPeArch(data, size);
			}
		}
		free(data);
		wimlib_free(wim);
		// wimlib must not take ownership of the retained source or its offset.
		if (fcntl(source.fd, F_GETFD) < 0)
			abort();
		if (source.info.st_size >= 8 && pread(source.fd, prefix, 8, 0) != 8)
			abort();
		if (argc == 4 && strcmp(mode, "replace-after-read") == 0)
			ReplacePath(argv[1], argv[3], &source.info);
		if (strcmp(mode, "change-after-read") == 0)
			ChangeInPlace(argv[1], &source.info);
		if (strcmp(mode, "truncate-after-read") == 0 && truncate(argv[1], 1) != 0)
			abort();
		if (strcmp(mode, "cancel-after-read") == 0)
			cancelled = 1;
		check_error = CheckImageFileSource(&source, argv[1]);
	}
	source_current = source_error == 0 && check_error == 0;
	printf("{\"source_error\":%d,\"check_error\":%d,\"is_wim\":%d,\"opened\":%d,"
		"\"wim_error\":%d,\"metadata_error\":%d,\"efi_error\":%d,\"observed_arch\":%d,"
		"\"archive_version\":%u,\"image_count\":%u,\"boot_index\":%u,"
		"\"valid_fields\":%u,\"invalid_fields\":%u,\"major\":%u,"
		"\"source_current\":%d,\"current_arch\":%d}\n", source_error, check_error, is_wim, opened,
		wim_error, metadata_error, efi_error, arch, metadata.archive.version,
		metadata.archive.image_count, metadata.archive.boot_index, metadata.valid_fields,
		metadata.invalid_fields, metadata.win_version.major, source_current, source_current ? arch : 0);
	CloseImageFileSource(&source);
	CloseImageFileSource(&source);
	if (source.fd != -1 || source.is_wim || DescriptorCount() != before)
		abort();
	wimlib_global_cleanup();
	return 0;
}
