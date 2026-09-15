/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "fat_format.h"
#include "rufus_reporting.h"
#include "syslinux/libfat/libfat.h"
#include "syslinux/libfat/fat.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/fs.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char** environ;

enum {
	FAT_SECTOR_BYTES = 512,
	FAT32_MAX_CLUSTER_BYTES = 32 * 1024,
	FAT32_COPIES = 2,
	FAT32_RESERVED_SECTORS = 32,
	FAT32_RESERVED_CLUSTERS = 2,
	FAT32_ENTRY_BYTES = sizeof(uint32_t),
	FAT32_MIN_CLUSTERS = 65525,
	FAT32_MAX_CLUSTERS = 0x0ffffff4,
	FORMAT_HEADS = 255,
	FORMAT_SECTORS_PER_TRACK = 63,
	// mkfs.fat can round down by at most one track minus one sector.
	FORMAT_MAX_UNUSED_SECTORS = FORMAT_SECTORS_PER_TRACK - 1,
	MKFS_BLOCK_BYTES = 1024,
	FORMAT_ARGUMENT_BYTES = 32,  // Fits uint64 decimal values and /proc/self/fd paths.
	FORMAT_TARGET_FD = STDERR_FILENO + 1,
	FORMAT_FIRST_STAGING_FD = FORMAT_TARGET_FD + 1,
	MILLISECONDS_PER_SECOND = 1000,
	NANOSECONDS_PER_MILLISECOND = 1000000,
	FORMAT_TIMEOUT_MS = 120 * MILLISECONDS_PER_SECOND,
	FORMAT_TERMINATE_GRACE_MS = 500,
	FORMAT_POLL_MS = 25,
	DIAGNOSTIC_READS_PER_POLL = 16
};

int CheckFat32FormatOptions(const FAT32_FORMAT_OPTIONS* options)
{
	if (options == NULL)
		return EINVAL;
	uint64_t sectors, clusters, fat_sectors, reserved_sectors, overhead_sectors;
	uint32_t cluster_bytes = options->cluster_size;
	uint32_t cluster_sectors;
	size_t length;

	const bool sector_aligned = options->offset % FAT_SECTOR_BYTES == 0 &&
		options->size % FAT_SECTOR_BYTES == 0;
	const bool fits_fat_sector_fields = options->offset / FAT_SECTOR_BYTES <= UINT32_MAX &&
		options->size / FAT_SECTOR_BYTES <= UINT32_MAX;
	const bool cluster_in_range = cluster_bytes >= FAT_SECTOR_BYTES &&
		cluster_bytes <= FAT32_MAX_CLUSTER_BYTES;
	const bool cluster_is_power_of_two = cluster_bytes != 0 &&
		(cluster_bytes & (cluster_bytes - 1)) == 0;

	if (!sector_aligned || !fits_fat_sector_fields)
		return EINVAL;
	if (!cluster_in_range || !cluster_is_power_of_two)
		return EINVAL;
	if (LIBFAT_SECTOR_SIZE != FAT_SECTOR_BYTES)
		return ENOTSUP;
	length = strnlen(options->label, sizeof(options->label));
	const bool label_too_long = length > FAT32_LABEL_LENGTH;
	const bool label_has_edge_spaces = length != 0 &&
		(options->label[0] == ' ' || options->label[length - 1] == ' ');
	if (label_too_long || label_has_edge_spaces)
		return EINVAL;
	for (size_t i = 0; i < length; i++) {
		unsigned char ch = (unsigned char)options->label[i];
		const bool printable_ascii = ch >= ' ' && ch <= '~';
		const bool lowercase = ch >= 'a' && ch <= 'z';
		const bool forbidden_punctuation = strchr("*?.,;:/\\|+=<>[]\"", ch) != NULL;
		if (!printable_ascii || lowercase || forbidden_punctuation)
			return EINVAL;
	}
	// Conservative space check using an upper bound for the two aligned FATs.
	// Cluster selection remains with the caller, not mkfs.fat's defaults.
	sectors = options->size / FAT_SECTOR_BYTES;
	cluster_sectors = cluster_bytes / FAT_SECTOR_BYTES;
	clusters = sectors / cluster_sectors;
	fat_sectors = ((clusters + FAT32_RESERVED_CLUSTERS) * FAT32_ENTRY_BYTES + FAT_SECTOR_BYTES - 1) / FAT_SECTOR_BYTES;
	fat_sectors = (fat_sectors + cluster_sectors - 1) / cluster_sectors * cluster_sectors;
	reserved_sectors = cluster_sectors > FAT32_RESERVED_SECTORS ? cluster_sectors : FAT32_RESERVED_SECTORS;
	overhead_sectors = reserved_sectors + FAT32_COPIES * fat_sectors + FORMAT_MAX_UNUSED_SECTORS;
	const bool has_data_space = sectors > overhead_sectors;
	if (!has_data_space)
		return ENOTSUP;
	const bool enough_clusters = (sectors - overhead_sectors) / cluster_sectors >= FAT32_MIN_CLUSTERS;
	const bool too_many_clusters = clusters > FAT32_MAX_CLUSTERS;
	if (!enough_clusters || too_many_clusters)
		return ENOTSUP;
	return 0;
}

