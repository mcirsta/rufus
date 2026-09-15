#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Qualify ISO extraction on kernel FAT32, using only a disposable QEMU USB disk."""
import argparse
import errno
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

import prepare

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from test_fat_format import copy_volume
from test_iso_extract import make_image, make_named_image, make_merged_image, check_tree

MIB = 1024 * 1024
DISK_BYTES = 64 * MIB
SECTOR_BYTES = 512
GUEST_TIMEOUT_SECONDS = 180
TOOL_TIMEOUT_SECONDS = 20
SUCCESS_CASES = ('rr', 'joliet', 'udf', 'recovery')
NAME_CASES = ('names-rr', 'names-udf')
MERGE_CASES = ('merge-rr', 'merge-udf')
EMPTY_CASES = ('invalid', 'write-fail', 'sync-fail', 'full', 'readonly')


def run(command, **kwargs):
    return subprocess.run(command, check=True, timeout=TOOL_TIMEOUT_SECONDS, **kwargs)


def digest(path):
    with path.open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def values(text):
    return {key: int(value) for key, value in re.findall(r'([A-Z_]+)=(-?\d+)', text)}


def make_disk(args, base):
    disk = base / 'usb.raw'
    with disk.open('xb') as output:
        output.truncate(DISK_BYTES)
    command = [str(args.build / 'partition_image_probe'), str(disk), str(DISK_BYTES),
               str(SECTOR_BYTES), '63', 'none']
    partition = run(command, capture_output=True, text=True)
    layout = values(partition.stdout)
    assert layout['ERROR'] == 0 and layout['FDS'] == 0, partition
    command = [str(args.build / 'fat_format_probe'), args.mkfs, str(disk),
               str(layout['OFFSET']), str(layout['SIZE']), str(SECTOR_BYTES), 'EXTRACT', 'none']
    formatted = run(command, capture_output=True, text=True)
    report = values(formatted.stdout)
    assert report['ERROR'] == 0 and report['FDS'] == 0, formatted
    (base / 'format.log').write_text(partition.stdout + partition.stderr + formatted.stdout + formatted.stderr)
    # Save every byte outside the filesystem, including both GPT copies.
    with disk.open('rb') as source:
        prefix = source.read(layout['OFFSET'])
        source.seek(layout['OFFSET'] + layout['SIZE'])
        tail = source.read()
    return layout, prefix, tail


