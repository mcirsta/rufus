/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Operates only on the disposable regular files owned by the Python driver. */
#include "platform/compressed_write.h"
#include "platform/image_hash.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { TRANSFER_BYTES = 65536, SHORT_BYTES = 1009, PARTIAL_BYTES = 17,
	MAX_ALLOCATIONS = 32, CONCURRENT_READERS = 3 };
static atomic_bool cancelled;
static _Thread_local bool active;
static _Thread_local unsigned allocations, live_allocations;
static _Thread_local void* allocation[MAX_ALLOCATIONS];
static _Thread_local size_t allocation_bytes[MAX_ALLOCATIONS], live_bytes, peak_bytes;
static const char* fault = "none";
static unsigned failed_allocation, writes, source_reads, target_reads, syncs;
static bool synced, changed, bzip2_source, lzma_source, lzw_source, zip_source, zstd_source;
static int source_fd = -1, target_fd = -1;
static IMAGE_FILE_SOURCE source = {.fd = -1};
static IMAGE_FILE_SOURCE replacement = {.fd = -1};
static COMPRESSED_WRITE_PLAN plan;
static RAW_WRITE_RESULT result;

static bool Fault(const char* name) { return strcmp(fault, name) == 0; }
int IsRufusJobCancelled(void) { return cancelled; }
int IsIsoScanCancelled(void) { return cancelled; }

static void ChangeTimestamp(void)
{
	struct timespec times[2] = {{1, 0}, {1, 0}};
	if (futimens(source_fd, times) != 0)
		abort();
}

void _UpdateProgressWithInfo(int operation, int message, uint64_t processed, uint64_t total, int forced)
{
	(void)message; (void)forced;
	if (!active)
		return;
	if (operation == RAW_WRITE_COPY || operation == RAW_WRITE_VERIFY) {
		struct stat target_info;
		const uint64_t expected_total = plan.image.compression_type == IMG_COMPRESSION_FFU
			&& fstat(target_fd, &target_info) == 0 ? (uint64_t)target_info.st_size : plan.target_bytes;
		if (processed > total || total != expected_total)
			abort();
	}
	if (operation == RAW_WRITE_COPY && processed == total && Fault("cancel-last-write"))
		cancelled = true;
	if (operation == RAW_WRITE_COPY && processed == 0 && Fault("source-copy-change"))
		ChangeTimestamp();
	if (operation == RAW_WRITE_VERIFY && processed != 0 && !changed && Fault("source-verify-change")) {
		changed = true;
		ChangeTimestamp();
	}
}

extern void* __real_calloc(size_t, size_t);
extern void __real_free(void*);
void* __wrap_calloc(size_t count, size_t size)
{
	if (active) {
		const size_t maximum = lzma_source || zip_source ? 1024 * 1024 * 1024U : bzip2_source ? 3600000U : 256 * 1024U;
		if (size == 0 || count == 0 || count > maximum / size)
			abort();
		allocations++;
		if (allocations == failed_allocation)
			return NULL;
	}
	void* memory = __real_calloc(count, size);
	if (active && memory != NULL) {
		bool stored = false;
		for (unsigned i = 0; i < MAX_ALLOCATIONS; i++) {
			if (allocation[i] == NULL) {
				allocation[i] = memory;
				allocation_bytes[i] = count * size;
				live_bytes += count * size;
				if (live_bytes > peak_bytes)
					peak_bytes = live_bytes;
			const size_t maximum_live = lzma_source || zip_source ? 1024 * 1024 * 1024U + 7 * 1024 * 1024U
					: bzip2_source ? 3600000U + 256 * 1024U : lzw_source ? 600 * 1024U : 512 * 1024U;
				if (live_bytes > maximum_live)
					abort();
				live_allocations++;
				stored = true;
				break;
			}
		}
		if (!stored)
			abort();
	}
	return memory;
}
void __wrap_free(void* memory)
{
	if (memory != NULL) {
		for (unsigned i = 0; i < MAX_ALLOCATIONS; i++) {
			if (allocation[i] == memory) {
				allocation[i] = NULL;
				live_bytes -= allocation_bytes[i];
				live_allocations--;
				break;
			}
		}
	}
	__real_free(memory);
}