static int CheckFormatRequest(int fd, const char* mkfs, const FAT32_FORMAT_OPTIONS* options,
	uint64_t device, struct stat* info)
{
	int error = CheckFat32FormatOptions(options);
	if (error != 0)
		return error;
	if (mkfs == NULL || mkfs[0] != '/')
		return EINVAL;
	int flags = fcntl(fd, F_GETFL);
	if (flags < 0 || fstat(fd, info) != 0)
		return errno;
	const bool partition = device != 0;
	const bool expected_type = partition ? S_ISBLK(info->st_mode) : S_ISREG(info->st_mode);
	if (!expected_type)
		return ENOTSUP;
	const bool read_write = (flags & O_ACCMODE) == O_RDWR;
	const bool incompatible_flags = (flags & (O_APPEND | O_DIRECT | O_PATH)) != 0;
	if (!read_write || incompatible_flags)
		return EINVAL;
	if (partition) {
		uint64_t size = 0;
		int sector_size = 0, read_only = 1;
		if ((uint64_t)info->st_rdev != device)
			return ESTALE;
		if (ioctl(fd, BLKGETSIZE64, &size) != 0 || ioctl(fd, BLKSSZGET, &sector_size) != 0)
			return errno;
		if (ioctl(fd, BLKROGET, &read_only) != 0)
			return errno;
		if (read_only)
			return EROFS;
		if (sector_size != FAT_SECTOR_BYTES)
			return ENOTSUP;
		if (size != options->size)
			return ESTALE;
		return IsRufusJobCancelled() ? ECANCELED : 0;
	}
	const bool offset_in_file = info->st_size >= 0 && options->offset <= (uint64_t)info->st_size;
	if (!offset_in_file)
		return ENOSPC;
	const uint64_t available_bytes = (uint64_t)info->st_size - options->offset;
	if (options->size > available_bytes)
		return ENOSPC;
	return IsRufusJobCancelled() ? ECANCELED : 0;
}

int CheckFat32ImageRequest(int fd, const char* mkfs, const FAT32_FORMAT_OPTIONS* options)
{
	struct stat info;
	if (options == NULL)
		return EINVAL;
	return CheckFormatRequest(fd, mkfs, options, 0, &info);
}

