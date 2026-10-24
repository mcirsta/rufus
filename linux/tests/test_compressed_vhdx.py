#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Independent MS-VHDX fixtures and qemu-img readback, using shared probes."""
import errno
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import uuid

MIB = 1024 * 1024
TABLE = 65536
BAT_GUID = '2dc27766-f623-4200-9d64-115e9bfd4a08'
META_GUID = '8b7ca206-4790-4b9a-b8fe-575f050f886e'
ITEMS = ('caa16737-fa36-4d43-b3b6-33f0aa44e76b', '2fa54224-cd1b-4876-b211-5dbed83bf4b8',
         'beca12ab-b2e6-4523-93ef-c309e000c746', '8141bf1d-a96f-4709-ba47-f233a8faab5f',
         'cda348c7-445d-4471-9cc9-e9885251c556')


def crc32c(data):
    value = 0xffffffff
    for byte in data:
        value ^= byte
        for bit in range(8):
            value = (value >> 1) ^ (0x82f63b78 if value & 1 else 0)
    return value ^ 0xffffffff


def set_checksum(image, start, size):
    struct.pack_into('<I', image, start + 4, 0)
    struct.pack_into('<I', image, start + 4, crc32c(image[start:start + size]))


def fixture():
    logical = bytearray(3 * MIB + 512)
    logical[:MIB] = b'A' * MIB
    logical[510:512] = b'\x55\xaa'
    logical[2 * MIB:3 * MIB] = b'B' * MIB
    image = bytearray(6 * MIB)
    image[:8] = b'vhdxfile'
    for sequence, start in enumerate((TABLE, 2 * TABLE), 10):
        image[start:start + 4] = b'head'
        struct.pack_into('<Q', image, start + 8, sequence)
        struct.pack_into('<HHIQ', image, start + 64, 0, 1, MIB, MIB)
        set_checksum(image, start, 4096)
    for start in (3 * TABLE, 4 * TABLE):
        image[start:start + 4] = b'regi'
        struct.pack_into('<I', image, start + 8, 2)
        for index, guid, offset in ((0, BAT_GUID, 3 * MIB), (1, META_GUID, 2 * MIB)):
            entry = start + 16 + 32 * index
            image[entry:entry + 16] = uuid.UUID(guid).bytes_le
            struct.pack_into('<QII', image, entry + 16, offset, MIB, 1)
        set_checksum(image, start, TABLE)
    image[2 * MIB:2 * MIB + 8] = b'metadata'
    struct.pack_into('<H', image, 2 * MIB + 10, 5)
    values = (struct.pack('<II', MIB, 0), struct.pack('<Q', len(logical)), bytes(16),
              struct.pack('<I', 512), struct.pack('<I', 4096))
    for index, (guid, value) in enumerate(zip(ITEMS, values)):
        entry = 2 * MIB + 32 + 32 * index
        image[entry:entry + 16] = uuid.UUID(guid).bytes_le
        offset = TABLE + 16 * index
        struct.pack_into('<IIII', image, entry + 16, offset, len(value), 4 if index == 0 else 6, 0)
        image[2 * MIB + offset:2 * MIB + offset + len(value)] = value
    struct.pack_into('<QQQQ', image, 3 * MIB, 4 * MIB | 6, 2, 5 * MIB | 6, 0)
    image[4 * MIB:5 * MIB] = logical[:MIB]
    image[5 * MIB:6 * MIB] = logical[2 * MIB:3 * MIB]
    return image, logical


def invoke(probe, path, maximum):
    process = subprocess.run([probe, str(maximum), 'hash', str(path)], check=True,
                             text=True, capture_output=True, timeout=30)
    return dict(line.split(' ', 1) for line in process.stdout.splitlines())


def accept(probe, path, image, logical):
    path.write_bytes(image)
    report = invoke(probe, path, len(logical))
    assert int(report['error']) == 0 and int(report['type']) == 12, report
    assert int(report['decoded']) == len(logical) and int(report['source']) == len(image), report
    assert report['prefix'] == logical[:512].hex(), report
    assert report['sha256'] == hashlib.sha256(logical).hexdigest(), report


def reject(probe, path, image, expected=errno.EBADMSG, maximum=2**63 - 1):
    path.write_bytes(image)
    report = invoke(probe, path, maximum)
    assert int(report['error']) == expected, report
    assert int(report['decoded']) == 0 and report['sha256'] == '', report