#if defined(__GLIBC__) && defined(__USE_FILE_OFFSET64)
#define WrappedRead __wrap_pread64
#define RealRead __real_pread64
#define WrappedWrite __wrap_pwrite64
#define RealWrite __real_pwrite64
#else
#define WrappedRead __wrap_pread
#define RealRead __real_pread
#define WrappedWrite __wrap_pwrite
#define RealWrite __real_pwrite
#endif
extern ssize_t RealRead(int, void*, size_t, off_t);
extern ssize_t RealWrite(int, const void*, size_t, off_t);
extern int __real_fsync(int);

ssize_t WrappedRead(int fd, void* buffer, size_t size, off_t offset)
{
	if (!active)
		return RealRead(fd, buffer, size, offset);
	const size_t maximum_read = fd == source_fd && zstd_source ? 128 * 1024 + 3 : TRANSFER_BYTES;
	if (size > maximum_read || offset < 0)
		abort();
	if (fd == source_fd) {
		source_reads++;
		const bool preflight = result.phase == RAW_WRITE_PREPARE;
		const bool copy = result.phase == RAW_WRITE_COPY;
		const bool copy_started = copy && result.bytes_written != 0;
		if ((preflight && Fault("preflight-read")) || (copy_started && Fault("read-fail"))
			|| (copy && Fault("copy-read-before"))) {
			errno = EIO;
			return -1;
		}
		if (copy_started && Fault("early-eof"))
			return 0;
		if ((preflight && Fault("preflight-cancel")) || (copy_started && Fault("read-cancel")))
			cancelled = true;
		if (Fault("eintr") && source_reads == 1) {
			errno = EINTR;
			return -1;
		}
	} else if (fd == target_fd) {
		const bool base_check = plan.image.compression_type == IMG_COMPRESSION_FFU
			&& result.phase == RAW_WRITE_PREPARE && result.bytes_written == 0 && !synced;
		const bool readback = result.phase == RAW_WRITE_VERIFY && synced;
		if (!base_check && !readback)
			abort();
		target_reads++;
		if (base_check && Fault("base-read-fail")) { errno = EIO; return -1; }
		if (base_check && Fault("base-eof")) return 0;
		if (base_check && Fault("base-cancel")) cancelled = true;
		if (base_check && Fault("base-source-change")) ChangeTimestamp();
		if (base_check && !changed && Fault("base-target-resize")) {
			struct stat info;
			if (fstat(fd, &info) != 0 || ftruncate(fd, info.st_size + 512) != 0) abort();
			changed = true;
		}
		if (readback && Fault("verify-fail")) {
			errno = EIO;
			return -1;
		}
		if (readback && Fault("verify-eof"))
			return 0;
		if (readback && Fault("verify-cancel"))
			cancelled = true;
		if (readback && !changed && Fault("target-verify-resize")) {
			struct stat info;
			if (fstat(fd, &info) != 0 || ftruncate(fd, info.st_size + 512) != 0)
				abort();
			changed = true;
		}
		if (readback && !changed && Fault("target-verify-append")) {
			if (fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_APPEND) != 0)
				abort();
			changed = true;
		}
		if (Fault("eintr") && target_reads == 1) {
			errno = EINTR;
			return -1;
		}
	} else
		abort();
	if (Fault("short") && size > SHORT_BYTES)
		size = SHORT_BYTES;
	const bool substitute = fd == source_fd && ((result.phase == RAW_WRITE_PREPARE && Fault("preflight-substitute"))
		|| (result.phase == RAW_WRITE_COPY && Fault("copy-substitute"))
		|| (result.phase == RAW_WRITE_VERIFY && Fault("verify-substitute"))
		|| (result.phase == RAW_WRITE_PREPARE && Fault("preflight-layout-substitute"))
		|| (result.phase == RAW_WRITE_COPY && Fault("copy-layout-substitute"))
		|| (result.phase == RAW_WRITE_VERIFY && Fault("verify-layout-substitute")));
	const ssize_t count = RealRead(substitute ? replacement.fd : fd, buffer, size, offset);
	const bool corrupt_preflight = result.phase == RAW_WRITE_PREPARE && Fault("preflight-corrupt");
	const bool corrupt_copy = result.phase == RAW_WRITE_COPY && Fault("copy-corrupt");
	if (fd == source_fd && count > 0 && (corrupt_preflight || corrupt_copy)) {
		if (lzw_source) {
			/* The dedicated fixture ends in CLEAR then 37 nine-bit literals.
			 * Turn its final literal into invalid code 511, after three full
			 * provisional transfers. There is no LZW format checksum. */
			const off_t position = source.info.st_size - 2;
			if (position >= offset && position - offset < count)
				((uint8_t*)buffer)[position - offset] |= 0xf0;
			if (position + 1 >= offset && position + 1 - offset < count)
				((uint8_t*)buffer)[position + 1 - offset] |= 0x1f;
		} else if (lzma_source) {
			/* LZMA has no CRC. Promise one extra byte, then require the native
			 * early-EOS check to fail after the complete provisional output. */
			for (unsigned i = 0; i < 8; i++) {
				const off_t position = 5 + i;
				if (position >= offset && position - offset < count)
					((uint8_t*)buffer)[position - offset] = (uint8_t)((plan.image.decoded_bytes + 1) >> (8 * i));
			}
		} else {
			const off_t crc = source.info.st_size - (bzip2_source ? 3 : 8);
			if (crc >= offset && crc - offset < count)
				((uint8_t*)buffer)[crc - offset] ^= 1;
		}
	}
	return count;
}

