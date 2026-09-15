/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "platform/fat_format.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static const char* fault;
static int64_t start;
static int syncs, ticks;
enum {
	MILLISECONDS_PER_SECOND = 1000,
	NANOSECONDS_PER_MILLISECOND = 1000000,
	TIMEOUT_STEP_SECONDS = 61,
	CANCEL_DELAY_MS = 150,
	INITIAL_POSITION = 17
};
int __real_fsync(int);
int __real_clock_gettime(clockid_t, struct timespec*);

static int64_t Milliseconds(void)
{
	struct timespec now;
	if (__real_clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		abort();
	return (int64_t)now.tv_sec * MILLISECONDS_PER_SECOND + now.tv_nsec / NANOSECONDS_PER_MILLISECOND;
}

int __wrap_clock_gettime(clockid_t clock, struct timespec* now)
{
	int result = __real_clock_gettime(clock, now);
	if (result == 0 && strcmp(fault, "timeout") == 0 && clock == CLOCK_MONOTONIC)
		now->tv_sec += TIMEOUT_STEP_SECONDS * ticks++;
	return result;
}

int IsRufusJobCancelled(void)
{
	bool cancel_running = strcmp(fault, "cancel-running") == 0 && Milliseconds() - start >= CANCEL_DELAY_MS;
	return strcmp(fault, "cancel-before") == 0 || cancel_running;
}

void uprintf(const char* format, ...)
{
	va_list arguments;
	va_start(arguments, format);
	vfprintf(stderr, format, arguments);
	va_end(arguments);
}

int __wrap_fsync(int fd)
{
	syncs++;
	if (strcmp(fault, "sync-fail") == 0) {
		errno = EIO;
		return -1;
	}
	if (strcmp(fault, "sync-eintr") == 0 && syncs == 1) {
		errno = EINTR;
		return -1;
	}
	return __real_fsync(fd);
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

int main(int argc, char** argv)
{
	FAT32_FORMAT_OPTIONS options = { 0 };
	FAT32_FORMAT_RESULT result;
	int fd, error, before, flags, status, child, child_error, leaks, original_flags;
	off_t position;

	if (argc != 8)
		return 64;
	fault = argv[7];
	options.offset = strtoull(argv[3], NULL, 10);
	options.size = strtoull(argv[4], NULL, 10);
	options.cluster_size = (uint32_t)strtoul(argv[5], NULL, 10);
	options.volume_id = 0x1234abcd;
	size_t label_size = strlen(argv[6]);
	if (label_size > sizeof(options.label))
		label_size = sizeof(options.label);
	memcpy(options.label, argv[6], label_size);
	flags = strcmp(fault, "readonly") == 0 ? O_RDONLY : O_RDWR;
	if (strcmp(fault, "append") == 0)
		flags |= O_APPEND;
	fd = open(argv[2], flags | O_CLOEXEC);
	if (fd < 0)
		return 65;
	if (strcmp(fault, "pipe") == 0) {
		int ends[2];
		close(fd);
		if (pipe2(ends, O_CLOEXEC) != 0)
			abort();
		fd = ends[0];
		close(ends[1]);
	} else if (lseek(fd, INITIAL_POSITION, SEEK_SET) != INITIAL_POSITION)
		return 66;
	original_flags = fcntl(fd, F_GETFD);
	if (strcmp(fault, "replace-path") == 0) {
		char retained[4096];
		snprintf(retained, sizeof(retained), "%s.held", argv[2]);
		if (rename(argv[2], retained) != 0)
			abort();
		int replacement = open(argv[2], O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
		if (replacement < 0 || write(replacement, "retained", 8) != 8)
			abort();
		close(replacement);
	}
	before = DescriptorCount();
	start = Milliseconds();
	error = FormatFat32Image(fd, argv[1], &options, &result);
	position = lseek(fd, 0, SEEK_CUR);
	leaks = DescriptorCount() - before;
	child = waitpid(-1, &status, WNOHANG);
	child_error = errno;
	printf("ERROR=%d STARTED=%d EXIT=%d SIGNAL=%d SYNC=%d SYNCS=%d BYTES=%" PRIu64
		" FDS=%d OFFSET=%jd FLAGS=%d CHILD=%d CHILD_ERRNO=%d DIAG=%zu MS=%" PRId64 "\n",
		error, result.started, result.exit_code, result.signal, result.sync_error, syncs,
		result.filesystem_bytes, leaks, (intmax_t)position, fcntl(fd, F_GETFD) == original_flags,
		child, child_error, strlen(result.diagnostics), Milliseconds() - start);
	close(fd);
	return 0;
}
