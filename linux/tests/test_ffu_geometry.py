#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""One grouped FFU exact-sector geometry/capture/planning corpus."""
import errno
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
from ffu_fixture import BLOCK, make_ffu
from test_compressed_ffu import check
from test_image_save import gpt_image, check_case


def decode_lznt1(packed):
    """Independent MS-XCA decoder for capture readback (no Rufus codec)."""
    decoded = bytearray(); cursor = 0
    while cursor < len(packed):
        header = struct.unpack_from('<H', packed, cursor)[0]; cursor += 2
        assert header & 0x7000 == 0x3000
        end = cursor + (header & 4095) + 1
        assert end <= len(packed)
        start = len(decoded)
        if header & 0x8000:
            while cursor < end:
                flags = packed[cursor]; cursor += 1
                for bit in range(8):
                    if cursor == end: break
                    if flags & (1 << bit):
                        token = struct.unpack_from('<H', packed, cursor)[0]; cursor += 2
                        shift = 12
                        while (1 << (16 - shift)) < len(decoded) - start: shift -= 1
                        distance, length = (token >> shift) + 1, (token & ((1 << shift) - 1)) + 3
                        assert distance <= len(decoded) - start
                        for unused in range(length): decoded.append(decoded[-distance])
                    else:
                        decoded.append(packed[cursor]); cursor += 1
        else:
            decoded.extend(packed[cursor:end]); cursor = end
        assert cursor == end and 0 < len(decoded) - start <= 4096
    return bytes(decoded)



def decode_xpress(packed):
    """Independent MS-XCA 2.4 decoder, including the terminal flag group."""
    result = bytearray(); cursor = 0; half = None
    while cursor < len(packed):
        flags = struct.unpack_from('<I', packed, cursor)[0]; cursor += 4
        for bit in range(31, -1, -1):
            if cursor == len(packed):
                return bytes(result)
            if not flags & (1 << bit):
                result.append(packed[cursor]); cursor += 1
                continue
            token = struct.unpack_from('<H', packed, cursor)[0]; cursor += 2
            distance, length = (token >> 3) + 1, token & 7
            if length == 7:
                if half is None:
                    half = cursor; length = packed[cursor] & 15; cursor += 1
                else:
                    length = packed[half] >> 4; half = None
                if length == 15:
                    length = packed[cursor]; cursor += 1
                    if length == 255:
                        length = struct.unpack_from('<H', packed, cursor)[0]; cursor += 2
                        if length == 0:
                            length = struct.unpack_from('<I', packed, cursor)[0]; cursor += 4
                        assert length >= 22
                        length -= 22
                    length += 15
                length += 7
            length += 3
            assert 0 < distance <= len(result)
            for unused in range(length): result.append(result[-distance])
    return bytes(result)


def decode_capture(path):
    # Independent FFU V1/V1-compressed decoder: verify each metadata/payload hash, resolve
    # descriptors directly at the manifest's exact sector count, no Rufus code.
    packed = path.read_bytes()
    chunk_kib, algorithm, catalog_bytes, hash_bytes = struct.unpack_from('<4I', packed, 16)
    assert algorithm == 0x800c
    block = chunk_kib * 1024
    image_offset = (32 + catalog_bytes + hash_bytes + block - 1) // block * block
    header_bytes, manifest_bytes = struct.unpack_from('<I12xI', packed, image_offset)
    manifest = packed[image_offset + header_bytes:image_offset + header_bytes + manifest_bytes].decode()
    values = dict(line.split('=', 1) for line in manifest.replace('\r', '').split('\n') if '=' in line)
    values = {key.strip(): value.strip() for key, value in values.items()}
    disk_bytes = int(values['MinSectorCount']) * int(values['SectorSize'])
    store = (image_offset + header_bytes + manifest_bytes + block - 1) // block * block
    stored_block, descriptors, table_bytes = struct.unpack_from('<3I', packed, store + 204)
    assert stored_block == block
    compressed = struct.unpack_from('<H', packed, store + 8)[0] == 3
    algorithm = struct.unpack_from('<I', packed, store + 248)[0] if compressed else 0
    assert algorithm in (0, 2, 3)
    table_offset = store + (252 if compressed else 248)
    payload_offset = (table_offset + table_bytes + block - 1) // block * block
    hashes = packed[32 + catalog_bytes:32 + catalog_bytes + hash_bytes]
    raw = bytearray(disk_bytes)
    hash_index = 0
    for offset in range(image_offset, payload_offset, block):
        assert hashlib.sha256(packed[offset:offset + block]).digest() == hashes[hash_index:hash_index + 32]
        hash_index += 32
    offset = table_offset
    final_method = None
    for unused in range(descriptors):
        locations, count = struct.unpack_from('<2I', packed, offset); offset += 8
        stored = struct.unpack_from('<I', packed, offset)[0] if compressed else count * block
        offset += 4 if compressed else 0
        payload = packed[payload_offset:payload_offset + stored]
        if algorithm == 2: payload = decode_lznt1(payload)
        elif algorithm == 3: payload = decode_xpress(payload)
        assert len(payload) == count * block
        for index in range(count):
            assert hashlib.sha256(payload[index * block:(index + 1) * block]).digest() == hashes[hash_index:hash_index + 32]
            hash_index += 32
        for unused_location in range(locations):
            method, index = struct.unpack_from('<2I', packed, offset); offset += 8
            position = index * block if method == 0 else disk_bytes - (index + 1) * block
            assert 0 <= position <= disk_bytes - len(payload)
            raw[position:position + len(payload)] = payload
            final_method = method
        payload_offset += stored
    assert hash_index == len(hashes) and payload_offset == len(packed)
    assert final_method == (2 if disk_bytes % block else 0)
    return bytes(raw), block