ssize_t WrappedWrite(int fd, const void* buffer, size_t size, off_t offset)
{
	if (!active)
		return RealWrite(fd, buffer, size, offset);
	if (fd != target_fd || result.phase != RAW_WRITE_COPY || size > TRANSFER_BYTES || offset < 0)
		abort();
	if (result.bytes_written == 0 && ((uint64_t)offset % plan.sector_size || size % plan.sector_size))
		abort();
	writes++;
	if (Fault("eintr") && writes == 1) {
		errno = EINTR;
		return -1;
	}
	const uint64_t final_offset = plan.image.decoded_bytes / TRANSFER_BYTES * TRANSFER_BYTES;
	if (Fault("zero-write") || (Fault("final-zero-write") && (uint64_t)offset >= final_offset))
		return 0;
	const bool final_failure = Fault("final-write-fail") && (uint64_t)offset >= final_offset;
	if (Fault("write-fail") || Fault("double-fail") || final_failure) {
		if (size <= PARTIAL_BYTES || (Fault("write-fail") && writes > 1) || (Fault("double-fail") && writes > 1)
			|| (final_failure && (uint64_t)offset > final_offset)) {
			errno = ENOSPC;
			return -1;
		}
		if (size > PARTIAL_BYTES)
			size = PARTIAL_BYTES;
	}
	if (Fault("short") && size > SHORT_BYTES)
		size = SHORT_BYTES;
	if ((Fault("write-cancel") || Fault("cancel-sync-fail")) && size > PARTIAL_BYTES)
		size = PARTIAL_BYTES;
	const ssize_t count = RealWrite(fd, buffer, size, offset);
	if (Fault("write-cancel") || Fault("cancel-sync-fail"))
		cancelled = true;
	return count;
}

int __wrap_fsync(int fd)
{
	if (!active)
		return __real_fsync(fd);
	if (fd != target_fd || result.bytes_written == 0 || result.phase == RAW_WRITE_DONE)
		abort();
	syncs++;
	if (Fault("eintr") && syncs <= 2) {
		errno = EINTR;
		return -1;
	}
	if (Fault("sync-fail") || Fault("double-fail") || Fault("cancel-sync-fail")) {
		errno = EIO;
		return -1;
	}
	if (Fault("sync-cancel"))
		cancelled = true;
	if (Fault("source-sync-change"))
		ChangeTimestamp();
	if (Fault("target-sync-resize")) {
		struct stat info;
		if (fstat(fd, &info) != 0 || ftruncate(fd, info.st_size + 512) != 0)
			abort();
	}
	if (Fault("corrupt") || Fault("padding-corrupt")) {
		const off_t offset = Fault("corrupt") ? 23 : (off_t)plan.image.decoded_bytes;
		uint8_t byte;
		if (RealRead(fd, &byte, 1, offset) != 1)
			abort();
		byte ^= 1;
		if (RealWrite(fd, &byte, 1, offset) != 1)
			abort();
	}
	const int error = __real_fsync(fd);
	if (error == 0)
		synced = true;
	return error;
}

static int DescriptorCount(void)
{
	DIR* directory = opendir("/proc/self/fd");
	if (directory == NULL)
		abort();
	int count = 0;
	while (readdir(directory))
		count++;
	closedir(directory);
	return count;
}

