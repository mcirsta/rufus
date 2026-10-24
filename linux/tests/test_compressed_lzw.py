#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Unix-compress differential corpus and retained-source contracts."""
import errno
import hashlib
import pathlib
import random
import subprocess
import sys
import tempfile

from lzw_reference import CodeWriter, codes, encode, encode_clear_tail
from test_compressed_source import probe, reject


def original(program, path, *options):
    run = subprocess.run([program, str(path), *options], capture_output=True,
                         text=True, timeout=30)
    if run.returncode != 0:
        assert run.returncode == 2, run
        return None
    if run.stderr.strip():
        return None  # Original can report corruption yet return a previous positive write count.
    result = dict(line.split(' ', 1) for line in run.stdout.splitlines())
    return result if int(result['result']) >= 0 else None


def check(program, legacy, path, packed, data, nine_bit=False):
    if not nine_bit:
        independent = subprocess.run(['gzip', '-dc'], input=packed, capture_output=True,
                                     check=True, timeout=30)
        assert independent.stdout == data
    path.write_bytes(packed)
    result = probe(program, path, max(1, len(data)))
    assert int(result['error']) == 0, result
    association = 3 if path.suffix.lower() == '.gz' else 2
    assert int(result['type']) == association and int(result['source']) == len(packed)
    assert int(result['decoded']) == len(data) and int(result['members']) == 1
    assert int(result['trailing']) == 0 and result['prefix'] == data[:512].hex()
    digest = hashlib.sha256(data).hexdigest()
    assert result['sha256'] == digest
    marker = len(data) >= 512 and data[510:512] == b'\x55\xaa'
    assert int(result['marker']) == marker
    assert int(result['forced']) == (1 if marker else 2 if len(data) >= 512 else 0)
    for options in ((), ('--windows-buffer',), ('--gzip-dispatch',)):
        old = original(legacy, path, *options)
        if nine_bit:
            continue  # Original may reject or silently shorten saturated 9-bit data.
        assert old and int(old['result']) == len(data) and int(old['decoded']) == len(data), old
        assert old['sha256'] == digest and int(old['consumed']) == len(packed), old


def differential(program, legacy, path, packed):
    path.write_bytes(packed)
    old = original(legacy, path)
    windows = original(legacy, path, '--windows-buffer')
    if old is None:
        assert windows is None
        reject(program, path, packed, errno.EBADMSG)
    else:
        assert windows is not None and old['sha256'] == windows['sha256']
        assert old['decoded'] == windows['decoded']
        result = probe(program, path, 8 * 1024 * 1024)
        assert int(result['error']) == 0, (old, result)
        assert result['sha256'] == old['sha256']
        assert result['decoded'] == old['decoded'] and int(result['trailing']) == 0