def main():
    reader, writer, saver = sys.argv[1:]
    cases = []
    with tempfile.TemporaryDirectory(prefix='rufus-ffu-geometry-') as temporary:
        root = Path(temporary); source = root / 'source.ffu'; target = root / 'target.img'
        prefix = bytearray(b'A' * BLOCK); prefix[510:512] = b'\x55\xaa'
        for algorithm in (0, 1, 2, 3, 4):
            for sector in (512, 4096):
                for remainder in (sector, 3 * sector, BLOCK - sector):
                    size = 8 * BLOCK + remainder
                    entries = [(bytes(prefix), [(0, 0)]), (b'B' * (2 * BLOCK), [(0, 3), (2, 1)]),
                               (b'C' * BLOCK, [(0, 6)]), (b'D' * BLOCK, [(2, 0)])]
                    packed, logical, offsets = make_ffu(entries, version=3, algorithm=algorithm,
                        sector=sector, disk_bytes=size)
                    check(reader, source, packed, logical)
                    target.write_bytes(bytes(size))
                    report = json.loads(subprocess.check_output([writer, str(source), str(target), str(sector),
                        '0', str(2**63 - 1), 'none'], text=True, timeout=60))
                    assert report['error'] == 0 and target.read_bytes() == logical, report
                    cases.append(f'exact logical source and equal-target writer codec {algorithm} sector {sector} remainder {remainder}')
        for sector in (512, 4096):
            for remainder in (sector, 3 * sector, BLOCK - sector):
                raw = gpt_image(size=8 * 1024 * 1024 + remainder, sector=sector)
                check_case(Path(saver), root, raw, 'ffu', sector=sector)
                captured = root / 'saved.ffu'
                catalog_bytes = struct.unpack_from('<I', captured.read_bytes(), 24)[0]
                plan = json.loads(subprocess.check_output([saver, '--ffu-plan', str(len(raw)), str(catalog_bytes)], text=True))
                assert plan['error'] == 0 and plan['bytes'] >= captured.stat().st_size, plan
                actual, block = decode_capture(captured)
                assert actual == raw and block == BLOCK
                check(reader, source, captured.read_bytes(), raw)
                target.write_bytes(b'\xa5' * len(raw))
                report = json.loads(subprocess.check_output([writer, str(source), str(target), str(sector),
                    '0', str(2**63 - 1), 'none'], text=True, timeout=60))
                assert report['error'] == 0 and report['written'] == len(raw) and target.read_bytes() == raw, report
                cases.append(f'capture independent hashes/reconstruction and restored source {sector} remainder {remainder}')
        for mode, expected in (('short', 0), ('cancel-copy', errno.ECANCELED), ('mutate', errno.ESTALE)):
            check_case(Path(saver), root, gpt_image(size=8 * 1024 * 1024 + 1536), 'ffu', mode=mode,
                expected=expected, old=b'OLD BACKUP')
            cases.append('non-aligned capture ' + mode)
        # Native math is tested at table transitions without reading or staging
        # enormous artificial disks. Actual chunk decoding is checked below.
        for size in (1024, 64 * 1024, 128 * 1024, 128 * 1024 + 512,
                     128 << 30, (256 << 30) - 512, 256 << 30, 512 << 30, 1 << 40,
                     2 << 40, 4 << 40, 8 << 40, 16 << 40, (32 << 40) - 64 * 1024 * 1024, 32 << 40):
            report = json.loads(subprocess.check_output([saver, '--ffu-plan', str(size), '600'], text=True))
            if size == 32 << 40:
                assert report['error'] == errno.ENOTSUP and not report['bytes'], report
            else:
                assert report['error'] == 0, report
                block = report['block']; count = (size + block - 1) // block
                assert 1024 <= block <= 16 * 1024 * 1024 and block & (block - 1) == 0
                assert count == report['blocks'] and count * 32 <= 64 * 1024 * 1024
                assert count * 24 <= 64 * 1024 * 1024 and count * 20 <= 64 * 1024 * 1024
                assert report['hashes'] <= 64 * 1024 * 1024
                assert report['metadata'] % block == report['security'] % block == 0
                assert report['bytes'] == report['metadata'] + report['security'] + count * (block + 4 * (block // 32 + 1))
                if size >= 256 << 30: assert block > BLOCK
            cases.append(f'bounded capture geometry {size}')
        for block in (2 * 1024 * 1024, 4 * 1024 * 1024, 8 * 1024 * 1024, 16 * 1024 * 1024):
            raw = bytearray(b'Q' * block); raw[510:512] = b'\x55\xaa'
            packed, logical, offsets = make_ffu([(bytes(raw), [(0, 0), (2, 0)])], block=block, disk_bytes=block + 512)
            check(reader, source, packed, logical)
            cases.append(f'large chunk exact-end decode {block}')
        print(json.dumps({'passed': len(cases), 'cases': cases}, indent=2))


if __name__ == '__main__':
    main()
