#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Mount a native-formatted NTFS file in an offline VM and test offsets beyond 4 GiB."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess

import prepare
from fat_extract import add_executable, digest, run, values
from test_ntfs_format import inspect

DISK_BYTES = 6 * 1024**3
CLUSTER_BYTES = 4096
EXPECTED_FILE_BYTES = 5 * 1024**3 + 512
# NTFS3 can allocate/zero the 5 GiB gap; software-emulated USB needs more time
# than the small GPT guest. This deadline does not alter formatter timeouts.
GUEST_TIMEOUT_SECONDS = 300


def make_guest(args, base):
    root = base / 'root'
    root.mkdir()
    (root / 'lib').symlink_to('usr/lib')
    prepare.bootstrap(base, root, args.kernel)
    init = base / 'initramfs'
    (init / 'target').mkdir()
    for name in ('dd', 'stat', 'cmp', 'umount'):
        add_executable(init, Path('/usr/bin') / name)
    kernel = Path('/usr/lib/modules') / args.kernel
    for name in ('ntfs3', 'nls_utf8'):
        dependencies = run(['modprobe', '--show-depends', '-S', args.kernel, name],
                           capture_output=True, text=True).stdout
        for line in dependencies.splitlines():
            if not line.startswith('insmod '):
                continue
            module = Path(line.split()[1]).resolve()
            destination = init / 'usr/lib/modules' / args.kernel / module.relative_to(kernel)
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(module, destination)
    run(['depmod', '-b', str(init), args.kernel])
    prepare.write(init, 'init', (prepare.HERE / 'guest-ntfs').read_text(), 0o755)
    names = run(['find', '.', '-print0'], cwd=init, capture_output=True).stdout
    with (base / 'initramfs.cpio').open('wb') as output:
        run(['cpio', '--null', '-o', '-H', 'newc', '--owner=0:0'], cwd=init, input=names, stdout=output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--kernel', required=True)
    parser.add_argument('--accel', choices=['kvm', 'tcg'], default='kvm')
    for name, default in (('mkfs', 'mkntfs'), ('info', 'ntfsinfo'), ('label', 'ntfslabel')):
        parser.add_argument('--' + name, default='/usr/bin/' + default)
    parser.add_argument('--fsck', help='Optional read-only checker, e.g. /usr/bin/ntfsck')
    parser.add_argument('output', type=Path, help='New output directory; must not exist')
    args = parser.parse_args()
    args.build = args.build.resolve()
    base = args.output.resolve()
    if any(char in str(base) for char in ',\n\r'):
        parser.error('Output path must not contain QEMU option separators')
    probe = args.build / 'ntfs_format_probe'
    if not probe.is_file():
        parser.error('Build the NTFS formatter tests first')
    base.mkdir()
    make_guest(args, base)
    disk = base / 'ntfs.raw'
    with disk.open('xb') as output:
        output.truncate(DISK_BYTES)
    label = 'Rufus ș 😀'
    inputs = (base / 'vmlinuz', base / 'initramfs.cpio', probe, Path(args.mkfs))
    manifest = {'kernel': args.kernel, 'accel': args.accel, 'volume_bytes': DISK_BYTES,
                'inputs': {str(path): digest(path) for path in inputs}}
    command = [str(probe), args.mkfs, str(disk), '0', str(DISK_BYTES), str(CLUSTER_BYTES), label, 'none']
    formatted = run(command, env=dict(os.environ, LC_ALL='C'), capture_output=True, text=True)
    (base / 'formatter.log').write_text(formatted.stdout + formatted.stderr)
    report = values(formatted.stdout)
    assert report['ERROR'] == 0 and report['BYTES'] == DISK_BYTES, report
    inspect(args, disk, DISK_BYTES, 0, CLUSTER_BYTES, label, report)
    manifest['formatted_sha256'] = digest(disk)
    (base / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    serial = base / 'serial.log'
    command = ['qemu-system-x86_64', '-accel', args.accel,
               '-cpu', 'host' if args.accel == 'kvm' else 'max', '-m', '256',
               '-display', 'none', '-monitor', 'none', '-serial', 'file:' + str(serial),
               '-no-reboot', '-nic', 'none', '-kernel', str(base / 'vmlinuz'),
               '-initrd', str(base / 'initramfs.cpio'),
               '-append', 'console=ttyS0 panic=-1 rufus.ntfs_fixture=1',
               '-smbios', 'type=1,manufacturer=RufusTest,product=RufusNtfsFixture',
               '-device', 'qemu-xhci,id=xhci',
               '-drive', 'if=none,id=stick,format=raw,file=' + str(disk),
               '-device', 'usb-storage,drive=stick,removable=on,serial=RUFUS-NTFS-TEST']
    (base / 'qemu-command.json').write_text(json.dumps(command, indent=2) + '\n')
    print(f'Running NTFS large-file USB guest: {base}', flush=True)
    with (base / 'qemu.log').open('wb') as log:
        subprocess.run(command, check=True, timeout=GUEST_TIMEOUT_SECONDS, stdout=log, stderr=subprocess.STDOUT)
    text = serial.read_text(errors='replace')
    for marker in (f'RUFUS_NTFS_LARGE_FILE SIZE={EXPECTED_FILE_BYTES}', 'RUFUS_NTFS_DONE', 'RUFUS_NTFS_EXIT 0'):
        assert marker in text, (marker, serial)
    inspect(args, disk, DISK_BYTES, 0, CLUSTER_BYTES, label, report)
    for path in inputs:
        assert digest(path) == manifest['inputs'][str(path)], path
    (base / 'results.json').write_text(json.dumps({'formatter': report, 'large_file_bytes': EXPECTED_FILE_BYTES,
        'kernel_ntfs3_remount_readback': True, 'final_sha256': digest(disk)}, indent=2) + '\n')
    print('PASS: native NTFS format, kernel NTFS3 write/remount/read beyond 4 GiB and clean metadata.', flush=True)


if __name__ == '__main__':
    main()