def main():
    fallback = len(sys.argv) == 5 and sys.argv[4] == '--gzip-fallback'
    assert len(sys.argv) == (5 if fallback else 4)
    program, legacy, contract = [str(pathlib.Path(p).resolve()) for p in sys.argv[1:4]]
    cases = 0
    with tempfile.TemporaryDirectory(prefix='rufus-lzw-vectors-') as directory:
        path = pathlib.Path(directory) / ('image ü.GZ' if fallback else 'image ü.Z')
        raw = bytearray(random.Random(28).randbytes(262181))
        raw[510:512] = b'\x55\xaa'
        for block in (True, False):
            for bits in range(9, 17):
                sizes = (0, 1, 7, 8, 9, 255, 256, 257, 511, 512, 513,
                         65535, 65536, 65537, len(raw))
                for size in sizes:
                    data = bytes(raw[:size])
                    check(program, legacy, path, encode(data, bits, block), data,
                          nine_bit=bits == 9 and size >= 255)
                    cases += 1
                for data in (b'a' * 1000001, bytes(range(256)) * 3907):
                    check(program, legacy, path, encode(data, bits, block), data, nine_bit=bits == 9)
                    cases += 1
        # CLEAR at different code-group positions, after width growth, and
        # repeated resets. New segments begin with a literal, as the format requires.
        segments = [bytes(raw[:n]) for n in (1, 7, 8, 9, 255, 256, 257, 1025, 65537)]
        for bits in (9, 10, 12, 16):
            writer = CodeWriter(bits)
            for i, segment in enumerate(segments):
                if i:
                    writer.emit(256)
                for code in codes(segment, bits):
                    writer.emit(code)
            check(program, legacy, path, writer.finish(), b''.join(segments), nine_bit=bits == 9)
            cases += 1
        # Original flags 0x20/0x40 are ignored; there is no lower width guard.
        single = encode(b'abc')
        for flags in (*range(9), 0x30, 0x50, 0x70, 0xb0, 0xd0, 0xf0):
            packed = single[:2] + bytes((flags,)) + single[3:]
            differential(program, legacy, path, packed)
            cases += 1
        for flags in (17, 31, 0x91, 0xff):
            reject(program, path, single[:2] + bytes((flags,)) + single[3:], errno.EBADMSG)
            cases += 1
        # Original Rufus rejects some valid saturated 9-bit streams and silently
        # shortens others. Preserve its Windows preprocessing; correct native
        # resets like the fixed independent ncompress decoder.
        for block in (True, False):
            packed = encode(raw[:4096], 9, block)
            path.write_bytes(packed)
            assert original(legacy, path) is None
            check(program, legacy, path, packed, raw[:4096], nine_bit=True)
            cases += 1
        writer = CodeWriter(9)
        for _ in range(4096):
            writer.emit(0)
        packed = writer.finish()
        path.write_bytes(packed)
        old = original(legacy, path)
        assert int(old['decoded']) == 3712 and int(old['result']) == 3712
        check(program, legacy, path, packed, bytes(4096), nine_bit=True)
        cases += 1
        packed = encode(raw)
        for maximum in (1, 511, 512, 65536, len(raw) - 1):
            reject(program, path, packed, errno.EFBIG, maximum)
            cases += 1
        for maximum in (0, 2**63):
            reject(program, path, packed, errno.EINVAL, maximum)
            cases += 1
        check(program, legacy, path.with_suffix('.gz'), packed, raw)
        cases += 1
        # EOF terminates a stream; incomplete code bits, truncation and appended
        # bytes do not have a generic checksum/end-marker failure to detect.
        for cut in (*range(12), 65535, 65536, 65537, len(packed) - 2, len(packed) - 1):
            differential(program, legacy, path, packed[:cut])
            cases += 1
        for tail in (b'\0', b'\xff', b'\0' * 7, b'tail', encode(b'next'), b'\xff' * 19):
            differential(program, legacy, path, packed + tail)
            cases += 1
        corrupt = random.Random(280)
        for _ in range(48):
            changed = bytearray(packed)
            offset = corrupt.randrange(3, len(changed))
            changed[offset] ^= 1 << corrupt.randrange(8)
            differential(program, legacy, path, changed)
            cases += 1
        for bad in (b'', b'\x1f', b'\x1f\x9d', b'\x1f\x8b\x90',
                    b'\x1f\x9d\x90\xff\x01', b'\x1f\x9d\x90\x41\xfe\x07'):
            differential(program, legacy, path, bad)
            cases += 1
        for chunks in (1, 2, 3):
            data = bytes(raw[:chunks * 65536 + 37])
            damaged = bytearray(encode_clear_tail(data))
            damaged[-2] |= 0xf0
            damaged[-1] |= 0x1f
            path.write_bytes(damaged)
            old = subprocess.run([legacy, str(path)], capture_output=True, text=True, check=True, timeout=30)
            facts = dict(line.split(' ', 1) for line in old.stdout.splitlines())
            assert 'corrupted data' in old.stderr and int(facts['result']) == 65536, facts
            assert int(facts['decoded']) == chunks * 65536
            reject(program, path, damaged, errno.EBADMSG)
            cases += 1
        # Demonstrate the original final-output failure, then exercise Linux's
        # positive/negative errno propagation at that exact final callback.
        state = 27
        contract_raw = bytearray()
        for _ in range(3 * 65536 + 37):
            state ^= (state << 13) & 0xffffffff
            state ^= state >> 17
            state ^= (state << 5) & 0xffffffff
            contract_raw.append(state & 255)
        contract_raw[510:512] = b'\x55\xaa'
        path.write_bytes(encode(contract_raw))
        for option in ('--fail-final', '--enospc-final'):
            for buffer in ((), ('--windows-buffer',)):
                old = original(legacy, path, option, *buffer)
                assert int(old['result']) == len(contract_raw)
                assert int(old['decoded']) < len(contract_raw)
                cases += 1
        arguments = [contract, str(path)] + (['--lzw-gzip'] if fallback else [])
        run = subprocess.run(arguments, capture_output=True, text=True,
                             check=True, timeout=30)
        print(run.stdout.strip())
    print(f'LZW: {cases} corpus/differential cases passed, including original callback divergence')


if __name__ == '__main__':
    main()
