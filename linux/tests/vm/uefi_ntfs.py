#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Boot the bundled UEFI:NTFS image into a generated NTFS EFI/Linux tree, offline."""
import argparse
import gzip
import json
import os
from pathlib import Path
import shutil
import subprocess

import ntfs
import prepare
from boot_linux import firmware_options
from fat_extract import add_executable, digest, run, values
from test_fat_format import copy_volume
from test_ntfs_format import inspect
from test_partition_image import inspect_gpt, trailing_extent

DISK_BYTES = 256 * 1024 * 1024
CLUSTER_BYTES = 4096
BOOT_TIMEOUT_SECONDS = 120
BOOT_LOADER = Path('/usr/lib/systemd/boot/efi/systemd-bootx64.efi')
BUNDLED = prepare.HERE.parents[2] / 'res/uefi/uefi-ntfs.img'


def pack_initramfs(init, archive):
    names = run(['find', '.', '-print0'], cwd=init, capture_output=True).stdout
    with archive.open('wb') as output:
        run(['cpio', '--null', '-o', '-H', 'newc', '--owner=0:0'], cwd=init, input=names, stdout=output)


def make_fixture(args, base):
    ntfs.make_guest(args, base)
    init = base / 'initramfs'
    for name in ('cp', 'rm', 'sha256sum'):
        add_executable(init, Path('/usr/bin') / name)
    prepare.write(init, 'init', (prepare.HERE / 'guest-uefi-ntfs').read_text(), 0o755)
    prepare.write(init, 'etc/rufus-expected-kernel', args.kernel + '\n')
    boot_archive = base / 'boot-initramfs.cpio'
    pack_initramfs(init, boot_archive)
    payload = base / 'payload'
    for directory in ('EFI/BOOT', 'boot', 'loader/entries'):
        (payload / directory).mkdir(parents=True)
    shutil.copy2(BOOT_LOADER, payload / 'EFI/BOOT/BOOTX64.EFI')
    shutil.copy2(base / 'vmlinuz', payload / 'boot/vmlinuz')
    with boot_archive.open('rb') as source, (payload / 'boot/initrd.gz').open('wb') as output:
        with gzip.GzipFile(fileobj=output, mode='wb', mtime=0) as compressed:
            shutil.copyfileobj(source, compressed)
    prepare.write(payload, 'loader/loader.conf',
                  'default rufus.conf\ntimeout 0\neditor no\nauto-entries no\nauto-firmware no\n')
    prepare.write(payload, 'loader/entries/rufus.conf',
                  'title Rufus NTFS fixture\nlinux /boot/vmlinuz\ninitrd /boot/initrd.gz\n'
                  'options console=ttyS0 panic=-1 rufus.ntfs_boot=1\n')
    prepare.write(payload, 'rufus-ntfs-payload', 'RUFUS-NTFS-EFI-TREE\n')
    prepare.write(payload, 'Unicode-ș😀.txt', 'NTFS Unicode filename and content: ș😀\n')
    checksums = [digest(path) + '  ' + str(path.relative_to(payload))
                 for path in sorted(payload.rglob('*')) if path.is_file()]
    prepare.write(payload, 'payload.sha256', '\n'.join(checksums) + '\n')
    shutil.copytree(payload, init / 'payload')
    pack_initramfs(init, base / 'initramfs.cpio')
    return checksums


def run_guest(args, output, base, disk, boot, missing=False):
    output.mkdir()
    original = digest(disk)
    product = 'RufusNtfsBootFixture' if boot else 'RufusNtfsPrepareFixture'
    command = ['qemu-system-x86_64', '-accel', args.accel,
               '-cpu', 'host' if args.accel == 'kvm' else 'max', '-m', '512',
               '-display', 'none', '-monitor', 'none', '-serial', 'file:' + str(output / 'serial.log'),
               '-no-reboot', '-nic', 'none',
               '-smbios', 'type=1,manufacturer=RufusTest,product=' + product,
               '-device', 'qemu-xhci,id=xhci',
               '-drive', 'if=none,id=stick,format=raw,file=' + str(disk),
               '-device', 'usb-storage,drive=stick,removable=on,bootindex=1,serial=RUFUS-NTFS-BOOT']
    if boot:
        command.extend(['-snapshot'] + firmware_options(output, 'uefi', 'ntfs'))
    else:
        options = 'console=ttyS0 panic=-1 rufus.ntfs_prepare=1'
        if missing:
            options += ' rufus.ntfs_missing=1'
        command.extend(['-kernel', str(base / 'vmlinuz'), '-initrd', str(base / 'initramfs.cpio'),
                        '-append', options])
    (output / 'qemu-command.json').write_text(json.dumps(command, indent=2) + '\n')
    with (output / 'qemu.log').open('wb') as log:
        subprocess.run(command, check=True, timeout=BOOT_TIMEOUT_SECONDS,
                       env=dict(os.environ, TMPDIR=str(output)), stdout=log, stderr=subprocess.STDOUT)
    serial = (output / 'serial.log').read_text(errors='replace')
    if boot:
        assert 'UEFI:NTFS' in serial, 'Bundled firmware handoff banner missing: ' + str(output)
        expected = 'RUFUS_UEFI_NTFS_MISSING_PAYLOAD' if missing else 'RUFUS_UEFI_NTFS_BOOT_OK'
        assert expected in serial, output
        assert f'RUFUS_UEFI_NTFS_EXIT {int(missing)}' in serial, output
        assert digest(disk) == original, 'Firmware boot changed its backing image'
    else:
        assert 'RUFUS_UEFI_NTFS_PREPARED' in serial and 'RUFUS_UEFI_NTFS_EXIT 0' in serial, output


