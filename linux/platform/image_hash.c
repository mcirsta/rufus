/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "image_hash.h"
#include "hash_native.h"
#include "rufus_reporting.h"
#include "progress_policy.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum {
	HASH_READ_BYTES = 64 * 1024,
	HASH_BUFFER_COUNT = 3,
	HASH_PROGRESS_MESSAGE = 271,
	HASH_CANCEL_POLL_NS = 50 * 1000 * 1000,
	NANOSECONDS_PER_SECOND = 1000 * 1000 * 1000,
	HASH_ALIGNMENT = 64
};
enum HASH_CONDITION { HASH_DATA_READY, HASH_SPACE_READY, HASH_STATUS_READY, HASH_CONDITION_COUNT };

static const size_t digest_size[HASH_MAX] = {
	MD5_HASHSIZE, SHA1_HASHSIZE, SHA256_HASHSIZE, SHA512_HASHSIZE
};

typedef struct {
	uint8_t ALIGNED(64) bytes[HASH_READ_BYTES];
	size_t size;
	unsigned remaining;
} HASH_BUFFER;

typedef struct HASH_PIPELINE HASH_PIPELINE;
typedef struct {
	HASH_CONTEXT context;
	HASH_PIPELINE* pipeline;
	unsigned algorithm;
	pthread_t thread;
} HASH_WORKER;

struct HASH_PIPELINE {
	HASH_BUFFER buffer[HASH_BUFFER_COUNT];
	HASH_WORKER worker[HASH_MAX];
	const IMAGE_FILE_SOURCE* source;
	uint64_t total, published, completed;
	unsigned count, started, finished, conditions;
	pthread_t reader;
	pthread_mutex_t mutex;
	pthread_cond_t condition[HASH_CONDITION_COUNT];
	bool mutex_ready, reader_started, reader_done, stop;
	int error;
};

/* Called with the mutex held. Wake readers, consumers and the coordinator on
 * every failure/cancel path, including partially created worker groups. */
static void StopHashPipelineLocked(HASH_PIPELINE* pipeline, int error)
{
	if (pipeline->error == 0)
		pipeline->error = error;
	pipeline->stop = true;
	for (unsigned i = 0; i < pipeline->conditions; i++)
		pthread_cond_broadcast(&pipeline->condition[i]);
}

static bool HashPipelineStopped(HASH_PIPELINE* pipeline)
{
	pthread_mutex_lock(&pipeline->mutex);
	const bool stopped = pipeline->stop;
	pthread_mutex_unlock(&pipeline->mutex);
	return stopped;
}

static int ReadHashBuffer(HASH_PIPELINE* pipeline, HASH_BUFFER* buffer, uint64_t offset)
{
	const uint64_t remaining = pipeline->total - offset;
	const size_t requested = remaining < HASH_READ_BYTES ? (size_t)remaining : HASH_READ_BYTES;
	size_t used = 0;

	while (used < requested) {
		if (HashPipelineStopped(pipeline))
			return ECANCELED;
		const ssize_t count = pread(pipeline->source->fd, buffer->bytes + used,
			requested - used, (off_t)(offset + used));
		if (count < 0 && errno == EINTR)
			continue;
		if (count < 0)
			return errno;
		if (count == 0)
			return EIO;
		used += (size_t)count;
	}
	buffer->size = used;
	return 0;
}

static void* ReadHashSource(void* argument)
{
	HASH_PIPELINE* pipeline = argument;
	uint64_t offset = 0, sequence = 0;

	pthread_mutex_lock(&pipeline->mutex);
	while (!pipeline->stop && offset < pipeline->total) {
		HASH_BUFFER* buffer = &pipeline->buffer[sequence % HASH_BUFFER_COUNT];
		while (!pipeline->stop && buffer->remaining != 0) {
			const int error = pthread_cond_wait(&pipeline->condition[HASH_SPACE_READY], &pipeline->mutex);
			if (error != 0)
				StopHashPipelineLocked(pipeline, error);
		}
		if (pipeline->stop)
			break;
		pthread_mutex_unlock(&pipeline->mutex);
		const int error = ReadHashBuffer(pipeline, buffer, offset);
		pthread_mutex_lock(&pipeline->mutex);
		if (error != 0) {
			StopHashPipelineLocked(pipeline, error);
			break;
		}
		if (pipeline->stop)
			break;
		buffer->remaining = pipeline->count;
		pipeline->published++;
		sequence++;
		offset += buffer->size;
		pthread_cond_broadcast(&pipeline->condition[HASH_DATA_READY]);
	}
	pipeline->reader_done = true;
	pthread_cond_broadcast(&pipeline->condition[HASH_DATA_READY]);
	pthread_cond_signal(&pipeline->condition[HASH_STATUS_READY]);
	pthread_mutex_unlock(&pipeline->mutex);
	return NULL;
}

