/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "fat_format.h"
#include "rufus_reporting.h"
#include "syslinux/libfat/libfat.h"
#include "syslinux/libfat/fat.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char** environ;

static int CheckFormatRequest(int fd, const char* mkfs, const FAT32_FORMAT_OPTIONS* options,
	struct stat* info)
{
	uint64_t sectors, clusters, fat, reserved;
	size_t length;
	int flags;
	uint32_t cluster = options->cluster_size;

	if (mkfs == NULL || mkfs[0] != '/' || options->offset % 512 || options->size % 512 ||
		options->offset / 512 > UINT32_MAX || options->size / 512 > UINT32_MAX ||
		cluster < 512 || cluster > 32768 || (cluster & (cluster - 1)))
		return EINVAL;
	if (LIBFAT_SECTOR_SIZE != 512)
		return ENOTSUP;
	length = strnlen(options->label, sizeof(options->label));
	if (length > 11 || (length && (options->label[0] == ' ' || options->label[length - 1] == ' ')))
		return EINVAL;
	for (size_t i = 0; i < length; i++) {
		unsigned char ch = (unsigned char)options->label[i];
		if (ch < 32 || ch >= 127 || (ch >= 'a' && ch <= 'z') || strchr("*?.,;:/\\|+=<>[]\"", ch))
			return EINVAL;
	}
	// Conservative space check using an upper bound for the two aligned FATs.
	// Cluster selection remains with the caller, not mkfs.fat's defaults.
	sectors = options->size / 512;
	cluster /= 512;
	clusters = sectors / cluster;
	fat = ((clusters + 2) * 4 + 511) / 512;
	fat = (fat + cluster - 1) / cluster * cluster;
	reserved = cluster > 32 ? cluster : 32;
	if (sectors <= reserved + 2 * fat + 62 ||
		(sectors - reserved - 2 * fat - 62) / cluster < 65525 || clusters > 0x0ffffff4)
		return ENOTSUP;
	flags = fcntl(fd, F_GETFL);
	if (flags < 0 || fstat(fd, info) != 0)
		return errno;
	if (!S_ISREG(info->st_mode))
		return ENOTSUP;
	if ((flags & O_ACCMODE) != O_RDWR || (flags & (O_APPEND | O_DIRECT | O_PATH)))
		return EINVAL;
	if (info->st_size < 0 || options->offset > (uint64_t)info->st_size ||
		options->size > (uint64_t)info->st_size - options->offset)
		return ENOSPC;
	return IsRufusJobCancelled() ? ECANCELED : 0;
}

static int StartFormatter(int fd, char* const* arguments, pid_t* child, int* output)
{
	posix_spawn_file_actions_t actions;
	int pipefd[2], copy, error;

	copy = fcntl(fd, F_DUPFD_CLOEXEC, 4);
	if (copy < 0)
		return errno;
	if (pipe2(pipefd, O_CLOEXEC) != 0) {
		error = errno;
		close(copy);
		return error;
	}
	// Keep staging descriptors out of the child's stdin/stdout/stderr/target slots.
	for (int i = 0; i < 2; i++) {
		if (pipefd[i] >= 4)
			continue;
		int moved = fcntl(pipefd[i], F_DUPFD_CLOEXEC, 4);
		if (moved < 0) { error = errno; goto out; }
		close(pipefd[i]);
		pipefd[i] = moved;
	}
	if (fcntl(pipefd[0], F_SETFL, O_NONBLOCK) != 0) { error = errno; goto out; }
	error = posix_spawn_file_actions_init(&actions);
	if (error != 0)
		goto out;
	error = posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
	if (!error) error = posix_spawn_file_actions_adddup2(&actions, pipefd[1], 1);
	if (!error) error = posix_spawn_file_actions_adddup2(&actions, pipefd[1], 2);
	if (!error) error = posix_spawn_file_actions_adddup2(&actions, copy, 3);
	if (!error) error = posix_spawn(child, arguments[0], &actions, NULL, arguments, environ);
	posix_spawn_file_actions_destroy(&actions);
out:
	close(copy);
	close(pipefd[1]);
	if (error != 0)
		close(pipefd[0]);
	else
		*output = pipefd[0];
	return error;
}

