/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Original Windows LZMA wrapper with host-file shims, for differential tests. */
#include "hash_core.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define LIBBB_H 1
#define UNARCHIVE_H 1
#define FAST_FUNC
#define ALWAYS_INLINE inline __attribute__((always_inline))
#define IF_DESKTOP(x) x
#define IF_NOT_DESKTOP(x)
#define IF_NOT_FEATURE_LZMA_FAST(x) x
#define BB_BUFSIZE 65536
#define SECTOR_ALIGNMENT 4096
#define PRAGMA_BEGIN_PACKED
#define PRAGMA_END_PACKED
#define PACKED __attribute__((packed))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define SWAP_LE32(x) (x)
#define SWAP_LE64(x) (x)
#else
#define SWAP_LE32(x) __builtin_bswap32(x)
#define SWAP_LE64(x) __builtin_bswap64(x)
#endif
#define aligned_xmalloc malloc
#define aligned_free free
#define xmalloc malloc
#define xzalloc(size) calloc(1, size)
#define bb_simple_error_msg(message) fprintf(stderr, "%s\n", message)
#define bb_simple_error_msg_and_die(message) do { bb_simple_error_msg(message); exit(2); } while (0)

typedef struct {
	int src_fd;
	size_t mem_output_size_max;
	HASH_CONTEXT hash;
	uint64_t bytes;
} transformer_state_t;

static bool one_byte_input;

static int safe_read(int fd, void* bytes, unsigned size)
{
	if (one_byte_input && size == BB_BUFSIZE)
		size = 1;
	ssize_t count;
	do {
		count = read(fd, bytes, size);
	} while (count < 0 && errno == EINTR);
	return (int)count;
}
#define full_read safe_read

static ssize_t transformer_write(transformer_state_t* state, const void* bytes, size_t size)
{
	hash_write[HASH_SHA256](&state->hash, bytes, size);
	state->bytes += size;
	return (ssize_t)size;
}

#include "bled/decompress_unlzma.c"

int main(int argc, char** argv)
{
	if (argc != 2 && (argc != 3 || strcmp(argv[2], "--one-byte-input") != 0))
		return 2;
	one_byte_input = argc == 3;
	transformer_state_t state = {.src_fd = open(argv[1], O_RDONLY | O_CLOEXEC)};
	if (state.src_fd < 0)
		return 2;
	hash_init[HASH_SHA256](&state.hash);
	const long long result = unpack_lzma_stream(&state);
	const off_t consumed = lseek(state.src_fd, 0, SEEK_CUR);
	if (close(state.src_fd) != 0)
		return 2;
	hash_final[HASH_SHA256](&state.hash);
	printf("result %lld\ndecoded %llu\nconsumed %lld\nsha256 ",
		result, (unsigned long long)state.bytes, (long long)consumed);
	for (size_t i = 0; i < SHA256_HASHSIZE; i++)
		printf("%02x", state.hash.buf[i]);
	printf("\n");
	return 0;
}
