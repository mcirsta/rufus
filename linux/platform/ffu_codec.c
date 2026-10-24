/* SPDX-License-Identifier: GPL-3.0-or-later */
/* MS-XCA 2.2 and 2.5. Bounded, exact-output FFU buffer decoders. */
#include "ffu_codec.h"
#include "rufus_reporting.h"
#include <errno.h>
#include <stdbool.h>
#include <string.h>

enum { LZNT_CHUNK = 4096, HUFF_CHUNK = 65536, HUFF_SYMBOLS = 512,
    HUFF_BITS = 15, HUFF_TABLE = 1 << HUFF_BITS,
    XPRESS_WINDOW = 8192, XPRESS_FLAGS = 32, XPRESS_MAX_MATCH = UINT16_MAX + 3 };
static unsigned Word(const uint8_t* p) { return p[0] | (unsigned)p[1] << 8; }

static uint32_t Codec32(const uint8_t* bytes)
{
    return Word(bytes) | (uint32_t)Word(bytes + 2) << 16;
}

static void PutCodec32(uint8_t* bytes, uint32_t value)
{
    for (unsigned index = 0; index < 4; index++) bytes[index] = (uint8_t)(value >> (8 * index));
}

size_t FfuXpressCapacity(size_t size)
{
    if (size == 0 || size > UINT32_MAX) return 0;
    const size_t flags = (size / XPRESS_FLAGS + 1) * 4;
    return size > SIZE_MAX - flags ? 0 : size + flags;
}

static int EncodeXpressMatch(uint8_t* output, size_t capacity, size_t* stored,
    size_t* half_byte, size_t distance, size_t length)
{
    const bool extended = length >= 10, long_length = length >= 25;
    const size_t extra = long_length ? (length < 280 ? 1 : 3) : 0;
    const size_t bytes = 2 + (extended && *half_byte == SIZE_MAX ? 1 : 0) + extra;
    if (*stored > capacity || bytes > capacity - *stored) return ENOBUFS;
    const unsigned token = (unsigned)(distance - 1) << 3 | (extended ? 7u : (unsigned)(length - 3));
    output[(*stored)++] = (uint8_t)token; output[(*stored)++] = (uint8_t)(token >> 8);
    if (extended) {
        const unsigned nibble = long_length ? 15u : (unsigned)(length - 10);
        if (*half_byte == SIZE_MAX) {
            *half_byte = *stored;
            output[(*stored)++] = (uint8_t)nibble;
        } else {
            output[*half_byte] |= (uint8_t)(nibble << 4);
            *half_byte = SIZE_MAX;
        }
    }
    if (long_length) {
        if (length < 280) output[(*stored)++] = (uint8_t)(length - 25);
        else {
            output[(*stored)++] = 255;
            const unsigned value = (unsigned)(length - 3);
            output[(*stored)++] = (uint8_t)value; output[(*stored)++] = (uint8_t)(value >> 8);
        }
    }
    return 0;
}

/* MS-XCA 2.3.4. A single recent three-byte candidate gives bounded linear
 * capture work. A match may overlap its source. Capping its length at 65538
 * avoids a 32-bit length escape without changing decoded content. */
int EncodeFfuXpress(const uint8_t* input, size_t size, uint8_t* output, size_t capacity, size_t* written)
{
    if (written == NULL) return EINVAL;
    *written = 0;
    if (input == NULL || output == NULL || FfuXpressCapacity(size) == 0) return EINVAL;
    if (capacity < 4) return ENOBUFS;
    if (IsRufusJobCancelled()) return ECANCELED;
    uint32_t history[XPRESS_WINDOW];
    memset(history, 0xff, sizeof(history));
    size_t position = 0, stored = 4, flag_offset = 0, half_byte = SIZE_MAX, cancel_position = 0;
    uint32_t flags = 0;
    unsigned count = 0;
    while (position < size) {
        if (position >= cancel_position) {
            *written = stored;
            if (IsRufusJobCancelled()) return ECANCELED;
            cancel_position = position + 4096;
        }
        size_t length = 0, previous = UINT32_MAX;
        if (size - position >= 3) {
            const unsigned key = ((unsigned)input[position] * 251u
                ^ (unsigned)input[position + 1] * 31u ^ input[position + 2]) & (XPRESS_WINDOW - 1);
            previous = history[key]; history[key] = (uint32_t)position;
            if (previous < position && position - previous <= XPRESS_WINDOW) {
                const size_t limit = size - position < XPRESS_MAX_MATCH ? size - position : XPRESS_MAX_MATCH;
                while (length < limit && input[previous + length] == input[position + length]) length++;
            }
        }
        const bool match = length >= 3;
        if (match) {
            const int error = EncodeXpressMatch(output, capacity, &stored, &half_byte, position - previous, length);
            if (error != 0) return error;
            position += length;
        } else {
            if (stored == capacity) return ENOBUFS;
            output[stored++] = input[position++];
        }
        flags = (flags << 1) | (match ? 1u : 0u);
        if (++count == XPRESS_FLAGS) {
            PutCodec32(output + flag_offset, flags);
            if (capacity - stored < 4) return ENOBUFS;
            flag_offset = stored; stored += 4; flags = 0; count = 0;
        }
    }
    const unsigned unused = XPRESS_FLAGS - count;
    const uint32_t terminal = count == 0 ? UINT32_MAX : (flags << unused) | ((UINT32_C(1) << unused) - 1);
    PutCodec32(output + flag_offset, terminal);
    *written = stored;
    return 0;
}

