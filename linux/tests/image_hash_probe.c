/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "platform/image_hash.h"

#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

static const size_t digest_size[HASH_MAX] = { 16, 20, 32, 64 };
static const char* digest_name[HASH_MAX] = { "md5", "sha1", "sha256", "sha512" };

enum { PARALLEL_READERS = 4 };
struct HashReader {
	const IMAGE_FILE_SOURCE* source;
	const char* path;
	pthread_barrier_t* start;
	IMAGE_HASH_RESULT result;
	int error;
};

int IsRufusJobCancelled(void) { return 0; }
int IsIsoScanCancelled(void) { return 0; }
void _UpdateProgressWithInfo(int operation, int message, uint64_t processed, uint64_t total, int force)
{
	(void)operation; (void)message; (void)processed; (void)total; (void)force;
}

static void* ReadNativeHashes(void* argument)
{
	struct HashReader* reader = argument;
	pthread_barrier_wait(reader->start);
	reader->error = HashImageFileSource(reader->source, reader->path, true, &reader->result);
	return NULL;
}

static int HashParallel(const IMAGE_FILE_SOURCE* source, const char* path, IMAGE_HASH_RESULT* result)
{
	pthread_t threads[PARALLEL_READERS];
	pthread_barrier_t start;
	struct HashReader readers[PARALLEL_READERS] = { 0 };
	if (pthread_barrier_init(&start, NULL, PARALLEL_READERS + 1) != 0)
		abort();
	for (unsigned i = 0; i < PARALLEL_READERS; i++) {
		readers[i].source = source;
		readers[i].path = path;
		readers[i].start = &start;
		if (pthread_create(&threads[i], NULL, ReadNativeHashes, &readers[i]) != 0)
			abort();
	}
	pthread_barrier_wait(&start);
	for (unsigned i = 0; i < PARALLEL_READERS; i++) {
		if (pthread_join(threads[i], NULL) != 0)
			abort();
	}
	pthread_barrier_destroy(&start);
	*result = readers[0].result;
	for (unsigned i = 0; i < PARALLEL_READERS; i++) {
		if (readers[i].error != 0)
			return readers[i].error;
		const bool same_size = readers[i].result.size == result->size;
		const bool same_count = readers[i].result.count == result->count;
		const bool same_digest = memcmp(readers[i].result.digest, result->digest, sizeof(result->digest)) == 0;
		if (!same_size || !same_count || !same_digest)
			return 1;
	}
	return 0;
}

static int HashCoreFile(const char* path, const char* mode, size_t chunk, IMAGE_HASH_RESULT* result)
{
	HASH_CONTEXT context[HASH_MAX];
	/* Deliberately exercise buffers with no word or SIMD alignment. */
	uint8_t ALIGNED(64) buffer[65536 + 1];
	FILE* file = fopen(path, "rb");
	size_t count;
	if (file == NULL)
		return 1;
	const int software = strcmp(mode, "software") == 0;
	cpu_has_sha1_accel = software ? 0 : DetectSHA1Acceleration();
	cpu_has_sha256_accel = software ? 0 : DetectSHA256Acceleration();
	for (unsigned i = 0; i < HASH_MAX; i++)
		hash_init[i](&context[i]);
	while ((count = fread(buffer + 1, 1, chunk, file)) != 0) {
		for (unsigned i = 0; i < HASH_MAX; i++)
			hash_write[i](&context[i], buffer + 1, count);
		result->size += count;
	}
	const int error = ferror(file);
	fclose(file);
	if (error)
		return 1;
	result->count = HASH_MAX;
	for (unsigned i = 0; i < HASH_MAX; i++) {
		hash_final[i](&context[i]);
		memcpy(result->digest[i], context[i].buf, digest_size[i]);
	}
	return 0;
}

int main(int argc, char** argv)
{
	IMAGE_HASH_RESULT result = { 0 };
	IMAGE_FILE_SOURCE source = { .fd = -1 };
	if (argc != 4)
		return 2;
	const bool software = strcmp(argv[1], "software") == 0;
	const bool automatic = strcmp(argv[1], "auto") == 0;
	const bool defaults = strcmp(argv[1], "default") == 0;
	const bool extra = strcmp(argv[1], "extra") == 0;
	const bool parallel = strcmp(argv[1], "parallel") == 0;
	char* end;
	const unsigned long chunk = strtoul(argv[2], &end, 10);
	if (*end || chunk == 0 || chunk > 65536)
		return 2;
	int error;
	if (software || automatic) {
		error = HashCoreFile(argv[3], argv[1], chunk, &result);
	} else if (defaults || extra || parallel) {
		error = OpenImageFileSource(argv[3], &source);
		if (error == 0) {
			if (parallel)
				error = HashParallel(&source, argv[3], &result);
			else
				error = HashImageFileSource(&source, argv[3], extra, &result);
		}
		CloseImageFileSource(&source);
	} else {
		return 2;
	}
	if (error != 0) {
		fprintf(stderr, "hash error: %d\n", error);
		return 1;
	}
	printf("size %llu\ncount %u\nsha1_acceleration %d\nsha256_acceleration %d\n",
		(unsigned long long)result.size, result.count, cpu_has_sha1_accel, cpu_has_sha256_accel);
	for (unsigned i = 0; i < HASH_MAX; i++) {
		printf("%s ", digest_name[i]);
		for (size_t j = 0; j < sizeof(result.digest[i]); j++) {
			if (j < digest_size[i] && i < result.count)
				printf("%02x", result.digest[i][j]);
			else if (result.digest[i][j] != 0)
				return 1;
		}
		putchar('\n');
	}
	return 0;
}
