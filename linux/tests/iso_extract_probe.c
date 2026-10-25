/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Fault injection is linked only into this disposable-directory test binary. */
#include "platform/iso_extract.h"
#include "iso_file.h"
#include "progress_policy.h"
#include "image_file.h"
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
static int replacements, truncations;
static int backup_links, backup_unlinks;
static uint64_t written;
static size_t max_read, max_write;
static int output_flags;
static bool generating_config, duplicating_config;
static int duplicate_reads, duplicate_writes;

enum {
	SHORT_WRITE_BYTES = 127,
	SHORT_WRITE_CALLS = 4
};

#if defined(__GLIBC__) && defined(__USE_FILE_OFFSET64)
#define WrappedOpenat __wrap_openat64
#define RealOpenat __real_openat64
#define WrappedPread __wrap_pread64
#define RealPread __real_pread64
#define WrappedTruncate __wrap_ftruncate64
#define RealTruncate __real_ftruncate64
#else
#define WrappedOpenat __wrap_openat
#define RealOpenat __real_openat
#define WrappedPread __wrap_pread
#define RealPread __real_pread
#define WrappedTruncate __wrap_ftruncate
#define RealTruncate __real_ftruncate
#endif

int RealOpenat(int, const char*, int, ...);
int RealTruncate(int, off_t);
ssize_t RealPread(int, void*, size_t, off_t);
bool __real_GetEfiSyslinuxConfigName(const RUFUS_IMG_REPORT*, const ISO_FILE_PROPS*, const char*, const char*, char*);
int __real_close(int);
int __real_fsync(int);
int __real_mkdirat(int, const char*, mode_t);
ssize_t __real_write(int, const void*, size_t);
int __real_ReadIsoFileRange(ISO_FILE_READER*, uint64_t, void*, size_t);
int __real_AnalyzeIsoImageWithSource(const char*, uint64_t, const ISO_ANALYSIS_OPTIONS*, ISO_ANALYSIS*, ISO_ANALYSIS_SOURCE*);
void __real_GetIsoSyslinuxConfig(const RUFUS_IMG_REPORT*, bool, ISO_SYSLINUX_CONFIG*);
int __real_renameat2(int, const char*, int, const char*, unsigned int);
int __real_linkat(int, const char*, int, const char*, int);
int __real_unlinkat(int, const char*, int);

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

static uint64_t progress_blocks, progress_total;
static int progress_samples, progress_intermediate, progress_invalid;

