/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Fault injection is linked only into this disposable-directory test binary. */
#include "platform/iso_extract.h"
#include "iso_file.h"
#include <cdio/logging.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char* fault;
static const char* image_path;
static const char* target_path;
static int cancelled, output_fd = -1, writes, reads, syncs, closes, fired;
static uint64_t written;
static size_t max_read, max_write;
static int output_flags;

enum {
	SHORT_WRITE_BYTES = 127,
	SHORT_WRITE_CALLS = 4
};

#if defined(__GLIBC__) && defined(__USE_FILE_OFFSET64)
#define WrappedOpenat __wrap_openat64
#define RealOpenat __real_openat64
#else
#define WrappedOpenat __wrap_openat
#define RealOpenat __real_openat
#endif

int RealOpenat(int, const char*, int, ...);
int __real_close(int);
int __real_fsync(int);
int __real_mkdirat(int, const char*, mode_t);
ssize_t __real_write(int, const void*, size_t);
int __real_ReadIsoFileRange(ISO_FILE_READER*, uint64_t, void*, size_t);
int __real_AnalyzeIsoImageWithSource(const char*, uint64_t, const ISO_ANALYSIS_OPTIONS*, ISO_ANALYSIS*, ISO_ANALYSIS_SOURCE*);

static bool Fault(const char* name)
{
	return strcmp(fault, name) == 0;
}

int IsIsoScanCancelled(void)
{
	return cancelled;
}

void uprintf(const char* format, ...)
{
	va_list arguments;
	va_start(arguments, format);
	vfprintf(stderr, format, arguments);
	va_end(arguments);
	fputc('\n', stderr);
}

static void LogCdio(cdio_log_level_t level, const char* message)
{
	(void)level;
	fprintf(stderr, "%s\n", message);
}

static int DescriptorCount(void)
{
	DIR* directory = opendir("/proc/self/fd");
	struct dirent* entry;
	int count = 0;

	if (directory == NULL)
		abort();
	while ((entry = readdir(directory)) != NULL)
		if (entry->d_name[0] != '.')
			count++;
	closedir(directory);
	return count;
}

static void ChangeSource(void)
{
	struct stat info;
	struct timespec times[2];
	int fd;
	char marker;

	if (stat(image_path, &info) != 0)
		abort();
	times[0] = info.st_atim;
	times[1] = info.st_mtim;
	fd = open(image_path, O_RDWR | O_CLOEXEC);
	if (fd < 0 || pread(fd, &marker, 1, 1) != 1)
		abort();
	marker ^= 1;
	if (pwrite(fd, &marker, 1, 1) != 1 || futimens(fd, times) != 0)
		abort();
	__real_close(fd);
}

int WrappedOpenat(int parent, const char* path, int flags, ...)
{
	mode_t mode = 0;
	va_list arguments;
	int fd;

	if (flags & O_CREAT) {
		va_start(arguments, flags);
		mode = (mode_t)va_arg(arguments, int);
		va_end(arguments);
		if (Fault("open-fail")) {
			errno = EACCES;
			return -1;
		}
		// An entry appearing after the empty-directory check must not be overwritten.
		if (Fault("collision") && !fired++) {
			fd = RealOpenat(parent, path, O_WRONLY | O_CREAT | O_EXCL, 0600);
			if (fd < 0 || __real_write(fd, "existing", 8) != 8)
				abort();
			__real_close(fd);
		}
	}
	fd = RealOpenat(parent, path, flags, mode);
	if (fd >= 0 && (flags & O_CREAT)) {
		output_fd = fd;
		output_flags = flags;
	}
	return fd;
}

int __wrap_mkdirat(int parent, const char* name, mode_t mode)
{
	int result;
	char outside[4096];
	if (Fault("mkdir-fail")) {
		errno = EACCES;
		return -1;
	}
	result = __real_mkdirat(parent, name, mode);
	if (result == 0 && Fault("parent-link") && !fired++) {
		snprintf(outside, sizeof(outside), "%s/../outside", target_path);
		if (unlinkat(parent, name, AT_REMOVEDIR) != 0 || symlinkat(outside, parent, name) != 0)
			abort();
	}
	return result;
}