static int StartFormatter(int fd, char* const* arguments, pid_t* child, int* output)
{
	posix_spawn_file_actions_t actions;
	int pipefd[2], copy, error;

	copy = fcntl(fd, F_DUPFD_CLOEXEC, FORMAT_FIRST_STAGING_FD);
	if (copy < 0)
		return errno;
	if (pipe2(pipefd, O_CLOEXEC) != 0) {
		error = errno;
		close(copy);
		return error;
	}
	// Keep staging descriptors out of the child's stdin/stdout/stderr/target slots.
	for (int i = 0; i < 2; i++) {
		if (pipefd[i] >= FORMAT_FIRST_STAGING_FD)
			continue;
		int moved = fcntl(pipefd[i], F_DUPFD_CLOEXEC, FORMAT_FIRST_STAGING_FD);
		if (moved < 0) {
			error = errno;
			goto out;
		}
		close(pipefd[i]);
		pipefd[i] = moved;
	}
	if (fcntl(pipefd[0], F_SETFL, O_NONBLOCK) != 0) {
		error = errno;
		goto out;
	}
	error = posix_spawn_file_actions_init(&actions);
	if (error != 0)
		goto out;
	error = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
	if (error == 0)
		error = posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
	if (error == 0)
		error = posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDERR_FILENO);
	if (error == 0)
		error = posix_spawn_file_actions_adddup2(&actions, copy, FORMAT_TARGET_FD);
	if (error == 0)
		error = posix_spawn(child, arguments[0], &actions, NULL, arguments, environ);
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
	return (int64_t)now.tv_sec * MILLISECONDS_PER_SECOND + now.tv_nsec / NANOSECONDS_PER_MILLISECOND;
}

static void DrainDiagnostics(int fd, FAT32_FORMAT_RESULT* result, size_t* used)
{
	char buffer[FAT32_DIAGNOSTIC_CAPACITY];
	ssize_t count;
	size_t keep;

	// Bound work per poll so a noisy child cannot starve cancellation.
	for (int i = 0; i < DIAGNOSTIC_READS_PER_POLL; i++) {
		count = read(fd, buffer, sizeof(buffer));
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			break;
		keep = sizeof(result->diagnostics) - 1 - *used;
		if (keep > (size_t)count)
			keep = (size_t)count;
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
			const bool clock_failed = start < 0 || now < 0;
			const bool timed_out = !clock_failed && now - start >= FORMAT_TIMEOUT_MS;
			if (IsRufusJobCancelled())
				error = ECANCELED;
			else if (clock_failed)
				error = EIO;
			else if (timed_out)
				error = ETIMEDOUT;
			if (error != 0) {
				kill(child, SIGTERM);
				stopped = now;
			}
		}
		if (error != 0) {
			const bool clock_failed = now < 0 || stopped < 0;
			const bool grace_expired = !clock_failed && now - stopped >= FORMAT_TERMINATE_GRACE_MS;
			if (clock_failed || grace_expired)
				kill(child, SIGKILL);
		}
		// If the child closes output early, still wait without spinning on POLLHUP.
		if (event.revents & POLLHUP)
			event.fd = -1;
		poll(&event, 1, FORMAT_POLL_MS);
	}
	DrainDiagnostics(output, result, &used);
	if (WIFEXITED(status))
		result->exit_code = WEXITSTATUS(status);
	if (WIFSIGNALED(status))
		result->signal = WTERMSIG(status);
	if (error != 0)
		return error;
	return result->exit_code == 0 ? 0 : EIO;
}

typedef struct {
	int fd;
	const FAT32_FORMAT_OPTIONS* options;
	uint64_t data_offset;
} FAT_FORMAT_READER;

static int ReadFatSector(intptr_t data, void* buffer, size_t size, libfat_sector_t sector)
{
	FAT_FORMAT_READER* reader = (FAT_FORMAT_READER*)data;
	ssize_t count;
	const bool sector_in_range = sector < reader->options->size / FAT_SECTOR_BYTES;
	if (size != FAT_SECTOR_BYTES || !sector_in_range)
		return 0;
	do {
		count = pread(reader->fd, buffer, size, (off_t)(reader->data_offset + sector * FAT_SECTOR_BYTES));
	} while (count < 0 && errno == EINTR);
	return count < 0 ? 0 : (int)count;
}

