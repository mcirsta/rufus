/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "platform/compressed_source.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zlib.h>

enum { RAW_BYTES = 3 * 65536 + 37, MAX_ALLOCATIONS = 32, CONCURRENT_READERS = 3 };
enum FAULT { NORMAL, SHORT_READ, INTERRUPTED_READ, IO_ERROR, IO_ERROR_LATE, EARLY_EOF, CANCEL_READ, CANCEL_BUFFERED,
	CALLBACK_ERROR, CALLBACK_NEGATIVE, CALLBACK_FINAL_ERROR, CALLBACK_FINAL_NEGATIVE,
	CANCEL_CALLBACK, CHANGE_TIMESTAMP, REPLACE_SOURCE, REMOVE_SOURCE };
static atomic_int checks, failures;
static atomic_bool cancelled;
static _Thread_local bool track_reads, track_allocations;
static _Thread_local int read_calls, allocation_calls, fail_allocation, active_allocations;
static _Thread_local void* allocation[MAX_ALLOCATIONS];
static enum FAULT read_fault;
static int source_fd = -1;
static uint8_t raw[RAW_BYTES], packed[2 * RAW_BYTES + 65536];
static size_t packed_bytes;
static int source_type = 3;
static bool lzw_gzip;
static bool zip_stored;
static bool zip_bzip2;
static bool zip_xz;
static bool zip_lzma;
static bool zip_repeated;
static uint64_t expected_trailing;
static unsigned expected_members = 1;
static _Thread_local size_t live_bytes, peak_bytes;
static _Thread_local size_t allocation_bytes[MAX_ALLOCATIONS];
static _Thread_local unsigned cancel_polls;
static char path[256], moved[256];

#define CHECK(value) do { checks++; if (!(value)) { fprintf(stderr, "line %d: %s\n", __LINE__, #value); failures++; } } while (0)

int IsRufusJobCancelled(void)
{
	if (track_reads && read_fault == CANCEL_BUFFERED && ++cancel_polls >= 20)
		cancelled = true;
	return cancelled;
}
int IsIsoScanCancelled(void) { return cancelled; }

extern void* __real_calloc(size_t, size_t);
extern void __real_free(void*);
void* __wrap_calloc(size_t count, size_t size)
{
	if (track_allocations) {
		allocation_calls++;
		const size_t maximum = source_type == 6 || zip_xz ? 64 * 1024 * 1024U
			: source_type == 5 || zip_bzip2 ? 3600000U
			: source_type == 4 || zip_lzma ? 1024 * 1024 * 1024U : 256 * 1024U;
		CHECK(count == 1 && size <= maximum);
		if (allocation_calls == fail_allocation)
			return NULL;
	}
	void* memory = __real_calloc(count, size);
	if (track_allocations && memory != NULL) {
		bool stored = false;
		for (int i = 0; i < MAX_ALLOCATIONS; i++) {
			if (allocation[i] == NULL) {
				allocation[i] = memory;
				allocation_bytes[i] = size;
				live_bytes += size;
				if (live_bytes > peak_bytes)
					peak_bytes = live_bytes;
				const size_t maximum_live = source_type == 5 || zip_bzip2 ? 3600000U + 256 * 1024U
					: zip_xz ? 64 * 1024 * 1024U + 256 * 1024U
					: source_type == 4 || zip_lzma ? 1024 * 1024 * 1024U + 7 * 1024 * 1024U
					: source_type == 1 ? 256 * 1024U
				: source_type == 2 || lzw_gzip ? 600 * 1024U
				: 64 * 1024 * 1024U + 64 * 1024U;
				CHECK(live_bytes <= maximum_live);
				active_allocations++;
				stored = true;
				break;
			}
		}
		CHECK(stored);
	}
	return memory;
}
void __wrap_free(void* memory)
{
	if (memory != NULL) {
		for (int i = 0; i < MAX_ALLOCATIONS; i++) {
			if (allocation[i] == memory) {
				allocation[i] = NULL;
				live_bytes -= allocation_bytes[i];
				active_allocations--;
				break;
			}
		}
	}
	__real_free(memory);
}

