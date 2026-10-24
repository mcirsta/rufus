/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "platform/image_hash.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum { SOURCE_BYTES = 7 * 65536 + 19, WAIT_SECONDS = 5, MAX_THREADS = 5 };
enum FAILURE { FAIL_NONE, FAIL_ALLOC, FAIL_MUTEX, FAIL_ATTRIBUTES, FAIL_CLOCK, FAIL_CONDITION, FAIL_THREAD };
enum WAIT_STATE { WAIT_READ_AHEAD, WAIT_FAST_HASHES, WAIT_CANCEL_JOIN };
static enum FAILURE failure;
static unsigned failure_index, algorithms, all_algorithms;
static int source_fd = -1, cases;
static atomic_int checks, failures, reads, created, joined, live, peak_live, join_attempts, reader_exits;
static atomic_int mutexes, conditions, attributes, allocations, write_calls[HASH_MAX];
static atomic_bool cancelled, returned;
static unsigned create_calls, condition_calls;
static void* allocation;
static bool gated;
static unsigned entered_mask, release_mask;
static pthread_t owner;
static unsigned progress_calls, endpoints;
static atomic_uint_fast64_t last_progress;
static pthread_mutex_t gate_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_condition = PTHREAD_COND_INITIALIZER;
static hash_write_t* original_write[HASH_MAX];

/* Independent hashlib digests for byte i = (i * 37 + i / 251) & 255. */
static const char* expected[HASH_MAX] = {
	"e9bb04c37112d6e32ca3def0d9a74a74",
	"a265dd7d12a56e3aab7e38b782758cecbfa31f7d",
	"f9193c3af5e036834b182294b939f37acb68b2ac600fa037c5d229f305f4dfc0",
	"d9119592aeb9c7a346408001e7f7b29b9c7eed6aaa2f0ba2ca272b544a8bb08e"
	"29a00e154700b378f41c0d127b42542ccf900cf63ff94ab6ac8cc2b6d288fcc9"
};
static const size_t digest_bytes[HASH_MAX] = {16, 20, 32, 64};

