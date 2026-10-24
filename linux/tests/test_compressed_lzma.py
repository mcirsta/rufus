#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""LZMA-Alone differential corpus, size/history guards and retained-source faults."""
import errno
import hashlib
import lzma
import pathlib
import random
import struct
import subprocess
import sys
import tempfile

from test_compressed_source import probe, reject


def encode(data, dictionary=4096, **properties):
    return lzma.compress(data, format=lzma.FORMAT_ALONE,
                         filters=[{'id': lzma.FILTER_LZMA1, 'dict_size': dictionary, **properties}])


def declared(packed, size):
    return packed[:5] + struct.pack('<Q', size) + packed[13:]


def legacy_result(program, path):
    run = subprocess.run([program, str(path), '--one-byte-input'], text=True, capture_output=True,
                         check=True, timeout=30)
    return dict(line.split(' ', 1) for line in run.stdout.splitlines())


def check(program, legacy, path, packed, unpacked, trailing=None, independent=True):
    if independent:
        oracle = packed
        declared_size = struct.unpack_from('<Q', packed, 5)[0]
        if declared_size != 2**64 - 1:
            assert declared_size == len(unpacked), (declared_size, len(unpacked))
            # liblzma 5.2 rejects an EOS marker with a declared length; 5.4
            # permits it. Verify the unchanged compressed payload using its
            # original unknown-size header. Both native/Windows decoders below
            # still receive the exact known-size fixture, including its tail.
            oracle = declared(packed, 2**64 - 1)
        decoder = lzma.LZMADecompressor(format=lzma.FORMAT_ALONE)
        assert decoder.decompress(oracle) == unpacked and decoder.eof
    path.write_bytes(packed)
    result = probe(program, path, max(1, len(unpacked)))
    assert int(result['error']) == 0, result
    assert int(result['type']) == 4 and int(result['source']) == len(packed)
    assert int(result['decoded']) == len(unpacked) and int(result['members']) == 1
    if trailing is not None:
        assert int(result['trailing']) == trailing, result
    else:
        assert 0 <= int(result['trailing']) <= len(packed) - 18, result
    assert result['prefix'] == unpacked[:512].hex()
    digest = hashlib.sha256(unpacked).hexdigest()
    assert result['sha256'] == digest
    marker = len(unpacked) >= 512 and unpacked[510:512] == b'\x55\xaa'
    assert int(result['marker']) == marker
    assert int(result['forced']) == (1 if marker else 2 if len(unpacked) >= 512 else 0)
    original = legacy_result(legacy, path)
    assert int(original['result']) == len(unpacked) and int(original['decoded']) == len(unpacked), original
    assert original['sha256'] == digest
    assert int(result['trailing']) == len(packed) - int(original['consumed']), (result, original)
    return result