def main():
    probe, writer, fault = sys.argv[1:]
    assert crc32c(b'123456789') == 0xe3069283
    with tempfile.TemporaryDirectory(prefix='rufus-vhdx-') as directory:
        root = Path(directory)
        path = root / 'source.vhdx'
        image, logical = fixture()
        accept(probe, path, image, logical)
        count = 1
        # Each redundant root can recover from a damaged peer.
        for start, size in ((TABLE, 4096), (2 * TABLE, 4096), (3 * TABLE, TABLE), (4 * TABLE, TABLE)):
            changed = image.copy()
            changed[start + 4] ^= 1
            accept(probe, path, changed, logical)
            count += 1
        for positions in ((TABLE, 2 * TABLE), (3 * TABLE, 4 * TABLE)):
            changed = image.copy()
            for start in positions:
                changed[start + 4] ^= 1
            reject(probe, path, changed)
            count += 1
        # Dirty log, parent, invalid size/sector, overlapping and invalid BAT.
        mutations = ((2 * TABLE + 48, '<B', 1, errno.EBADMSG),
                     (2 * MIB + TABLE + 4, '<I', 2, errno.EBADMSG),
                     (2 * MIB + TABLE, '<I', 3 * MIB, errno.EBADMSG),
                     (2 * MIB + TABLE + 48, '<I', 1024, errno.EBADMSG),
                     (3 * MIB, '<Q', 2 * MIB | 6, errno.EBADMSG),
                     (3 * MIB + 16, '<Q', 4 * MIB | 6, errno.EBADMSG),
                     (3 * MIB, '<Q', 7 * MIB | 6, errno.EBADMSG),
                     (3 * MIB, '<Q', 4, errno.EBADMSG),
                     (3 * MIB, '<Q', 5, errno.EBADMSG),
                     (3 * MIB, '<Q', 7, errno.EBADMSG))
        for offset, fmt, value, expected in mutations:
            changed = image.copy()
            struct.pack_into(fmt, changed, offset, value)
            if TABLE <= offset < 3 * TABLE:
                set_checksum(changed, offset // TABLE * TABLE, 4096)
            reject(probe, path, changed, expected)
            count += 1
        reject(probe, path, image, errno.EFBIG, len(logical) - 1)
        reject(probe, path, image[:5 * MIB])
        count += 2
        # Optional unknown metadata is safe to ignore; required unknown is not.
        changed = image.copy()
        struct.pack_into('<H', changed, 2 * MIB + 10, 6)
        entry = 2 * MIB + 32 + 5 * 32
        changed[entry:entry + 16] = uuid.UUID('00112233-4455-6677-8899-aabbccddeeff').bytes_le
        accept(probe, path, changed, logical)
        struct.pack_into('<I', changed, entry + 24, 4)
        reject(probe, path, changed, errno.ENOTSUP)
        count += 2
        changed = image.copy()
        changed[2 * MIB + 32 + 2 * 32] ^= 1
        reject(probe, path, changed, errno.ENOTSUP)
        count += 1
        # Original output may be undefined in states 0,1,3. Choose stable zeros.
        for state in (0, 1, 2, 3):
            changed = image.copy()
            struct.pack_into('<Q', changed, 3 * MIB + 8, 5 * MIB | state)
            accept(probe, path, changed, logical)
            count += 1
        changed = image.copy()
        logical_4k = logical + bytes(4096 - 512)
        struct.pack_into('<Q', changed, 2 * MIB + TABLE + 16, len(logical_4k))
        struct.pack_into('<I', changed, 2 * MIB + TABLE + 48, 4096)
        accept(probe, path, changed, logical_4k)
        count += 1
        path.write_bytes(image)
        for mode, expected in ((1, 0), (2, 0), (3, errno.EIO), (6, errno.ECANCELED), (8, errno.ENOSPC)):
            process = subprocess.run([fault, str(mode), str(len(logical)), str(path)],
                                     check=True, text=True, capture_output=True, timeout=30)
            report = dict(line.split(' ', 1) for line in process.stdout.splitlines())
            assert int(report['error']) == expected and int(report['offset']) == 7, report
            count += 1
        # Independent external format oracle and the unchanged native writer.
        if shutil.which('qemu-img') is None:
            print(f'VHDX: {count} independent cases passed; qemu-img oracle unavailable')
            return
        raw = root / 'raw.img'
        raw.write_bytes(logical)
        for kind in ('fixed', 'dynamic'):
            for block in (MIB, 2 * MIB):
                output = root / f'{kind}-{block}.VHDX'
                subprocess.run(['qemu-img', 'convert', '-f', 'raw', '-O', 'vhdx', '-o',
                                f'subformat={kind},block_size={block}', str(raw), str(output)],
                               check=True, capture_output=True, timeout=30)
                report = invoke(probe, output, len(logical))
                assert int(report['error']) == 0 and report['sha256'] == hashlib.sha256(logical).hexdigest(), report
                target = root / 'target.img'
                target.write_bytes(b'\xa5' * len(logical) + b'TAIL')
                process = subprocess.run([writer, str(output), str(target), '512', '0', str(2**63 - 1), 'none'],
                                         check=True, text=True, capture_output=True, timeout=30)
                report = json.loads(process.stdout)
                assert report['error'] == 0 and report['compression'] == 12, report
                assert target.read_bytes() == logical + b'TAIL', report
                count += 2
        print(f'VHDX: {count} format/source/native-write cases passed; shared helper faults reused')


if __name__ == '__main__':
    main()
