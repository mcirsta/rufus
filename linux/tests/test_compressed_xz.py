#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Independent XZ corpus, original-check differential and retained-source faults."""
import errno
import hashlib
import lzma
import os
import pathlib
import random
import struct
import subprocess
import sys
import tempfile
import zlib

from test_compressed_source import probe, reject


def vli(value):
    encoded = bytearray()
    while value >= 128:
        encoded.append((value & 127) | 128)
        value >>= 7
    encoded.append(value)
    return encoded


def read_vli(data, position):
    value, shift = 0, 0
    while True:
        byte = data[position]
        position += 1
        value |= (byte & 127) << shift
        if byte < 128:
            return value, position
        shift += 7


def block_checks(packed, check_size):
    index_size = (struct.unpack_from('<I', packed, len(packed) - 8)[0] + 1) * 4
    index = len(packed) - 12 - index_size
    assert packed[index] == 0
    count, position = read_vli(packed, index + 1)
    start, positions = 12, []
    for _ in range(count):
        unpadded, position = read_vli(packed, position)
        _, position = read_vli(packed, position)
        padded = (unpadded + 3) & ~3
        positions.append(start + padded - check_size)
        start += padded
    assert start == index
    return positions


def check(program, path, packed, unpacked, trailing=0, independent=True):
    if independent:
        decoder = lzma.LZMADecompressor()
        assert decoder.decompress(packed) == unpacked
        assert len(decoder.unused_data) == trailing and decoder.eof
    path.write_bytes(packed)
    result = probe(program, path, max(1, len(unpacked)))
    assert int(result['error']) == 0, result
    assert int(result['type']) == 6 and int(result['source']) == len(packed)
    assert int(result['decoded']) == len(unpacked) and int(result['members']) == 1
    assert int(result['trailing']) == trailing, result
    assert result['prefix'] == unpacked[:512].hex()
    assert result['sha256'] == hashlib.sha256(unpacked).hexdigest()
    marker = len(unpacked) >= 512 and unpacked[510:512] == b'\x55\xaa'
    assert int(result['marker']) == marker


def with_dictionary(packed, property_value):
    packed = bytearray(packed)
    header_size = (packed[12] + 1) * 4
    # Single LZMA2 filter, no optional size fields in these independent fixtures.
    assert packed[13:16] == b'\x00\x21\x01'
    packed[16] = property_value
    crc_offset = 12 + header_size - 4
    struct.pack_into('<I', packed, crc_offset, zlib.crc32(packed[12:crc_offset]))
    return packed


