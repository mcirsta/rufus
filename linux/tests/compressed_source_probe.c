/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "platform/compressed_source.h"
#include "platform/compressed_ffu.h"
#include "hash_core.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zlib.h>

int IsRufusJobCancelled(void) { return 0; }
int IsIsoScanCancelled(void) { return 0; }

typedef struct { HASH_CONTEXT hash; uint64_t position; } HASH_DATA;
static int HashOutput(void* context, const uint8_t* bytes, size_t size, uint64_t offset)
{
	HASH_DATA* data = context;
	if (offset != data->position || size > 65536)
		return EIO;
	hash_write[HASH_SHA256](&data->hash, bytes, size);
	data->position += size;
	return 0;
}

static int PrintStore(void* context, unsigned index, uint64_t bytes, unsigned sector,
	const uint8_t* path, size_t path_bytes)
{
	unsigned* count = context;
	printf("%s{\"index\":%u,\"bytes\":%llu,\"sector\":%u,\"path\":\"", *count == 0 ? "" : ",",
		index, (unsigned long long)bytes, sector);
	for (size_t i = 0; i < path_bytes; i++) printf("%02x", path[i]);
	printf("\"}");
	(*count)++;
	return 0;
}

int main(int argc, char** argv)
{
	if (argc == 3 && strcmp(argv[1], "ffu-stores") == 0) {
		IMAGE_FILE_SOURCE source = {.fd = -1};
		int error = OpenImageFileSource(argv[2], &source);
		unsigned count = 0;
		printf("{\"stores\":[");
		if (error == 0) error = ReadFfuStoreList(&source, PrintStore, &count);
		CloseImageFileSource(&source);
		printf("],\"error\":%d}\n", error);
		return 0;
	}
	if (argc == 3 && strcmp(argv[1], "ffu-base") == 0) {
		IMAGE_FILE_SOURCE source = {.fd = -1};
		bool required = false;
		int error = OpenImageFileSource(argv[2], &source);
		if (error == 0) error = FfuRequiresBase(&source, &required);
		CloseImageFileSource(&source);
		printf("{\"error\":%d,\"required\":%d}\n", error, required);
		return 0;
	}
	if (argc != 4 && argc != 5)
		return 2;
	char* end;
	errno = 0;
	uint64_t maximum = strtoull(argv[1], &end, 10);
	if (errno || *end || *argv[1] == 0)
		return 2;
	const bool hash = strcmp(argv[2], "hash") == 0;
	if (!hash && strcmp(argv[2], "count") != 0)
		return 2;
	IMAGE_FILE_SOURCE source = {.fd = -1};
	COMPRESSED_IMAGE_RESULT result = {0};
	HASH_DATA data = {0};
	hash_init[HASH_SHA256](&data.hash);
	int error = OpenImageFileSource(argv[3], &source);
	if (argc == 5) source.ffu_store = (unsigned)strtoul(argv[4], NULL, 10);
	if (error == 0 && lseek(source.fd, 7, SEEK_SET) != 7)
		error = EIO;
	if (error == 0)
		error = ReadCompressedImageSource(&source, argv[3], maximum, hash ? HashOutput : NULL, &data, &result);
	if (source.fd >= 0 && lseek(source.fd, 0, SEEK_CUR) != 7)
		return 3;
	CloseImageFileSource(&source);
	printf("error %d\ntype %d\nsource %llu\ndecoded %llu\nmembers %llu\ntrailing %llu\nmarker %d\nforced %d\nprefix ",
		error, result.compression_type, (unsigned long long)result.source_bytes,
		(unsigned long long)result.decoded_bytes, (unsigned long long)result.members,
		(unsigned long long)result.trailing_bytes, result.has_boot_marker,
		GetCompressedBootMarker(result.prefix, result.prefix_size, true));
	for (size_t i = 0; i < result.prefix_size; i++)
		printf("%02x", result.prefix[i]);
	printf("\nsha256 ");
	if (error == 0 && hash) {
		hash_final[HASH_SHA256](&data.hash);
		for (size_t i = 0; i < SHA256_HASHSIZE; i++)
			printf("%02x", data.hash.buf[i]);
	}
	printf("\nprovider %s\n", zlibVersion());
	printf("store %u\nstores %u\n", result.ffu_store, result.ffu_stores);
	return 0;
}
