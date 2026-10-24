/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "platform/image_source.h"
#include "file_timestamp_fixture.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures, checks, cancelled, read_calls;
enum READ_FAULT { READ_NORMAL, READ_INTERRUPTED, READ_SHORT, READ_ERROR,
	READ_CANCELLED, READ_EOF, READ_CHANGED };
static enum READ_FAULT fault;
static const enum READ_FAULT read_faults[] = {
	READ_INTERRUPTED, READ_SHORT, READ_ERROR, READ_CANCELLED, READ_EOF, READ_CHANGED
};
static const char signature[8] = { 'M', 'S', 'W', 'I', 'M', 0, 0, 0 };

#define CHECK(expression) do { checks++; if (!(expression)) { \
	fprintf(stderr, "line %d: %s\n", __LINE__, #expression); failures++; } } while (0)

int IsIsoScanCancelled(void) { return cancelled; }

// glibc redirects pread to pread64 for the production target's large-file ABI.
#if defined(__GLIBC__) && defined(__USE_FILE_OFFSET64)
#define WrappedPread __wrap_pread64
#define RealPread __real_pread64
#else
#define WrappedPread __wrap_pread
#define RealPread __real_pread
#endif
ssize_t RealPread(int fd, void* data, size_t size, off_t offset);

ssize_t WrappedPread(int fd, void* data, size_t size, off_t offset)
{
	read_calls++;
	if (fault == READ_INTERRUPTED && read_calls == 1) {
		errno = EINTR;
		return -1;
	}
	if (fault == READ_SHORT)
		size = 1;
	if (fault == READ_ERROR) {
		errno = EIO;
		return -1;
	}
	if (fault == READ_CANCELLED)
		cancelled = 1;
	if (fault == READ_EOF)
		return 0;
	if (fault == READ_CHANGED) {
		struct timespec times[2] = { { 1, 0 }, { 1, 0 } };
		if (futimens(fd, times) != 0)
			abort();
	}
	return RealPread(fd, data, size, offset);
}

static void WriteFile(const char* path, const void* data, size_t size)
{
	FILE* file = fopen(path, "wb");
	if (!file || fwrite(data, 1, size, file) != size || fclose(file) != 0)
		abort();
}

static int DescriptorCount(void)
{
	DIR* dir = opendir("/proc/self/fd");
	int count = 0;
	if (!dir)
		abort();
	while (readdir(dir)) count++;
	closedir(dir);
	return count;
}

static void CheckOpen(const char* path)
{
	IMAGE_FILE_SOURCE source = { .fd = -1 };
	size_t size;
	int error, before = DescriptorCount();

	for (size = 0; size <= sizeof(signature); size++) {
		WriteFile(path, signature, size);
		CHECK(OpenImageFileSource(path, &source) == 0);
		CHECK(source.fd >= 0 && source.is_wim == (size == sizeof(signature)));
		CHECK(source.info.st_size == (off_t)size);
		CHECK(lseek(source.fd, 0, SEEK_CUR) == 0);
		CHECK((fcntl(source.fd, F_GETFD) & FD_CLOEXEC) != 0);
		CHECK((fcntl(source.fd, F_GETFL) & O_ACCMODE) == O_RDONLY);
		CHECK(CheckImageFileSource(&source, path) == 0);
		CloseImageFileSource(&source);
		CloseImageFileSource(&source);
		CHECK(source.fd == -1 && !source.is_wim && source.info.st_size == 0);
	}
	for (size_t i = 0; i < sizeof(read_faults) / sizeof(read_faults[0]); i++) {
		fault = read_faults[i];
		read_calls = 0;
		error = OpenImageFileSource(path, &source);
		CHECK(read_calls > 0);
		bool recoverable = fault == READ_INTERRUPTED || fault == READ_SHORT;
		if (!recoverable) {
			int expected_error = EIO;
			if (fault == READ_CANCELLED)
				expected_error = ECANCELED;
			else if (fault == READ_CHANGED)
				expected_error = ESTALE;
			CHECK(error == expected_error);
			CHECK(source.fd == -1 && !source.is_wim && source.info.st_size == 0);
		} else {
			CHECK(error == 0 && source.is_wim);
			int expected_reads = fault == READ_INTERRUPTED ? 2 : (int)sizeof(signature);
			CHECK(read_calls == expected_reads);
		}
		cancelled = 0;
		CloseImageFileSource(&source);
	}
	fault = READ_NORMAL;
	cancelled = 1; read_calls = 0;
	CHECK(OpenImageFileSource(path, &source) == ECANCELED && read_calls == 0);
	cancelled = 0;
	CHECK(OpenImageFileSource(NULL, &source) == EINVAL);
	CHECK(OpenImageFileSource("", &source) == EINVAL);
	CHECK(OpenImageFileSource(path, NULL) == EINVAL);
	CHECK(CheckImageFileSource(&source, path) == EINVAL);
	CloseImageFileSource(NULL);
	CHECK(DescriptorCount() == before);
}

static void CheckChanges(const char* path, const char* moved, const char* link)
{
	IMAGE_FILE_SOURCE source = { .fd = -1 };
	struct timespec times[2];
	char bytes[8];
	int fd;

	WriteFile(path, signature, sizeof(signature));
	CHECK(OpenImageFileSource(path, &source) == 0);
	CHECK(lseek(source.fd, 3, SEEK_SET) == 3);
	CHECK(rename(path, moved) == 0);
	WriteFile(path, "replaced", 8);
	CHECK(CheckImageFileSource(&source, path) == ESTALE);
	CHECK(RealPread(source.fd, bytes, 8, 0) == 8 && memcmp(bytes, signature, 8) == 0);
	CHECK(lseek(source.fd, 0, SEEK_CUR) == 3);
	CloseImageFileSource(&source);
	CHECK(unlink(moved) == 0);
	CHECK(OpenImageFileSource(path, &source) == 0);
	CHECK(unlink(path) == 0);
	CHECK(CheckImageFileSource(&source, path) != 0);
	CloseImageFileSource(&source);
	WriteFile(path, signature, 8);
	CHECK(OpenImageFileSource(path, &source) == 0);
	times[0] = source.info.st_atim;
	times[1] = source.info.st_mtim;
	CHECK(WaitForFileTimestampTick(&source.info.st_ctim));
	fd = open(path, O_WRONLY);
	CHECK(fd >= 0 && pwrite(fd, "changed!", 8, 0) == 8);
	CHECK(futimens(fd, times) == 0);
	close(fd);
	CHECK(CheckImageFileSource(&source, path) == ESTALE);
	CloseImageFileSource(&source);
	CHECK(OpenImageFileSource(path, &source) == 0);
	CHECK(truncate(path, 2) == 0);
	CHECK(CheckImageFileSource(&source, path) == ESTALE);
	CloseImageFileSource(&source);
	CHECK(symlink(path, link) == 0);
	CHECK(OpenImageFileSource(link, &source) == 0);
	CHECK(CheckImageFileSource(&source, link) == 0);
	WriteFile(moved, "another!", 8);
	CHECK(unlink(link) == 0 && symlink(moved, link) == 0);
	CHECK(CheckImageFileSource(&source, link) == ESTALE);
	cancelled = 1;
	CHECK(CheckImageFileSource(&source, link) == ECANCELED);
	cancelled = 0;
	CloseImageFileSource(&source);
	CHECK(unlink(link) == 0 && unlink(moved) == 0);
}

static void CheckBootMarker(const char* path)
{
	IMAGE_FILE_SOURCE source = { .fd = -1 };
	unsigned char sector[512] = { 0 };
	const size_t sizes[] = { 0, 8, 510, 511, 512 };
	int before = DescriptorCount();

	memcpy(sector, signature, sizeof(signature));
	sector[510] = 0x55;
	sector[511] = 0xaa;
	for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
		WriteFile(path, sector, sizes[i]);
		CHECK(OpenImageFileSource(path, &source) == 0);
		CHECK(source.is_wim == (sizes[i] >= 8));
		CHECK(source.has_boot_marker == (sizes[i] == 512));
		CHECK(lseek(source.fd, 0, SEEK_CUR) == 0);
		CloseImageFileSource(&source);
		CHECK(!source.has_boot_marker);
	}
	for (size_t i = 0; i < sizeof(read_faults) / sizeof(read_faults[0]); i++) {
		fault = read_faults[i];
		read_calls = 0;
		int error = OpenImageFileSource(path, &source);
		bool recoverable = fault == READ_INTERRUPTED || fault == READ_SHORT;
		if (recoverable) {
			CHECK(error == 0 && source.has_boot_marker);
			int expected_reads = fault == READ_INTERRUPTED ? 2 : (int)sizeof(sector);
			CHECK(read_calls == expected_reads);
		} else {
			CHECK(error != 0 && source.fd == -1 && !source.has_boot_marker);
		}
		cancelled = 0;
		CloseImageFileSource(&source);
	}
	fault = READ_NORMAL;
	sector[511] = 0;
	WriteFile(path, sector, sizeof(sector));
	CHECK(OpenImageFileSource(path, &source) == 0 && !source.has_boot_marker);
	CloseImageFileSource(&source);
	CHECK(DescriptorCount() == before);
}

int main(void)
{
	char directory[] = "/tmp/rufus-image-source-XXXXXX";
	char path[128], moved[128], link[128], fifo[128];
	IMAGE_FILE_SOURCE source = { .fd = -1 };
	int before = DescriptorCount();

	if (!mkdtemp(directory))
		return 2;
	snprintf(path, sizeof(path), "%s/image", directory);
	snprintf(moved, sizeof(moved), "%s/moved", directory);
	snprintf(link, sizeof(link), "%s/link", directory);
	snprintf(fifo, sizeof(fifo), "%s/fifo", directory);
	CheckOpen(path);
	CheckChanges(path, moved, link);
	CheckBootMarker(path);
	CHECK(mkfifo(fifo, 0600) == 0);
	CHECK(OpenImageFileSource(fifo, &source) == EINVAL && source.fd == -1);
	CHECK(OpenImageFileSource(directory, &source) == EINVAL && source.fd == -1);
	CHECK(OpenImageFileSource(moved, &source) == ENOENT && source.fd == -1);
	CHECK(unlink(fifo) == 0 && unlink(path) == 0 && rmdir(directory) == 0);
	CHECK(DescriptorCount() == before);
	printf("Image source: %d checks, %d failures.\n", checks, failures);
	return failures != 0;
}
