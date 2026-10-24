#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Grouped FFU capture layouts and independent equal/larger-disk reconstruction."""
import errno
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import uuid
import zlib
from test_ffu_geometry import decode_capture
from test_image_save import MIB, check_case


def layout_image(sector=512, entries=128, entry_bytes=128, partitions=0, table_lba=2):
    table = bytearray(entries * entry_bytes)
    table_sectors = (len(table) + sector - 1) // sector
    first = max(MIB // sector, table_lba + table_sectors)
    minimum = (first + max(1, partitions * 2) + table_sectors + 2) * sector
    size = max(8 * MIB, (minimum + MIB - 1) // MIB * MIB) + 3 * sector
    last = size // sector - 2 - table_sectors
    assert partitions <= entries and first + max(0, partitions - 1) * 2 <= last
    for index in range(partitions):
        offset = index * entry_bytes
        table[offset:offset + 16] = uuid.UUID('ebd0a0a2-b9e5-4433-87c0-68b6b72699c7').bytes_le
        table[offset + 16:offset + 32] = uuid.UUID(int=index + 1).bytes_le
        struct.pack_into('<QQQ', table, offset + 32, first + index * 2, first + index * 2, 0)
        name = ('Partition ' + str(index)).encode('utf-16le')
        table[offset + 56:offset + 56 + len(name)] = name
    image = bytearray(size)
    image[510:512] = b'\x55\xaa'
    image[446:462] = struct.pack('<B3sB3sII', 0, b'\0\2\0', 0xee, b'\xff' * 3, 1, size // sector - 1)
    backup = size // sector - 1 - table_sectors
    for current, alternate, location in ((1, size // sector - 1, table_lba), (size // sector - 1, 1, backup)):
        header = bytearray(sector)
        struct.pack_into('<8sIIIIQQQQ16sQIII', header, 0, b'EFI PART', 0x10000, 92, 0, 0,
                         current, alternate, first, last, uuid.UUID(int=1234).bytes_le,
                         location, entries, entry_bytes, zlib.crc32(table))
        struct.pack_into('<I', header, 16, zlib.crc32(header[:92]))
        image[current * sector:(current + 1) * sector] = header
        image[location * sector:location * sector + len(table)] = table
    return image


def expected_restore(raw, capacity, sector, block):
    """Apply capture's forward/tail placement, then independently move the GPT."""
    result = bytearray(b'\xa5' * capacity)
    forward = len(raw) // block * block
    result[:forward] = raw[:forward]
    result[-block:] = raw[-block:]
    delta = (capacity - len(raw)) // sector
    primary, backup = bytearray(raw[sector:2 * sector]), bytearray(raw[-sector:])
    entries, entry_bytes = struct.unpack_from('<II', primary, 80)
    count = (entries * entry_bytes + sector - 1) // sector * sector
    old_table = struct.unpack_from('<Q', backup, 72)[0] * sector
    new_table = old_table + delta * sector
    result[new_table:new_table + count] = raw[old_table:old_table + count]
    for header, location in ((primary, sector), (backup, capacity - sector)):
        fields = (32, 48) if location == sector else (24, 48, 72)
        for field in fields:
            value = struct.unpack_from('<Q', header, field)[0]
            struct.pack_into('<Q', header, field, value + delta)
        struct.pack_into('<I', header, 16, 0)
        struct.pack_into('<I', header, 16, zlib.crc32(header[:92]))
        result[location:location + sector] = header
    result[:sector] = raw[:sector]
    struct.pack_into('<I', result, 458, min(capacity // sector - 1, 0xffffffff))
    return result


def main():
    saver, reader, writer = sys.argv[1:]
    cases = []; captures = []
    layouts = [('empty', 128, 128, 0, 2), ('many partitions', 8192, 256, 1024, 2),
               ('wide entries', 128, 8192, 3, 8), ('many empty entries', 32768, 128, 1, 2)]
    with tempfile.TemporaryDirectory(prefix='rufus-ffu-layouts-') as temporary:
        root = Path(temporary)
        for sector in (512, 4096):
            for label, entries, entry_bytes, partitions, table_lba in layouts:
                raw = layout_image(sector, entries, entry_bytes, partitions, table_lba)
                check_case(Path(saver), root, raw, 'ffu', sector=sector, old=b'OLD BACKUP')
                path = root / 'saved.ffu'
                decoded, block = decode_capture(path)
                assert decoded == raw
                data = path.read_bytes()
                catalog, hash_bytes = struct.unpack_from('<II', data, 24)
                image = (32 + catalog + hash_bytes + block - 1) // block * block
                manifest_bytes = struct.unpack_from('<I', data, image + 16)[0]
                if partitions == 1024: assert manifest_bytes > block
                plan = json.loads(subprocess.check_output([saver, '--ffu-plan', str(len(raw)),
                    str(catalog), str(manifest_bytes)], text=True))
                assert plan['error'] == 0 and plan['image'] >= manifest_bytes + 24
                assert plan['image'] % block == 0 and len(data) <= plan['bytes']
                for capacity in (len(raw), 2 * len(raw)):
                    target = root / 'target.img'; target.write_bytes(b'\xa5' * capacity)
                    report = json.loads(subprocess.check_output([writer, str(path), str(target), str(sector),
                        '0', str(2**63 - 1), 'none'], text=True, timeout=60))
                    assert report['error'] == 0 and report['written'] == report['verified'], (label, report)
                    expected = raw if capacity == len(raw) else expected_restore(raw, capacity, sector, block)
                    assert target.read_bytes() == expected, (label, sector, capacity)
                captures.append({'layout': label, 'sector': sector, 'manifest_bytes': manifest_bytes,
                                 'gpt_table_bytes': entries * entry_bytes, 'stored_bytes': len(data)})
                cases.append(f'{label}, sector {sector}: capture, independent hashes/decode, equal/larger restore')
        # Refuse malformed source geometry before publication, including empty
        # disks where there are no used entries to expose an invalid usable range.
        for field, value in ((40, 1), (48, 0), (72, 1), (72, 2048)):
            raw = layout_image()
            struct.pack_into('<Q', raw, 512 + field, value)
            struct.pack_into('<I', raw, 528, 0)
            struct.pack_into('<I', raw, 528, zlib.crc32(raw[512:604]))
            check_case(Path(saver), root, raw, 'ffu', expected=errno.EBADMSG, old=b'OLD BACKUP')
            cases.append(f'empty GPT invalid field {field}/{value}, old destination preserved')
        # Independent planner boundaries cover cross-chunk manifests without
        # introducing a new writer fault matrix.
        for manifest in (0, 131048, 131049, 262120, 262121, MIB, MIB + 1, 2**64 - 1):
            report = json.loads(subprocess.check_output([saver, '--ffu-plan', str(8 * MIB), '328', str(manifest)], text=True))
            assert report['error'] == (0 if manifest <= MIB else errno.ENOTSUP), report
            if not report['error']:
                assert report['image'] == (24 + manifest + report['block'] - 1) // report['block'] * report['block']
            cases.append(f'manifest planning {manifest}')
        for size in (256 << 30, 400 << 30, 1 << 40, 16 << 40):
            maximum = json.loads(subprocess.check_output([saver, '--ffu-plan', str(size), '328'], text=True))
            for manifest in (0, 131049, MIB):
                actual = json.loads(subprocess.check_output([saver, '--ffu-plan', str(size), '328', str(manifest)], text=True))
                assert maximum['error'] == actual['error'] == 0
                assert maximum['block'] == actual['block'] and actual['bytes'] <= maximum['bytes']
            cases.append(f'pre-acquisition space bound covers every manifest at {size}')
        for entries, entry_bytes in ((32769, 128), (128, 2**31)):
            raw = layout_image()
            struct.pack_into('<II', raw, 592, entries, entry_bytes)
            struct.pack_into('<I', raw, 528, 0)
            struct.pack_into('<I', raw, 528, zlib.crc32(raw[512:604]))
            check_case(Path(saver), root, raw, 'ffu', expected=errno.ENOTSUP, old=b'OLD BACKUP')
            cases.append(f'GPT table budget {entries}/{entry_bytes}, old destination preserved')
        # Source analysis must accept the large metadata region too.
        from test_compressed_ffu import check
        check(reader, root / 'analyzed.ffu', data, logical=decoded)
        cases.append('retained-source analysis of extended GPT capture')
    print(json.dumps({'passed': len(cases), 'cases': cases, 'captures': captures}, indent=2))


if __name__ == '__main__':
    main()