#if defined(__GLIBC__) && defined(__USE_FILE_OFFSET64)
#define WrappedPread __wrap_pread64
#define RealPread __real_pread64
#else
#define WrappedPread __wrap_pread
#define RealPread __real_pread
#endif
extern ssize_t RealPread(int, void*, size_t, off_t);
ssize_t WrappedPread(int fd, void* data, size_t size, off_t offset)
{
	if (!track_reads || fd != source_fd)
		return RealPread(fd, data, size, offset);
	read_calls++;
	CHECK(size <= 65536 && offset >= 0);
	if (read_fault == SHORT_READ && size > 7)
		size = 7;
	if (read_fault == INTERRUPTED_READ && read_calls <= 3) {
		errno = EINTR;
		return -1;
	}
	const int late_read = source_type == 1 ? 7 : lzw_gzip ? 5 : source_type == 2 ? 4 : 2;
	if (read_fault == IO_ERROR || (read_fault == IO_ERROR_LATE && read_calls > late_read)) {
		errno = EIO;
		return -1;
	}
	if (read_fault == EARLY_EOF)
		return 0;
	if (read_fault == CANCEL_READ)
		cancelled = true;
	return RealPread(fd, data, size, offset);
}

static void WritePacked(const char* name)
{
	const int fd = open(name, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0 || write(fd, packed, packed_bytes) != (ssize_t)packed_bytes || close(fd) != 0)
		abort();
}

typedef struct { enum FAULT fault; uint64_t received; bool mutated; } CONSUMER;
static int Consume(void* context, const uint8_t* bytes, size_t size, uint64_t offset)
{
	CONSUMER* consumer = context;
	CHECK(size <= 65536 && offset == consumer->received);
	CHECK(offset <= sizeof(raw));
	if (offset > sizeof(raw) || size > sizeof(raw) - offset)
		return EOVERFLOW;
	CHECK(memcmp(bytes, raw + offset, size) == 0);
	consumer->received += size;
	if (consumer->fault == CALLBACK_ERROR)
		return ENOSPC;
	if (consumer->fault == CALLBACK_NEGATIVE)
		return -1;
	if (consumer->received == RAW_BYTES && consumer->fault == CALLBACK_FINAL_ERROR)
		return ENOSPC;
	if (consumer->received == RAW_BYTES && consumer->fault == CALLBACK_FINAL_NEGATIVE)
		return -1;
	if (consumer->fault == CANCEL_CALLBACK)
		cancelled = true;
	if (!consumer->mutated) {
		consumer->mutated = true;
		if (consumer->fault == CHANGE_TIMESTAMP) {
			struct timespec times[2] = {{1, 0}, {1, 0}};
			CHECK(futimens(source_fd, times) == 0);
		} else if (consumer->fault == REPLACE_SOURCE) {
			CHECK(rename(path, moved) == 0);
			WritePacked(path);
		} else if (consumer->fault == REMOVE_SOURCE)
			CHECK(unlink(path) == 0);
	}
	return 0;
}

