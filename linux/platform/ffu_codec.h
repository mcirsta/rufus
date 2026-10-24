/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* LZNT1 emits at most size + 2 * ceil(size / 4096) bytes. Incompressible
 * chunks use the format's literal representation. No allocation or history
 * crosses a 4 KiB chunk; cancellation is checked between chunks. */
int EncodeFfuLznt1(const uint8_t* input, size_t size, uint8_t* output, size_t capacity, size_t* written);
/* Desktop FFU capture uses plain XPRESS (COMPRESSION_FORMAT_XPRESS = 3).
 * The capacity includes literal expansion and the terminal flag word; zero
 * means an invalid/overflowing input size. History is bounded to 8192 bytes. */
size_t FfuXpressCapacity(size_t size);
int EncodeFfuXpress(const uint8_t* input, size_t size, uint8_t* output, size_t capacity, size_t* written);
int DecodeFfuXpress(const uint8_t* input, size_t size, uint8_t* output, size_t capacity);
int DecodeFfuLznt1(const uint8_t* input, size_t size, uint8_t* output, size_t capacity);
int DecodeFfuHuffman(const uint8_t* input, size_t size, uint8_t* output, size_t capacity);
#ifdef __cplusplus
}
#endif
