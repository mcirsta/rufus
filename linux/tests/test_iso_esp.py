#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Grouped ESP/GPT metadata and fresh ISO preflight; no hardware access."""
import errno
import json
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
import uuid
import zlib

MIB = 1024 * 1024
SECTOR = 512


def efi_header():
    header = bytearray(512); header[:2] = b'MZ'
    struct.pack_into('<I', header, 60, 128); header[128:132] = b'PE\0\0'
    for offset, value in ((132, 0x8664), (148, 240), (152, 0x20b), (220, 10)):
        struct.pack_into('<H', header, offset, value)
    return header


def inspect(path, esp, persistence, cluster):
    size = path.stat().st_size
    with path.open('rb') as stream:
        stream.seek(512); primary = bytearray(stream.read(512))
        stream.seek(size - 512); secondary = bytearray(stream.read(512))
        tables = []
        for header in (primary, secondary):
            expected = struct.unpack_from('<I', header, 16)[0]; struct.pack_into('<I', header, 16, 0)
            assert zlib.crc32(header[:92]) == expected
            location = struct.unpack_from('<Q', header, 72)[0]
            stream.seek(location * 512); table = stream.read(16384)
            assert zlib.crc32(table) == struct.unpack_from('<I', header, 88)[0]
            tables.append(table)
        assert tables[0] == tables[1]
        table = tables[0]
        assert uuid.UUID(bytes_le=table[:16]) == uuid.UUID('c12a7328-f81f-11d2-ba4b-00a0c93ec93b' if esp
            else 'ebd0a0a2-b9e5-4433-87c0-68b6b72699c7')
        name = table[56:128].decode('utf-16le').rstrip('\0')
        assert name == ('EFI System Partition' if esp else 'Main Data Partition')
        assert struct.unpack_from('<Q', table, 48)[0] == 0
        begin, end = struct.unpack_from('<QQ', table, 32)
        assert begin == 2048 and (end - begin + 1) * 512 % cluster == 0
        if persistence:
            assert uuid.UUID(bytes_le=table[128:144]) == uuid.UUID('0fc63daf-8483-4772-8e79-3d69d8477de4')
            second_begin, second_end = struct.unpack_from('<QQ', table, 128 + 32)
            assert second_begin > end and (second_end - second_begin + 1) * 512 >= persistence
        assert table[(256 if persistence else 128):] == bytes(16384 - (256 if persistence else 128))


def main():
    partition_probe, source_probe = sys.argv[1:]
    cases = []
    with tempfile.TemporaryDirectory(prefix='rufus-iso-esp-') as temporary:
        root = Path(temporary); disk = root / 'disk.img'
        for esp in (False, True):
            for persistence in (0, 256 * MIB):
                for cluster in (512, 4096, 65536):
                    with disk.open('wb') as stream: stream.truncate(512 * MIB)
                    text = subprocess.check_output([partition_probe, str(disk), str(disk.stat().st_size),
                        str(cluster), '63', 'none', '0', 'esp' if esp else 'data', str(persistence)], text=True)
                    report = {key: int(value) for key, value in re.findall(r'([A-Z_]+)=(-?\d+)', text)}
                    assert report['ERROR'] == 0 and report['FDS'] == 0 and report['POSITION'] == 17 and report['FLAGS'] == 1, report
                    inspect(disk, esp, persistence, cluster)
                    cases.append(f'GPT esp={esp} persistence={persistence} layout cluster={cluster}, both CRCs/types/names/attributes/extents')
        text = subprocess.check_output([partition_probe, str(disk), str(disk.stat().st_size), '512', '63',
            'none', str(MIB), 'esp', '0'], text=True)
        assert f'ERROR={errno.EINVAL} STARTED=0' in text
        cases.append('ESP/UEFI:NTFS conflict prewrite refusal')
        tree = root / 'tree'; (tree / 'EFI/BOOT').mkdir(parents=True)
        binary = tree / 'EFI/BOOT/BOOTX64.EFI'; binary.write_bytes(efi_header())
        (tree / 'proof.txt').write_text('Rufus ISO ESP independent fixture\n')
        for present in (True, False):
            if not present: binary.unlink()
            image = root / 'source.iso'
            subprocess.run(['bsdtar', '--format=iso9660', '-cf', str(image), '-C', str(tree), '.'], check=True, capture_output=True)
            for bits, expected_cluster in ((16, 1024), (32, 512)):
                command = [source_probe, str(image), str(64 * MIB), '4096', 'RUFUS', f'esp{bits}']
                result = subprocess.run(command, capture_output=True, text=True)
                report = json.loads(result.stdout)
                assert report['error'] == (0 if present else errno.ENOTSUP), (present, bits, report)
                if present:
                    assert report['formatCluster'] == expected_cluster and report['choices'] == [1, 2], report
                cases.append(f'fresh source regular EFI={present} FAT{bits}, default format cluster and ESP choice')
    print(json.dumps({'passed': len(cases), 'cases': cases, 'temporary_media_removed': True}, indent=2))


if __name__ == '__main__':
    main()