static int64_t MonotonicMilliseconds(void)
{
	struct timespec now;
	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		return -1;
	return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static void DrainDiagnostics(int fd, FAT32_FORMAT_RESULT* result, size_t* used)
{
	char buffer[4096];
	ssize_t count;
	size_t keep;

	// Bound work per poll so a noisy child cannot starve cancellation.
	for (int i = 0; i < 16; i++) {
		count = read(fd, buffer, sizeof(buffer));
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			break;
		keep = sizeof(result->diagnostics) - 1 - *used;
		if (keep > (size_t)count) keep = (size_t)count;
		memcpy(result->diagnostics + *used, buffer, keep);
		*used += keep;
	}
	result->diagnostics[*used] = 0;
}

static int WaitForFormatter(pid_t child, int output, FAT32_FORMAT_RESULT* result)
{
	struct pollfd event = { output, POLLIN, 0 };
	int64_t start = MonotonicMilliseconds(), stopped = 0, now;
	size_t used = 0;
	int status, error = 0;
	pid_t waited;

	for (;;) {
		DrainDiagnostics(output, result, &used);
		waited = waitpid(child, &status, WNOHANG);
		if (waited == child)
			break;
		if (waited < 0 && errno != EINTR) {
			error = errno;
			if (error != ECHILD) {
				kill(child, SIGKILL);
				while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
			}
			return error;
		}
		now = MonotonicMilliseconds();
		if (error == 0) {
			if (IsRufusJobCancelled()) error = ECANCELED;
			else if (start < 0 || now < 0) error = EIO;
			else if (now - start >= 120000) error = ETIMEDOUT;
			if (error != 0) {
				kill(child, SIGTERM);
				stopped = now;
			}
		}
		if (error != 0 && (now < 0 || stopped < 0 || now - stopped >= 500))
			kill(child, SIGKILL);
		// If the child closes output early, still wait without spinning on POLLHUP.
		if (event.revents & POLLHUP) event.fd = -1;
		poll(&event, 1, 25);
	}
	DrainDiagnostics(output, result, &used);
	if (WIFEXITED(status)) result->exit_code = WEXITSTATUS(status);
	if (WIFSIGNALED(status)) result->signal = WTERMSIG(status);
	if (error != 0)
		return error;
	return result->exit_code == 0 ? 0 : EIO;
}

typedef struct {
	int fd;
	const FAT32_FORMAT_OPTIONS* options;
} FAT_FORMAT_READER;

static int ReadFatSector(intptr_t data, void* buffer, size_t size, libfat_sector_t sector)
{
	FAT_FORMAT_READER* reader = (FAT_FORMAT_READER*)data;
	ssize_t count;
	if (size != 512 || sector >= reader->options->size / 512)
		return 0;
	do {
		count = pread(reader->fd, buffer, size, (off_t)(reader->options->offset + sector * 512));
	} while (count < 0 && errno == EINTR);
	return count < 0 ? 0 : (int)count;
}

static int CheckFormattedImage(int fd, const FAT32_FORMAT_OPTIONS* options, FAT32_FORMAT_RESULT* result)
{
	FAT_FORMAT_READER reader = { fd, options };
	struct fat_bootsect boot;
	struct libfat_filesystem* fs;
	char label[11];
	uint64_t sectors;
	int error = EBADMSG;

	if (LIBFAT_SECTOR_SIZE != 512 || ReadFatSector((intptr_t)&reader, &boot, 512, 0) != 512)
		return EIO;
	memset(label, ' ', sizeof(label));
	memcpy(label, options->label[0] ? options->label : "NO NAME", options->label[0] ? strlen(options->label) : 7);
	sectors = read32(&boot.bsHugeSectors);
	if (read16(&boot.bsSignature) != BS_SIGNATURE || read16(&boot.bsBytesPerSec) != 512 ||
		read16(&boot.bsFATsecs) != 0 || read16(&boot.bsRootDirEnts) != 0 || boot.bsFATs != 2 ||
		boot.bsSecPerClust * 512u != options->cluster_size || sectors > options->size / 512 ||
		options->size / 512 - sectors >= 63 || read32(&boot.bsHiddenSecs) != options->offset / 512 ||
		read32(&boot.u.fat32.bsVolumeID) != options->volume_id ||
		memcmp(boot.u.fat32.bsVolumeLabel, label, sizeof(label)) != 0)
		return EBADMSG;
	fs = libfat_open(ReadFatSector, (intptr_t)&reader);
	if (fs == NULL)
		return EBADMSG;
	if (libfat_get_sector(fs, libfat_clustertosector(fs, 0)) != NULL) {
		result->filesystem_bytes = sectors * 512;
		error = 0;
	}
	libfat_close(fs);
	return error;
}

int FormatFat32Image(int fd, const char* mkfs, const FAT32_FORMAT_OPTIONS* options,
	FAT32_FORMAT_RESULT* result)
{
	struct stat before, after;
	char offset[32], blocks[32], cluster[16], serial[16];
	pid_t child;
	int output, error;

	if (result == NULL)
		return EINVAL;
	memset(result, 0, sizeof(*result));
	result->exit_code = -1;
	if (options == NULL)
		return EINVAL;
	error = CheckFormatRequest(fd, mkfs, options, &before);
	if (error != 0)
		return error;
	snprintf(offset, sizeof(offset), "%" PRIu64, options->offset / 512);
	snprintf(blocks, sizeof(blocks), "%" PRIu64, options->size / 1024);
	snprintf(cluster, sizeof(cluster), "%u", options->cluster_size / 512);
	snprintf(serial, sizeof(serial), "%08" PRIx32, options->volume_id);
	char* arguments[] = { (char*)mkfs, "-F", "32", "-S", "512", "-s", cluster,
		"-f", "2", "-R", "32", "-b", "6", "-g", "255/63", "-D", "128", "-M", "248",
		"-h", offset, "-i", serial, "-n", (char*)options->label,
		"--mbr=n", "--variant=standard", "--offset", offset, "/proc/self/fd/3", blocks, NULL, NULL };
	if (options->size % 1024) {
		// mkfs.fat takes whole KiB blocks. For an odd sector count, format
		// one sector less without another track-rounding loss. Cluster size is
		// unchanged; skipping internal alignment is only needed for this case.
		memmove(arguments + 2, arguments + 1, sizeof(arguments) - 2 * sizeof(*arguments));
		arguments[1] = "-a";
	}
	error = StartFormatter(fd, arguments, &child, &output);
	if (error != 0)
		return error;
	result->started = true;
	error = WaitForFormatter(child, output, result);
	close(output);
	if (result->diagnostics[0]) uprintf("mkfs.fat: %s", result->diagnostics);
	while (fsync(fd) != 0) {
		if (errno == EINTR) continue;
		result->sync_error = errno;
		break;
	}
	if (error == 0 && IsRufusJobCancelled()) error = ECANCELED;
	if (error == 0) error = result->sync_error;
	if (error == 0 && fstat(fd, &after) != 0) error = errno;
	if (error == 0 && (before.st_size != after.st_size || before.st_dev != after.st_dev || before.st_ino != after.st_ino))
		error = ESTALE;
	if (error == 0) error = CheckFormattedImage(fd, options, result);
	return error;
}