def large_stream_file(path, chunks):
    """One valid LZMA2 block with sparse, uncompressed 64-KiB zero chunks.

    Exercise real source/output offsets beyond 4 GiB without the instrumented
    range decoder's billions of per-byte operations or an expanded image file.
    """
    chunk_bytes = 65536
    flags = b'\0\0'
    header = b'\xfd7zXZ\0' + flags + struct.pack('<I', zlib.crc32(flags))
    block = b'\x02\x00\x21\x01\x08\0\0\0'
    block += struct.pack('<I', zlib.crc32(block))
    with path.open('wb') as file:
        file.write(header + block)
        # Control 1 resets the dictionary; two-byte size is big-endian minus 1.
        for _ in range(chunks):
            file.write(b'\x01\xff\xff')
            file.seek(chunk_bytes, os.SEEK_CUR)
        file.write(b'\x01\x01\xff')
        file.seek(512, os.SEEK_CUR)
        file.write(b'\0')
        unpadded = file.tell() - len(header)
        file.write(bytes(-file.tell() % 4))
        index = b'\0\x01' + vli(unpadded) + vli(512 + chunks * chunk_bytes)
        index += bytes(-len(index) % 4)
        index += struct.pack('<I', zlib.crc32(index))
        footer = struct.pack('<I', len(index) // 4 - 1) + flags
        footer = struct.pack('<I', zlib.crc32(footer)) + footer + b'YZ'
        file.write(index + footer)
    return path.stat()


def main():
    program, legacy, contract = [str(pathlib.Path(p).resolve()) for p in sys.argv[1:]]
    cases = 0
    with tempfile.TemporaryDirectory(prefix='rufus-xz-vectors-') as directory:
        path = pathlib.Path(directory) / 'image ü.XZ'
        raw = bytearray(random.Random(25).randbytes(262181))
        raw[510:512] = b'\x55\xaa'
        filters = [{'id': lzma.FILTER_LZMA2, 'dict_size': 65536}]
        for size in (0, 1, 511, 512, 513, 65535, 65536, 65537, len(raw)):
            for kind in (lzma.CHECK_NONE, lzma.CHECK_CRC32, lzma.CHECK_CRC64, lzma.CHECK_SHA256):
                data = raw[:size]
                packed = lzma.compress(data, check=kind, filters=filters)
                check(program, path, packed, data)
                check(legacy, path, packed, data)
                cases += 2
        for kind in (1, 4, 10):
            # An explicit zero-output block exercises check initialization and
            # finalization even though there are no decoded data callbacks.
            flags = bytes((0, kind))
            stream_header = b'\xfd7zXZ\0' + flags + struct.pack('<I', zlib.crc32(flags))
            header = b'\x02\x00\x21\x01\x08\0\0\0'
            header += struct.pack('<I', zlib.crc32(header))
            checksum = {1: bytes(4), 4: bytes(8), 10: hashlib.sha256(b'').digest()}[kind]
            block = header + b'\0\0\0\0' + checksum
            index = b'\0\x01' + vli(len(header) + 1 + len(checksum)) + b'\0'
            index += bytes(-len(index) % 4)
            index += struct.pack('<I', zlib.crc32(index))
            footer = struct.pack('<I', len(index) // 4 - 1) + flags
            footer = struct.pack('<I', zlib.crc32(footer)) + footer + b'YZ'
            check(program, path, stream_header + block + index + footer, b'')
            cases += 1
        for kind, size in ((1, 4), (4, 8), (10, 32)):
            packed = lzma.compress(raw, check=kind, filters=filters)
            for offset in block_checks(packed, size):
                for byte in range(size):
                    damaged = bytearray(packed)
                    damaged[offset + byte] ^= 1
                    reject(program, path, damaged, errno.EBADMSG)
                    if kind != 1:
                        # Prove the concrete Windows/native difference using
                        # the same original core with native hooks compiled out.
                        check(legacy, path, damaged, raw, independent=False)
                    cases += 1
        packed = lzma.compress(raw, check=4, filters=filters)
        for cut in (*range(32), len(packed)//2, len(packed)-20, len(packed)-12, len(packed)-1):
            reject(program, path, packed[:cut], errno.EBADMSG)
            cases += 1
        for offset in (0, 6, 8, 13, 20, len(packed)-15, len(packed)-10, len(packed)-1):
            damaged = bytearray(packed)
            damaged[offset] ^= 0x80
            reject(program, path, damaged, errno.EBADMSG)
            cases += 1
        for tail in (b'\0'*4, b'\0', b'junk', b'\xfd7zXZ\0', packed):
            check(program, path, packed + tail, raw, len(tail))
            check(legacy, path, packed + tail, raw, len(tail))
            cases += 2
        for maximum in (1, 511, 512, 65536, len(raw)-1):
            reject(program, path, packed, errno.EFBIG, maximum)
            cases += 1
        for maximum in (0, 2**63):
            reject(program, path, packed, errno.EINVAL, maximum)
            cases += 1
        # Original dynamic dictionary cap is 64 MiB; test it independently of
        # output size, including valid boundary and over-limit declarations.
        check(program, path, with_dictionary(packed, 28), raw)
        reject(program, path, with_dictionary(packed, 29), errno.EFBIG)
        reject(program, path, with_dictionary(packed, 40), errno.ENOTSUP)
        cases += 3
        reserved = bytearray(lzma.compress(raw, check=1, filters=filters))
        reserved[7] = reserved[-3] = 2
        struct.pack_into('<I', reserved, 8, zlib.crc32(reserved[6:8]))
        struct.pack_into('<I', reserved, len(reserved)-12, zlib.crc32(reserved[-8:-2]))
        reject(program, path, reserved, errno.ENOTSUP)
        check(legacy, path, reserved, raw, independent=False)
        cases += 2
        for kind in ('crc32', 'crc64', 'sha256'):
            packed = subprocess.run(['xz', '--stdout', '--check='+kind, '--block-size=32KiB',
                                     '--lzma2=dict=64KiB'], input=raw, capture_output=True, check=True).stdout
            check(program, path, packed, raw)
            size = {'crc32': 4, 'crc64': 8, 'sha256': 32}[kind]
            offsets = block_checks(packed, size)
            assert len(offsets) > 1
            for offset in offsets:
                damaged = bytearray(packed)
                damaged[offset] ^= 1
                reject(program, path, damaged, errno.EBADMSG)
                cases += 1
            cases += 1
        # On x86 these are the original host's enabled BCJ filters.
        import platform
        if platform.machine().lower() in ('x86_64', 'amd64', 'i386', 'i686'):
            bcj = [{'id': lzma.FILTER_X86}, *filters]
            for kind in (1, 4, 10):
                packed = lzma.compress(raw, check=kind, filters=bcj)
                check(program, path, packed, raw)
                cases += 1
        fault_raw = bytearray((i * 37 + i // 251) & 255 for i in range(3 * 65536 + 37))
        fault_raw[510:512] = b'\x55\xaa'
        for kind in (1, 4, 10):
            path.write_bytes(lzma.compress(fault_raw, check=kind, filters=filters))
            tested = subprocess.run([contract, str(path)], text=True, capture_output=True, check=True, timeout=30)
            print(tested.stdout, end='', flush=True)
            assert '0 failures' in tested.stdout
        # Validate the hand-assembled format with an independent decoder first.
        large_stream_file(path, 2)
        assert lzma.decompress(path.read_bytes()) == bytes(512 + 2 * 65536)
        stat = large_stream_file(path, 65536)
        assert stat.st_size > 2**32 and stat.st_blocks * 512 < 512 * 1024**2
        print(f'XZ sparse large fixture: {stat.st_size} logical bytes, {stat.st_blocks * 512} allocated bytes', flush=True)
        print(f'XZ: {cases} corpus/differential cases passed; counting actual >4-GiB output', flush=True)
        result = probe(program, path, 2**32 + 512, 'count', timeout=90)
        assert int(result['error']) == 0 and int(result['decoded']) == 2**32+512, result
        assert result['prefix'] == bytes(512).hex() and int(result['members']) == 1
        result = probe(program, path, 2**32, 'count', timeout=90)
        assert int(result['error']) == errno.EFBIG and int(result['decoded']) == 0, result
        cases += 2
    print(f'XZ: {cases} corpus/differential/large-stream cases passed; temporary fixtures removed')


if __name__ == '__main__':
    main()
