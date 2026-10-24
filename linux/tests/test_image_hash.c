/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "platform/image_hash.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static atomic_int checks, failures, cancelled;
static int read_calls, source_fd = -1;
static int progress_calls, endpoints, interrupt_calls;
static uint64_t previous_progress, reported_total;
static char path[256], moved[256], link_path[256];
static const size_t image_bytes = 131109;
static const unsigned interrupt_limit = 3;
static bool progress_cancel;
enum READ_FAULT {
	READ_NORMAL, READ_INTERRUPTED, READ_SHORT, READ_ERROR, READ_LAST_ERROR,
	READ_EOF, READ_CANCELLED, READ_LAST_CANCELLED, READ_REPLACED,
	READ_REMOVED, READ_CHANGED, READ_TRUNCATED, READ_RETARGETED
};
static enum READ_FAULT fault;

#define CHECK(expression) do { checks++; if (!(expression)) { \
	fprintf(stderr, "line %d: %s\n", __LINE__, #expression); failures++; } } while (0)

int IsRufusJobCancelled(void) { return cancelled; }
int IsIsoScanCancelled(void) { return cancelled; }

void _UpdateProgressWithInfo(int operation, int message, uint64_t processed, uint64_t total, int force)
{
	CHECK(operation == -3 && message == 271);
	CHECK(processed >= previous_progress && processed <= total);
	if (progress_calls != 0)
		CHECK(total == reported_total);
	else
		CHECK(processed == 0 && force);
	if (progress_calls != 0 && processed == total) {
		CHECK(force);
		endpoints++;
	}
	previous_progress = processed;
	reported_total = total;
	progress_calls++;
	if (progress_cancel)
		cancelled = 1;
}

#if defined(__GLIBC__) && defined(__USE_FILE_OFFSET64)
#define WrappedPread __wrap_pread64
#define RealPread __real_pread64
#else
#define WrappedPread __wrap_pread
#define RealPread __real_pread
#endif
ssize_t RealPread(int fd, void* data, size_t size, off_t offset);

static void WriteFile(const char* name)
{
	uint8_t data[4096];
	for (size_t i = 0; i < sizeof(data); i++)
		data[i] = (uint8_t)i;
	FILE* file = fopen(name, "wb");
	if (file == NULL)
		abort();
	for (size_t offset = 0; offset < image_bytes;) {
		const size_t remaining = image_bytes - offset;
		const size_t count = remaining < sizeof(data) ? remaining : sizeof(data);
		if (fwrite(data, 1, count, file) != count)
			abort();
		offset += count;
	}
	if (fclose(file) != 0)
		abort();
}

static void MutateFile(void)
{
	if (fault == READ_REPLACED) {
		CHECK(rename(path, moved) == 0);
		WriteFile(path);
	} else if (fault == READ_REMOVED) {
		CHECK(unlink(path) == 0);
	} else if (fault == READ_TRUNCATED) {
		CHECK(truncate(path, 1) == 0);
	} else if (fault == READ_RETARGETED) {
		WriteFile(moved);
		CHECK(unlink(link_path) == 0 && symlink(moved, link_path) == 0);
	} else if (fault == READ_CHANGED) {
		struct stat info;
		CHECK(fstat(source_fd, &info) == 0);
		struct timespec times[2] = { info.st_atim, info.st_mtim };
		const int writer = open(path, O_WRONLY | O_CLOEXEC);
		CHECK(writer >= 0);
		CHECK(pwrite(writer, "changed", 7, 0) == 7);
		/* Restoring mtime must not conceal the ctime change. */
		CHECK(futimens(writer, times) == 0);
		CHECK(close(writer) == 0);
	}
}

ssize_t WrappedPread(int fd, void* data, size_t size, off_t offset)
{
	if (fd != source_fd)
		return RealPread(fd, data, size, offset);
	read_calls++;
	CHECK(size <= 65536 && offset >= 0);
	if (fault == READ_INTERRUPTED && interrupt_calls++ < (int)interrupt_limit) {
		errno = EINTR;
		return -1;
	}
	const bool last = (uint64_t)offset + size == image_bytes;
	if (fault == READ_ERROR || (fault == READ_LAST_ERROR && last)) {
		errno = EIO;
		return -1;
	}
	if (fault == READ_EOF)
		return 0;
	if (fault == READ_SHORT && size > 17)
		size = 17;
	if (fault == READ_CANCELLED || (fault == READ_LAST_CANCELLED && last))
		cancelled = 1;
	const ssize_t count = RealPread(fd, data, size, offset);
	if (last)
		MutateFile();
	return count;
}

static int DescriptorCount(void)
{
	DIR* directory = opendir("/proc/self/fd");
	int count = 0;
	if (directory == NULL)
		abort();
	while (readdir(directory))
		count++;
	closedir(directory);
	return count;
}

static void ResetReports(void)
{
	read_calls = 0;
	interrupt_calls = 0;
	progress_calls = 0;
	endpoints = 0;
	previous_progress = 0;
	reported_total = 0;
}

static bool IsZero(const IMAGE_HASH_RESULT* result)
{
	const unsigned char* bytes = (const unsigned char*)result;
	for (size_t i = 0; i < sizeof(*result); i++) {
		if (bytes[i] != 0)
			return false;
	}
	return true;
}

static void CheckRead(enum READ_FAULT mode, int expected_error, bool extra)
{
	IMAGE_FILE_SOURCE source = { .fd = -1 };
	IMAGE_HASH_RESULT result;
	IMAGE_HASH_RESULT reference;
	const int before = DescriptorCount();
	WriteFile(path);
	const char* selected = mode == READ_RETARGETED ? link_path : path;
	if (mode == READ_RETARGETED)
		CHECK(symlink(path, link_path) == 0);
	CHECK(OpenImageFileSource(selected, &source) == 0);
	CHECK(HashImageFileSource(&source, selected, extra, &reference) == 0);
	CHECK(lseek(source.fd, 11, SEEK_SET) == 11);
	source_fd = source.fd;
	fault = mode;
	ResetReports();
	memset(&result, 0xa5, sizeof(result));
	const int error = HashImageFileSource(&source, selected, extra, &result);
	if (error != expected_error)
		fprintf(stderr, "read fault %d, extra %d: error %d, expected %d\n",
			mode, extra, error, expected_error);
	CHECK(error == expected_error);
	CHECK(lseek(source.fd, 0, SEEK_CUR) == 11);
	CHECK(read_calls > 0);
	CHECK(progress_calls > 0 && reported_total == image_bytes);
	if (expected_error == 0) {
		CHECK(endpoints == 1);
		CHECK(result.count == (extra ? 4 : 3) && result.size == image_bytes);
		CHECK(result.count == reference.count && result.size == reference.size);
		CHECK(memcmp(result.digest, reference.digest, sizeof(result.digest)) == 0);
	} else {
		CHECK(endpoints == 0);
		CHECK(IsZero(&result));
	}
	fault = READ_NORMAL;
	cancelled = 0;
	source_fd = -1;
	ResetReports();
	CloseImageFileSource(&source);
	if (mode != READ_REMOVED)
		CHECK(unlink(path) == 0);
	if (mode == READ_REPLACED || mode == READ_RETARGETED)
		CHECK(unlink(moved) == 0);
	if (mode == READ_RETARGETED)
		CHECK(unlink(link_path) == 0);
	CHECK(DescriptorCount() == before);
}

static void CheckGuards(void)
{
	IMAGE_FILE_SOURCE source = { .fd = -1 };
	IMAGE_HASH_RESULT result;
	WriteFile(path);
	CHECK(OpenImageFileSource(path, &source) == 0);
	ResetReports();
	CHECK(HashImageFileSource(&source, path, false, NULL) == EINVAL);
	memset(&result, 0xa5, sizeof(result));
	CHECK(HashImageFileSource(NULL, path, false, &result) == EINVAL && IsZero(&result));
	CHECK(HashImageFileSource(&source, NULL, false, &result) == EINVAL);
	CHECK(HashImageFileSource(&source, "", false, &result) == EINVAL);
	cancelled = 1;
	CHECK(HashImageFileSource(&source, path, false, &result) == ECANCELED && IsZero(&result));
	cancelled = 0;
	CHECK(progress_calls == 0);
	CHECK(rename(path, moved) == 0);
	WriteFile(path);
	CHECK(HashImageFileSource(&source, path, false, &result) == ESTALE && IsZero(&result));
	CHECK(progress_calls == 0);
	CHECK(unlink(moved) == 0);
	CloseImageFileSource(&source);
	CHECK(OpenImageFileSource(path, &source) == 0);
	/* Even a fabricated retained source cannot admit direct/path-only/write I/O. */
	CHECK(close(source.fd) == 0);
	source.fd = open(path, O_PATH | O_CLOEXEC);
	CHECK(source.fd >= 0);
	CHECK(HashImageFileSource(&source, path, false, &result) == EINVAL && IsZero(&result));
	CloseImageFileSource(&source);
	CHECK(OpenImageFileSource(path, &source) == 0);
	CHECK(close(source.fd) == 0);
	source.fd = open(path, O_WRONLY | O_CLOEXEC);
	CHECK(source.fd >= 0);
	CHECK(HashImageFileSource(&source, path, false, &result) == EINVAL && IsZero(&result));
	CloseImageFileSource(&source);
	CHECK(OpenImageFileSource(path, &source) == 0);
	progress_cancel = true;
	ResetReports();
	CHECK(HashImageFileSource(&source, path, false, &result) == ECANCELED && IsZero(&result));
	CHECK(progress_calls == 1 && endpoints == 0);
	progress_cancel = false;
	cancelled = 0;
	CloseImageFileSource(&source);
	const off_t large_size = ((off_t)1 << 32) + 37;
	CHECK(truncate(path, large_size) == 0);
	CHECK(OpenImageFileSource(path, &source) == 0);
	source_fd = source.fd;
	progress_cancel = true;
	ResetReports();
	CHECK(HashImageFileSource(&source, path, true, &result) == ECANCELED && IsZero(&result));
	CHECK(reported_total == UINT64_C(4294967333) && read_calls == 0 && endpoints == 0);
	progress_cancel = false;
	cancelled = 0;
	source_fd = -1;
	CloseImageFileSource(&source);
	CHECK(unlink(path) == 0);
	CHECK(OpenImageFileSource(path, &source) == ENOENT);
	CHECK(mkfifo(path, 0600) == 0);
	CHECK(OpenImageFileSource(path, &source) == EINVAL);
	CHECK(unlink(path) == 0);
}

int main(void)
{
	char directory[] = "/tmp/rufus-image-hash-XXXXXX";
	const int before = DescriptorCount();
	if (mkdtemp(directory) == NULL)
		return 2;
	snprintf(path, sizeof(path), "%s/image", directory);
	snprintf(moved, sizeof(moved), "%s/moved", directory);
	snprintf(link_path, sizeof(link_path), "%s/link", directory);
	for (unsigned extra = 0; extra <= 1; extra++) {
		CheckRead(READ_NORMAL, 0, extra);
		CheckRead(READ_INTERRUPTED, 0, extra);
		CheckRead(READ_SHORT, 0, extra);
		CheckRead(READ_ERROR, EIO, extra);
		CheckRead(READ_LAST_ERROR, EIO, extra);
		CheckRead(READ_EOF, EIO, extra);
		CheckRead(READ_CANCELLED, ECANCELED, extra);
		CheckRead(READ_LAST_CANCELLED, ECANCELED, extra);
		CheckRead(READ_REPLACED, ESTALE, extra);
		CheckRead(READ_REMOVED, ESTALE, extra);
		CheckRead(READ_CHANGED, ESTALE, extra);
		CheckRead(READ_TRUNCATED, ESTALE, extra);
		CheckRead(READ_RETARGETED, ESTALE, extra);
	}
	CheckGuards();
	CHECK(rmdir(directory) == 0);
	CHECK(DescriptorCount() == before);
	printf("Image hash contract: %d checks, %d failures\n", checks, failures);
	return failures != 0;
}