static bool EmptyResult(const COMPRESSED_IMAGE_RESULT* result)
{
	const unsigned char* bytes = (const unsigned char*)result;
	for (size_t i = 0; i < sizeof(*result); i++) {
		if (bytes[i] != 0)
			return false;
	}
	return true;
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

static int CheckRead(enum FAULT fault, int memory_failure, uint64_t maximum, int expected_error)
{
	const int descriptors = DescriptorCount();
	COMPRESSED_IMAGE_RESULT result;
	IMAGE_FILE_SOURCE source = {.fd = -1};
	CONSUMER consumer = {.fault = fault};
	WritePacked(path);
	CHECK(OpenImageFileSource(path, &source) == 0);
	CHECK(lseek(source.fd, 11, SEEK_SET) == 11);
	source_fd = source.fd;
	read_fault = fault;
	read_calls = allocation_calls = 0;
	cancel_polls = 0;
	fail_allocation = memory_failure;
	track_reads = track_allocations = true;
	memset(&result, 0xa5, sizeof(result));
	const int error = ReadCompressedImageSource(&source, path, maximum, Consume, &consumer, &result);
	track_reads = track_allocations = false;
	if (error != expected_error)
		fprintf(stderr, "fault %d, allocation %d: error %d expected %d\n", fault, memory_failure, error, expected_error);
	CHECK(error == expected_error);
	CHECK(active_allocations == 0 && live_bytes == 0);
	CHECK(lseek(source.fd, 0, SEEK_CUR) == 11);
	CHECK((fcntl(source.fd, F_GETFL) & O_ACCMODE) == O_RDONLY);
	if (error == 0) {
		CHECK(result.compression_type == source_type && result.source_bytes == packed_bytes);
		CHECK(result.decoded_bytes == sizeof(raw) && result.members == expected_members);
		if (zip_lzma)
			CHECK(result.trailing_bytes >= expected_trailing && result.trailing_bytes <= expected_trailing + 5);
		else
			CHECK(result.trailing_bytes == expected_trailing);
		CHECK(result.prefix_size == 512 && result.has_boot_marker);
		CHECK(memcmp(result.prefix, raw, 512) == 0 && consumer.received == sizeof(raw));
	} else
		CHECK(EmptyResult(&result));
	if (fault == IO_ERROR || fault == EARLY_EOF || fault == CANCEL_READ || fault == CANCEL_BUFFERED)
		CHECK(consumer.received == 0);
	if (fault == IO_ERROR_LATE)
		CHECK(consumer.received > 0 && consumer.received < RAW_BYTES);
	if (maximum == 65536) {
		if (zip_lzma)
			CHECK(consumer.received == 0);
		else if (source_type == 1 && !zip_stored)
			CHECK(consumer.received > 0 && consumer.received <= maximum);
		else
			CHECK(consumer.received == (zip_stored ? 0 : 65536));
	}
	const int allocations = allocation_calls;
	cancelled = false;
	read_fault = NORMAL;
	source_fd = -1;
	CloseImageFileSource(&source);
	if (fault != REMOVE_SOURCE)
		CHECK(unlink(path) == 0);
	if (fault == REPLACE_SOURCE)
		CHECK(unlink(moved) == 0);
	CHECK(DescriptorCount() == descriptors);
	return allocations;
}

static void CheckGuards(void)
{
	IMAGE_FILE_SOURCE source = {.fd = -1};
	COMPRESSED_IMAGE_RESULT result;
	WritePacked(path);
	CHECK(OpenImageFileSource(path, &source) == 0);
	CHECK(ReadCompressedImageSource(NULL, path, RAW_BYTES, NULL, NULL, &result) == EINVAL && EmptyResult(&result));
	CHECK(ReadCompressedImageSource(&source, path, RAW_BYTES, NULL, NULL, NULL) == EINVAL);
	CHECK(ReadCompressedImageSource(&source, NULL, RAW_BYTES, NULL, NULL, &result) == EINVAL);
	CHECK(ReadCompressedImageSource(&source, path, 0, NULL, NULL, &result) == EINVAL);
	CHECK(ReadCompressedImageSource(&source, path, UINT64_MAX, NULL, NULL, &result) == EINVAL);
	cancelled = true;
	CHECK(ReadCompressedImageSource(&source, path, RAW_BYTES, NULL, NULL, &result) == ECANCELED && EmptyResult(&result));
	cancelled = false;
	CHECK(rename(path, moved) == 0);
	WritePacked(path);
	CHECK(ReadCompressedImageSource(&source, path, RAW_BYTES, NULL, NULL, &result) == ESTALE && EmptyResult(&result));
	CloseImageFileSource(&source);
	CHECK(unlink(moved) == 0);
	CHECK(OpenImageFileSource(path, &source) == 0);
	CHECK(close(source.fd) == 0);
	source.fd = open(path, O_PATH | O_CLOEXEC);
	CHECK(source.fd >= 0);
	CHECK(ReadCompressedImageSource(&source, path, RAW_BYTES, NULL, NULL, &result) == EINVAL && EmptyResult(&result));
	CloseImageFileSource(&source);
	CHECK(unlink(path) == 0);
}

typedef struct { const IMAGE_FILE_SOURCE* source; CONSUMER consumer; int error; } READER;
static void* ConcurrentRead(void* context)
{
	READER* reader = context;
	COMPRESSED_IMAGE_RESULT result;
	track_allocations = true;
	reader->error = ReadCompressedImageSource(reader->source, path, RAW_BYTES, Consume, &reader->consumer, &result);
	CHECK(active_allocations == 0 && live_bytes == 0);
	track_allocations = false;
	CHECK(result.decoded_bytes == RAW_BYTES && result.has_boot_marker);
	return NULL;
}

static void CheckConcurrent(void)
{
	IMAGE_FILE_SOURCE source = {.fd = -1};
	WritePacked(path);
	CHECK(OpenImageFileSource(path, &source) == 0);
	CHECK(lseek(source.fd, 11, SEEK_SET) == 11);
	pthread_t threads[CONCURRENT_READERS];
	READER reader[CONCURRENT_READERS] = {0};
	for (int i = 0; i < CONCURRENT_READERS; i++) {
		reader[i].source = &source;
		CHECK(pthread_create(&threads[i], NULL, ConcurrentRead, &reader[i]) == 0);
	}
	for (int i = 0; i < CONCURRENT_READERS; i++) {
		CHECK(pthread_join(threads[i], NULL) == 0);
		CHECK(reader[i].error == 0 && reader[i].consumer.received == RAW_BYTES);
	}
	CHECK(lseek(source.fd, 0, SEEK_CUR) == 11);
	CloseImageFileSource(&source);
	CHECK(unlink(path) == 0);
}

int main(int argc, char** argv)
{
	char directory[] = "/tmp/rufus-compressed-source-XXXXXX";
	if (mkdtemp(directory) == NULL)
		return 2;
	if (argc >= 2)
		source_type = GetImageCompressionType(argv[1]);
	lzw_gzip = argc == 3 && strcmp(argv[2], "--lzw-gzip") == 0;
	zip_repeated = argc == 3 && source_type == 1 && strcmp(argv[2], "--zip-repeated") == 0;
	if (argc > 3 || (argc == 3 && strcmp(argv[2], "2") != 0 && !lzw_gzip && !zip_repeated)
		|| (source_type != 1 && source_type != 2 && source_type != 3 && source_type != 4 && source_type != 5 && source_type != 6))
		return 2;
	if (lzw_gzip && source_type != 3)
		return 2;
	if (argc == 3 && !lzw_gzip && !zip_repeated)
		expected_members = 2;
	snprintf(path, sizeof(path), "%s/image.%s", directory, source_type == 6 ? "xz" : source_type == 5 ? "bz2"
		: source_type == 4 ? "lzma" : source_type == 2 ? "Z" : source_type == 1 ? "zip" : "gz");
	snprintf(moved, sizeof(moved), "%s/moved.gz", directory);
	for (size_t i = 0; i < sizeof(raw); i++)
		raw[i] = (uint8_t)(i * 37 + i / 251);
	if ((source_type == 1 && !zip_repeated) || source_type == 4 || source_type == 2 || lzw_gzip) {
		uint32_t random_state = 27;
		for (size_t i = 0; i < sizeof(raw); i++) {
			random_state ^= random_state << 13;
			random_state ^= random_state >> 17;
			random_state ^= random_state << 5;
			raw[i] = (uint8_t)random_state;
		}
	}
	raw[510] = 0x55; raw[511] = 0xaa;
	if (source_type != 3 || lzw_gzip) {
		FILE* fixture = fopen(argv[1], "rb");
		if (fixture == NULL)
			return 2;
		packed_bytes = fread(packed, 1, sizeof(packed), fixture);
		CHECK(packed_bytes > 0 && !ferror(fixture) && feof(fixture));
		CHECK(fclose(fixture) == 0);
		if (source_type == 1) {
			CHECK(packed_bytes >= 30);
			zip_stored = packed[8] == 0;
			zip_bzip2 = packed[8] == 12;
			zip_xz = packed[8] == 95;
			zip_lzma = packed[8] == 14;
			uint32_t size = 0;
			for (unsigned i = 0; i < 4; i++)
				size |= (uint32_t)packed[18 + i] << (8 * i);
			const size_t name = packed[26] + 256U * packed[27], extra = packed[28] + 256U * packed[29];
			CHECK(size <= packed_bytes && 30 + name + extra <= packed_bytes - size);
			expected_trailing = packed_bytes - size - 30 - name - extra;
		}
	} else {
		z_stream encoder = {0};
		CHECK(deflateInit2(&encoder, 1, Z_DEFLATED, 31, 8, Z_DEFAULT_STRATEGY) == Z_OK);
		encoder.next_in = raw; encoder.avail_in = sizeof(raw);
		encoder.next_out = packed; encoder.avail_out = sizeof(packed);
		CHECK(deflate(&encoder, Z_FINISH) == Z_STREAM_END);
		packed_bytes = sizeof(packed) - encoder.avail_out;
		CHECK(deflateEnd(&encoder) == Z_OK);
	}
	const int descriptors = DescriptorCount();
	/* Concurrent first use also exercises the shared hash dispatch initializer. */
	CheckConcurrent();
	const int allocations = CheckRead(NORMAL, 0, RAW_BYTES, 0);
	CHECK(allocations > 0);
	for (int index = 1; index <= allocations; index++)
		CheckRead(NORMAL, index, RAW_BYTES, ENOMEM);
	CheckRead(SHORT_READ, 0, RAW_BYTES, 0);
	CheckRead(INTERRUPTED_READ, 0, RAW_BYTES, 0);
	CheckRead(IO_ERROR, 0, RAW_BYTES, EIO);
	CheckRead(EARLY_EOF, 0, RAW_BYTES, EIO);
	CheckRead(CANCEL_READ, 0, RAW_BYTES, ECANCELED);
	if (source_type == 1 || source_type == 4 || source_type == 2 || lzw_gzip) {
		if (!zip_repeated && !zip_bzip2 && !zip_xz && !zip_lzma)
			CheckRead(IO_ERROR_LATE, 0, RAW_BYTES, EIO);
		CheckRead(CANCEL_BUFFERED, 0, RAW_BYTES, ECANCELED);
	}
	if (source_type == 1 || source_type == 2 || lzw_gzip) {
		CheckRead(CALLBACK_FINAL_ERROR, 0, RAW_BYTES, ENOSPC);
		CheckRead(CALLBACK_FINAL_NEGATIVE, 0, RAW_BYTES, EIO);
	}
	CheckRead(CALLBACK_ERROR, 0, RAW_BYTES, ENOSPC);
	CheckRead(CALLBACK_NEGATIVE, 0, RAW_BYTES, EIO);
	CheckRead(CANCEL_CALLBACK, 0, RAW_BYTES, ECANCELED);
	CheckRead(CHANGE_TIMESTAMP, 0, RAW_BYTES, ESTALE);
	CheckRead(REPLACE_SOURCE, 0, RAW_BYTES, ESTALE);
	CheckRead(REMOVE_SOURCE, 0, RAW_BYTES, ESTALE);
	CheckRead(NORMAL, 0, 65536, EFBIG);
	CheckGuards();
	CheckConcurrent();
	CHECK(rmdir(directory) == 0 && DescriptorCount() == descriptors);
	printf("%s source contract: %d checks, %d failures; %d provider allocations exercised; peak %zu bytes\n",
		source_type == 6 ? "XZ" : source_type == 5 ? "BZIP2" : source_type == 4 ? "LZMA" : source_type == 2 ? "LZW" : source_type == 1 ? "ZIP" : "Gzip",
		checks, failures, allocations, peak_bytes);
	return failures != 0;
}