void _UpdateProgressWithInfo(int operation, int message, uint64_t processed, uint64_t total, int force)
{
	if (force && processed == 0 && progress_samples != 0) {
		progress_blocks = progress_total = 0;
		progress_samples = progress_intermediate = 0;
	}
	bool initial = progress_samples == 0;
	bool wrong_initial = initial && (processed != 0 || !force);
	bool changed_total = !initial && total != progress_total;
	const bool unexpected_report = operation != OP_FILE_COPY || message != 0;
	const bool invalid_count = processed > total || processed < progress_blocks;
	if (unexpected_report || invalid_count)
		progress_invalid++;
	if (wrong_initial || changed_total)
		progress_invalid++;
	if (processed != 0 && processed < total)
		progress_intermediate++;
	progress_samples++;
	progress_blocks = processed;
	progress_total = total;
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

void __wrap_GetIsoSyslinuxConfig(const RUFUS_IMG_REPORT* report, bool exists, ISO_SYSLINUX_CONFIG* config)
{
	duplicating_config = false;
	__real_GetIsoSyslinuxConfig(report, exists, config);
	generating_config = config->create;
	if (generating_config && Fault("config-source-change"))
		ChangeSource();
	if (generating_config && Fault("config-cancel"))
		cancelled = 1;
}

bool __wrap_GetEfiSyslinuxConfigName(const RUFUS_IMG_REPORT* report, const ISO_FILE_PROPS* props,
	const char* directory, const char* source, char* name)
{
	duplicating_config = __real_GetEfiSyslinuxConfigName(report, props, directory, source, name);
	if (duplicating_config && Fault("duplicate-cancel"))
		cancelled = 1;
	if (duplicating_config && Fault("duplicate-source-change"))
		ChangeSource();
	return duplicating_config;
}

int __wrap_renameat2(int old_parent, const char* old_name, int new_parent, const char* new_name, unsigned int flags)
{
	if (Fault("config-backup-fail")) {
		errno = EACCES;
		return -1;
	}
	const bool fallback = strncmp(fault, "config-backup-", strlen("config-backup-")) == 0;
	if (fallback) {
		errno = Fault("config-backup-enosys") ? ENOSYS :
			Fault("config-backup-unsupported") ? EOPNOTSUPP : EINVAL;
		return -1;
	}
	return __real_renameat2(old_parent, old_name, new_parent, new_name, flags);
}

int __wrap_linkat(int old_parent, const char* old_name, int new_parent, const char* new_name, int flags)
{
	if (strcmp(new_name, "syslinux.org") == 0)
		backup_links++;
	if (Fault("config-backup-link-fail") || Fault("config-backup-link-unsupported")) {
		errno = Fault("config-backup-link-fail") ? EIO : EOPNOTSUPP;
		return -1;
	}
	return __real_linkat(old_parent, old_name, new_parent, new_name, flags);
}

int __wrap_unlinkat(int parent, const char* name, int flags)
{
	if (backup_links != 0 && strcmp(name, "syslinux.cfg") == 0) {
		backup_unlinks++;
		if (Fault("config-backup-unlink-fail")) {
			errno = EIO;
			return -1;
		}
	}
	return __real_unlinkat(parent, name, flags);
}

static void InsertTargetEntry(int parent, const char* path)
{
	char outside[4096];
	int fd, status;

	snprintf(outside, sizeof(outside), "%s/../outside/sentinel", target_path);
	if (Fault("target-symlink") || Fault("duplicate-symlink"))
		status = symlinkat(outside, parent, path);
	else if (Fault("target-hardlink") || Fault("duplicate-hardlink"))
		status = linkat(AT_FDCWD, outside, parent, path, 0);
	else if (Fault("target-fifo") || Fault("duplicate-fifo"))
		status = mkfifoat(parent, path, 0600);
	else if (Fault("target-directory") || Fault("duplicate-directory"))
		status = __real_mkdirat(parent, path, 0700);
	else {
		fd = RealOpenat(parent, path, O_WRONLY | O_CREAT | O_EXCL, 0600);
		if (fd < 0 || __real_write(fd, "existing", 8) != 8)
			abort();
		status = __real_close(fd);
	}
	if (status != 0)
		abort();
	fired = 1;
}

int WrappedOpenat(int parent, const char* path, int flags, ...)
{
	mode_t mode = 0;
	va_list arguments;
	int fd;

	if (duplicating_config && Fault("duplicate-input-fail") && (flags & O_ACCMODE) == O_RDONLY) {
		errno = EIO;
		return -1;
	}
	if (flags & O_CREAT) {
		va_start(arguments, flags);
		mode = (mode_t)va_arg(arguments, int);
		va_end(arguments);
		if (Fault("open-fail") || (generating_config && Fault("config-open-fail"))) {
			errno = EACCES;
			return -1;
		}
		if (duplicating_config) {
			if (Fault("duplicate-open-fail")) {
				errno = EACCES;
				return -1;
			}
			bool existing_entry = Fault("duplicate-existing") || Fault("duplicate-directory");
			bool linked_entry = Fault("duplicate-symlink") || Fault("duplicate-hardlink") || Fault("duplicate-fifo");
			if (existing_entry || linked_entry)
				InsertTargetEntry(parent, path);
		}
		bool replacement = Fault("collision") || strstr(fault, "replace-") == fault;
		bool invalid_target = strstr(fault, "target-") == fault;
		if (!fired && (replacement || invalid_target))
			InsertTargetEntry(parent, path);
	}
	fd = RealOpenat(parent, path, flags, mode);
	bool writable = (flags & O_ACCMODE) == O_WRONLY;
	if (fd >= 0 && writable) {
		output_fd = fd;
		output_flags = flags;
		if (!(flags & O_CREAT)) {
			replacements++;
			if (Fault("replace-cancel"))
				cancelled = 1;
		}
	}
	return fd;
}

int WrappedTruncate(int fd, off_t size)
{
	if (fd == output_fd) {
		truncations++;
		if (Fault("replace-truncate-fail")) {
			errno = EIO;
			return -1;
		}
	}
	return RealTruncate(fd, size);
}

int __wrap_mkdirat(int parent, const char* name, mode_t mode)
{
	int result;
	char outside[4096];
	if (Fault("mkdir-fail")) {
		errno = EACCES;
		return -1;
	}
	if (Fault("directory-file") && !fired)
		InsertTargetEntry(parent, name);
	if (Fault("directory-link") && !fired++) {
		snprintf(outside, sizeof(outside), "%s/../outside", target_path);
		if (symlinkat(outside, parent, name) != 0)
			abort();
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
	if (duplicating_config) {
		duplicate_writes++;
		if (Fault("duplicate-write-fail") && duplicate_writes > 1) {
			errno = ENOSPC;
			return -1;
		}
		if (Fault("duplicate-zero-write"))
			return 0;
		if (Fault("duplicate-write-eintr") && duplicate_writes == 1) {
			errno = EINTR;
			return -1;
		}
		bool partial = Fault("duplicate-write-fail") || Fault("duplicate-short") || Fault("duplicate-write-cancel");
		if (partial && size > SHORT_WRITE_BYTES)
			size = SHORT_WRITE_BYTES;
	}
	if (size > max_write)
		max_write = size;
	if (Fault("zero-write"))
		return 0;
	if (generating_config && Fault("config-write-fail")) {
		errno = ENOSPC;
		return -1;
	}
	if (Fault("eintr") && writes == 1) {
		errno = EINTR;
		return -1;
	}
	bool write_failure = Fault("write-fail") || Fault("replace-write-fail");
	if (write_failure && writes > 1) {
		errno = ENOSPC;
		return -1;
	}
	bool short_write = Fault("short") && writes <= SHORT_WRITE_CALLS;
	bool stop_after_write = write_failure || Fault("write-cancel");
	if ((short_write || stop_after_write) && size > SHORT_WRITE_BYTES)
		size = SHORT_WRITE_BYTES;
	count = __real_write(fd, buffer, size);
	if (count > 0)
		written += (uint64_t)count;
	if (Fault("write-cancel") || (duplicating_config && Fault("duplicate-write-cancel")))
		cancelled = 1;
	if (Fault("source-change") && writes == 1)
		ChangeSource();
	return count;
}

ssize_t WrappedPread(int fd, void* buffer, size_t size, off_t offset)
{
	if (duplicating_config && output_fd >= 0) {
		duplicate_reads++;
		reads++;
		if (size > max_read)
			max_read = size;
		if (Fault("duplicate-read-fail")) {
			errno = EIO;
			return -1;
		}
		if (Fault("duplicate-read-eintr") && duplicate_reads == 1) {
			errno = EINTR;
			return -1;
		}
		if (Fault("duplicate-eof"))
			return 0;
		if (Fault("duplicate-short") && size > SHORT_WRITE_BYTES)
			size = SHORT_WRITE_BYTES;
	}
	return RealPread(fd, buffer, size, offset);
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
	bool sync_failure = Fault("sync-fail") || Fault("replace-sync-fail")
		|| (generating_config && Fault("config-sync-fail"))
		|| (duplicating_config && Fault("duplicate-sync-fail"));
	if (sync_failure && fd == output_fd) {
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
		if (Fault("close-fail") || (generating_config && Fault("config-close-fail")) ||
			(duplicating_config && Fault("duplicate-close-fail"))) {
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
	ISO_EXTRACT_OPTIONS options = { { true, true, { 0, 0 } }, NULL, NULL, false , 0, NULL, false, false, false, NULL, NULL};
	ISO_EXTRACT_RESULT result;
	int before, after, error, retry = -1;

	if (argc != 5 && argc != 7)
		return 64;
	if (argc == 7) {
		options.usb_label = argv[5];
		options.image_path = argv[6];
	}
	if (strcmp(argv[3],"ntfs") == 0) { options.windows_links = true; options.analysis.enable_joliet = false; }
	image_path = argv[1];
	target_path = argv[2];
	fault = argv[4];
	if (strcmp(argv[3], "joliet") == 0)
		options.analysis.enable_rockridge = false;
	if (strcmp(argv[3], "rr") == 0)
		options.analysis.enable_joliet = false;
	if (strcmp(argv[3], "plain") == 0)
		options.analysis.enable_rockridge = options.analysis.enable_joliet = false;
	cdio_log_set_handler(LogCdio);
	cancelled = Fault("cancel-before");
	before = DescriptorCount();
	if (Fault("preflight") || Fault("preflight-small") || Fault("preflight-config-limit")) {
		const uint64_t limit = Fault("preflight-config-limit") ? 12 : (Fault("preflight-small") ? 1 : UINT32_MAX);
		error = CheckIsoTree(image_path, &options, limit, &result);
	}
	else
		error = ExtractIsoTree(image_path, target_path, &options, &result);
	after = DescriptorCount();
	printf("ERROR=%d FILES=%" PRIu64 " DIRECTORIES=%" PRIu64 " SKIPPED=%" PRIu64
		" PATCHED=%" PRIu64 " WRITTEN=%" PRIu64 " GENERATED=%" PRIu64 " ACTUAL=%" PRIu64 " FDS=%d READS=%d WRITES=%d SYNCS=%d CLOSES=%d"
		" MAX_READ=%zu MAX_WRITE=%zu FLAGS=%d REPLACEMENTS=%d TRUNCATIONS=%d\n",
		error, result.files, result.directories, result.skipped, result.patched_files, result.bytes_written, result.generated_bytes, written,
		after - before, reads, writes, syncs, closes, max_read, max_write, output_flags,
		replacements, truncations);
	printf("PROGRESS_SAMPLES=%d PROGRESS_INTERMEDIATE=%d PROGRESS_INVALID=%d PROGRESS_BLOCKS=%" PRIu64 " PROGRESS_TOTAL=%" PRIu64 "\n",
		progress_samples, progress_intermediate, progress_invalid, progress_blocks, progress_total);
	printf("BACKUP_LINKS=%d BACKUP_UNLINKS=%d\n", backup_links, backup_unlinks);
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