static void* ComputeHash(void* argument)
{
	HASH_WORKER* worker = argument;
	HASH_PIPELINE* pipeline = worker->pipeline;
	uint64_t sequence = 0;

	hash_init[worker->algorithm](&worker->context);
	pthread_mutex_lock(&pipeline->mutex);
	while (!pipeline->stop) {
		while (!pipeline->stop && sequence == pipeline->published && !pipeline->reader_done) {
			const int error = pthread_cond_wait(&pipeline->condition[HASH_DATA_READY], &pipeline->mutex);
			if (error != 0)
				StopHashPipelineLocked(pipeline, error);
		}
		if (pipeline->stop)
			break;
		if (sequence == pipeline->published && pipeline->reader_done) {
			pthread_mutex_unlock(&pipeline->mutex);
			hash_final[worker->algorithm](&worker->context);
			pthread_mutex_lock(&pipeline->mutex);
			break;
		}
		HASH_BUFFER* buffer = &pipeline->buffer[sequence % HASH_BUFFER_COUNT];
		pthread_mutex_unlock(&pipeline->mutex);
		hash_write[worker->algorithm](&worker->context, buffer->bytes, buffer->size);
		pthread_mutex_lock(&pipeline->mutex);
		buffer->remaining--;
		if (buffer->remaining == 0) {
			/* Every consumer keeps source order, so last-consumer releases
			 * are ordered too. Count bytes hashed by ALL algorithms. */
			pipeline->completed += buffer->size;
			pthread_cond_signal(&pipeline->condition[HASH_SPACE_READY]);
			pthread_cond_signal(&pipeline->condition[HASH_STATUS_READY]);
		}
		sequence++;
	}
	pipeline->finished++;
	pthread_cond_signal(&pipeline->condition[HASH_STATUS_READY]);
	pthread_mutex_unlock(&pipeline->mutex);
	return NULL;
}

static int InitializeHashPipeline(HASH_PIPELINE* pipeline)
{
	pthread_condattr_t attributes;
	int error = pthread_mutex_init(&pipeline->mutex, NULL);
	if (error != 0)
		return error;
	pipeline->mutex_ready = true;
	error = pthread_condattr_init(&attributes);
	if (error != 0)
		return error;
	error = pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC);
	while (error == 0 && pipeline->conditions < HASH_CONDITION_COUNT) {
		error = pthread_cond_init(&pipeline->condition[pipeline->conditions], &attributes);
		if (error == 0)
			pipeline->conditions++;
	}
	pthread_condattr_destroy(&attributes);
	return error;
}

static int StartHashWorkers(HASH_PIPELINE* pipeline)
{
	for (unsigned i = 0; i < pipeline->count; i++) {
		HASH_WORKER* worker = &pipeline->worker[i];
		worker->algorithm = i;
		worker->pipeline = pipeline;
		const int error = pthread_create(&worker->thread, NULL, ComputeHash, worker);
		if (error != 0)
			return error;
		pipeline->started++;
	}
	const int error = pthread_create(&pipeline->reader, NULL, ReadHashSource, pipeline);
	if (error == 0)
		pipeline->reader_started = true;
	return error;
}