typedef struct {
	const char* path;
	pthread_barrier_t* barrier;
	bool checksum;
	int error;
	COMPRESSED_WRITE_PLAN plan;
	IMAGE_HASH_RESULT hashes;
} PARALLEL_READER;

static void* ParallelRead(void* context)
{
	PARALLEL_READER* reader = context;
	pthread_barrier_wait(reader->barrier);
	if (reader->checksum)
		reader->error = HashImageFileSource(&source, reader->path, false, &reader->hashes);
	else
		reader->error = PrepareCompressedWrite(&source, reader->path, INT64_MAX, 512, false, &reader->plan);
	return NULL;
}

static int CheckParallel(const char* path, uint8_t digest[SHA256_HASHSIZE])
{
	pthread_barrier_t barrier;
	pthread_t threads[CONCURRENT_READERS];
	PARALLEL_READER readers[CONCURRENT_READERS] = {0};
	if (pthread_barrier_init(&barrier, NULL, CONCURRENT_READERS + 1) != 0)
		abort();
	for (unsigned i = 0; i < CONCURRENT_READERS; i++) {
		readers[i].path = path;
		readers[i].barrier = &barrier;
		readers[i].checksum = i == 0;
		if (pthread_create(&threads[i], NULL, ParallelRead, &readers[i]) != 0)
			abort();
	}
	pthread_barrier_wait(&barrier);
	for (unsigned i = 0; i < CONCURRENT_READERS; i++) {
		if (pthread_join(threads[i], NULL) != 0)
			abort();
	}
	if (pthread_barrier_destroy(&barrier) != 0)
		abort();
	for (unsigned i = 0; i < CONCURRENT_READERS; i++) {
		if (readers[i].error != 0)
			return readers[i].error;
	}
	if (memcmp(readers[1].plan.decoded_sha256, readers[2].plan.decoded_sha256, SHA256_HASHSIZE) != 0)
		return EBADMSG;
	memcpy(digest, readers[0].hashes.digest[HASH_SHA256], SHA256_HASHSIZE);
	return 0;
}

static void CheckGuards(void)
{
	COMPRESSED_WRITE_PLAN invalid_plan;
	RAW_WRITE_RESULT invalid_result;
	uint64_t size = 1;
	if (CompressedImageWriteSize(512, 512, NULL) != EINVAL
		|| CompressedImageWriteSize(0, 512, &size) != EINVAL || size != 0)
		abort();
	memset(&invalid_plan, 0xa5, sizeof(invalid_plan));
	if (PrepareCompressedWrite(NULL, NULL, 512, 512, false, &invalid_plan) != EINVAL
		|| PrepareCompressedWrite(NULL, NULL, 512, 512, false, NULL) != EINVAL
		|| WriteCompressedImageToFile(NULL, NULL, NULL, -1, NULL) != EINVAL)
		abort();
	memset(&invalid_result, 0xa5, sizeof(invalid_result));
	if (WriteCompressedImageToFile(NULL, NULL, NULL, -1, &invalid_result) != EINVAL)
		abort();
	const uint8_t* bytes = (const uint8_t*)&invalid_plan;
	for (size_t i = 0; i < sizeof(invalid_plan); i++) {
		if (bytes[i] != 0)
			abort();
	}
	bytes = (const uint8_t*)&invalid_result;
	for (size_t i = 0; i < sizeof(invalid_result); i++) {
		if (bytes[i] != 0)
			abort();
	}
}

static void ChangePlan(void)
{
	if (Fault("plan-compression"))
		plan.image.compression_type = plan.image.compression_type == BLED_COMPRESSION_GZIP ? BLED_COMPRESSION_XZ : BLED_COMPRESSION_GZIP;
	if (Fault("plan-compression-unsupported"))
		plan.image.compression_type = BLED_COMPRESSION_7ZIP;
	if (Fault("plan-hash"))
		plan.decoded_sha256[0] ^= 1;
	if (Fault("plan-vtsi-layout"))
		plan.layout_sha256[0] ^= 1;
	if (Fault("plan-size-small") || Fault("plan-size-large")) {
		plan.image.decoded_bytes += Fault("plan-size-small") ? -1 : 1;
		if (CompressedImageWriteSize(plan.image.decoded_bytes, plan.sector_size, &plan.target_bytes) != 0)
			abort();
	}
	if (Fault("plan-source"))
		plan.source_info.st_ino ^= 1;
	if (Fault("plan-tail"))
		plan.image.trailing_bytes++;
	if (Fault("plan-members"))
		plan.image.members++;
	if (Fault("plan-prefix"))
		plan.image.prefix[4] ^= 1;
	if (Fault("plan-marker"))
		plan.image.has_boot_marker = !plan.image.has_boot_marker;
	if (Fault("plan-prefix-size"))
		plan.image.prefix_size = SIZE_MAX;
	if (Fault("plan-sector"))
		plan.sector_size = 0;
	if (Fault("plan-write-size"))
		plan.target_bytes++;
}

