#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Grouped optimized geometry, native writes, recovery XML and NTFS growth.

All media are owned ordinary files. GPT/manifest serialization and expected
placement are independent of the production planner.
"""
import base64
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
import xml.etree.ElementTree as ET
import zlib
from ffu_fixture import BLOCK, make_ffu

MIB = 1024 * 1024
DISK_ID = uuid.UUID('87654321-1234-5678-aaaa-123456789abc')
BASIC = uuid.UUID('ebd0a0a2-b9e5-4433-87c0-68b6b72699c7')
RECOVERY = uuid.UUID('de94bba4-06d1-4d40-a16a-bfd50179d6ac')


def optimized_fixture(sector=512, last=False, volumes=None, size=16 * MIB, definitions=None):
    raw = bytearray(size)
    raw[510:512] = b'\x55\xaa'
    raw[446:462] = struct.pack('<B3sB3sII', 0, b'\0\2\0', 0xee, b'\xff' * 3, 1, size // sector - 1)
    # Holes in the GPT prove that the manifest ordinal is not a partition node.
    if definitions is None:
        definitions = [(0, MIB, MIB, BASIC, 'System'), (3, 2 * MIB, 4 * MIB, BASIC, 'Windows Ω')]
        if not last:
            definitions.append((7, 12 * MIB, 3 * MIB, RECOVERY, 'Recovery'))
    table = bytearray(128 * 128)
    first, usable_end = 2 * sector + len(table), size - sector - len(table)
    manifest = (f'[FullFlash]\r\nOptimized=True\r\n[Store]\r\nMinSectorCount={size // sector}\r\n'
        f'SectorSize={sector}\r\nStoreId={DISK_ID}\r\nStartingUsableOffset={first}\r\n'
        f'UsableLength={usable_end - first}\r\nMaxPartitionCount=128\r\n')
    for ordinal, (slot, offset, length, kind, name) in enumerate(definitions):
        identity = uuid.UUID(int=ordinal + 1)
        struct.pack_into('<16s16sQQQ72s', table, slot * 128, kind.bytes_le, identity.bytes_le,
            offset // sector, (offset + length) // sector - 1, 0, name.encode('utf-16le'))
        encoded = base64.b64encode((name + '\0').encode('utf-16le')).decode()
        manifest += (f'[Partition]\r\nId={identity}\r\nType={kind}\r\nOffsetInSectors={offset // sector}\r\n'
            f'TotalSectors={length // sector}\r\nAttributes=0\r\nName=ignored fallback\r\nName_U={encoded}\r\n'
            f'UseAllSpace={"True" if ordinal == 1 else "False"}\r\nRewritePartition=True\r\n')
        if volumes is not None and ordinal in volumes:
            data = volumes[ordinal]
            assert len(data) <= length
            raw[offset:offset + len(data)] = data
    if volumes is None:
        boot = bytearray(sector)
        boot[3:11] = b'NTFS    '
        struct.pack_into('<H', boot, 11, sector)
        boot[13] = 8
        struct.pack_into('<Q', boot, 40, 4 * MIB // sector - 1)
        boot[510:512] = b'\x55\xaa'
        raw[2 * MIB:2 * MIB + sector] = boot
    raw[2 * sector:2 * sector + len(table)] = table
    raw[usable_end:usable_end + len(table)] = table
    for location, other, entries in ((sector, size // sector - 1, 2), (size - sector, 1, usable_end // sector)):
        header = bytearray(sector)
        struct.pack_into('<8sIIIIQQQQ16sQIII', header, 0, b'EFI PART', 0x10000, 92, 0, 0,
            location // sector, other, first // sector, usable_end // sector - 1,
            DISK_ID.bytes_le, entries, 128, 128, zlib.crc32(table))
        struct.pack_into('<I', header, 16, zlib.crc32(header[:92]))
        raw[location:location + sector] = header
    # All beginning-relative runs include the trailing source block. The final
    # end-relative duplicate proves it remains tied to the actual target end.
    selected_end = definitions[1][1] + definitions[1][2]
    following = definitions[2][1] if len(definitions) > 2 else size
    entries = [(bytes(raw[i:i + BLOCK]), [(0, i // BLOCK)]) for i in range(0, size, BLOCK)
               if i < selected_end or i >= following or i == size - BLOCK]
    entries.append((bytes(raw[-BLOCK:]), [(2, 0)]))
    return raw, manifest.encode(), entries


def expected_disk(raw, entries, capacity, sector, last):
    shift = capacity // BLOCK - len(raw) // BLOCK
    boundary = len(raw) - sector - 16384 if last else 12 * MIB
    if not last:
        aligned = (boundary + shift * BLOCK) // MIB * MIB
        shift = (aligned - boundary) // BLOCK
    threshold = len(raw) // BLOCK - 1 if last else boundary // BLOCK
    delta = shift * BLOCK
    result = bytearray(b'\xa5' * capacity)
    for data, locations in entries:
        for method, index in locations:
            position = capacity - (index + 1) * BLOCK if method == 2 else (index + (shift if index >= threshold else 0)) * BLOCK
            result[position:position + len(data)] = data
    table = bytearray(raw[2 * sector:2 * sector + 16384])
    struct.pack_into('<Q', table, 3 * 128 + 40, (boundary + delta) // sector - 1)
    if not last:
        for field in (32, 40):
            value = struct.unpack_from('<Q', table, 7 * 128 + field)[0]
            struct.pack_into('<Q', table, 7 * 128 + field, value + delta // sector)
    result[2 * sector:2 * sector + len(table)] = table
    result[capacity - sector - len(table):capacity - sector] = table
    for old, location, current, other, table_lba in ((sector, sector, 1, capacity // sector - 1, 2),
            (len(raw) - sector, capacity - sector, capacity // sector - 1, 1, (capacity - sector - len(table)) // sector)):
        header = bytearray(raw[old:old + sector])
        struct.pack_into('<QQ', header, 24, current, other)
        struct.pack_into('<Q', header, 48, struct.unpack_from('<Q', header, 48)[0] + delta // sector)
        struct.pack_into('<Q', header, 72, table_lba)
        struct.pack_into('<I', header, 88, zlib.crc32(table))
        struct.pack_into('<I', header, 16, 0)
        struct.pack_into('<I', header, 16, zlib.crc32(header[:92]))
        result[location:location + sector] = header
    result[:sector] = raw[:sector]
    struct.pack_into('<I', result, 458, capacity // sector - 1)
    return result


def invoke(arguments):
    result = subprocess.run([str(value) for value in arguments], capture_output=True, text=True, check=True, timeout=120)
    return json.loads(result.stdout.splitlines()[-1])


def main():
    probe, writer = sys.argv[1:3]
    mkntfs = sys.argv[3] if len(sys.argv) > 3 else shutil.which('mkntfs')
    ntfsresize = sys.argv[4] if len(sys.argv) > 4 else shutil.which('ntfsresize')
    assert mkntfs and ntfsresize, 'The grouped real-filesystem checks require mkntfs and ntfsresize'
    cases = []
    with tempfile.TemporaryDirectory(prefix='rufus-ffu-optimized-') as temporary:
        root = Path(temporary)
        source, target = root / 'source.ffu', root / 'target.img'
        # Windows shifts a complete location run according to its starting
        # block. Splitting the run into chunks must retain that decision even
        # for a run spanning the movable boundary.
        raw, manifest, entries = optimized_fixture()
        boundary = 12 * MIB // BLOCK
        grouped = []
        for data, locations in entries:
            if locations == [(0, boundary)]:
                grouped.append((bytes(raw[(boundary - 1) * BLOCK:(boundary + 1) * BLOCK]), [(0, boundary - 1)]))
            else:
                grouped.append((data, locations))
        source.write_bytes(make_ffu(grouped, disk_bytes=len(raw), manifest=manifest)[0])
        target.write_bytes(b'\xa5' * (20 * MIB))
        report = invoke([writer, source, target, 512, 1, 2**63 - 1, 'none'])
        assert report['error'] == 0 and target.read_bytes() == expected_disk(raw, grouped, 20 * MIB, 512, False), report
        cases.append('grouped descriptor crossing movable boundary retains its original run start')
        for sector in (512, 4096):
            for last in (False, True):
                raw, manifest, entries = optimized_fixture(sector, last)
                packed = make_ffu(entries, sector=sector, disk_bytes=len(raw), manifest=manifest)[0]
                source.write_bytes(packed)
                for capacity in (10 * MIB, 16 * MIB, 21 * MIB + 3 * sector):
                    plan = invoke([probe, 'plan', source, capacity, sector])
                    assert plan['error'] == 0 and plan['optimized'] and plan['partition'] == 4, plan
                    assert int(plan['captured']) == len(raw) and int(plan['required']) == int(plan['minimum']) <= capacity, plan
                    poison = b'\xa5' * capacity
                    target.write_bytes(poison)
                    report = invoke([writer, source, target, sector, 1, 2**63 - 1, 'none'])
                    assert report['error'] == 0 and report['written'] == report['verified'], report
                    assert target.read_bytes() == expected_disk(raw, entries, capacity, sector, last), (sector, last, capacity, plan)
                    cases.append(f'geometry/write/readback sector{sector} last{last} capacity{capacity}')
                minimum = int(plan['minimum'])
                target.write_bytes(b'\xa5' * (minimum - sector))
                report = invoke([writer, source, target, sector, 1, 2**63 - 1, 'none'])
                assert report['error'] == errno.ENOSPC and report['written'] == 0, report
                assert target.read_bytes() == b'\xa5' * (minimum - sector)
                cases.append(f'too-small prewrite refusal sector{sector} last{last}')
                for original, replacement in ((b'Optimized=True', b'Optimized=invalid'),
                        (b'UseAllSpace=True', b'UseAllSpace=False'), (b'Attributes=0', b'Attributes=1'),
                        (b'MaxPartitionCount=128', b'MaxPartitionCount=127'), (b'Name_U=', b'Name_U=!')):
                    malformed = manifest.replace(original, replacement, 1)
                    source.write_bytes(make_ffu(entries, sector=sector, disk_bytes=len(raw), manifest=malformed)[0])
                    report = invoke([probe, 'plan', source, 16 * MIB, sector])
                    assert report['error'] == errno.EBADMSG, (original, report)
                    cases.append(f'malformed {original!r} sector{sector} last{last}')
        xml, rewritten = root / 'ReAgent.xml', root / 'updated.xml'
        original = (f'<?xml version="1.0" encoding="UTF-8"?><WindowsRE version="2.0"><!--keep-->'
            f'<WinreLocation path="\\Recovery\\WindowsRE" id="7" offset="123" guid="{{{DISK_ID}}}"/>'
            f'<ImageLocation offset="456" guid="{DISK_ID}"/><Other value="preserved"/></WindowsRE>').encode()
        for encoding in ('utf-8', 'utf-16'):
            xml.write_bytes(original.decode().replace('UTF-8', encoding).encode(encoding))
            rewritten.unlink(missing_ok=True)
            report = invoke([probe, 'xml', xml, DISK_ID.bytes_le.hex(), 987654, rewritten])
            assert report['error'] == 0, report
            tree = ET.fromstring(rewritten.read_bytes())
            assert tree.find('WinreLocation').attrib == {'path': '\\Recovery\\WindowsRE', 'id': '7', 'offset': '987654', 'guid': '{' + str(DISK_ID) + '}'}
            assert tree.find('ImageLocation').get('offset') == '987654' and tree.find('Other').get('value') == 'preserved'
            assert b'<!--keep-->' in rewritten.read_bytes()
            cases.append('recovery XML ' + encoding)
        for body, error in ((original.replace(b'2.0', b'3.0'), errno.ENOTSUP),
                (original.replace(str(DISK_ID).encode(), b'bad-guid'), errno.EBADMSG),
                (b'<!DOCTYPE x [<!ENTITY a "expansion">]><WindowsRE>&a;</WindowsRE>', errno.EBADMSG),
                (b'<WindowsRE><invalid></WindowsRE>', errno.EBADMSG)):
            xml.write_bytes(body)
            rewritten.unlink(missing_ok=True)
            report = invoke([probe, 'xml', xml, DISK_ID.bytes_le.hex(), 987654, rewritten])
            assert report['error'] == error and not rewritten.exists(), report
            cases.append('recovery XML refusal ' + str(error))
        # Exercise the real provider on an owned regular file. Its data remains
        # readable and the expanded volume receives its own consistency flag.
        volume = root / 'volume.ntfs'
        volume.write_bytes(bytes(16 * MIB))
        subprocess.run([mkntfs, '-F', '-Q', '-s', '512', '-c', '4096', str(volume)], check=True, capture_output=True)
        with volume.open('r+b') as output: output.truncate(32 * MIB)
        report = invoke([probe, 'grow', volume, 16 * MIB, 32 * MIB, 512, ntfsresize])
        assert report['error'] == 0, report
        boot = volume.read_bytes()[:512]
        assert struct.unpack_from('<Q', boot, 40)[0] == (32 * MIB - 512) // 4096 * 8
        cases.append('real NTFS clean precheck, dry-run, grow, postcheck and backup boot')
        # A second call must refuse the pre-existing dirty state; never force it.
        digest = hashlib.sha256(volume.read_bytes()).digest()
        captured = (struct.unpack_from('<Q', boot, 40)[0] + 1) * 512
        report = invoke([probe, 'grow', volume, captured, 32 * MIB, 512, ntfsresize])
        assert report['error'] != 0 and hashlib.sha256(volume.read_bytes()).digest() == digest, report
        cases.append('pre-existing dirty NTFS refused without writes')
    print(json.dumps({'passed': len(cases), 'cases': cases}, indent=2))


if __name__ == '__main__':
    main()
