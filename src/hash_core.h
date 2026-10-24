/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Hash context and entry points shared with the original Windows implementation. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifndef ALIGNED
#if defined(__GNUC__)
#define ALIGNED(m) __attribute__ ((__aligned__(m)))
#elif defined(_MSC_VER)
#define ALIGNED(m) __declspec(align(m))
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Hash definitions */
enum hash_type {
	HASH_MD5 = 0,
	HASH_SHA1,
	HASH_SHA256,
	HASH_SHA512,
	HASH_MAX
};

/* Blocksize for each hash algorithm - Must be a power of 2 */
#define MD5_BLOCKSIZE       64
#define SHA1_BLOCKSIZE      64
#define SHA256_BLOCKSIZE    64
#define SHA512_BLOCKSIZE    128
#define MAX_BLOCKSIZE       SHA512_BLOCKSIZE

/* Hashsize for each hash algorithm */
#define MD5_HASHSIZE        16
#define SHA1_HASHSIZE       20
#define SHA256_HASHSIZE     32
#define SHA512_HASHSIZE     64
#define MAX_HASHSIZE        SHA512_HASHSIZE

/* Context for the hash algorithms */
typedef struct ALIGNED(64) {
	uint8_t buf[MAX_BLOCKSIZE];
	uint64_t state[8];
	uint64_t bytecount;
} HASH_CONTEXT;

/* Hash functions */
typedef void hash_init_t(HASH_CONTEXT* ctx);
typedef void hash_write_t(HASH_CONTEXT* ctx, const uint8_t* buf, size_t len);
typedef void hash_final_t(HASH_CONTEXT* ctx);
extern hash_init_t* hash_init[HASH_MAX];
extern hash_write_t* hash_write[HASH_MAX];
extern hash_final_t* hash_final[HASH_MAX];

/* Configure before concurrent use, as in Windows startup. The native file
 * adapter initializes these once before hashing; contexts remain caller-owned. */
extern int cpu_has_sha1_accel, cpu_has_sha256_accel;
int DetectSHA1Acceleration(void);
int DetectSHA256Acceleration(void);

#ifdef __cplusplus
}
#endif