def make_disk(args, base):
    disk = base / 'usb.raw'
    with disk.open('xb') as output:
        output.truncate(DISK_BYTES)
    partition = run([str(args.build / 'partition_image_probe'), str(disk), str(DISK_BYTES),
                     str(CLUSTER_BYTES), '63', 'none', str(BUNDLED.stat().st_size)], capture_output=True, text=True)
    (base / 'partition.log').write_text(partition.stdout + partition.stderr)
    layout = values(partition.stdout)
    assert layout['ERROR'] == 0, layout
    args.payload_size = BUNDLED.stat().st_size
    metadata = inspect_gpt(args, disk, DISK_BYTES, CLUSTER_BYTES, payload=args.payload_size)
    volume = base / 'ntfs.raw'
    with volume.open('xb') as output:
        output.truncate(layout['SIZE'])
    formatted = run([str(args.build / 'ntfs_format_probe'), args.mkfs, str(volume), str(layout['OFFSET']),
                     str(layout['SIZE']), str(CLUSTER_BYTES), 'Rufus NTFS ș😀', 'none'],
                    capture_output=True, text=True, env=dict(os.environ, LC_ALL='C'))
    (base / 'formatter.log').write_text(formatted.stdout + formatted.stderr)
    report = values(formatted.stdout)
    assert report['ERROR'] == 0, report
    inspect(args, volume, layout['SIZE'], layout['OFFSET'], CLUSTER_BYTES, 'Rufus NTFS ș😀', report)
    copied = run([str(args.build / 'raw_write_probe'), str(volume), str(disk), 'none',
                  str(layout['OFFSET']), str(layout['SIZE'])], capture_output=True, text=True)
    assert json.loads(copied.stdout)['error'] == 0, copied.stdout
    written = run([str(args.build / 'raw_write_probe'), str(BUNDLED), str(disk), 'none', 'uefi-ntfs',
                   str(DISK_BYTES), str(CLUSTER_BYTES), '63', str(args.payload_size)], capture_output=True, text=True)
    (base / 'payload-write.log').write_text(written.stdout + written.stderr)
    assert json.loads(written.stdout)['error'] == 0, written.stdout
    assert inspect_gpt(args, disk, DISK_BYTES, CLUSTER_BYTES, payload=args.payload_size) == metadata
    offset, size = trailing_extent(DISK_BYTES, args.payload_size)
    with disk.open('rb') as source:
        source.seek(offset)
        assert source.read(args.payload_size) == BUNDLED.read_bytes()
        assert source.read(size - args.payload_size) == bytes(size - args.payload_size)
    volume.unlink()
    return disk, layout, report, metadata


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--kernel', required=True)
    parser.add_argument('--accel', choices=['kvm', 'tcg'], default='kvm')
    for name, default in (('mkfs', 'mkntfs'), ('info', 'ntfsinfo'), ('label', 'ntfslabel'), ('sfdisk', 'sfdisk')):
        parser.add_argument('--' + name, default='/usr/bin/' + default)
    parser.add_argument('--fsck')
    parser.add_argument('output', type=Path, help='New output directory; must not exist')
    args = parser.parse_args()
    args.build = args.build.resolve()
    base = args.output.resolve()
    if any(char in str(base) for char in ',\n\r'):
        parser.error('Output path must not contain QEMU option separators')
    base.mkdir()
    checksums = make_fixture(args, base)
    inputs = [base / 'vmlinuz', base / 'initramfs.cpio', BOOT_LOADER, BUNDLED, Path(args.mkfs)]
    inputs.extend(args.build / name for name in ('raw_write_probe', 'ntfs_format_probe', 'partition_image_probe'))
    manifest = {'kernel': args.kernel, 'accel': args.accel, 'inputs': {str(path): digest(path) for path in inputs},
                'payload_checksums': checksums}
    (base / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    disk, layout, report, metadata = make_disk(args, base)
    before = disk.read_bytes()
    print(f'Populating generated NTFS boot tree: {base}', flush=True)
    run_guest(args, base / 'populate', base, disk, False)
    after = disk.read_bytes()
    end = layout['OFFSET'] + layout['SIZE']
    assert after[:layout['OFFSET']] == before[:layout['OFFSET']] and after[end:] == before[end:]
    assert inspect_gpt(args, disk, DISK_BYTES, CLUSTER_BYTES, payload=args.payload_size) == metadata
    volume = base / 'populated-ntfs.raw'
    copy_volume(disk, volume, layout['OFFSET'], layout['SIZE'])
    inspect(args, volume, layout['SIZE'], layout['OFFSET'], CLUSTER_BYTES, 'Rufus NTFS ș😀', report)
    volume.unlink()
    print('Booting only the prepared USB through OVMF and bundled UEFI:NTFS', flush=True)
    run_guest(args, base / 'boot', base, disk, True)
    broken = base / 'missing-payload.raw'
    shutil.copyfile(disk, broken)
    run_guest(args, base / 'populate-missing', base, broken, False, True)
    run_guest(args, base / 'boot-missing', base, broken, True, True)
    for path in inputs:
        assert digest(path) == manifest['inputs'][str(path)], path
    (base / 'results.json').write_text(json.dumps({'layout': layout, 'ntfs': report,
        'disk_sha256': digest(disk), 'uefi_ntfs_boot': True, 'missing_payload_refused': True}, indent=2) + '\n')
    print('PASS: unchanged bundled UEFI:NTFS booted the NTFS EFI tree; missing payload refused.', flush=True)


if __name__ == '__main__':
    main()