#define CHECK(expression) do { checks++; if (!(expression)) { \
	fprintf(stderr, "line %d: %s\n", __LINE__, #expression); failures++; } } while (0)

int IsRufusJobCancelled(void)
{
	CHECK(pthread_equal(pthread_self(), owner));
	return cancelled;
}
int IsIsoScanCancelled(void) { return cancelled; }

static void NotifyGate(void)
{
	pthread_mutex_lock(&gate_mutex);
	pthread_cond_broadcast(&gate_condition);
	pthread_mutex_unlock(&gate_mutex);
}

void _UpdateProgressWithInfo(int operation, int message, uint64_t processed, uint64_t total, int force)
{
	CHECK(pthread_equal(pthread_self(), owner));
	CHECK(operation == -3 && message == 271 && total == SOURCE_BYTES);
	CHECK(processed >= last_progress && processed <= total);
	if (progress_calls++ == 0)
		CHECK(processed == 0 && force);
	last_progress = processed;
	if (processed == total) {
		CHECK(force && !cancelled);
		CHECK(created == joined && live == 0 && allocations == 0);
		CHECK(mutexes == 0 && conditions == 0 && attributes == 0);
		endpoints++;
	}
}

extern int __real_posix_memalign(void**, size_t, size_t);
extern void __real_free(void*);
int __wrap_posix_memalign(void** memory, size_t alignment, size_t size)
{
	CHECK(alignment >= 64 && alignment % 64 == 0);
	CHECK(size >= 3 * 65536 && size <= 256 * 1024);
	if (failure == FAIL_ALLOC)
		return ENOMEM;
	const int error = __real_posix_memalign(memory, alignment, size);
	if (error == 0) {
		CHECK(allocation == NULL);
		allocation = *memory;
		allocations++;
	}
	return error;
}

void __wrap_free(void* memory)
{
	if (memory != NULL && memory == allocation) {
		CHECK(live == 0 && joined == created);
		CHECK(mutexes == 0 && conditions == 0 && attributes == 0);
		allocation = NULL;
		allocations--;
	}
	__real_free(memory);
}

extern int __real_pthread_mutex_init(pthread_mutex_t*, const pthread_mutexattr_t*);
extern int __real_pthread_mutex_destroy(pthread_mutex_t*);
int __wrap_pthread_mutex_init(pthread_mutex_t* mutex, const pthread_mutexattr_t* attrs)
{
	if (failure == FAIL_MUTEX)
		return EAGAIN;
	const int error = __real_pthread_mutex_init(mutex, attrs);
	if (error == 0)
		mutexes++;
	return error;
}
int __wrap_pthread_mutex_destroy(pthread_mutex_t* mutex)
{
	const int error = __real_pthread_mutex_destroy(mutex);
	CHECK(error == 0 && live == 0);
	if (error == 0)
		mutexes--;
	return error;
}

extern int __real_pthread_condattr_init(pthread_condattr_t*);
extern int __real_pthread_condattr_setclock(pthread_condattr_t*, clockid_t);
extern int __real_pthread_condattr_destroy(pthread_condattr_t*);
int __wrap_pthread_condattr_init(pthread_condattr_t* attrs)
{
	if (failure == FAIL_ATTRIBUTES)
		return ENOMEM;
	const int error = __real_pthread_condattr_init(attrs);
	if (error == 0)
		attributes++;
	return error;
}
int __wrap_pthread_condattr_setclock(pthread_condattr_t* attrs, clockid_t clock)
{
	CHECK(clock == CLOCK_MONOTONIC);
	return failure == FAIL_CLOCK ? ENOTSUP : __real_pthread_condattr_setclock(attrs, clock);
}
int __wrap_pthread_condattr_destroy(pthread_condattr_t* attrs)
{
	const int error = __real_pthread_condattr_destroy(attrs);
	CHECK(error == 0);
	if (error == 0)
		attributes--;
	return error;
}

extern int __real_pthread_cond_init(pthread_cond_t*, const pthread_condattr_t*);
extern int __real_pthread_cond_destroy(pthread_cond_t*);
int __wrap_pthread_cond_init(pthread_cond_t* condition, const pthread_condattr_t* attrs)
{
	const unsigned index = condition_calls++;
	if (failure == FAIL_CONDITION && index == failure_index)
		return ENOMEM;
	const int error = __real_pthread_cond_init(condition, attrs);
	if (error == 0)
		conditions++;
	return error;
}
int __wrap_pthread_cond_destroy(pthread_cond_t* condition)
{
	const int error = __real_pthread_cond_destroy(condition);
	CHECK(error == 0 && live == 0);
	if (error == 0)
		conditions--;
	return error;
}

struct ThreadStart { void* (*function)(void*); void* argument; unsigned index; };
static struct ThreadStart thread_start[MAX_THREADS];
extern int __real_pthread_create(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*);
extern int __real_pthread_join(pthread_t, void**);

static void* RunTrackedThread(void* argument)
{
	struct ThreadStart* start = argument;
	const int current = atomic_fetch_add(&live, 1) + 1;
	int previous = peak_live;
	while (current > previous && !atomic_compare_exchange_weak(&peak_live, &previous, current)) {}
	void* result = start->function(start->argument);
	if (start->index == algorithms)
		reader_exits++;
	live--;
	NotifyGate();
	return result;
}

int __wrap_pthread_create(pthread_t* thread, const pthread_attr_t* attrs, void* (*function)(void*), void* argument)
{
	const unsigned index = create_calls++;
	CHECK(index < MAX_THREADS);
	if (failure == FAIL_THREAD && index == failure_index)
		return EAGAIN;
	thread_start[index] = (struct ThreadStart){function, argument, index};
	const int error = __real_pthread_create(thread, attrs, RunTrackedThread, &thread_start[index]);
	if (error == 0)
		created++;
	return error;
}
int __wrap_pthread_join(pthread_t thread, void** result)
{
	join_attempts++;
	NotifyGate();
	const int error = __real_pthread_join(thread, result);
	CHECK(error == 0);
	if (error == 0)
		joined++;
	return error;
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
	if (fd == source_fd) {
		CHECK(!pthread_equal(pthread_self(), owner));
		CHECK(size <= 65536 && offset >= 0);
		reads++;
		NotifyGate();
	}
	return RealPread(fd, data, size, offset);
}

static void GatedHash(unsigned algorithm, HASH_CONTEXT* context, const uint8_t* data, size_t size)
{
	if (gated) {
		pthread_mutex_lock(&gate_mutex);
		entered_mask |= 1u << algorithm;
		pthread_cond_broadcast(&gate_condition);
		while ((release_mask & (1u << algorithm)) == 0)
			pthread_cond_wait(&gate_condition, &gate_mutex);
		pthread_mutex_unlock(&gate_mutex);
	}
	original_write[algorithm](context, data, size);
	write_calls[algorithm]++;
	NotifyGate();
}
static void GatedMd5(HASH_CONTEXT* context, const uint8_t* data, size_t size) { GatedHash(0, context, data, size); }
static void GatedSha1(HASH_CONTEXT* context, const uint8_t* data, size_t size) { GatedHash(1, context, data, size); }
static void GatedSha256(HASH_CONTEXT* context, const uint8_t* data, size_t size) { GatedHash(2, context, data, size); }
static void GatedSha512(HASH_CONTEXT* context, const uint8_t* data, size_t size) { GatedHash(3, context, data, size); }

static bool StateReady(enum WAIT_STATE state)
{
	if (state == WAIT_READ_AHEAD)
		return entered_mask == all_algorithms && reads == 3;
	if (state == WAIT_CANCEL_JOIN)
		return reader_exits == 1 && join_attempts >= 2;
	for (unsigned i = 0; i < algorithms; i++) {
		if (i != HASH_SHA256 && write_calls[i] < 3)
			return false;
	}
	return true;
}

static bool WaitForState(enum WAIT_STATE state)
{
	struct timespec deadline;
	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += WAIT_SECONDS;
	pthread_mutex_lock(&gate_mutex);
	int error = 0;
	while (!StateReady(state) && error == 0)
		error = pthread_cond_timedwait(&gate_condition, &gate_mutex, &deadline);
	const bool ready = StateReady(state);
	pthread_mutex_unlock(&gate_mutex);
	return ready;
}

static void ReleaseAlgorithms(unsigned mask)
{
	pthread_mutex_lock(&gate_mutex);
	release_mask = mask;
	pthread_cond_broadcast(&gate_condition);
	pthread_mutex_unlock(&gate_mutex);
}

static bool EmptyResult(const IMAGE_HASH_RESULT* result)
{
	const unsigned char* bytes = (const unsigned char*)result;
	for (size_t i = 0; i < sizeof(*result); i++) {
		if (bytes[i] != 0)
			return false;
	}
	return true;
}

static void CheckResult(const IMAGE_HASH_RESULT* result)
{
	CHECK(result->count == algorithms && result->size == SOURCE_BYTES);
	for (unsigned i = 0; i < HASH_MAX; i++) {
		for (size_t j = 0; j < MAX_HASHSIZE; j++) {
			unsigned value = 0;
			if (i < algorithms && j < digest_bytes[i])
				CHECK(sscanf(expected[i] + 2 * j, "%2x", &value) == 1);
			CHECK(result->digest[i][j] == value);
		}
	}
}

static void ResetState(bool extra)
{
	CHECK(live == 0 && allocations == 0 && mutexes == 0 && conditions == 0 && attributes == 0);
	algorithms = extra ? 4 : 3;
	all_algorithms = (1u << algorithms) - 1;
	reads = created = joined = live = peak_live = join_attempts = reader_exits = 0;
	create_calls = condition_calls = 0;
	progress_calls = endpoints = 0;
	last_progress = 0;
	cancelled = returned = false;
	entered_mask = release_mask = 0;
	gated = false;
	for (unsigned i = 0; i < HASH_MAX; i++)
		write_calls[i] = 0;
}

static void CheckResources(void)
{
	CHECK(created == joined && live == 0);
	CHECK(allocations == 0 && allocation == NULL);
	CHECK(mutexes == 0 && conditions == 0 && attributes == 0);
}

static void CheckSetupFailure(const IMAGE_FILE_SOURCE* source, const char* path,
	bool extra, enum FAILURE mode, unsigned index, int expected_error)
{
	IMAGE_HASH_RESULT result;
	cases++;
	ResetState(extra);
	failure = mode;
	failure_index = index;
	owner = pthread_self();
	memset(&result, 0xa5, sizeof(result));
	CHECK(HashImageFileSource(source, path, extra, &result) == expected_error);
	CHECK(EmptyResult(&result));
	CHECK(reads == 0 && endpoints == 0);
	CHECK(lseek(source->fd, 0, SEEK_CUR) == 11);
	CheckResources();
	failure = FAIL_NONE;
}

struct HashRun {
	const IMAGE_FILE_SOURCE* source;
	const char* path;
	bool extra;
	IMAGE_HASH_RESULT result;
	int error;
};
static void* RunHashes(void* argument)
{
	struct HashRun* run = argument;
	owner = pthread_self();
	run->error = HashImageFileSource(run->source, run->path, run->extra, &run->result);
	returned = true;
	return NULL;
}

static void CheckGatedRun(const IMAGE_FILE_SOURCE* source, const char* path, bool extra, bool cancel)
{
	cases++;
	ResetState(extra);
	gated = true;
	struct HashRun run = {.source = source, .path = path, .extra = extra};
	memset(&run.result, 0xa5, sizeof(run.result));
	pthread_t coordinator;
	CHECK(__real_pthread_create(&coordinator, NULL, RunHashes, &run) == 0);
	CHECK(WaitForState(WAIT_READ_AHEAD));
	CHECK(peak_live == (int)algorithms + 1);
	CHECK(reads == 3 && endpoints == 0 && !returned && last_progress == 0);
	CHECK(EmptyResult(&run.result));
	if (cancel) {
		cancelled = true;
		CHECK(WaitForState(WAIT_CANCEL_JOIN));
		CHECK(!returned && endpoints == 0 && reads == 3 && last_progress == 0);
	} else {
		ReleaseAlgorithms(all_algorithms & ~(1u << HASH_SHA256));
		CHECK(WaitForState(WAIT_FAST_HASHES));
		CHECK(reads == 3 && !returned && last_progress == 0); // A slow consumer owns all three slots.
	}
	ReleaseAlgorithms(all_algorithms);
	CHECK(__real_pthread_join(coordinator, NULL) == 0);
	CHECK(run.error == (cancel ? ECANCELED : 0));
	CHECK(endpoints == (cancel ? 0u : 1u));
	if (cancel)
		CHECK(EmptyResult(&run.result));
	else {
		CheckResult(&run.result);
		CHECK(reads == 8);
	}
	CHECK(lseek(source->fd, 0, SEEK_CUR) == 11);
	CheckResources();
	cancelled = false;
	gated = false;
}

int main(void)
{
	char path[] = "/tmp/rufus-hash-pipeline-XXXXXX";
	int file = mkstemp(path);
	if (file < 0)
		return 2;
	uint8_t data[SOURCE_BYTES];
	for (size_t i = 0; i < sizeof(data); i++)
		data[i] = (uint8_t)(i * 37 + i / 251);
	CHECK(write(file, data, sizeof(data)) == sizeof(data));
	CHECK(close(file) == 0);
	IMAGE_FILE_SOURCE source = {.fd = -1};
	CHECK(OpenImageFileSource(path, &source) == 0);
	source_fd = source.fd;
	CHECK(lseek(source_fd, 11, SEEK_SET) == 11);
	for (unsigned i = 0; i < HASH_MAX; i++)
		original_write[i] = hash_write[i];
	hash_write[0] = GatedMd5;
	hash_write[1] = GatedSha1;
	hash_write[2] = GatedSha256;
	hash_write[3] = GatedSha512;
	for (unsigned extra = 0; extra <= 1; extra++) {
		CheckSetupFailure(&source, path, extra, FAIL_ALLOC, 0, ENOMEM);
		CheckSetupFailure(&source, path, extra, FAIL_MUTEX, 0, EAGAIN);
		CheckSetupFailure(&source, path, extra, FAIL_ATTRIBUTES, 0, ENOMEM);
		CheckSetupFailure(&source, path, extra, FAIL_CLOCK, 0, ENOTSUP);
		for (unsigned i = 0; i < 3; i++)
			CheckSetupFailure(&source, path, extra, FAIL_CONDITION, i, ENOMEM);
		for (unsigned i = 0; i < (extra ? 5u : 4u); i++)
			CheckSetupFailure(&source, path, extra, FAIL_THREAD, i, EAGAIN);
		CheckGatedRun(&source, path, extra, false);
		CheckGatedRun(&source, path, extra, true);
	}
	for (unsigned i = 0; i < HASH_MAX; i++)
		hash_write[i] = original_write[i];
	CloseImageFileSource(&source);
	CHECK(unlink(path) == 0);
	printf("Hash pipeline: %d cases, %d checks, %d failures\n", cases, checks, failures);
	return failures != 0;
}