static void PrepareReplacement(const char* source_path)
{
	char path[PATH_MAX];
	const char* extension = plan.image.compression_type == IMG_COMPRESSION_FFU ? "ffu" : zip_source ? "zip" : plan.image.compression_type == BLED_COMPRESSION_VTSI ? "vtsi"
		: plan.image.compression_type == BLED_COMPRESSION_GZIP ? "gz" : lzw_source ? "Z" : "lzma";
	const int size = snprintf(path, sizeof(path), "%s.replacement.%s", source_path, extension);
	if (size < 0 || (size_t)size >= sizeof(path) || OpenImageFileSource(path, &replacement) != 0)
		abort();
	COMPRESSED_WRITE_PLAN actual;
	if (PrepareCompressedWrite(&replacement, path, INT64_MAX, plan.sector_size, plan.ignore_boot_marker, &actual) != 0)
		abort();
	const bool same_sizes = actual.image.source_bytes == plan.image.source_bytes
		&& actual.image.decoded_bytes == plan.image.decoded_bytes && actual.image.prefix_size == plan.image.prefix_size;
	const bool same_stream = actual.image.compression_type == plan.image.compression_type
		&& actual.image.members == plan.image.members && actual.image.trailing_bytes == plan.image.trailing_bytes;
	const bool same_prefix = actual.image.has_boot_marker == plan.image.has_boot_marker
		&& memcmp(actual.image.prefix, plan.image.prefix, sizeof(plan.image.prefix)) == 0;
	const bool layout_only = strstr(fault, "layout-substitute") != NULL;
	const bool same_hash = memcmp(actual.decoded_sha256, plan.decoded_sha256, SHA256_HASHSIZE) == 0;
	const bool same_layout = memcmp(actual.layout_sha256, plan.layout_sha256, SHA256_HASHSIZE) == 0;
	if (!same_sizes || !same_stream || !same_prefix
		|| (layout_only ? !same_hash || same_layout : same_hash)) {
		fprintf(stderr, "Replacement fixture must differ only in decoded hash, not stream facts\n");
		abort();
	}
}

