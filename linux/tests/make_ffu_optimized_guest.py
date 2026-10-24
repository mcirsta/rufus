#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build two small independent optimized FFUs with real clean NTFS filesystems."""
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
from ffu_fixture import make_ffu
from test_ffu_optimized import MIB, DISK_ID, BASIC, RECOVERY, optimized_fixture


def main():
    fixture, destination = Path(sys.argv[1]), Path(sys.argv[2])
    destination.mkdir()
    records = []
    with tempfile.TemporaryDirectory(prefix='rufus-ffu-ntfs-') as temporary:
        root = Path(temporary)
        payload = root / 'payload'
        payload.write_bytes(bytes(range(256)) * 1024)
        marker = root / 'marker'
        marker.write_bytes(b'FFU optimized recovery fixture; not a Windows kernel\n')
        for name, captured, recovery_offset in (('smaller', 80 * MIB, 48 * MIB), ('larger', 56 * MIB, 36 * MIB)):
            volumes = {}
            xml = root / 'ReAgent.xml'
            xml.write_text(f'<WindowsRE version="2.0"><WinreLocation path="\\Recovery\\WindowsRE" id="0" '
                f'offset="{recovery_offset}" guid="{{{DISK_ID}}}"/><ImageLocation path="" id="0" '
                f'offset="{recovery_offset}" guid="{DISK_ID}"/><Other value="preserved"/></WindowsRE>')
            for ordinal, length, directories, files in (
                    (1, 16 * MIB, ['Windows', 'Windows/System32', 'Windows/System32/Recovery'],
                     [('Windows/System32/ntoskrnl.exe', marker), ('Windows/System32/Recovery/ReAgent.xml', xml), ('payload.bin', payload)]),
                    (2, 12 * MIB, ['Recovery', 'Recovery/WindowsRE'], [('Recovery/WindowsRE/ReAgent.xml', xml), ('payload.bin', payload)])):
                volume = root / 'volume.ntfs'
                volume.write_bytes(bytes(length))
                subprocess.run(['mkntfs', '-F', '-Q', '-s', '512', '-c', '4096', str(volume)], capture_output=True, check=True)
                for directory in directories:
                    subprocess.run([fixture, 'mkdir', volume, directory], capture_output=True, check=True)
                for path, source in files:
                    subprocess.run([fixture, 'put', volume, path, source], capture_output=True, check=True)
                volumes[ordinal] = volume.read_bytes()
                subprocess.run(['ntfsresize', '--info', '--no-action', str(volume)], capture_output=True, check=True)
            definitions = [(0, MIB, MIB, BASIC, 'System'), (3, 8 * MIB, 16 * MIB, BASIC, 'Windows Ω'),
                           (7, recovery_offset, 12 * MIB, RECOVERY, 'Recovery')]
            raw, manifest, entries = optimized_fixture(volumes=volumes, size=captured, definitions=definitions)
            # Omit all-zero free areas as optimized captures do. Keep every
            # nonzero filesystem/partition block and both trailing descriptors.
            entries = [entry for entry in entries if any(entry[0]) or entry[1][0][0] == 2
                       or entry[1][0][1] == captured // (128 * 1024) - 1]
            packed = make_ffu(entries, disk_bytes=captured, manifest=manifest, version=3, algorithm=2)[0]
            (destination / (name + '.ffu')).write_bytes(packed)
            delta = 64 * MIB - captured
            records.append({'name': name, 'captured': str(captured), 'sha256': hashlib.sha256(raw).hexdigest(),
                'sourceSha256': hashlib.sha256(packed).hexdigest(), 'minimum': str(captured + 24 * MIB - recovery_offset),
                'osOffset': str(8 * MIB), 'osBytes': str(recovery_offset + delta - 8 * MIB),
                'recoveryOffset': str(recovery_offset + delta), 'recoveryBytes': str(12 * MIB),
                'payloadSha256': hashlib.sha256(payload.read_bytes()).hexdigest(), 'diskId': str(DISK_ID)})
    (destination / 'cases.json').write_text(json.dumps(records, indent=2) + '\n')
    print(json.dumps({'cases': len(records), 'bytes': sum(path.stat().st_size for path in destination.iterdir())}))


if __name__ == '__main__':
    main()