def make_fixtures(base, mkisofs):
    fixtures = base / 'fixtures'
    fixtures.mkdir()
    data = bytes(range(256)) * (MIB // 128) + b'tail'
    entries = {'MiXeD/ș-é.txt': b'UTF-8 names', 'empty': b'', 'one': b'x',
               'boot/data.bin': data, 'ldlinux.sys': b'installer owns root loader',
               'boot/ldlinux.sys': b'keep nested loader',
               'space dir/a long filename with spaces.txt': b'long FAT name'}
    make_image(fixtures, 'TREE', entries, mkisofs, directories=['empty-dir/nested'])
    make_image(fixtures, 'UDF', entries, mkisofs, udf=True, directories=['empty-dir/nested'])
    make_image(fixtures, 'DATA', {'bad:name.txt': data}, mkisofs)
    make_image(fixtures, 'COLLISION', {'Name.txt': b'first name', 'name.txt': b'second name'}, mkisofs)
    make_image(fixtures, 'INVALID', {'bad"name.txt': b'not covered by Rufus substitutions'}, mkisofs)
    make_named_image(fixtures, 'NAMEDRR', mkisofs)
    _, named_entries, named_empty = make_named_image(fixtures, 'NAMEDUDF', mkisofs, True)
    make_merged_image(fixtures, 'MERGEDRR', mkisofs)
    make_merged_image(fixtures, 'MERGEDUDF', mkisofs, True)
    make_image(fixtures, 'ZERO', {'zero.bin': b''}, mkisofs)
    del entries['ldlinux.sys']
    return entries, data, named_entries, named_empty


def add_executable(init, binary):
    shutil.copy2(binary, init / 'usr/bin' / binary.name)
    linked = run(['ldd', str(binary)], capture_output=True, text=True).stdout
    for name in re.findall(r'(/[^\s()]+)', linked):
        library = Path(name)
        if library.is_file():
            shutil.copy2(library, init / 'usr/lib' / library.name)


def make_guest(args, base):
    # Reuse the existing module/initramfs recipe; this guest does not need the
    # desktop root filesystem or package cache. Replace its init with our test.
    root = base / 'root'
    root.mkdir()
    (root / 'lib').symlink_to('usr/lib')
    prepare.bootstrap(base, root, args.kernel)
    init = base / 'initramfs'
    for name in ('mkdir', 'rm', 'dd', 'umount'):
        add_executable(init, Path('/usr/bin') / name)
    add_executable(init, args.build / 'iso_extract_probe')
    # UDF's compressed Unicode names also use glibc's dynamically loaded
    # converters. ldd alone does not include these runtime data/modules.
    shutil.copytree('/usr/lib/gconv', init / 'usr/lib/gconv')
    (init / 'target').mkdir()
    (init / 'images').mkdir()
    for image in (base / 'fixtures').glob('*.iso'):
        shutil.copy2(image, init / 'images' / image.name)
    prepare.write(init, 'init', (prepare.HERE / 'guest-fat-extract').read_text(), 0o755)
    names = run(['find', '.', '-print0'], cwd=init, capture_output=True).stdout
    with (base / 'initramfs.cpio').open('wb') as output:
        run(['cpio', '--null', '-o', '-H', 'newc', '--owner=0:0'], cwd=init, input=names, stdout=output)


def run_guest(args, base):
    serial = base / 'serial.log'
    command = ['qemu-system-x86_64', '-accel', args.accel,
               '-cpu', 'host' if args.accel == 'kvm' else 'max', '-m', '512',
               '-display', 'none', '-monitor', 'none', '-serial', 'file:' + str(serial),
               '-no-reboot', '-nic', 'none', '-kernel', str(base / 'vmlinuz'),
               '-initrd', str(base / 'initramfs.cpio'),
               '-append', 'console=ttyS0 panic=-1 rufus.fat_fixture=1',
               '-smbios', 'type=1,manufacturer=RufusTest,product=RufusFATExtractFixture',
               '-device', 'qemu-xhci,id=xhci',
               '-drive', 'if=none,id=stick,format=raw,file=' + str(base / 'usb.raw'),
               '-device', 'usb-storage,drive=stick,removable=on,serial=RUFUS-FAT-TEST']
    (base / 'qemu-command.json').write_text(json.dumps(command, indent=2) + '\n')
    with (base / 'qemu.log').open('wb') as log:
        # subprocess.run kills and reaps QEMU on timeout, before any host readback.
        subprocess.run(command, check=True, timeout=GUEST_TIMEOUT_SECONDS, stdout=log, stderr=subprocess.STDOUT)
    text = serial.read_text(errors='replace')
    for marker in ('RUFUS_FAT_MOUNTED', 'RUFUS_FAT_UNMOUNTED', 'RUFUS_FAT_EXIT 0'):
        assert marker in text, (marker, serial)
    return text


def check_reports(text, entries, data, named_entries):
    expected_errors = dict.fromkeys(SUCCESS_CASES + NAME_CASES + MERGE_CASES, 0)
    expected_errors.update({'collision': 0, 'replace-zero': 0, 'invalid': errno.EINVAL,
                            'write-fail': errno.ENOSPC, 'cancel': errno.ECANCELED,
                            'sync-fail': errno.EIO, 'full': errno.ENOSPC, 'readonly': errno.EROFS})
    reports = {}
    for name, error in expected_errors.items():
        pattern = rf'RUFUS_FAT_CASE {name}\r?\n(.*?)RUFUS_FAT_END {name}\r?\n'
        matches = re.findall(pattern, text, re.S)
        assert len(matches) == 1, (name, matches)
        report = values(matches[0])
        assert report['ERROR'] == error and report['FDS'] == 0, (name, report)
        assert report['WRITTEN'] == report['ACTUAL'], (name, report)
        assert report['MAX_READ'] <= MIB and report['MAX_WRITE'] <= MIB, (name, report)
        if report['FLAGS']:
            for flag in (os.O_NOFOLLOW, os.O_CLOEXEC):
                assert report['FLAGS'] & flag, (name, report)
            assert not report['FLAGS'] & os.O_TRUNC, (name, report)
            if report['FLAGS'] & os.O_CREAT:
                assert report['FLAGS'] & os.O_EXCL, (name, report)
            else:
                assert report['FLAGS'] & os.O_NONBLOCK, (name, report)
        if name in SUCCESS_CASES:
            assert report['FILES'] == len(entries) and report['SKIPPED'] == 1, (name, report)
            assert report['WRITTEN'] == sum(map(len, entries.values())), (name, report)
        elif name in NAME_CASES:
            assert report['FILES'] == len(named_entries) and report['DIRECTORIES'] == 4, (name, report)
            assert report['WRITTEN'] == sum(map(len, named_entries.values())), (name, report)
        elif name in MERGE_CASES:
            assert report['FILES'] == report['DIRECTORIES'] == 4, (name, report)
            copied_bytes = sum(map(len, (b'keep a', b'keep b', b'old content with a tail', b'new')))
            assert report['WRITTEN'] == copied_bytes, (name, report)
            assert report['REPLACEMENTS'] == report['TRUNCATIONS'] == 1, (name, report)
        elif name == 'collision':
            assert report['FILES'] == 2 and report['WRITTEN'] == len(b'first namesecond name'), report
            assert report['REPLACEMENTS'] == report['TRUNCATIONS'] == 1, report
        elif name == 'replace-zero':
            assert report['FILES'] == 1 and report['WRITTEN'] == 0, report
            assert report['REPLACEMENTS'] == report['TRUNCATIONS'] == 1, report
        else:
            assert report['FILES'] == 0, (name, report)
        if name == 'full':
            assert 0 < report['WRITTEN'] < len(data), report
        if name == 'cancel':
            assert report['RETRY'] == report['RETRY_FDS'] == 0, report
        reports[name] = report
    return reports


def inspect_disk(args, base, layout, prefix, tail, entries, data, named_entries, named_empty):
    disk = base / 'usb.raw'
    assert disk.stat().st_size == DISK_BYTES
    with disk.open('rb') as source:
        assert source.read(len(prefix)) == prefix, 'GPT/prefix changed'
        source.seek(layout['OFFSET'] + layout['SIZE'])
        assert source.read() == tail, 'GPT/tail changed'
    volume = base / 'volume.raw'
    copy_volume(disk, volume, layout['OFFSET'], layout['SIZE'])
    checked = run([args.fsck, '-n', str(volume)], capture_output=True)
    (base / 'fsck.log').write_bytes(checked.stdout + checked.stderr)
    extracted = base / 'readback'
    extracted.mkdir()
    run([args.mcopy, '-s', '-i', str(volume), '::*', str(extracted)], capture_output=True)
    expected_dirs = set(SUCCESS_CASES + NAME_CASES + MERGE_CASES + EMPTY_CASES +
                        ('collision', 'cancel', 'replace-zero'))
    assert {path.name for path in extracted.iterdir()} == expected_dirs
    assert all(path.is_dir() for path in extracted.iterdir())
    for name in SUCCESS_CASES:
        check_tree(extracted / name, entries, ['empty-dir/nested'])
    for name in NAME_CASES:
        check_tree(extracted / name, named_entries, named_empty)
    for name in MERGE_CASES:
        check_tree(extracted / name, {'part_dir/only-a.txt': b'keep a', 'part_dir/only-b.txt': b'keep b',
                                     'part_dir/same.txt': b'new'}, ['part_dir/empty'])
    for name in EMPTY_CASES:
        check_tree(extracted / name, {})
    # FAT keeps the first entry's spelling, while the later source supplies its bytes.
    check_tree(extracted / 'collision', {'name.txt': b'first name'})
    check_tree(extracted / 'replace-zero', {'zero.bin': b''})
    check_tree(extracted / 'cancel', {'bad_name.txt': data})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--kernel', required=True, help='Installed kernel with matching modules')
    parser.add_argument('--mkisofs', required=True)
    parser.add_argument('--mkfs', default='/usr/bin/mkfs.fat')
    parser.add_argument('--fsck', default='/usr/bin/fsck.fat')
    parser.add_argument('--mcopy', default='/usr/bin/mcopy')
    parser.add_argument('--accel', choices=['kvm', 'tcg'], default='kvm')
    parser.add_argument('output', type=Path, help='New output directory; must not exist')
    args = parser.parse_args()
    base = args.output.resolve()
    args.build = args.build.resolve()
    if any(char in str(base) for char in ',\n\r'):
        parser.error('Output path must not contain QEMU option separators')
    for name in ('iso_extract_probe', 'partition_image_probe', 'fat_format_probe'):
        if not (args.build / name).is_file():
            parser.error('Build the normal CMake test targets first: ' + name)
    base.mkdir()
    layout, prefix, tail = make_disk(args, base)
    entries, data, named_entries, named_empty = make_fixtures(base, args.mkisofs)
    make_guest(args, base)
    inputs = list((base / 'fixtures').glob('*.iso'))
    inputs += [args.build / name for name in ('iso_extract_probe', 'partition_image_probe', 'fat_format_probe')]
    inputs += [base / 'vmlinuz', base / 'initramfs.cpio', base / 'usb.raw']
    manifest = {'kernel': args.kernel, 'accel': args.accel, 'layout': layout,
                'qemu': run(['qemu-system-x86_64', '--version'], capture_output=True, text=True).stdout,
                'inputs': {str(path): digest(path) for path in inputs}}
    (base / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print('Running FAT32 extraction guest:', base, flush=True)
    text = run_guest(args, base)
    reports = check_reports(text, entries, data, named_entries)
    inspect_disk(args, base, layout, prefix, tail, entries, data, named_entries, named_empty)
    for path in inputs:
        if path.name != 'usb.raw':
            assert digest(path) == manifest['inputs'][str(path)], path
    (base / 'results.json').write_text(json.dumps(reports, indent=2) + '\n')
    print(f'PASS: {len(reports)} mounted FAT32 cases; byte/name readback, fsck and GPT guards passed.', flush=True)


if __name__ == '__main__':
    main()