ssize_t __wrap_write(int fd, const void* buffer, size_t size)
{
	ssize_t count;

	if (fd != output_fd)
		return __real_write(fd, buffer, size);
	writes++;
	if (size > max_write)
		max_write = size;
	if (Fault("zero-write"))
		return 0;
	if (Fault("eintr") && writes == 1) {
		errno = EINTR;
		return -1;
	}
	if (Fault("write-fail") && writes > 1) {
		errno = ENOSPC;
		return -1;
	}
	bool short_write = Fault("short") && writes <= SHORT_WRITE_CALLS;
	bool stop_after_write = Fault("write-fail") || Fault("write-cancel");
	if ((short_write || stop_after_write) && size > SHORT_WRITE_BYTES)
		size = SHORT_WRITE_BYTES;
	count = __real_write(fd, buffer, size);
	if (count > 0)
		written += (uint64_t)count;
	if (Fault("write-cancel"))
		cancelled = 1;
	if (Fault("source-change") && writes == 1)
		ChangeSource();
	return count;
}

int __wrap_ReadIsoFileRange(ISO_FILE_READER* reader, uint64_t offset, void* buffer, size_t size)
{
	if (output_fd >= 0) {
		reads++;
		if (size > max_read)
			max_read = size;
		if (Fault("read-fail") && reads == 2)
			return ISO_SCAN_PARTIAL;
		if (Fault("read-cancel")) {
			cancelled = 1;
			return ISO_SCAN_CANCELLED;
		}
	}
	return __real_ReadIsoFileRange(reader, offset, buffer, size);
}

int __wrap_fsync(int fd)
{
	struct stat info;
	if (fstat(fd, &info) != 0)
		abort();
	syncs++;
	if (Fault("sync-eintr") && syncs == 1) {
		errno = EINTR;
		return -1;
	}
	if (Fault("sync-fail") && fd == output_fd) {
		errno = EIO;
		return -1;
	}
	if (Fault("directory-sync-fail") && S_ISDIR(info.st_mode)) {
		errno = EIO;
		return -1;
	}
	if (Fault("sync-cancel"))
		cancelled = 1;
	return __real_fsync(fd);
}

int __wrap_close(int fd)
{
	bool output = fd == output_fd;
	int result = __real_close(fd);
	if (output) {
		closes++;
		output_fd = -1;
		if (Fault("close-fail")) {
			errno = EIO;
			return -1;
		}
	}
	return result;
}

int __wrap_AnalyzeIsoImageWithSource(const char* image, uint64_t size,
	const ISO_ANALYSIS_OPTIONS* options, ISO_ANALYSIS* analysis, ISO_ANALYSIS_SOURCE* source)
{
	int status = __real_AnalyzeIsoImageWithSource(image, size, options, analysis, source);
	if (Fault("cancel-after-scan"))
		cancelled = 1;
	if (Fault("change-after-scan"))
		ChangeSource();
	if (Fault("replace-after-scan")) {
		char replacement[4096];
		snprintf(replacement, sizeof(replacement), "%s.replacement", image_path);
		if (rename(replacement, image_path) != 0)
			abort();
	}
	return status;
}

int main(int argc, char** argv)
{
	ISO_ANALYSIS_OPTIONS options = { true, true, { 0, 0 } };
	ISO_EXTRACT_RESULT result;
	int before, after, error, retry = -1;

	if (argc != 5)
		return 64;
	image_path = argv[1];
	target_path = argv[2];
	fault = argv[4];
	if (strcmp(argv[3], "joliet") == 0)
		options.enable_rockridge = false;
	if (strcmp(argv[3], "plain") == 0)
		options.enable_rockridge = options.enable_joliet = false;
	cdio_log_set_handler(LogCdio);
	cancelled = Fault("cancel-before");
	before = DescriptorCount();
	error = ExtractIsoTree(image_path, target_path, &options, &result);
	after = DescriptorCount();
	printf("ERROR=%d FILES=%" PRIu64 " DIRECTORIES=%" PRIu64 " SKIPPED=%" PRIu64
		" WRITTEN=%" PRIu64 " ACTUAL=%" PRIu64 " FDS=%d READS=%d WRITES=%d SYNCS=%d CLOSES=%d"
		" MAX_READ=%zu MAX_WRITE=%zu FLAGS=%d\n",
		error, result.files, result.directories, result.skipped, result.bytes_written, written,
		after - before, reads, writes, syncs, closes, max_read, max_write, output_flags);
	fprintf(stderr, "Last image path: %s\n", result.path);
	if (Fault("write-cancel")) {
		// Recover within this process: no stale cancellation, source or iterator.
		cancelled = 0;
		fault = "none";
		retry = ExtractIsoTree(image_path, target_path, &options, &result);
		printf("RETRY=%d RETRY_FDS=%d\n", retry, DescriptorCount() - before);
	}
	return output_fd >= 0 ? 65 : 0;
}