/* MS-XCA 2.5. A single recent three-byte match per bucket keeps capture work
 * bounded. Raw chunks cap expansion when flags and matches would cost more.
 * Source bytes can be compared past the current position: the decoder's
 * forward copies reproduce precisely that repeated prefix. */
static size_t EncodeLzntChunk(const uint8_t* input, size_t size, uint8_t packed[LZNT_CHUNK])
{
    uint16_t history[LZNT_CHUNK];
    memset(history, 0xff, sizeof(history));
    size_t position = 0, stored = 0;
    while (position < size) {
        if (stored == LZNT_CHUNK) return size;
        const size_t flags = stored++;
        packed[flags] = 0;
        for (unsigned bit = 0; bit < 8 && position < size; bit++) {
            size_t match = 0, previous = UINT16_MAX;
            unsigned shift = 12, mask = 4095;
            for (size_t value = position == 0 ? 0 : position - 1; value >= 16; value >>= 1) {
                shift--; mask >>= 1;
            }
            if (size - position >= 3) {
                const unsigned key = ((unsigned)input[position] * 251u
                    ^ (unsigned)input[position + 1] * 31u ^ input[position + 2]) & (LZNT_CHUNK - 1);
                previous = history[key]; history[key] = (uint16_t)position;
                if (previous < position) {
                    const size_t limit = size - position < mask + 3 ? size - position : mask + 3;
                    while (match < limit && input[previous + match] == input[position + match]) match++;
                }
            }
            if (match >= 3) {
                if (LZNT_CHUNK - stored < 2) return size;
                const unsigned token = (unsigned)(position - previous - 1) << shift | (unsigned)(match - 3);
                packed[flags] |= (uint8_t)(1u << bit);
                packed[stored++] = (uint8_t)token; packed[stored++] = (uint8_t)(token >> 8);
                position += match;
            } else {
                if (stored == LZNT_CHUNK) return size;
                packed[stored++] = input[position++];
            }
        }
    }
    return stored;
}

int EncodeFfuLznt1(const uint8_t* input, size_t size, uint8_t* output, size_t capacity, size_t* written)
{
    if (written == NULL) return EINVAL;
    *written = 0;
    if (input == NULL || output == NULL || size == 0) return EINVAL;
    uint8_t packed[LZNT_CHUNK];
    for (size_t position = 0; position < size;) {
        if (IsRufusJobCancelled()) return ECANCELED;
        const size_t bytes = size - position < LZNT_CHUNK ? size - position : LZNT_CHUNK;
        const size_t encoded = EncodeLzntChunk(input + position, bytes, packed);
        const bool compressed = encoded < bytes;
        const size_t stored = compressed ? encoded : bytes;
        if (*written > capacity || capacity - *written < stored + 2) return ENOBUFS;
        const unsigned header = (compressed ? 0xb000 : 0x3000) | (unsigned)(stored - 1);
        output[(*written)++] = (uint8_t)header; output[(*written)++] = (uint8_t)(header >> 8);
        memcpy(output + *written, compressed ? packed : input + position, stored);
        *written += stored; position += bytes;
    }
    return 0;
}

/* Copy from validated history. Each nonoverlapping memcpy grows the available
 * repeated prefix, preserving forward LZ semantics without per-byte overhead. */