static int CheckFormattedImage(int fd, const FAT32_FORMAT_OPTIONS* options, uint64_t data_offset,
	FAT32_FORMAT_RESULT* result)
{
	FAT_FORMAT_READER reader = { fd, options, data_offset };
	struct fat_bootsect boot;
	struct libfat_filesystem* fs;
	char label[FAT32_LABEL_LENGTH];
	const char* expected_label = options->label[0] ? options->label : "NO NAME";
	const char* mismatch = NULL;
	const uint64_t requested_sectors = options->size / FAT_SECTOR_BYTES;
	int error = EBADMSG;

	if (LIBFAT_SECTOR_SIZE != FAT_SECTOR_BYTES)
		return EIO;
	if (ReadFatSector((intptr_t)&reader, &boot, sizeof(boot), 0) != sizeof(boot))
		return EIO;
	memset(label, ' ', sizeof(label));
	memcpy(label, expected_label, strlen(expected_label));

	const uint64_t sectors = read32(&boot.bsHugeSectors);
	const bool valid_signature = read16(&boot.bsSignature) == BS_SIGNATURE;
	const bool sector_size_matches = read16(&boot.bsBytesPerSec) == FAT_SECTOR_BYTES;
	const bool has_fat32_header = read16(&boot.bsFATsecs) == 0 && read16(&boot.bsRootDirEnts) == 0;
	const bool fat_count_matches = boot.bsFATs == FAT32_COPIES;
	const bool cluster_size_matches = boot.bsSecPerClust * FAT_SECTOR_BYTES == options->cluster_size;
	const bool fits_partition = sectors <= requested_sectors;
	const bool acceptable_unused_tail = fits_partition &&
		requested_sectors - sectors <= FORMAT_MAX_UNUSED_SECTORS;
	const bool partition_offset_matches = read32(&boot.bsHiddenSecs) == options->offset / FAT_SECTOR_BYTES;
	const bool volume_id_matches = read32(&boot.u.fat32.bsVolumeID) == options->volume_id;
	const bool label_matches = memcmp(boot.u.fat32.bsVolumeLabel, label, sizeof(label)) == 0;

	if (!valid_signature)
		mismatch = "invalid boot signature";
	else if (!sector_size_matches)
		mismatch = "unexpected sector size";
	else if (!has_fat32_header)
		mismatch = "FAT12/16 fields are nonzero";
	else if (!fat_count_matches)
		mismatch = "unexpected number of FAT copies";
	else if (!cluster_size_matches)
		mismatch = "cluster size differs from the request";
	else if (!fits_partition)
		mismatch = "filesystem extends past the requested partition";
	else if (!acceptable_unused_tail)
		mismatch = "unused partition tail exceeds formatter rounding";
	else if (!partition_offset_matches)
		mismatch = "hidden-sector offset differs from the request";
	else if (!volume_id_matches)
		mismatch = "volume ID differs from the request";
	else if (!label_matches)
		mismatch = "volume label differs from the request";
	if (mismatch != NULL) {
		uprintf("FAT32 verification failed: %s", mismatch);
		return EBADMSG;
	}

	fs = libfat_open(ReadFatSector, (intptr_t)&reader);
	if (fs == NULL)
		return EBADMSG;
	if (libfat_get_sector(fs, libfat_clustertosector(fs, 0)) != NULL) {
		result->filesystem_bytes = sectors * FAT_SECTOR_BYTES;
		error = 0;
	}
	libfat_close(fs);
	return error;
}

