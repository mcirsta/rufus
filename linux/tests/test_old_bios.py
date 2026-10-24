#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""One old-BIOS layout corpus; unchanged MBR I/O fault matrices are reused."""
import errno
import json
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile

MIB = 1024 * 1024
SECTOR = 512
TRACK = 63 * SECTOR


def invoke(probe, path, size, cluster, filesystem, boot, payload=0, persistence=0, fault='none', error=0):
    result = subprocess.check_output([probe, str(path), str(size), str(cluster), '63', '512',
        'prepare', fault, str(filesystem), boot, str(payload), 'old-bios', str(persistence)], text=True)
    report = {key: int(value) for key, value in re.findall(r'([A-Z_]+)=(-?\d+)', result)}
    assert report['ERROR'] == error, (filesystem, boot, report)
    assert report['FDS'] == 0 and report['POSITION'] == 17 and report['FLAGS'] == 1, report
    return report


def main():
    probe = sys.argv[1]; cases = []
    with tempfile.TemporaryDirectory(prefix='rufus-old-bios-') as temporary:
        path = Path(temporary) / 'disk.img'
        for boot in ('syslinux', 'rufus', 'win7', 'grub2', 'grub4dos', 'rufus-masquerade'):
            filesystems = ('16', '32', 'ntfs', 'ext2', 'ext3') if boot == 'grub2' else ('16', '32', 'ntfs')
            for filesystem in filesystems:
                cluster = 256 if filesystem.startswith('ext') else 2048 if filesystem == '16' else 4096
                size = 320 * MIB if filesystem.startswith('ext') else 64 * MIB
                with path.open('wb') as stream: stream.truncate(size)
                report = invoke(probe, path, size, cluster, filesystem, boot)
                expected_offset = (TRACK + cluster - 1) // cluster * cluster * 2
                compat = (size - TRACK) // TRACK * TRACK
                expected_size = (compat - expected_offset) // TRACK * TRACK
                if cluster % SECTOR == 0: expected_size = expected_size // cluster * cluster
                assert (report['OFFSET'], report['SIZE']) == (expected_offset, expected_size)
                assert (report['COMPAT_OFFSET'], report['COMPAT_SIZE']) == (compat, TRACK)
                with path.open('rb') as stream:
                    mbr = stream.read(512)
                    stream.seek(compat); assert stream.read(TRACK) == bytes(TRACK)
                assert mbr[446] == (0x81 if boot == 'rufus-masquerade' else 0x80)
                assert mbr[462] == 0 and mbr[466] == 0xea and mbr[478:510] == bytes(32)
                assert struct.unpack_from('<II', mbr, 470) == (compat // SECTOR, 63)
                cases.append(f'{boot}/{filesystem}: original alignment, active data, inactive compatibility track, clear/readback')
        for payload, persistence in ((0, 256 * MIB), (1048576, 0), (1048576, 256 * MIB)):
            size = 512 * MIB
            with path.open('wb') as stream: stream.truncate(size)
            report = invoke(probe, path, size, 4096, 'ntfs', 'grub2', payload, persistence)
            with path.open('rb') as stream: mbr = stream.read(512)
            entries = [mbr[446 + i * 16:462 + i * 16] for i in range(4)]
            types = [entry[4] for entry in entries if entry[4]]
            assert types == [7] + ([0x83] if persistence else []) + ([0xef] if payload else [0xea]), types
            assert all(entry[0] == 0 for entry in entries[1:])
            assert bool(report['COMPAT_SIZE']) == (payload == 0)
            cases.append(f'NTFS priority payload={payload} persistence={persistence}')
        with path.open('wb') as stream: stream.truncate(64 * MIB)
        report = invoke(probe, path, 64 * MIB, 4096, '32', 'grub2', fault='bad-boot-type', error=errno.EBADMSG)
        assert report['STARTED'] and report['SYNCS'] > 0
        cases.append('compatibility table tampering fails readback after sync')
        report = invoke(probe, path, 64 * MIB, 4096, 'exfat', 'non-bootable', error=errno.ENOTSUP)
        assert not report['STARTED'] and not report['WRITES']
        cases.append('non-BIOS compatibility request refuses before writes')
    print(json.dumps({'passed': len(cases), 'cases': cases, 'temporary_media_removed': True}, indent=2))


if __name__ == '__main__':
    main()
