#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Independent BZIP2 streams, original-wrapper differential and source faults."""
import bz2
import errno
import hashlib
import pathlib
import random
import subprocess
import sys
import tempfile

from test_compressed_source import probe, reject


def legacy_result(program, path):
    run = subprocess.run([program, str(path)], text=True, capture_output=True,
                         check=True, timeout=30)
    return dict(line.split(' ', 1) for line in run.stdout.splitlines())


def check(program, legacy, path, packed, unpacked, members=1, trailing=0):
    remaining, decoded, count = packed, b'', 0
    while remaining.startswith(b'BZ'):
        independent = bz2.BZ2Decompressor()
        decoded += independent.decompress(remaining)
        assert independent.eof
        remaining = independent.unused_data
        count += 1
    assert decoded == unpacked and count == members and len(remaining) == trailing
    path.write_bytes(packed)
    result = probe(program, path, max(1, len(unpacked)))
    assert int(result['error']) == 0, result
    assert int(result['type']) == 5 and int(result['source']) == len(packed)
    assert int(result['decoded']) == len(unpacked) and int(result['members']) == members
    assert int(result['trailing']) == trailing, result
    assert result['prefix'] == unpacked[:512].hex()
    digest = hashlib.sha256(unpacked).hexdigest()
    assert result['sha256'] == digest
    marker = len(unpacked) >= 512 and unpacked[510:512] == b'\x55\xaa'
    assert int(result['marker']) == marker
    assert int(result['forced']) == (1 if marker else 2 if len(unpacked) >= 512 else 0)
    original = legacy_result(legacy, path)
    assert int(original['result']) == len(unpacked) and int(original['decoded']) == len(unpacked), original
    assert original['sha256'] == digest


def markers(packed, signature):
    bits = ''.join(f'{byte:08b}' for byte in packed)
    magic = f'{signature:048b}'
    positions, start = [], 32
    while True:
        position = bits.find(magic, start)
        if position < 0:
            return positions
        positions.append(position)
        start = position + 48


