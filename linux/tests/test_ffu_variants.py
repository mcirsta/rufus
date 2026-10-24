#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""One grouped FFU codec/descriptor/capture corpus; shared writer faults are reused."""
import errno
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
from ffu_fixture import BLOCK, make_ffu, lznt1_literals, huffman_literals
from test_compressed_ffu import check
from test_image_save import gpt_image, check_case

checks = []


def digest(data):
    return hashlib.sha256(data).hexdigest()


def codec(probe, root, algorithm, packed, raw=None, expected=0, label='', mode='decode', capacity=None):
    source = root / 'codec.bin'; source.write_bytes(packed)
    size = capacity if capacity is not None else len(raw)
    result = subprocess.check_output([probe, str(algorithm), str(size), str(source), mode], text=True, timeout=30).split()
    assert int(result[0]) == expected, (label, result, expected)
    if raw is not None and expected == 0: assert result[1] == digest(raw), (label, result, digest(raw))
    checks.append(label)


def lznt_match(prefix):
    raw = (bytes(range(prefix)) * (4096 // prefix + 1))[:4096]
    tokens = [(0, bytes([byte])) for byte in raw[:prefix]]
    position = prefix
    while position < len(raw):
        shift = 12; threshold = position - 1
        while threshold >= 16: shift -= 1; threshold >>= 1
        count = min((1 << shift) + 2, len(raw) - position)
        if count < 3:
            tokens.append((0, raw[position:position + 1])); position += 1
        else:
            tokens.append((1, struct.pack('<H', ((prefix - 1) << shift) | (count - 3)))); position += count
    packed = bytearray()
    for start in range(0, len(tokens), 8):
        group = tokens[start:start + 8]
        packed.append(sum(flag << i for i, (flag, value) in enumerate(group)))
        packed += b''.join(value for flag, value in group)
    return struct.pack('<H', 0xb000 | (len(packed) - 1)) + packed, raw


def writer_case(probe, path, raw):
    target = path.with_suffix('.target'); target.write_bytes(bytes(len(raw)))
    result = subprocess.check_output([probe, str(path), 'write', str(target)], text=True, timeout=60).split()
    assert list(map(int, result)) == [0, 0], result
    assert target.read_bytes() == raw
    target.unlink(); checks.append('verified native captured-size write')


def main():
    source_probe, writer, codec_probe, save_probe = sys.argv[1:]
    oracle = False
    try:
        from dissect.util.compression import lznt1
        oracle = True
    except ImportError:
        pass
    with tempfile.TemporaryDirectory(prefix='rufus-ffu-batch-') as temporary:
        root = Path(temporary); path = root / 'source.ffu'
        for algorithm, encode in ((2, lznt1_literals), (4, huffman_literals)):
            for size in (1024, 4096, 32768, 65536, 131072, 1048576):
                raw = (bytes(range(256)) * (size // 256 + 1))[:size]
                packed = encode(raw)
                codec(codec_probe, root, algorithm, packed, raw, label=f'codec {algorithm} literal {size}')
                if oracle and algorithm == 2:
                    assert lznt1.decompress(packed) == raw
                if algorithm == 4 and size <= 65536:
                    codec(codec_probe, root, algorithm, packed, raw, label=f'upstream decode literal {size}', mode='reference')
                for cut in (1, len(packed) // 2):
                    codec(codec_probe, root, algorithm, packed[:-cut], expected=errno.EBADMSG,
                          capacity=size, label=f'codec {algorithm} truncation {size}/{cut}')
            codec(codec_probe, root, algorithm, packed, expected=errno.ECANCELED,
                  capacity=len(raw), label=f'codec {algorithm} cancellation', mode='cancel')
        for prefix in (1, 16, 17, 31, 32, 33, 255):
            packed, raw = lznt_match(prefix)
            codec(codec_probe, root, 2, packed, raw, label=f'LZNT1 match split {prefix}')
            if oracle: assert lznt1.decompress(packed) == raw
        packed, raw = lznt_match(1)
        codec(codec_probe, root, 2, packed + bytes(2), raw, label='optional LZNT1 terminator')
        for packed in (b'\x00\x00', b'\x00\x20A', b'\x02\xb0\x01\x00\x00', b'\x00\xb0\x00', b'\x03\xb0\x02A\xff\x0f'):
            codec(codec_probe, root, 2, packed, expected=errno.EBADMSG, capacity=4096, label='bad LZNT1 signature/token/length')
        for packed in (bytes(260), b'\x11' * 256 + bytes(100), b'\xff' * 256 + bytes(100)):
            codec(codec_probe, root, 4, packed, expected=errno.EBADMSG, capacity=4096, label='bad Huffman empty/oversubscribed/undefined tree')
        # The long-length form need not be canonical; MS-XCA decodes its value
        # directly. Compare this alternate encoding with upstream wimlib too.
        table = bytearray(256); table[32] = table[135] = 0x10
        packed = table + b'\x00\x40\x00\x00\xff\x04\x00'
        codec(codec_probe, root, 4, packed, b'A' * 8, label='Huffman alternate long length')
        codec(codec_probe, root, 4, packed, b'A' * 8, label='upstream alternate long length', mode='reference')
        uniform = table + b'\x00\x40\x00\x00\xff\xfc\xff'
        # This table's literal is 'A', so each chunk emits exactly 64 KiB.
        codec(codec_probe, root, 4, uniform * 2, b'A' * 131072, label='Huffman full-chunk repeated matches')
        codec(codec_probe, root, 4, uniform, b'A' * 65536, label='upstream repeated full chunk', mode='reference')
        # Upstream wimlib is an independent real-match Huffman encoder, test only.
        for size in (1024, 4096, 32768, 65536):
            raw = ((b'Rufus-FFU-' + bytes(range(256))) * (size // 266 + 1))[:size]
            source, encoded = root / 'encoder.raw', root / 'encoder.huff'; source.write_bytes(raw)
            subprocess.run([codec_probe, 'encode', str(source), str(encoded)], check=True, timeout=30)
            packed = encoded.read_bytes()
            codec(codec_probe, root, 4, packed, raw, label=f'upstream Huffman matches {size}')
            # The FFU payload hash/size contract is independently tested below.
            if size == 32768:
                image, logical, offsets = make_ffu([(raw, [(0, 0)], packed)], disk_blocks=4,
                    version=3, algorithm=4, block=size)
                check(source_probe, path, image, logical); checks.append('upstream Huffman FFU')
                writer_case(writer, path, logical)
        # Grouped compressed buffers, repeated destinations, end-relative runs,
        # overwrites and sparse gaps exercise cache and mapping together.
        raw = bytearray(BLOCK * 2); raw[:BLOCK] = b'A' * BLOCK; raw[BLOCK:] = b'B' * BLOCK; raw[510:512] = b'\x55\xaa'
        for algorithm in (0, 1, 2, 3, 4):
            entries = [(bytes(raw), [(0, 0), (0, 4), (2, 1)]), (b'C' * BLOCK, [(0, 1)])]
            image, logical, offsets = make_ffu(entries, version=3, algorithm=algorithm)
            before = digest(image); check(source_probe, path, image, logical)
            assert digest(path.read_bytes()) == before
            checks.append(f'FFU grouped codec {algorithm}'); writer_case(writer, path, logical)
            bad = image.copy(); bad[offsets['data'] + (264 if algorithm == 4 else 16)] ^= 1
            check(source_probe, path, bad, expected=errno.EBADMSG); checks.append(f'FFU corrupt codec {algorithm}')
        # Plain V1 end-relative groups and illegal overflow are resolved at captured size.
        image, logical, offsets = make_ffu([(bytes(raw), [(0, 0), (2, 1)])])
        check(source_probe, path, image, logical); checks.append('V1 end-relative run')
        bad = image.copy(); struct.pack_into('<I', bad, offsets['descriptors'] + 20, 0)
        check(source_probe, path, bad, expected=errno.EBADMSG); checks.append('end-relative overflow')
        bad = image.copy(); struct.pack_into('<I', bad, offsets['descriptors'] + 4, 10000000)
        check(source_probe, path, bad, expected=errno.ENOTSUP); checks.append('bounded map allocation')
        # Names are optional ASCII manifest metadata; GPT payloads preserve Unicode
        # and empty names byte-for-byte. Unrepresentable names stay out of the INI.
        for name in ('', 'EFI\u03a9', '\u6570\u636e\U0001f4be', 'X' * 36):
            captured = gpt_image(name=name)
            report = check_case(Path(save_probe), root, captured, 'ffu')
            saved = root / 'saved.ffu'; check(source_probe, path, saved.read_bytes(), captured)
            checks.append('capture GPT name ' + repr(name))
        check_case(Path(save_probe), root, gpt_image(name='bad\n[Store]'), 'ffu')
        checks.append('capture preserves opaque name without manifest line injection')
    print(json.dumps({'passed': len(checks), 'independent_lznt1_oracle': oracle,
                      'independent_huffman_encoder': 'upstream wimlib 1.14.5',
                      'independent_huffman_decoder': 'upstream wimlib 1.14.5', 'checks': checks}, indent=2))


if __name__ == '__main__':
    main()