static int WaitForHashWorkers(HASH_PIPELINE* pipeline)
{
	uint64_t reported = 0;
	int error = 0;

	for (;;) {
		if (IsRufusJobCancelled())
			return ECANCELED;
		struct timespec deadline;
		if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0)
			return errno;
		deadline.tv_nsec += HASH_CANCEL_POLL_NS;
		if (deadline.tv_nsec >= NANOSECONDS_PER_SECOND) {
			deadline.tv_nsec -= NANOSECONDS_PER_SECOND;
			deadline.tv_sec++;
		}
		pthread_mutex_lock(&pipeline->mutex);
		bool done = pipeline->reader_done && pipeline->finished == pipeline->count;
		if (!pipeline->stop && !done && pipeline->completed == reported) {
			error = pthread_cond_timedwait(&pipeline->condition[HASH_STATUS_READY], &pipeline->mutex, &deadline);
			if (error != 0 && error != ETIMEDOUT)
				StopHashPipelineLocked(pipeline, error);
		}
		const uint64_t processed = pipeline->completed;
		done = pipeline->reader_done && pipeline->finished == pipeline->count;
		const bool stopped = pipeline->stop;
		error = pipeline->error;
		pthread_mutex_unlock(&pipeline->mutex);
		if (stopped)
			return error;
		if (processed != reported) {
			reported = processed;
			if (processed < pipeline->total)
				_UpdateProgressWithInfo(OP_NOOP_WITH_TASKBAR, HASH_PROGRESS_MESSAGE,
					processed, pipeline->total, 0);
		}
		if (done)
			return 0;
	}
}

static void JoinHashWorkers(HASH_PIPELINE* pipeline)
{
	/* Only successfully created, joinable, non-self handles reach here. A join
	 * failure would be a broken internal lifetime invariant: never free buffers
	 * or report completion while an unjoined reader/consumer can still use them. */
	if (pipeline->reader_started && pthread_join(pipeline->reader, NULL) != 0)
		abort();
	for (unsigned i = 0; i < pipeline->started; i++) {
		if (pthread_join(pipeline->worker[i].thread, NULL) != 0)
			abort();
	}
}

static void DestroyHashPipeline(HASH_PIPELINE* pipeline)
{
	for (unsigned i = 0; i < pipeline->conditions; i++) {
		if (pthread_cond_destroy(&pipeline->condition[i]) != 0)
			abort();
	}
	if (pipeline->mutex_ready && pthread_mutex_destroy(&pipeline->mutex) != 0)
		abort();
	free(pipeline);
}

int HashImageFileSource(const IMAGE_FILE_SOURCE* source, const char* path,
	bool extra, IMAGE_HASH_RESULT* result)
{
	IMAGE_HASH_RESULT completed = { 0 };
	HASH_PIPELINE* pipeline;
	int flags, error;

	if (result == NULL)
		return EINVAL;
	memset(result, 0, sizeof(*result));
	error = CheckImageFileSource(source, path);
	if (error != 0)
		return error;
	flags = fcntl(source->fd, F_GETFL);
	if (flags < 0)
		return errno;
	const bool readable = (flags & O_ACCMODE) != O_WRONLY;
	const bool ordinary_io = (flags & (O_DIRECT | O_PATH)) == 0;
	if (!readable || !ordinary_io || source->info.st_size < 0)
		return EINVAL;
	error = InitializeNativeHashes();
	if (error != 0)
		return error;
	completed.count = extra ? HASH_MAX : HASH_SHA512;
	completed.size = (uint64_t)source->info.st_size;
	_UpdateProgressWithInfo(OP_NOOP_WITH_TASKBAR, HASH_PROGRESS_MESSAGE, 0, completed.size, 1);
	if (IsRufusJobCancelled())
		return ECANCELED;
	error = posix_memalign((void**)&pipeline, HASH_ALIGNMENT, sizeof(*pipeline));
	if (error != 0)
		return error;
	memset(pipeline, 0, sizeof(*pipeline));
	pipeline->source = source;
	pipeline->total = completed.size;
	pipeline->count = completed.count;
	error = InitializeHashPipeline(pipeline);
	if (error == 0)
		error = StartHashWorkers(pipeline);
	if (error == 0)
		error = WaitForHashWorkers(pipeline);
	if (pipeline->mutex_ready) {
		pthread_mutex_lock(&pipeline->mutex);
		StopHashPipelineLocked(pipeline, error);
		pthread_mutex_unlock(&pipeline->mutex);
	}
	JoinHashWorkers(pipeline);
	if (error == 0)
		error = CheckImageFileSource(source, path);
	if (error == 0 && IsRufusJobCancelled())
		error = ECANCELED;
	if (error == 0) {
		for (unsigned i = 0; i < completed.count; i++)
			memcpy(completed.digest[i], pipeline->worker[i].context.buf, digest_size[i]);
	}
	DestroyHashPipeline(pipeline);
	if (error != 0)
		return error;
	*result = completed;
	_UpdateProgressWithInfo(OP_NOOP_WITH_TASKBAR, HASH_PROGRESS_MESSAGE,
		completed.size, completed.size, 1);
	return 0;
}