def main():
    program, legacy, contract = [str(pathlib.Path(p).resolve()) for p in sys.argv[1:]]
    cases = 0
    with tempfile.TemporaryDirectory(prefix='rufus-lzma-vectors-') as directory:
        path = pathlib.Path(directory) / 'image ü.LZMA'
        raw = bytearray(random.Random(27).randbytes(262181))
        raw[510:512] = b'\x55\xaa'
        for dictionary in (4096, 65536, 1024 * 1024):
            for size in (0, 1, 511, 512, 513, 4095, 4096, 4097, 65535, 65536, 65537, len(raw)):
                data = raw[:size]
                packed = encode(data, dictionary)
                check(program, legacy, path, packed, data)
                check(program, legacy, path, declared(packed, size), data, trailing=None)
                cases += 2
        for data in (b'a' * 1000001, bytes(range(256)) * 3907, bytes(raw) * 5):
            check(program, legacy, path, encode(data), data)
            cases += 1
        # All 225 original lc/lp/pb combinations, including lc+lp > liblzma's
        # encoder range. At the first literal all contexts have initial models;
        # keep pb's independent encoding and vary lc/lp without re-encoding it.
        for pb in range(5):
            one = encode(b'a', lc=0, lp=0, pb=pb)
            for lp in range(5):
                for lc in range(9):
                    packed = bytes([(pb * 5 + lp) * 9 + lc]) + one[1:]
                    check(program, legacy, path, packed, b'a', independent=lc + lp <= 4)
                    cases += 1
        for lc, lp, pb in ((0, 0, 0), (0, 4, 4), (4, 0, 4), (1, 3, 1), (2, 2, 2)):
            packed = encode(raw, lc=lc, lp=lp, pb=pb)
            check(program, legacy, path, packed, raw)
            cases += 1
        # Zero means a one-byte dictionary in the original; arbitrary original
        # sizes and the full 1-GiB cap remain accepted, not XZ's 64-MiB cap.
        single = encode(b'a')
        for dictionary in (0, 1, 2, 4095, 4096, 65537, 2**26 + 1, 2**30):
            packed = single[:1] + struct.pack('<I', dictionary) + single[5:]
            packed = declared(packed, 1)
            check(program, legacy, path, packed, b'a', trailing=None, independent=False)
            cases += 1
        packed = encode(raw)
        base_tail = int(check(program, legacy, path, packed, raw)['trailing'])
        for tail in (b'x', b'\0' * 8193, b'junk' + packed, packed, b'\x5d\0\0'):
            check(program, legacy, path, packed + tail, raw, trailing=base_tail + len(tail))
            cases += 1
        cases += 1
        # Known-size termination deliberately leaves EOS bytes unread, as on
        # Windows. Removing only the reported tail must preserve exact output.
        known = declared(packed, len(raw))
        result = check(program, legacy, path, known, raw, trailing=None)
        tail = int(result['trailing'])
        assert tail > 0
        check(program, legacy, path, known[:-tail], raw, trailing=0, independent=False)
        cases += 2
        consumed = len(packed) - base_tail
        check(program, legacy, path, packed[:consumed], raw, trailing=0, independent=False)
        cases += 1
        for cut in (*range(18), len(packed) // 2, consumed - 5, consumed - 1):
            reject(program, path, packed[:cut], errno.EBADMSG)
            cases += 1
        for prop in (225, 226, 255):
            reject(program, path, bytes([prop]) + packed[1:], errno.EBADMSG)
            cases += 1
        reject(program, path, packed[:1] + struct.pack('<I', 2**30 + 1) + packed[5:], errno.ENOTSUP)
        cases += 1
        # Prove the source FIXME: after a dictionary wrap the original can
        # overshoot a mutated known size; it also accepts premature EOS.
        for data, size in ((bytes(20000), 4097), (bytes(range(256)) * 80, 8192)):
            malformed = declared(encode(data), size)
            path.write_bytes(malformed)
            assert int(legacy_result(legacy, path)['decoded']) > size
            reject(program, path, malformed, errno.EBADMSG)
            cases += 1
        for size in (20001, 2**32 + 512):
            malformed = declared(encode(bytes(20000)), size)
            path.write_bytes(malformed)
            assert int(legacy_result(legacy, path)['decoded']) == 20000
            reject(program, path, malformed, errno.EBADMSG)
            cases += 1
        # Range decisions match/rep/rep0/short-rep before the first output.
        # The original copies unwritten history; native rejects that distance.
        invalid = struct.pack('<BIQ', 93, 4096, 1) + b'\0\xc0\0\0\0' + bytes(32)
        path.write_bytes(invalid)
        assert int(legacy_result(legacy, path)['decoded']) == 1
        reject(program, path, invalid, errno.EBADMSG)
        cases += 1
        for maximum in (1, 511, 512, 65536, len(raw) - 1):
            reject(program, path, packed, errno.EFBIG, maximum)
            cases += 1
        for maximum in (0, 2**63):
            reject(program, path, packed, errno.EINVAL, maximum)
            cases += 1
        # Count a retained sparse source and ignored tail above 4 GiB. This is
        # distinct from the separate actual >4-GiB decoded-output qualification.
        path.write_bytes(packed)
        with path.open('r+b') as file:
            file.truncate(2**32 + 512)
        result = probe(program, path, len(raw))
        assert int(result['error']) == 0 and int(result['source']) == 2**32 + 512
        assert int(result['decoded']) == len(raw) and int(result['trailing']) == 2**32 + 512 - consumed
        assert result['sha256'] == hashlib.sha256(raw).hexdigest()
        cases += 1
        fault_raw = bytearray()
        random_state = 27
        for _ in range(3 * 65536 + 37):
            random_state ^= (random_state << 13) & 0xffffffff
            random_state ^= random_state >> 17
            random_state ^= (random_state << 5) & 0xffffffff
            fault_raw.append(random_state & 255)
        fault_raw[510:512] = b'\x55\xaa'
        path.write_bytes(encode(fault_raw))
        print(subprocess.run([contract, str(path)], text=True, capture_output=True,
                             check=True, timeout=30).stdout, end='')
    print('LZMA: original known-size overshoot/early EOS/unwritten history reproduced; native refuses')
    print(f'LZMA source: {cases} corpus/differential/property/64-bit-source cases passed')


if __name__ == '__main__':
    main()