int main(int argc, char** argv)
{
	if (argc == 4 && strcmp(argv[1], "geometry") == 0) {
		uint64_t size = 77;
		const int error = CompressedImageWriteSize(strtoull(argv[2], NULL, 10), (unsigned)strtoul(argv[3], NULL, 10), &size);
		printf("{\"error\":%d,\"size\":%llu}\n", error, (unsigned long long)size);
		return 0;
	}
	if (argc != 7 && argc != 8)
		return 2;
	/* Guard-only calls do not initialize the core: preserve concurrent first use. */
	CheckGuards();
	const int descriptors = DescriptorCount();
	fault = argv[6];
	bzip2_source = GetImageCompressionType(argv[1]) == BLED_COMPRESSION_BZIP2;
	lzma_source = GetImageCompressionType(argv[1]) == BLED_COMPRESSION_LZMA;
	lzw_source = GetImageCompressionType(argv[1]) == BLED_COMPRESSION_LZW;
	zip_source = GetImageCompressionType(argv[1]) == BLED_COMPRESSION_ZIP;
	zstd_source = GetImageCompressionType(argv[1]) == BLED_COMPRESSION_ZSTD;
	if (strncmp(fault, "alloc-", 6) == 0)
		failed_allocation = (unsigned)strtoul(fault + 6, NULL, 10);
	const uint64_t maximum = strtoull(argv[5], NULL, 10);
	const unsigned sector = (unsigned)strtoul(argv[3], NULL, 10);
	const bool force = strcmp(argv[4], "1") == 0;
	uint8_t parallel_sha256[SHA256_HASHSIZE] = {0};
	int error = OpenImageFileSource(argv[1], &source);
	if (argc == 8) source.ffu_store = (unsigned)strtoul(argv[7], NULL, 10);
	if (error == 0 && GetImageCompressionType(argv[1]) == BLED_COMPRESSION_GZIP) {
		uint8_t magic[2];
		lzw_source = RealRead(source.fd, magic, sizeof(magic), 0) == sizeof(magic)
			&& magic[0] == 0x1f && magic[1] == 0x9d;
	}
	if (error == 0 && lseek(source.fd, 7, SEEK_SET) != 7)
		abort();
	source_fd = source.fd;
	if (error == 0 && Fault("parallel"))
		error = CheckParallel(argv[1], parallel_sha256);
	if (error == 0)
		error = PrepareCompressedWrite(&source, argv[1], maximum, sector, force, &plan);
	if (error != 0) {
		const uint8_t* bytes = (const uint8_t*)&plan;
		for (size_t i = 0; i < sizeof(plan); i++) {
			if (bytes[i] != 0)
				abort();
		}
	}
	if (error == 0) {
		if (Fault("preflight-substitute") || Fault("copy-substitute") || Fault("verify-substitute")
			|| Fault("preflight-layout-substitute") || Fault("copy-layout-substitute")
			|| Fault("verify-layout-substitute"))
			PrepareReplacement(argv[1]);
		int flags = O_RDWR | O_CLOEXEC | O_NONBLOCK;
		if (Fault("readonly")) flags = O_RDONLY | O_CLOEXEC;
		if (Fault("writeonly")) flags = O_WRONLY | O_CLOEXEC;
		if (Fault("append")) flags |= O_APPEND;
		if (Fault("direct")) flags |= O_DIRECT;
		if (Fault("path")) flags = O_PATH | O_CLOEXEC;
		target_fd = open(argv[2], flags);
		if (target_fd < 0)
			error = errno;
		else if (!Fault("fifo") && !Fault("path") && lseek(target_fd, 13, SEEK_SET) != 13)
			abort();
		if (Fault("cancel-before"))
			cancelled = true;
		if (Fault("source-before-change"))
			ChangeTimestamp();
		ChangePlan();
		if (Fault("ffu-store-change")) source.ffu_store = source.ffu_store == 1 ? 2 : 1;
		active = true;
		if (error == 0)
			error = WriteCompressedImageToFile(&source, argv[1], &plan, target_fd, &result);
		active = false;
		if (live_allocations != 0 || live_bytes != 0)
			abort();
		if (target_fd >= 0 && !Fault("fifo") && !Fault("path") && lseek(target_fd, 0, SEEK_CUR) != 13)
			abort();
	}
	if (source.fd >= 0 && lseek(source.fd, 0, SEEK_CUR) != 7)
		abort();
	if (target_fd >= 0 && close(target_fd) != 0)
		abort();
	CloseImageFileSource(&source);
	CloseImageFileSource(&replacement);
	if (DescriptorCount() != descriptors)
		abort();
	printf("{\"error\":%d,\"phase\":%d,\"written\":%llu,\"verified\":%llu,\"sync_error\":%d,"
		"\"syncs\":%u,\"writes\":%u,\"source_reads\":%u,\"target_reads\":%u,\"allocations\":%u,"
		"\"decoded\":%llu,\"padded\":%llu,\"compression\":%d,\"members\":%llu,\"trailing\":%llu,\"peak_memory\":%zu,\"plan_sha256\":\"",
		error, result.phase, (unsigned long long)result.bytes_written, (unsigned long long)result.bytes_verified,
		result.sync_error, syncs, writes, source_reads, target_reads, allocations,
		(unsigned long long)plan.image.decoded_bytes, (unsigned long long)plan.target_bytes,
		plan.image.compression_type, (unsigned long long)plan.image.members,
		(unsigned long long)plan.image.trailing_bytes, peak_bytes);
	for (unsigned i = 0; i < SHA256_HASHSIZE; i++)
		printf("%02x", plan.decoded_sha256[i]);
	printf("\",\"parallel_sha256\":\"");
	for (unsigned i = 0; i < SHA256_HASHSIZE; i++)
		printf("%02x", parallel_sha256[i]);
	printf("\"}\n");
	return 0;
}
