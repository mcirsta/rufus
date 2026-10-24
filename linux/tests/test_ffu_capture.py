#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compressed FFU capture, independent codecs, exact dirty-disk restores and faults."""
import errno
import hashlib
import json
from pathlib import Path
import random
import struct
import subprocess
import sys
import tempfile
import zlib
from test_ffu_geometry import decode_capture, decode_xpress
from test_image_save import gpt_image, check_case, MIB


def opaque_name(raw, sector):
    table_bytes = 16384
    for offset in (2 * sector, len(raw) - sector - table_bytes):
        raw[offset + 56:offset + 62] = b'\x00\xd8\x0a\x00\xff\xdf'
    crc = zlib.crc32(raw[2 * sector:2 * sector + table_bytes])
    for offset in (sector, len(raw) - sector):
        struct.pack_into('<I', raw, offset + 88, crc)
        struct.pack_into('<I', raw, offset + 16, 0)
        struct.pack_into('<I', raw, offset + 16, zlib.crc32(raw[offset:offset + 92]))
    return raw


def main():
    reader, writer, codec, saver = sys.argv[1:]
    try:
        from dissect.util.compression import lzxpress
        oracle = lzxpress.decompress
    except ImportError:
        oracle = None
    cases = []; captures = []
    with tempfile.TemporaryDirectory(prefix='rufus-ffu-capture-') as scratch:
        root = Path(scratch); source = root / 'codec.raw'; output = root / 'codec.xpress'
        rng = random.Random(734185)
        # XPRESS length tiers, paired nibbles, 32-symbol terminal flag groups and
        # multi-MiB adaptive capture buffers share one desktop-compatible corpus.
        vectors = [bytes(n) for n in (1, 2, 3, 15, 16, 17, 31, 32, 33, 255, 256,
                                     257, 279, 280, 4095, 4096, 4097, 8191, 8192, 8193, 65535, 65538, 65539, 131072)]
        vectors += [bytes(range(256)) * 512, b'\xff' * 131072,
                    rng.randbytes(131072), (b'Linux FFU\0' * 1800000)[:16 * MIB]]
        vectors += [rng.randbytes(n) for n in (31, 32, 33, 63, 64, 65, 8192)]
        for distance in (8191, 8192, 8193):
            prefix = rng.randbytes(distance)
            vectors.append(prefix + prefix[:1024])
        vectors += [(rng.randbytes(n) * (4096 // n + 1))[:4096] for n in (15, 16, 17, 31, 32, 33, 127, 128, 129, 2048, 2049)]
        for index, raw in enumerate(vectors):
            source.write_bytes(raw)
            report = subprocess.check_output([codec, 'encode-xpress', str(source), str(output)], text=True).split()
            packed = output.read_bytes()
            assert int(report[0]) == 0 and int(report[1]) == len(packed)
            assert len(packed) <= len(raw) + 4 * (len(raw) // 32 + 1)
            assert decode_xpress(packed) == raw
            if oracle: assert oracle(packed) == raw
            decoded = subprocess.check_output([codec, '3', str(len(raw)), str(output), 'decode'], text=True).split()
            assert decoded == ['0', hashlib.sha256(raw).hexdigest()], decoded
            cases.append(f'XPRESS encoder vector {index}, independent decode, bounds and native decode')
        # MS-XCA's terminal flag word follows a complete 32-token group.
        # Older sources without it stay readable; arbitrary trailers do not.
        literals = bytes(range(32))
        for trailer, error in ((b'', 0), (b'\xff' * 4, 0), (bytes(4), errno.EBADMSG),
                               (b'\xff' * 3, errno.EBADMSG), (b'\xff' * 5, errno.EBADMSG)):
            output.write_bytes(bytes(4) + literals + trailer)
            report = subprocess.check_output([codec, '3', '32', str(output), 'decode'], text=True).split()
            assert int(report[0]) == error, report
        cases.append('complete flag group: standard/legacy end and malformed trailers')
        source.write_bytes(bytes(131072))
        for mode, error in (('small', errno.ENOBUFS), ('cancel', errno.ECANCELED), ('cancel-late', errno.ECANCELED)):
            output.unlink(missing_ok=True)
            result = subprocess.check_output([codec, 'encode-xpress', str(source), str(output), mode], text=True).split()
            assert int(result[0]) == error and not output.exists(), result
            if mode == 'cancel-late': assert int(result[1]) > 0
            cases.append('encoder ' + mode + ' with output canary')
        for sector in (512, 4096):
            for kind in ('sparse', 'incompressible', 'opaque-name'):
                raw = gpt_image(size=8 * MIB + 3 * sector, sector=sector, name='EFI\nInjected=Value')
                if kind == 'incompressible': raw[MIB:-MIB] = rng.randbytes(len(raw) - 2 * MIB)
                if kind == 'opaque-name': raw = opaque_name(raw, sector)
                check_case(Path(saver), root, raw, 'ffu', sector=sector, old=b'OLD BACKUP')
                capture = root / 'saved.ffu'; packed = capture.read_bytes()
                decoded, block = decode_capture(capture)
                assert decoded == raw
                _, _, catalog_bytes, hash_bytes = struct.unpack_from('<4I', packed, 16)
                image = (32 + catalog_bytes + hash_bytes + block - 1) // block * block
                header_bytes, manifest_bytes = struct.unpack_from('<I12xI', packed, image)
                manifest = packed[image + header_bytes:image + header_bytes + manifest_bytes]
                assert b'Injected' not in manifest
                store = (image + header_bytes + manifest_bytes + block - 1) // block * block
                assert struct.unpack_from('<HHHH', packed, store + 4) == (1, 0, 3, 0)
                assert struct.unpack_from('<I', packed, store + 248)[0] == 3
                if kind != 'incompressible': assert len(packed) < len(raw) // 8
                # A zero block has to overwrite old bytes too. This is a full
                # capture, so omission of empty space is not an allowed shortcut.
                target = root / 'target.raw'; target.write_bytes(b'\xa5' * len(raw))
                result = json.loads(subprocess.check_output([writer, str(capture), str(target), str(sector),
                    '0', str(2**63 - 1), 'none'], text=True))
                assert result['error'] == 0 and result['written'] == result['verified'] == len(raw), result
                assert target.read_bytes() == raw
                plan = json.loads(subprocess.check_output([saver, '--ffu-plan', str(len(raw)), str(catalog_bytes)], text=True))
                assert plan['error'] == 0 and len(packed) <= plan['bytes']
                captures.append({'sector': sector, 'kind': kind, 'source_bytes': len(raw), 'stored_bytes': len(packed)})
                cases.append(f'compressed {kind} capture, atomic replace, hashes and dirty restore, sector {sector}')
        # FFU-specific publication failures reuse the existing destination and
        # helper matrix, while exercising the newly variable-length output.
        for mode, error in (('short', 0), ('write-fail', errno.ENOSPC), ('sync-fail', errno.EIO),
                            ('cancel-copy', errno.ECANCELED), ('cancel-verify', errno.ECANCELED),
                            ('mutate', errno.ESTALE), ('destination-race', errno.ESTALE)):
            check_case(Path(saver), root, gpt_image(), 'ffu', mode=mode, expected=error, old=b'OLD BACKUP')
            cases.append('compressed capture ' + mode)
    print(json.dumps({'passed': len(cases), 'cases': cases, 'captures': captures,
                      'independent_xpress_oracle': bool(oracle)}, indent=2))


if __name__ == '__main__':
    main()