static void CopyMatch(uint8_t* output, size_t* position, size_t distance, size_t length)
{
    const size_t source = *position - distance, end = *position + length;
    while (*position < end) {
        const size_t available = *position - source, remaining = end - *position;
        const size_t count = available < remaining ? available : remaining;
        memcpy(output + *position, output + source, count);
        *position += count;
    }
}

int DecodeFfuXpress(const uint8_t* input, size_t size, uint8_t* output, size_t capacity)
{
	if (input == NULL || output == NULL || capacity == 0) return EINVAL;
	size_t position = 0, produced = 0, half_byte = SIZE_MAX;
	uint32_t flags = 0;
	unsigned bits = 0;
	while (produced < capacity) {
		if (bits == 0) {
			if (IsIsoScanCancelled()) return ECANCELED;
			if (size - position < 4)
				return EBADMSG;
			flags = Codec32(input + position);
			position += 4;
			bits = 32;
		}
		if ((flags & (UINT32_C(1) << --bits)) == 0) {
			if (position == size)
				return EBADMSG;
			output[produced++] = input[position++];
			continue;
		}
		if (size - position < 2)
			return EBADMSG;
		const unsigned match = Word(input + position);
		position += 2;
		const size_t distance = (match >> 3) + 1;
		size_t length = match & 7;
		if (length == 7) {
			if (half_byte == SIZE_MAX) {
				if (position == size)
					return EBADMSG;
				half_byte = position++;
				length = input[half_byte] & 15;
			} else {
				length = input[half_byte] >> 4;
				half_byte = SIZE_MAX;
			}
			if (length == 15) {
				if (position == size)
					return EBADMSG;
				length = input[position++];
				if (length == 255) {
					if (size - position < 2)
						return EBADMSG;
					length = Word(input + position);
					position += 2;
					if (length == 0) {
						if (size - position < 4)
							return EBADMSG;
						length = Codec32(input + position);
						position += 4;
					}
					if (length < 22)
						return EBADMSG;
					length -= 22;
				}
				length += 15;
			}
			length += 7;
		}
		length += 3;
		if (distance > produced || length > capacity - produced)
			return EBADMSG;
		CopyMatch(output, &produced, distance, length);
	}
	if (position == size) return 0;
	/* MS-XCA emits another all-match flag word after exactly 32 tokens. */
	return bits == 0 && size - position == 4 && Codec32(input + position) == UINT32_MAX ? 0 : EBADMSG;
}


int DecodeFfuLznt1(const uint8_t* input, size_t size, uint8_t* output, size_t capacity)
{
    size_t position = 0, produced = 0;
    while (position < size && produced < capacity) {
        if (IsIsoScanCancelled()) return ECANCELED;
        if (size - position < 2) return EBADMSG;
        const unsigned header = Word(input + position); position += 2;
        if ((header & 0x7000) != 0x3000) return EBADMSG;
        const size_t stored = (header & 4095) + 1;
        if (stored > size - position) return EBADMSG;
        const size_t end = position + stored, start = produced;
        const size_t room = capacity - produced < LZNT_CHUNK ? capacity - produced : LZNT_CHUNK;
        if (!(header & 0x8000)) {
            if (stored > room) return EBADMSG;
            memcpy(output + produced, input + position, stored); produced += stored; position = end;
        } else {
            while (position < end) {
                const unsigned flags = input[position++];
                if (position == end) return EBADMSG;
                for (unsigned bit = 0; bit < 8 && position < end; bit++) {
                    const size_t within = produced - start;
                    if (within >= room) return EBADMSG;
                    if (!(flags & (1u << bit))) output[produced++] = input[position++];
                    else {
                        if (end - position < 2 || within == 0) return EBADMSG;
                        const unsigned token = Word(input + position); position += 2;
                        unsigned shift = 12, mask = 4095;
                        for (size_t value = within - 1; value >= 16; value >>= 1) { shift--; mask >>= 1; }
                        const size_t distance = (token >> shift) + 1, length = (token & mask) + 3;
                        if (distance > within || length > room - within) return EBADMSG;
                        CopyMatch(output, &produced, distance, length);
                    }
                }
            }
        }
        // Each non-final chunk represents a complete 4 KiB unit.
        if (produced - start != room) return EBADMSG;
    }
    // RtlCompressBuffer normally excludes the optional end marker from its size.
    if (produced == capacity && size - position == 2 && Word(input + position) == 0) position += 2;
    return produced == capacity && position == size ? 0 : EBADMSG;
}

