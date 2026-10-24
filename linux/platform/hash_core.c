/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Native host facilities for Rufus's unchanged digest algorithms. */
#include "hash_native.h"

#include <pthread.h>
#include <string.h>

#if defined(__i386__) || defined(__x86_64__)
#include <immintrin.h>
#endif

/* The original core explicitly assumes little-endian hosts. Do not silently
 * offer incorrect digests on a new host architecture. */
#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "The Rufus hash core needs qualification on big-endian hosts"
#endif

typedef int BOOL;
#define TRUE 1
#define FALSE 0

int cpu_has_sha1_accel = FALSE, cpu_has_sha256_accel = FALSE;

/* Short reads and caller buffers need not be word-aligned. memcpy preserves
 * the original byte order without the Windows adapter's unaligned C casts. */
static uint32_t ReadSwap32(const void* data)
{
	uint32_t value;
	memcpy(&value, data, sizeof(value));
	return __builtin_bswap32(value);
}

static uint64_t ReadSwap64(const void* data)
{
	uint64_t value;
	memcpy(&value, data, sizeof(value));
	return __builtin_bswap64(value);
}

static void WriteSwap32(void* data, uint32_t value)
{
	value = __builtin_bswap32(value);
	memcpy(data, &value, sizeof(value));
}

static void WriteSwap64(void* data, uint64_t value)
{
	value = __builtin_bswap64(value);
	memcpy(data, &value, sizeof(value));
}

#define read_swap32(p) ReadSwap32(p)
#define read_swap64(p) ReadSwap64(p)
#define write_swap32(p, v) WriteSwap32(p, v)
#define write_swap64(p, v) WriteSwap64(p, v)
#define PREFETCH64(m) do { __builtin_prefetch((m), 0, 0); __builtin_prefetch((m) + 32, 0, 0); } while(0)

#include "hash_core.inc"

static pthread_once_t acceleration_once = PTHREAD_ONCE_INIT;

static void InitializeAcceleration(void)
{
	cpu_has_sha1_accel = DetectSHA1Acceleration();
	cpu_has_sha256_accel = DetectSHA256Acceleration();
}

int InitializeNativeHashes(void)
{
	return pthread_once(&acceleration_once, InitializeAcceleration);
}
