/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "platform/compressed_source.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum FAULT { NORMAL, SHORT_READ, INTERRUPTED, IO_FIRST, IO_LATE, EARLY_EOF,
	CANCEL_READ, CANCEL_CALLBACK, CALLBACK_ERROR, CALLBACK_NEGATIVE };
static enum FAULT fault;
static int source_fd, calls, cancelled;
static uint64_t received;

int IsRufusJobCancelled(void) { return cancelled; }
int IsIsoScanCancelled(void) { return cancelled; }

#if defined(__GLIBC__) && defined(__USE_FILE_OFFSET64)
#define WrappedPread __wrap_pread64
#define RealPread __real_pread64
#else
#define WrappedPread __wrap_pread
#define RealPread __real_pread
#endif
extern ssize_t RealPread(int, void*, size_t, off_t);
ssize_t WrappedPread(int fd, void* buffer, size_t size, off_t offset)
{
	if (fd != source_fd)
		return RealPread(fd, buffer, size, offset);
	calls++;
	if (fault == SHORT_READ && size > 7)
		size = 7;
	if (fault == INTERRUPTED && calls <= 3) {
		errno = EINTR;
		return -1;
	}
	if (fault == IO_FIRST || (fault == IO_LATE && calls > 1)) {
		errno = EIO;
		return -1;
	}
	if (fault == EARLY_EOF)
		return 0;
	if (fault == CANCEL_READ)
		cancelled = 1;
	return RealPread(fd, buffer, size, offset);
}

static int Consume(void* context, const uint8_t* bytes, size_t size, uint64_t offset)
{
	(void)context;
	(void)bytes;
	if (offset != received || size == 0 || size > 65536)
		return EIO;
	received += size;
	if (fault == CANCEL_CALLBACK)
		cancelled = 1;
	if (fault == CALLBACK_ERROR)
		return ENOSPC;
	if (fault == CALLBACK_NEGATIVE)
		return -1;
	return 0;
}

int main(int argc, char** argv)
{
	if (argc != 4)
		return 2;
	fault = (enum FAULT)atoi(argv[1]);
	const uint64_t maximum = strtoull(argv[2], NULL, 10);
	IMAGE_FILE_SOURCE source = {.fd = -1};
	int error = OpenImageFileSource(argv[3], &source);
	if (error != 0)
		return 3;
	source_fd = source.fd;
	if (lseek(source.fd, 7, SEEK_SET) != 7)
		return 4;
	COMPRESSED_IMAGE_RESULT result;
	memset(&result, 0xa5, sizeof(result));
	error = ReadCompressedImageSource(&source, argv[3], maximum, Consume, NULL, &result);
	const off_t offset = lseek(source.fd, 0, SEEK_CUR);
	const int flags = fcntl(source.fd, F_GETFL);
	CloseImageFileSource(&source);
	COMPRESSED_IMAGE_RESULT empty = {0};
	printf("error %d\nreceived %llu\nresult_empty %d\nresult_size %llu\nreads %d\noffset %lld\nreadonly %d\n",
		error, (unsigned long long)received, memcmp(&result, &empty, sizeof(result)) == 0,
		(unsigned long long)result.decoded_bytes, calls, (long long)offset,
		(flags & O_ACCMODE) == O_RDONLY);
	return 0;
}