typedef struct {
    const uint8_t* input;
    size_t size, position;
    uint32_t bits;
    int extra;
} HUFF_STREAM;

static int DropBits(HUFF_STREAM* stream, unsigned count)
{
    stream->bits <<= count; stream->extra -= (int)count;
    if (stream->extra < 0) {
        if (stream->size - stream->position < 2) return EBADMSG;
        stream->bits |= (uint32_t)Word(stream->input + stream->position) << -stream->extra;
        stream->position += 2; stream->extra += 16;
    }
    return 0;
}

static int HuffmanTable(const uint8_t lengths[256], uint16_t table[HUFF_TABLE])
{
    unsigned counts[16] = {0}, codes[16] = {0}, code = 0;
    for (unsigned i = 0; i < HUFF_SYMBOLS; i++) counts[(lengths[i / 2] >> (4 * (i % 2))) & 15]++;
    if (counts[0] == HUFF_SYMBOLS) return EBADMSG;
    counts[0] = 0;
    for (unsigned bits = 1; bits <= HUFF_BITS; bits++) {
        code = (code + counts[bits - 1]) << 1;
        if (code + counts[bits] > 1u << bits) return EBADMSG;
        codes[bits] = code;
    }
    memset(table, 0xff, HUFF_TABLE * sizeof(*table));
    for (unsigned symbol = 0; symbol < HUFF_SYMBOLS; symbol++) {
        const unsigned bits = (lengths[symbol / 2] >> (4 * (symbol % 2))) & 15;
        if (bits == 0) continue;
        const unsigned begin = codes[bits]++ << (HUFF_BITS - bits), count = 1u << (HUFF_BITS - bits);
        for (unsigned i = 0; i < count; i++) table[begin + i] = (uint16_t)(symbol << 4 | bits);
    }
    return 0;
}

static int HuffmanSymbol(HUFF_STREAM* stream, const uint16_t table[HUFF_TABLE], unsigned* symbol)
{
    const uint16_t value = table[stream->bits >> (32 - HUFF_BITS)];
    if (value == UINT16_MAX) return EBADMSG;
    *symbol = value >> 4;
    return DropBits(stream, value & 15);
}

int DecodeFfuHuffman(const uint8_t* input, size_t size, uint8_t* output, size_t capacity)
{
    HUFF_STREAM stream = {input, size, 0, 0, 16};
    uint16_t table[HUFF_TABLE];
    size_t produced = 0, next_cancel = 0;
    while (produced < capacity) {
        if (IsIsoScanCancelled()) return ECANCELED;
        if (size - stream.position < 260) return EBADMSG;
        int error = HuffmanTable(input + stream.position, table);
        if (error != 0) return error;
        stream.position += 256;
        stream.bits = (uint32_t)Word(input + stream.position) << 16 | Word(input + stream.position + 2);
        stream.position += 4; stream.extra = 16;
        const size_t end = capacity - produced < HUFF_CHUNK ? capacity : produced + HUFF_CHUNK;
        while (produced < end) {
            if (produced >= next_cancel) {
                if (IsIsoScanCancelled()) return ECANCELED;
                next_cancel = produced + 4096;
            }
            unsigned symbol;
            error = HuffmanSymbol(&stream, table, &symbol);
            if (error != 0) return error;
            if (symbol < 256) { output[produced++] = (uint8_t)symbol; continue; }
            const unsigned offset_bits = ((symbol - 256) >> 4) & 15;
            const size_t distance = (size_t)1 << offset_bits | (offset_bits ? stream.bits >> (32 - offset_bits) : 0);
            size_t length = symbol & 15;
            if (length == 15) {
                if (stream.position == size) return EBADMSG;
                length += input[stream.position++];
                if (length == 270) {
                    if (size - stream.position < 2) return EBADMSG;
                    length = Word(input + stream.position); stream.position += 2;
                }
            }
            error = DropBits(&stream, offset_bits);
            length += 3;
            if (error != 0 || distance > produced || length > capacity - produced) return EBADMSG;
            CopyMatch(output, &produced, distance, length);
        }
    }
    if (stream.position == size) return 0;
    // Some encoders append the optional symbol 256 after the last output byte.
    unsigned end;
    const int error = HuffmanSymbol(&stream, table, &end);
    return error == 0 && end == 256 && stream.position == size ? 0 : EBADMSG;
}
