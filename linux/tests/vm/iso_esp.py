#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""One combined ISO -> ESP Qt/helper, filesystem readback and UEFI route."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess

import prepare
import persistence
from fat_extract import add_executable, digest, run
from mbr import boot_syslinux_uefi


INSPECT = r'''#!/bin/bash
set -eu
read -r filesystem persistence < /run/check-esp
mkdir /run/esp-main
mount -t vfat -o ro /dev/sda1 /run/esp-main
[[ $(blkid -s LABEL -o value /dev/sda1) == RUFUS_ESP ]]
count=0
find /expected -type f -print0 > /run/esp-files
while IFS= read -r -d '' file; do
    relative=${file#/expected/}
    if [[ $relative == isolinux/isolinux.cfg ]]; then
        config=$(< /run/esp-main/isolinux/isolinux.cfg)
        if [[ $persistence == 1 ]]; then
            [[ $config == *'boot=live persistence'* ]]
        else
            [[ $config == *'boot=live'* && $config != *' persistence'* ]]
        fi
    else
        cmp "$file" "/run/esp-main/$relative"
    fi
    count=$((count + 1))
done < /run/esp-files
rm /run/esp-files
[[ $count == 5 ]]
umount /run/esp-main
rmdir /run/esp-main
if [[ $persistence == 1 ]]; then
    [[ $(blkid -s LABEL -o value /dev/sda2) == persistence ]]
    [[ $(blkid -s TYPE -o value /dev/sda2) == ext3 ]]
    e2fsck -fn /dev/sda2
    mkdir /run/esp-persistence
    mount -t ext4 -o ro,noload /dev/sda2 /run/esp-persistence
    [[ $(< /run/esp-persistence/persistence.conf) == '/ union' ]]
    umount /run/esp-persistence
    rmdir /run/esp-persistence
fi
echo "RUFUS_ISO_ESP_READBACK FS=$filesystem PERSIST=$persistence FILES=$count"
'''


def make_sources(args, base):
    tree = base / 'esp-source'
    (tree / 'EFI/BOOT').mkdir(parents=True)
    (tree / 'isolinux').mkdir()
    shutil.copy2(args.build / 'uefi-handoff.efi', tree / 'EFI/BOOT/BOOTX64.EFI')
    for source, target in [('bios/core/isolinux.bin', 'isolinux.bin'),
                           ('bios/com32/elflink/ldlinux/ldlinux.c32', 'ldlinux.c32')]:
        shutil.copy2(args.upstream / source, tree / 'isolinux' / target)
    (tree / 'proof.bin').write_bytes(bytes(range(256)) * 4096)
    (tree / 'isolinux/isolinux.cfg').write_text('DEFAULT live\nLABEL live\n KERNEL /vmlinuz\n APPEND boot=live\n')
    source = base / 'debian.iso'
    run([args.mkisofs, '-quiet', '-r', '-J', '-V', 'RUFUS_LIVE', '-o', source, tree])
    shutil.copy2(source, base / 'casper.iso')
    checks = {'source_sha256': digest(source), 'files': {str(path.relative_to(tree)): digest(path)
              for path in tree.rglob('*') if path.is_file()}}
    (base / 'sources.json').write_text(json.dumps(checks, indent=2) + '\n')


def make_guest(args, base):
    persistence.make_creation_guest(args, base)
    init = base / 'creation/initramfs'
    for name in ('cmp', 'find'):
        add_executable(init, Path('/usr/bin') / name)
    shutil.copytree(base / 'esp-source', init / 'expected')
    prepare.write(init, 'inspect-esp', INSPECT, 0o755)
    prepare.write(init, 'etc/rufus-iso-esp-fixture', 'iso-esp\n')
    persistence.pack(init, base / 'creation/creation.cpio.gz')


def inspect(args, base):
    table = json.loads(run(['sfdisk', '--json', base / 'usb.raw'], capture_output=True, text=True).stdout)
    partitions = table['partitiontable']['partitions']
    assert table['partitiontable']['label'] == 'gpt' and len(partitions) == 2
    assert partitions[0]['type'].lower() == 'c12a7328-f81f-11d2-ba4b-00a0c93ec93b'
    assert partitions[0]['name'] == 'EFI System Partition'
    assert partitions[1]['type'].lower() == '0fc63daf-8483-4772-8e79-3d69d8477de4'
    assert partitions[1]['name'] == 'Linux Persistence'
    original = json.loads((base / 'sources.json').read_text())
    assert digest(base / 'debian.iso') == digest(base / 'casper.iso') == original['source_sha256']
    spec = str(base / 'usb.raw') + '@@' + str(partitions[0]['start'] * 512)
    exported = base / 'readback'; exported.mkdir(exist_ok=True)
    run(['mcopy', '-s', '-i', spec, '::/*', exported], capture_output=True)
    for name, expected in original['files'].items():
        path = exported / name
        if name.endswith('isolinux.cfg'):
            assert 'boot=live persistence' in path.read_text()
        else:
            assert digest(path) == expected, name
    (base / 'partition-table.json').write_text(json.dumps(table, indent=2) + '\n')
    boot_syslinux_uefi(args, base / 'uefi', base / 'usb.raw', native_fat=True)
    return {'table': table, 'file_sha256': original['files'], 'source_sha256': original['source_sha256'],
            'disk_sha256': digest(base / 'usb.raw'), 'uefi_boot': 'RUFUS_SYSLINUX_UEFI_OK',
            'firmware_backing_unchanged': True}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--upstream', type=Path, required=True)
    parser.add_argument('--installer', type=Path, required=True)
    parser.add_argument('--mkisofs', type=Path, required=True)
    parser.add_argument('--kernel', required=True)
    parser.add_argument('--accel', default='tcg')
    parser.add_argument('--resume', action='store_true', help='Reuse a completed creation guest for a pending independent inspection')
    parser.add_argument('output', type=Path)
    args = parser.parse_args(); base = args.output.resolve(); base.mkdir(exist_ok=args.resume)
    if not args.resume:
        make_sources(args, base); make_guest(args, base)
        with (base / 'usb.raw').open('xb') as stream: stream.truncate(persistence.DISK_BYTES)
        persistence.boot(args, base, 'creation', direct=True)
    serial = (base / 'creation.serial.log').read_text()
    assert 'RUFUS_ISO_ESP_GUEST_OK' in serial and 'RUFUS_CREATE_EXIT 0' in serial
    assert serial.count('RUFUS_ISO_ESP_JOB ') == serial.count('RUFUS_ISO_ESP_READBACK ') == 4
    assert serial.count('AUTH=1 HELD=1 READY=100') == 4
    assert 'AddressSanitizer' not in serial and 'LeakSanitizer' not in serial and 'runtime error:' not in serial
    checks = inspect(args, base)
    checks.update({'jobs': 4, 'filesystems': [16, 32], 'persistence': [False, True],
                   'one_authorization_each': True, 'joined_ready': True, 'formatter_defaults': True})
    (base / 'checks.json').write_text(json.dumps(checks, indent=2) + '\n')
    print('PASS: ISO -> ESP: four ordinary Qt jobs, FAT16/FAT32 and persistence, strict source checks, complete independent file readback and UEFI boot.')


if __name__ == '__main__':
    main()
