/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Execute the original Windows Unix-compress wrapper with host-file shims. */
#include "hash_core.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define LIBBB_H 1
#define UNARCHIVE_H 1
#define FAST_FUNC
#define IF_DESKTOP(x) x
#define COMPRESS_MAGIC 0x9d1f
#define xzalloc(size) calloc(1, size)
#define aligned_xzalloc(size) calloc(1, size)
#define aligned_free free
#define bb_simple_error_msg(message) fprintf(stderr, "%s\n", message)
#define bb_error_msg(...) fprintf(stderr, __VA_ARGS__)
#define bb_msg_read_error "read error"
#define bb_simple_error_msg_and_die(message) do { bb_simple_error_msg(message); exit(2); } while (0)

static unsigned BB_BUFSIZE = 65536;

typedef struct {
	int src_fd, signature_skipped;
	size_t mem_output_size_max;
	HASH_CONTEXT hash;
	uint64_t bytes, attempted;
	unsigned calls;
	int final_failure;
} transformer_state_t;

static int safe_read(int fd, void* bytes, unsigned size)
{
	ssize_t count;
	do {
		count = read(fd, bytes, size);
	} while (count < 0 && errno == EINTR);
	return (int)count;
}
#define full_read safe_read

static int check_signature16(transformer_state_t* state, unsigned magic)
{
	if (state->signature_skipped == 2)
		return 0;
	uint8_t bytes[2];
	return safe_read(state->src_fd, bytes, sizeof(bytes)) != sizeof(bytes)
		|| (unsigned)(bytes[0] + 256 * bytes[1]) != magic;
}

static ssize_t transformer_write(transformer_state_t* state, const void* bytes, size_t size)
{
	state->attempted += size;
	state->calls++;
	if (state->final_failure != 0 && size < BB_BUFSIZE)
		return state->final_failure == 1 ? -1 : -ENOSPC;
	hash_write[HASH_SHA256](&state->hash, bytes, size);
	state->bytes += size;
	return (ssize_t)size;
}

#include "bled/decompress_uncompress.c"

int main(int argc, char** argv)
{
	if (argc < 2 || argc > 4)
		return 2;
	transformer_state_t state = {.src_fd = open(argv[1], O_RDONLY | O_CLOEXEC)};
	if (state.src_fd < 0)
		return 2;
	for (int i = 2; i < argc; i++) {
		if (strcmp(argv[i], "--fail-final") == 0) state.final_failure = 1;
		else if (strcmp(argv[i], "--enospc-final") == 0) state.final_failure = 2;
		else if (strcmp(argv[i], "--windows-buffer") == 0) BB_BUFSIZE = 262144;
		else if (strcmp(argv[i], "--gzip-dispatch") == 0) {
			uint8_t magic[2];
			if (safe_read(state.src_fd, magic, sizeof(magic)) != 2 || magic[0] != 0x1f || magic[1] != 0x9d)
				return 2;
			state.signature_skipped = 2;
		} else return 2;
	}
	hash_init[HASH_SHA256](&state.hash);
	const long long result = unpack_Z_stream(&state);
	const off_t consumed = lseek(state.src_fd, 0, SEEK_CUR);
	if (close(state.src_fd) != 0)
		return 2;
	hash_final[HASH_SHA256](&state.hash);
	printf("result %lld\ndecoded %llu\nattempted %llu\ncalls %u\nconsumed %lld\nsha256 ", result,
		(unsigned long long)state.bytes, (unsigned long long)state.attempted, state.calls, (long long)consumed);
	for (size_t i = 0; i < SHA256_HASHSIZE; i++)
		printf("%02x", state.hash.buf[i]);
	printf("\n");
	return 0;
}
