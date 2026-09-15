#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Qualify retained GPT block descriptors in disposable 512-byte and 4K USB guests."""
import argparse
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess

import prepare
from fat_extract import add_executable, digest, run, values
from test_partition_image import make_image, inspect_gpt, check_preserved
from test_fat_format import copy_volume

DISK_BYTES = 64 * 1024 * 1024
GUEST_TIMEOUT_SECONDS = 120
CASES = ('missing-device', 'wrong-device', 'wrong-capacity', 'cancel-before',
         'cancel-before-write', 'native-readonly', 'readonly-descriptor',
         'sync-fail', 'cancel-after-write', 'success')


def make_guest(args, base):
    root = base / 'root'
    root.mkdir()
    (root / 'lib').symlink_to('usr/lib')
    prepare.bootstrap(base, root, args.kernel)
    init = base / 'initramfs'
    (init / 'target').mkdir()
    for binary in (args.build / 'test_gpt_guest', Path('/usr/bin/mkfs.fat')):
        add_executable(init, binary)
    shutil.copytree('/usr/lib/gconv', init / 'usr/lib/gconv')
    prepare.write(init, 'etc/rufus-gpt-fixture', 'RUFUS-GPT-TEST\n')
    prepare.write(init, 'init', (prepare.HERE / 'guest-gpt').read_text(), 0o755)
    names = run(['find', '.', '-print0'], cwd=init, capture_output=True).stdout
    with (base / 'initramfs.cpio').open('wb') as output:
        run(['cpio', '--null', '-o', '-H', 'newc', '--owner=0:0'], cwd=init, input=names, stdout=output)


def run_guest(args, base, disk, sector):
    serial = base / f'serial-{sector}.log'
    command = ['qemu-system-x86_64', '-accel', args.accel,
               '-cpu', 'host' if args.accel == 'kvm' else 'max', '-m', '256',
               '-display', 'none', '-monitor', 'none', '-serial', 'file:' + str(serial),
               '-no-reboot', '-nic', 'none', '-kernel', str(base / 'vmlinuz'),
               '-initrd', str(base / 'initramfs.cpio'),
               '-append', 'console=ttyS0 panic=-1 rufus.gpt_fixture=1',
               '-smbios', 'type=1,manufacturer=RufusTest,product=RufusGPTFixture',
               '-device', 'qemu-xhci,id=xhci',
               '-drive', 'if=none,id=stick,format=raw,file=' + str(disk),
               '-device', 'usb-storage,drive=stick,removable=on,serial=RUFUS-GPT-TEST,'
               f'logical_block_size={sector},physical_block_size={sector}']
    (base / f'qemu-command-{sector}.json').write_text(json.dumps(command, indent=2) + '\n')
    with (base / f'qemu-{sector}.log').open('wb') as log:
        subprocess.run(command, check=True, timeout=GUEST_TIMEOUT_SECONDS, stdout=log, stderr=subprocess.STDOUT)
    text = serial.read_text(errors='replace')
    for marker in ('RUFUS_GPT_DONE', 'RUFUS_GPT_EXIT 0', f'SECTOR={sector} CAPACITY={DISK_BYTES}'):
        assert marker in text, (marker, serial)
    reports = {name: values(report) for name, report in re.findall(r'RUFUS_GPT_CASE (\S+) ([^\r\n]+)', text)}
    expected = CASES if sector == 512 else ('native-4k',)
    assert set(reports) == set(expected), reports
    if sector == 512:
        assert 'RUFUS_GPT_FORMATTER_BUSY' in text, serial
        assert re.search(r'mkfs.fat: unable to open /proc/self/fd/\d+: Device or resource busy', text), serial
        for marker in ('RUFUS_GPT_FORMATTER_HANDOFF', 'RUFUS_GPT_FORMATTER_MOUNTED_REFUSED', 'RUFUS_GPT_REACQUIRED'):
            assert marker in text, (marker, serial)
        assert 'unable to open /dev/sda1: Device or resource busy' in text, serial
    return reports


def inspect_formatted_partition(args, base, disk, partition):
    offset, size = partition['OFFSET'], partition['SIZE']
    with disk.open('rb') as source:
        source.seek(offset)
        boot = source.read(512)
        sector, spc, reserved, fats = struct.unpack_from('<HBHB', boot, 11)
        assert sector == 512 and spc == 1 and fats == 2
        assert boot[510:] == b'\x55\xaa' and boot[82:90] == b'FAT32   '
        assert struct.unpack_from('<I', boot, 28)[0] == offset // 512
        assert struct.unpack_from('<I', boot, 67)[0] == 0x1234abcd
        assert boot[71:82] == b'RUFUS TEST '
        sectors, fat_size = struct.unpack_from('<II', boot, 32)
        assert 0 <= size - sectors * sector < 63 * sector
        assert (sectors - reserved - fats * fat_size) // spc >= 65525
        source.seek(offset + 6 * sector)
        assert source.read(512) == boot
    volume = base / 'volume.raw'
    copy_volume(disk, volume, offset, size)
    checked = run([args.fsck, '-n', str(volume)], capture_output=True)
    (base / 'fsck.log').write_bytes(checked.stdout + checked.stderr)
    listing = run([args.mdir, '-i', str(volume), '::'], capture_output=True)
    (base / 'mdir.log').write_bytes(listing.stdout + listing.stderr)
    assert b'No files' in listing.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--kernel', required=True)
    parser.add_argument('--accel', choices=['kvm', 'tcg'], default='kvm')
    parser.add_argument('--sfdisk', default='/usr/bin/sfdisk')
    parser.add_argument('--fsck', default='/usr/bin/fsck.fat')
    parser.add_argument('--mdir', default='/usr/bin/mdir')
    parser.add_argument('output', type=Path, help='New output directory; must not exist')
    args = parser.parse_args()
    args.build = args.build.resolve()
    base = args.output.resolve()
    if any(char in str(base) for char in ',\n\r'):
        parser.error('Output path must not contain QEMU option separators')
    if not (args.build / 'test_gpt_guest').is_file():
        parser.error('Build with RUFUS_BUILD_VM_TEST=ON first')
    base.mkdir()
    make_guest(args, base)
    inputs = [base / 'vmlinuz', base / 'initramfs.cpio', args.build / 'test_gpt_guest', Path('/usr/bin/mkfs.fat')]
    manifest = {'kernel': args.kernel, 'accel': args.accel,
                'qemu': run(['qemu-system-x86_64', '--version'], capture_output=True, text=True).stdout,
                'inputs': {str(path): digest(path) for path in inputs}, 'disks': {}}
    results = {}
    for sector in (512, 4096):
        disk, segments = make_image(base, f'usb-{sector}.raw', DISK_BYTES)
        original = digest(disk)
        manifest['disks'][str(sector)] = original
        (base / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
        print(f'Running {sector}-byte GPT USB guest: {base}', flush=True)
        results[str(sector)] = run_guest(args, base, disk, sector)
        if sector == 512:
            inspect_gpt(args, disk, DISK_BYTES, 512)
            partition = results[str(sector)]['success']
            check_preserved(disk, DISK_BYTES, segments, (partition['OFFSET'], partition['SIZE']))
            inspect_formatted_partition(args, base, disk, partition)
        else:
            assert digest(disk) == original, 'Rejected 4K device changed'
    for path in inputs:
        assert digest(path) == manifest['inputs'][str(path)], path
    (base / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
    print('PASS: 11 GPT block cases, retained ownership, independent GPT CRC/layout readback; '
          'CLI handoff, mounted-volume refusal and FAT32 readback passed.', flush=True)


if __name__ == '__main__':
    main()