static int FormatFat32(int fd, uint64_t device, const char* mkfs, const FAT32_FORMAT_OPTIONS* options,
	FAT32_FORMAT_RESULT* result)
{
	struct stat before, after;
	char offset[FORMAT_ARGUMENT_BYTES], hidden[FORMAT_ARGUMENT_BYTES], blocks[FORMAT_ARGUMENT_BYTES];
	char cluster[FORMAT_ARGUMENT_BYTES], serial[FORMAT_ARGUMENT_BYTES];
	char sector_bytes[FORMAT_ARGUMENT_BYTES], fat_copies[FORMAT_ARGUMENT_BYTES];
	char reserved[FORMAT_ARGUMENT_BYTES], geometry[FORMAT_ARGUMENT_BYTES], target[FORMAT_ARGUMENT_BYTES];
	pid_t child;
	int output, error;

	if (result == NULL)
		return EINVAL;
	memset(result, 0, sizeof(*result));
	result->exit_code = -1;
	if (options == NULL)
		return EINVAL;
	error = CheckFormatRequest(fd, mkfs, options, device, &before);
	if (error != 0)
		return error;
	const uint64_t data_offset = device != 0 ? 0 : options->offset;
	snprintf(offset, sizeof(offset), "%" PRIu64, data_offset / FAT_SECTOR_BYTES);
	snprintf(hidden, sizeof(hidden), "%" PRIu64, options->offset / FAT_SECTOR_BYTES);
	snprintf(blocks, sizeof(blocks), "%" PRIu64, options->size / MKFS_BLOCK_BYTES);
	snprintf(cluster, sizeof(cluster), "%u", options->cluster_size / FAT_SECTOR_BYTES);
	snprintf(serial, sizeof(serial), "%08" PRIx32, options->volume_id);
	snprintf(sector_bytes, sizeof(sector_bytes), "%d", FAT_SECTOR_BYTES);
	snprintf(fat_copies, sizeof(fat_copies), "%d", FAT32_COPIES);
	snprintf(reserved, sizeof(reserved), "%d", FAT32_RESERVED_SECTORS);
	snprintf(geometry, sizeof(geometry), "%d/%d", FORMAT_HEADS, FORMAT_SECTORS_PER_TRACK);
	snprintf(target, sizeof(target), "/proc/self/fd/%d", FORMAT_TARGET_FD);
	char* arguments[] = { (char*)mkfs,
		"-F", "32",                         // FAT32 entries.
		"-S", sector_bytes, "-s", cluster,
		"-f", fat_copies, "-R", reserved,
		"-b", "6",                          // Backup boot sector at sector 6.
		"-g", geometry,
		"-D", "128", "-M", "248",           // BIOS hard disk (0x80), fixed-disk media (0xf8).
		"-h", hidden, "-i", serial, "-n", (char*)options->label,
		"--mbr=n", "--variant=standard", "--offset", offset, target, blocks, NULL, NULL };
	const bool partial_mkfs_block = options->size % MKFS_BLOCK_BYTES != 0;
	if (partial_mkfs_block) {
		// mkfs.fat takes whole KiB blocks. For an odd sector count, format
		// one sector less without another track-rounding loss. Cluster size is
		// unchanged; skipping internal alignment is only needed for this case.
		// Shift options and their terminator, leaving argv[0] and the spare slot out.
		const size_t shifted_arguments = sizeof(arguments) / sizeof(*arguments) - 2;
		memmove(arguments + 2, arguments + 1, shifted_arguments * sizeof(*arguments));
		arguments[1] = "-a";
	}
	error = StartFormatter(fd, arguments, &child, &output);
	if (error != 0)
		return error;
	result->started = true;
	error = WaitForFormatter(child, output, result);
	close(output);
	if (result->diagnostics[0])
		uprintf("mkfs.fat: %s", result->diagnostics);
	while (fsync(fd) != 0) {
		if (errno == EINTR)
			continue;
		result->sync_error = errno;
		break;
	}
	if (error == 0 && IsRufusJobCancelled())
		error = ECANCELED;
	if (error == 0)
		error = result->sync_error;
	if (error == 0 && fstat(fd, &after) != 0)
		error = errno;
	if (error == 0) {
		const bool same_file = before.st_dev == after.st_dev && before.st_ino == after.st_ino;
		const bool same_size = before.st_size == after.st_size;
		if (!same_file || !same_size)
			error = ESTALE;
	}
	if (error == 0)
		error = CheckFormatRequest(fd, mkfs, options, device, &after);
	if (error == 0)
		error = CheckFormattedImage(fd, options, data_offset, result);
	return error;
}

int FormatFat32Image(int fd, const char* mkfs, const FAT32_FORMAT_OPTIONS* options, FAT32_FORMAT_RESULT* result)
{
	return FormatFat32(fd, 0, mkfs, options, result);
}

int FormatFat32Partition(int fd, uint64_t device, const char* mkfs, const FAT32_FORMAT_OPTIONS* options,
	FAT32_FORMAT_RESULT* result)
{
	if (device == 0) {
		if (result != NULL) {
			memset(result, 0, sizeof(*result));
			result->exit_code = -1;
		}
		return EINVAL;
	}
	return FormatFat32(fd, device, mkfs, options, result);
}
