# SPDX-License-Identifier: GPL-3.0-or-later
"""Unchanged NTFS record to prepared legacy loader, without mounts/root."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
from types import SimpleNamespace

from winpe_bios import prepare_root, run, digest, disk_identity, boot
from test_mbr_image import extent, invoke as create_mbr, inspect_mbr
from test_ntfs_format import invoke as format_ntfs
from test_ntfs_boot import original_code

MIB = 1024 * 1024
DISK_BYTES = 65 * MIB
CLUSTER_BYTES = 4096
SECTOR_BYTES = 512
CODE_OFFSET = 0x54
LABEL = 'Rufus test'


def make_disk(args, work, output, root, uses_minint, loader_name):
    disk = work / 'usb.raw'
    with disk.open('xb') as target:
        target.truncate(DISK_BYTES)
    code = 'rufus' if uses_minint else 'rufus-masquerade'
    mbr = create_mbr(str(args.build / 'mbr_image_probe'), str(disk), DISK_BYTES,
                     cluster=CLUSTER_BYTES, file_system='ntfs', boot_code=code)
    inspect_mbr(disk, DISK_BYTES, cluster=CLUSTER_BYTES, file_system='ntfs', boot_code=code)
    offset, size = extent(DISK_BYTES, CLUSTER_BYTES)
    volume = work / 'volume.raw'
    with volume.open('xb') as target:
        target.truncate(size)
    formatter = SimpleNamespace(probe=args.build / 'ntfs_format_probe', mkfs='/usr/bin/mkntfs')
    format_ntfs(formatter, volume, size=size, offset=offset, cluster=CLUSTER_BYTES, label=LABEL)
    before = volume.read_bytes()
    first = original_code('br_ntfs_0x0.h')
    original = original_code('br_ntfs_0x54.h')
    record_bytes = CODE_OFFSET + len(original)
    result = run([args.build / 'ntfs_boot_probe', volume, offset, size, CLUSTER_BYTES, 'none'],
                 output, 'ntfs-record')
    values = {key.decode(): int(value) for key, value in re.findall(rb'([A-Z_]+)=(-?\d+)', result)}
    assert values['ERROR'] == 0 and values['SYNC'] == 0 and values['BYTES'] == record_bytes
    assert values['FDS'] == 0 and values['FLAGS'] == 1 and values['OFFSET'] == 17
    expected = bytearray(before)
    expected[:len(first)] = first
    expected[CODE_OFFSET:record_bytes] = original
    assert volume.read_bytes() == expected, 'Record changed BPB/backup/other volume data'
    files = {path.name: path for path in root.iterdir() if path.is_file()}
    if loader_name == 'NTLDR':
        files['NTLDR'] = files.pop('BOOTMGR')
    elif loader_name is None:
        files.pop('BOOTMGR')
    for name, path in files.items():
        run([args.build / 'ntfs_file_fixture', 'put', volume, name, path], output, 'put-' + name)
    identity = digest(volume)
    for name, path in files.items():
        actual = run([args.build / 'ntfs_file_fixture', 'get', volume, name], output, 'read-' + name)
        assert actual == path.read_bytes(), name
        (output / ('read-' + name + '.log')).write_text(hashlib.sha256(actual).hexdigest() + '\n')
    assert digest(volume) == identity, 'Read-only file lookup changed the volume'
    run(['ntfsck', '-n', volume], output, 'ntfsck')
    info = run(['ntfsinfo', '-m', volume], output, 'ntfsinfo')
    assert b'Volume Version: 3.1' in info and b'Volume Flags: 0x0000' in info
    with volume.open('rb') as source:
        assert source.read(record_bytes) == expected[:record_bytes]
        source.seek(-SECTOR_BYTES, os.SEEK_END)
        assert source.read(SECTOR_BYTES) == before[-SECTOR_BYTES:]
        source.seek(0)
        with disk.open('r+b') as target:
            target.seek(offset)
            shutil.copyfileobj(source, target, MIB)
            target.flush()
            os.fsync(target.fileno())
    inspect_mbr(disk, DISK_BYTES, cluster=CLUSTER_BYTES, file_system='ntfs', boot_code=code)
    with disk.open('rb') as target:
        target.seek(offset)
        assert hashlib.sha256(target.read(size)).hexdigest() == identity
    (output / 'disk-readback.json').write_text(json.dumps({
        'mbr': mbr, 'pbr': values, 'files': {name: digest(path) for name, path in files.items()},
        'volume_sha256': identity, 'original_record_code': True, 'bpb_backup_unchanged': True,
        'filesystem_clean': True, 'source_loader_alias': False,
        'loader_name_control': loader_name, 'disk_identity': disk_identity(disk)}, indent=2) + '\n')
    volume.unlink()
    return disk


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.build = args.build.resolve()
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    results = []
    cases = [('ntfs-i386', 'i386', False, 'BOOTMGR'), ('ntfs-amd64', 'amd64', False, 'BOOTMGR'),
             ('ntfs-minint', 'minint', True, 'BOOTMGR'), ('ntfs-ntldr-only', 'i386', False, 'NTLDR'),
             ('ntfs-no-loader', 'i386', False, None)]
    for name, family, uses_minint, loader_name in cases:
        output = args.output / name
        output.mkdir()
        with tempfile.TemporaryDirectory(prefix='rufus-winpe-ntfs-') as temporary:
            work = Path(temporary)
            root = prepare_root(args, work, output, uses_minint, family)
            disk = make_disk(args, work, output, root, uses_minint, loader_name)
            result = boot(disk, output, 0x80 if uses_minint else 0x81,
                          'BOOTMGR is missing' if loader_name is None else None)
            result.update({'case': name, 'family': family, 'uses_minint': uses_minint,
                           'loader_name_control': loader_name})
            results.append(result)
            print('PASS', name, result, flush=True)
    (args.output / 'result.json').write_text(json.dumps({
        'cases': results, 'windows_setup_qualified': False, 'production_route_enabled': False,
        'ntfs_record_and_bpb_unmodified': True, 'temporary_disks_removed': True}, indent=2) + '\n')


if __name__ == '__main__':
    main()