def flip(packed, bit):
    damaged = bytearray(packed)
    damaged[bit // 8] ^= 1 << (7 - bit % 8)
    return damaged


def main():
    program, legacy, contract = [str(pathlib.Path(p).resolve()) for p in sys.argv[1:]]
    cases = 0
    with tempfile.TemporaryDirectory(prefix='rufus-bzip2-vectors-') as directory:
        path = pathlib.Path(directory) / 'image ü.BZ2'
        raw = bytearray(random.Random(26).randbytes(262181))
        raw[510:512] = b'\x55\xaa'
        for level in range(1, 10):
            for size in (0, 1, 511, 512, 513, 65535, 65536, 65537, len(raw)):
                data = raw[:size]
                check(program, legacy, path, bz2.compress(data, compresslevel=level), data)
                cases += 1
        # Each block size and RLE-heavy versus incompressible BWT output.
        for level in (1, 9):
            for data in (b'a' * 1000001, bytes(range(256)) * 3907, bytes(raw) * 5):
                check(program, legacy, path, bz2.compress(data, compresslevel=level), data)
                cases += 1
        for chunks in ([raw[:123], raw[123:]], [b'', raw, b''], [raw, raw],
                       [b''] * 17, [raw[:511], b'\x55', raw[512:]]):
            packed = b''.join(bz2.compress(c, compresslevel=(i % 9) + 1) for i, c in enumerate(chunks))
            check(program, legacy, path, packed, b''.join(chunks), len(chunks))
            cases += 1
        # Independent deterministic encoding puts 'B' at byte 65535 and 'Z'
        # after the input-buffer boundary, exercising retained one-byte input.
        first = bz2.compress(raw[:64890], compresslevel=9)
        assert len(first) == 65535
        check(program, legacy, path, first + bz2.compress(raw[64890:]), raw, 2)
        cases += 1
        packed = bz2.compress(raw, compresslevel=1)
        for tail in (b'B', b'x', b'\0' * 10, b'unrelated trailing bytes', b'junk' + packed):
            check(program, legacy, path, packed + tail, raw, trailing=len(tail))
            cases += 1
        # Flip all bytes of each real block and combined CRC, also when a valid
        # later member follows. Native callbacks are provisional on any failure.
        blocks = markers(packed, 0x314159265359)
        end = markers(packed, 0x177245385090)
        assert len(blocks) == 3 and len(end) == 1
        for position in blocks + end:
            for byte in range(4):
                damaged = flip(packed, position + 48 + 8 * byte)
                for suffix in (b'', packed):
                    reject(program, path, damaged + suffix, errno.EBADMSG)
                    assert int(legacy_result(legacy, path)['result']) < 0
                    cases += 1
        # The original wrapper returns success on a corrupt empty combined CRC.
        # Establish the native divergence for empty, first, middle and last cases.
        empty = bz2.compress(b'')
        damaged_empty = flip(empty, 32 + 48)
        for prefix, suffix in ((b'', b''), (b'', packed), (packed, b''), (packed, packed)):
            reject(program, path, prefix + damaged_empty + suffix, errno.EBADMSG)
            old = legacy_result(legacy, path)
            expected = len(raw) if prefix else 0
            assert int(old['result']) == int(old['decoded']) == expected, old
            cases += 1
        print('BZIP2 differential: original accepts damaged empty combined CRC; native refuses all four placements', flush=True)
        # Header, block signature, randomization flag and original pointer.
        for bit in (0, 8, 16, 24, 32, 80):
            reject(program, path, flip(packed, bit), errno.EBADMSG)
            cases += 1
        reject(program, path, flip(packed, 112), errno.ENOTSUP)
        cases += 1
        bad_pointer = bytearray(packed)
        for bit in range(113, 137):
            bad_pointer[bit // 8] |= 1 << (7 - bit % 8)
        reject(program, path, bad_pointer, errno.EBADMSG)
        cases += 1
        for cut in (*range(0, 20), len(packed) // 2, len(packed) - 10, len(packed) - 1):
            reject(program, path, packed[:cut], errno.EBADMSG)
            cases += 1
        for suffix in (b'BZ', b'BZh', b'BZh0', b'BZhA', b'BZh9'):
            reject(program, path, packed + suffix, errno.EBADMSG)
            cases += 1
        for level in (b'0', b':', b'\0', b'\xff'):
            reject(program, path, packed[:3] + level + packed[4:], errno.EBADMSG)
            cases += 1
        for maximum in (1, 511, 512, 65536, len(raw) - 1):
            reject(program, path, packed, errno.EFBIG, maximum)
            cases += 1
        # A sparse >4-GiB stored file proves source/tail counts are not truncated;
        # no claim is made that the ignored tail is itself a BZIP2 member.
        path.write_bytes(packed)
        with path.open('r+b') as file:
            file.truncate(2**32 + 513)
        result = probe(program, path, len(raw))
        assert int(result['error']) == 0 and int(result['decoded']) == len(raw)
        assert int(result['source']) == 2**32 + 513
        assert int(result['trailing']) == 2**32 + 513 - len(packed)
        cases += 1
        # Existing offset/ownership, pread/consumer/mutation/cancel/allocation
        # and concurrent-reader contracts now exercise this provider too.
        fixture = pathlib.Path(directory) / 'contract.bz2'
        data = bytearray((i * 37 + i // 251) & 255 for i in range(3 * 65536 + 37))
        data[510:512] = b'\x55\xaa'
        fixture.write_bytes(bz2.compress(data, compresslevel=9))
        subprocess.run([contract, str(fixture)], check=True, timeout=30)
        fixture.write_bytes(bz2.compress(data[:65536], compresslevel=1)
                            + bz2.compress(data[65536:], compresslevel=9))
        subprocess.run([contract, str(fixture), '2'], check=True, timeout=30)
    print(f'BZIP2 source: {cases} corpus/differential/64-bit-source cases passed')


if __name__ == '__main__':
    main()
