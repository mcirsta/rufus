/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Execute the original Windows BZIP2 wrapper with ordinary host-file shims.
 * This is a differential test provider, never part of the application/helper. */
#include "hash_core.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define LIBBB_H 1
#define UNARCHIVE_H 1
#define FAST_FUNC
#define IF_DESKTOP(x) x
#define BB_BUFSIZE 65536
#define BZIP2_MAGIC ('B' + 256 * 'Z')
#define aligned_xmalloc malloc
#define aligned_free free
#define xzalloc(size) calloc(1, size)
#define malloc_or_warn malloc
#define bb_simple_error_msg(message) fprintf(stderr, "%s\n", message)
#define bb_error_msg(...) fprintf(stderr, __VA_ARGS__)

typedef struct {
	int src_fd;
	size_t mem_output_size_max;
	HASH_CONTEXT hash;
	uint64_t bytes;
} transformer_state_t;

static void xfunc_die(void) { exit(2); }

static int safe_read(int fd, void* buffer, unsigned size)
{
	ssize_t count;
	do {
		count = read(fd, buffer, size);
	} while (count < 0 && errno == EINTR);
	return (int)count;
}

static int check_signature16(transformer_state_t* state, unsigned magic)
{
	uint8_t bytes[2];
	return safe_read(state->src_fd, bytes, sizeof(bytes)) != sizeof(bytes)
		|| (unsigned)(bytes[0] + 256 * bytes[1]) != magic;
}

static uint32_t* crc32_filltable(uint32_t* table, int endian)
{
	if (endian != 1)
		abort();
	for (unsigned i = 0; i < 256; i++) {
		uint32_t crc = (uint32_t)i << 24;
		for (unsigned bit = 0; bit < 8; bit++)
			crc = (crc << 1) ^ ((crc & UINT32_C(0x80000000)) ? UINT32_C(0x04c11db7) : 0);
		table[i] = crc;
	}
	return table;
}

static ssize_t transformer_write(transformer_state_t* state, const void* bytes, size_t size)
{
	hash_write[HASH_SHA256](&state->hash, bytes, size);
	state->bytes += size;
	return (ssize_t)size;
}

#include "bled/decompress_bunzip2.c"

int main(int argc, char** argv)
{
	if (argc != 2)
		return 2;
	transformer_state_t state = {.src_fd = open(argv[1], O_RDONLY | O_CLOEXEC)};
	if (state.src_fd < 0)
		return 2;
	hash_init[HASH_SHA256](&state.hash);
	const long long result = unpack_bz2_stream(&state);
	if (close(state.src_fd) != 0)
		return 2;
	hash_final[HASH_SHA256](&state.hash);
	printf("result %lld\ndecoded %llu\nsha256 ", result, (unsigned long long)state.bytes);
	for (size_t i = 0; i < SHA256_HASHSIZE; i++)
		printf("%02x", state.hash.buf[i]);
	printf("\n");
	return 0;
}
