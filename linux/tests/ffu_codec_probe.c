/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Test-only upstream Huffman encoder and direct bounded decoder checks. */
#include "platform/ffu_codec.h"
#include "wimlib.h"
#include "platform/hash_native.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int cancelled, cancel_after, cancel_queries;
int IsIsoScanCancelled(void) { return cancelled; }
int IsRufusJobCancelled(void) { return cancelled || (cancel_after && ++cancel_queries >= cancel_after); }
static unsigned char* ReadFile(const char* path, size_t* size)
{
    FILE* file = fopen(path, "rb");
    if (!file || fseek(file, 0, SEEK_END) != 0) abort();
    long count = ftell(file);
    if (count < 0 || count > 32 * 1024 * 1024 || fseek(file, 0, SEEK_SET) != 0) abort();
    unsigned char* data = malloc((size_t)count + 1);
    if (!data || fread(data, 1, (size_t)count, file) != (size_t)count || fclose(file) != 0) abort();
    *size = (size_t)count; return data;
}
int main(int argc, char** argv)
{
    if (argc >= 4 && argc <= 5 && (strcmp(argv[1], "encode-lznt1") == 0 || strcmp(argv[1], "encode-xpress") == 0)) {
        size_t size; unsigned char* data = ReadFile(argv[2], &size);
        const int xpress = strcmp(argv[1], "encode-xpress") == 0;
        const size_t maximum = xpress ? FfuXpressCapacity(size) : size + 2 * ((size + 4095) / 4096);
        const size_t capacity = argc == 5 && strcmp(argv[4], "small") == 0 ? 1 : maximum;
        unsigned char* packed = malloc(capacity + 16); if (!packed) abort();
        memset(packed, 0xa5, capacity + 16);
        cancelled = argc == 5 && strcmp(argv[4], "cancel") == 0;
        cancel_after = argc == 5 && strcmp(argv[4], "cancel-late") == 0 ? 3 : 0;
        size_t bytes = SIZE_MAX;
        int error = xpress ? EncodeFfuXpress(data, size, packed, capacity, &bytes)
            : EncodeFfuLznt1(data, size, packed, capacity, &bytes);
        for (unsigned i = 0; i < 16; i++) if (packed[capacity + i] != 0xa5) abort();
        if (error == 0) {
            FILE* output = fopen(argv[3], "wb");
            if (!output || fwrite(packed, 1, bytes, output) != bytes || fclose(output) != 0) abort();
        }
        printf("%d %zu\n", error, bytes);
        free(data); free(packed); return 0;
    }
    if (argc == 4 && strcmp(argv[1], "encode") == 0) {
        size_t size; unsigned char* data = ReadFile(argv[2], &size);
        if (size == 0 || size > 65536) abort();
        struct wimlib_compressor* compressor = NULL;
        if (wimlib_create_compressor(WIMLIB_COMPRESSION_TYPE_XPRESS, 65536, 50, &compressor) != 0) abort();
        unsigned char* packed = malloc(2 * size + 1024); if (!packed) abort();
        size_t bytes = wimlib_compress(data, size, packed, 2 * size + 1024, compressor);
        if (bytes == 0) abort();
        FILE* output = fopen(argv[3], "wb");
        if (!output || fwrite(packed, 1, bytes, output) != bytes || fclose(output) != 0) abort();
        wimlib_free_compressor(compressor); free(data); free(packed); return 0;
    }
    if (argc != 5) return 2;
    unsigned algorithm = (unsigned)strtoul(argv[1], NULL, 10);
    size_t capacity = (size_t)strtoull(argv[2], NULL, 10), size;
    if (capacity == 0 || capacity > 16 * 1024 * 1024) abort();
    unsigned char* packed = ReadFile(argv[3], &size);
    unsigned char* output = malloc(capacity + 16); if (!output) abort();
    memset(output + capacity, 0xa5, 16); cancelled = strcmp(argv[4], "cancel") == 0;
    int error;
    if (strcmp(argv[4], "reference") == 0) {
        struct wimlib_decompressor* reference = NULL;
        if (wimlib_create_decompressor(WIMLIB_COMPRESSION_TYPE_XPRESS, 65536, &reference) != 0) abort();
        error = wimlib_decompress(packed, size, output, capacity, reference) == 0 ? 0 : EBADMSG;
        wimlib_free_decompressor(reference);
    } else error = algorithm == 2 ? DecodeFfuLznt1(packed, size, output, capacity)
        : algorithm == 3 ? DecodeFfuXpress(packed, size, output, capacity)
        : DecodeFfuHuffman(packed, size, output, capacity);
    for (unsigned i = 0; i < 16; i++) if (output[capacity + i] != 0xa5) abort();
    printf("%d ", error);
    if (error == 0) {
        HASH_CONTEXT hash; if (InitializeNativeHashes() != 0) abort();
        hash_init[HASH_SHA256](&hash); hash_write[HASH_SHA256](&hash, output, capacity); hash_final[HASH_SHA256](&hash);
        for (unsigned i = 0; i < 32; i++) printf("%02x", hash.buf[i]);
    }
    putchar('\n'); free(output); free(packed); return 0;
}
