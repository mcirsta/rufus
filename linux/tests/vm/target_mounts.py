#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Native unmount qualification in a disposable USB VM; no host root or mounts."""
import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess

import prepare
from fat_extract import add_executable, digest, run

MIB = 1024 * 1024
DISK_BYTES = 64 * MIB
VOLUME_BYTES = 30 * MIB
GUEST_TIMEOUT_SECONDS = 180
CASES = {'unmounted', 'invalid-disk', 'regular-disk', 'partition-disk', 'cancel-before',
         'source-on-target', 'busy', 'nested-mount', 'bind-alias', 'system-alias',
         'current-directory', 'stale', 'syscall-failure', 'namespace-copy', 'partial-cancel', 'success'}
CASES.update({'missing-mountinfo', 'malformed-mountinfo', 'unsupported-filesystem', 'unresolved-source',
              'fuse-source-only',
              'untrusted-parent', 'system-mount', 'partition-holder', 'layered-device', 'swap-partition',
              'memory-source', 'two-volumes', 'worker-unshare-failure', 'worker-propagation-failure',
              'worker-namespace-children', 'worker-cancel-before'})
CASES.update({'home-root-uid', 'home-invalid-uid', 'home-unknown-uid', 'home-relative', 'home-missing',
              'home-file', 'home-cancel', 'home-on-target', 'home-unresolved', 'home-invalid-fd',
              'home-wrong-type', 'home-symlink-retained', 'home-account-buffer'})


def make_disk(base):
    disk = base / 'usb.raw'
    with disk.open('xb') as output:
        output.truncate(DISK_BYTES)
    run(['sfdisk', str(disk)], input=b'label: dos\nstart=2048,size=61440,type=6\nstart=65536,size=61440,type=6\n',
        capture_output=True)
    volume = base / 'volume.raw'
    with volume.open('xb') as output:
        output.truncate(VOLUME_BYTES)
    run(['mkfs.fat', '-F', '16', '-n', 'UNMOUNT', str(volume)], capture_output=True)
    run(['mmd', '-i', str(volume), '::nested'], capture_output=True)
    with disk.open('r+b') as output:
        for offset in (MIB, 32 * MIB):
            output.seek(offset)
            with volume.open('rb') as source:
                shutil.copyfileobj(source, output, length=MIB)
    volume.unlink()
    return disk


def make_guest(args, base):
    root = base / 'root'
    root.mkdir()
    (root / 'lib').symlink_to('usr/lib')
    prepare.bootstrap(base, root, args.kernel)
    init = base / 'initramfs'
    shutil.copytree('/usr/lib/locale/C.utf8', init / 'usr/lib/locale/C.utf8')
    add_executable(init, args.build / 'test_target_mounts_guest')
    for tool in ('dmsetup', 'mkswap', 'mkfs.fat'):
        add_executable(init, Path('/usr/bin') / tool)
    if args.ntfs_3g_package:
        for package in (args.ntfs_3g_package, args.ntfs_library_package):
            run(['bsdtar', '-xpf', str(package), '--no-same-owner', '--no-xattrs',
                 '--exclude=.PKGINFO', '--exclude=.BUILDINFO', '--exclude=.MTREE', '--exclude=.INSTALL', '-C', str(init)])
        # These packages stay inside the disposable guest; no host installation.
        shutil.copy2('/usr/lib/libfuse.so.2', init / 'usr/lib/libfuse.so.2')
        add_executable(init, Path('/usr/bin/mkntfs'))
        prepare.write(init, 'etc/rufus-fuse-fixture', 'NTFS-3G block mounts\n')
    kernel = Path('/usr/lib/modules') / args.kernel
    for name in (('dm_mod', 'fuse') if args.ntfs_3g_package else ('dm_mod',)):
        modules = subprocess.check_output(['modprobe', '--show-depends', '-S', args.kernel, name], text=True)
        for line in modules.splitlines():
            if line.startswith('insmod '):
                module = Path(line.split()[1]).resolve()
                destination = init / 'usr/lib/modules' / args.kernel / module.relative_to(kernel)
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(module, destination)
    run(['depmod', '-b', str(init), args.kernel])
    for name in ('mnt/usb one', 'mnt/usb two', 'mnt/alias', 'home', 'run/source'):
        (init / name).mkdir(parents=True, exist_ok=True)
    prepare.write(init, 'etc/machine-id', '3f072f338dfb4029b8f2c40f6a5ac313\n')
    prepare.write(init, 'etc/nsswitch.conf', 'passwd: files\ngroup: files\n')
    prepare.write(init, 'etc/passwd',
                  'caller:x:1000:1000:Caller:/home/caller:/bin/bash\n'
                  'target:x:1001:1001:Target home:/mnt/usb one/nested:/bin/bash\n'
                  'relative:x:1002:1002:Relative home:home/relative:/bin/bash\n'
                  'missing:x:1003:1003:Missing home:/missing/home:/bin/bash\n'
                  'file:x:1004:1004:File home:/etc/machine-id:/bin/bash\n'
                  'unresolved:x:1005:1005:Rootfs home:/home:/bin/bash\n'
                  + 'large:x:1006:1006:' + 'x' * 20000 + ':/run/source:/bin/bash\n')
    (init / 'home/caller').symlink_to('/run/source')
    prepare.write(init, 'init', '''#!/bin/bash
set -eu
[[ $$ == 1 ]] || exit 77
export PATH=/usr/bin LC_ALL=C.UTF-8
export ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1
mount -t devtmpfs devtmpfs /dev
mount -t proc proc /proc
mount -t sysfs sysfs /sys
finish() {
    result=$?
    trap - EXIT
    echo "RUFUS_MOUNTS_EXIT $result"
    echo o > /proc/sysrq-trigger
    while :; do sleep 1; done
}
trap finish EXIT
for module in xhci_pci usb_storage sd_mod vfat nls_cp437 nls_iso8859_1 dm_mod; do
    modprobe "$module"
done
if [[ -f /etc/rufus-fuse-fixture ]]; then modprobe fuse; fi
for attempt in {1..100}; do
    [[ -b /dev/sda2 ]] && break
    sleep .1
done
[[ -b /dev/sda2 ]]
test_target_mounts_guest
''', 0o755)
    names = run(['find', '.', '-print0'], cwd=init, capture_output=True).stdout
    with (base / 'initramfs.cpio').open('wb') as output:
        run(['cpio', '--null', '-o', '-H', 'newc', '--owner=0:0'], cwd=init, input=names, stdout=output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--kernel', required=True)
    parser.add_argument('--accel', choices=('kvm', 'tcg'), default='tcg')
    parser.add_argument('--ntfs-3g-package', type=Path, help='Optional NTFS-3G package, unpacked only in the guest')
    parser.add_argument('--ntfs-library-package', type=Path, help='Matching libntfs-3g package for the guest')
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    args.build = args.build.resolve()
    if bool(args.ntfs_3g_package) != bool(args.ntfs_library_package):
        parser.error('NTFS-3G qualification needs both driver and library packages')
    for name in ('ntfs_3g_package', 'ntfs_library_package'):
        package = getattr(args, name)
        if package:
            package = package.resolve(strict=True)
            setattr(args, name, package)
    base = args.directory.resolve()
    base.mkdir()
    disk = make_disk(base)
    make_guest(args, base)
    serial = base / 'serial.log'
    command = ['qemu-system-x86_64', '-accel', args.accel, '-cpu', 'host' if args.accel == 'kvm' else 'max',
               '-m', '512', '-display', 'none', '-monitor', 'none', '-serial', 'file:' + str(serial),
               '-no-reboot', '-nic', 'none', '-kernel', str(base / 'vmlinuz'),
               '-initrd', str(base / 'initramfs.cpio'), '-append', 'console=ttyS0 panic=-1 rufus.mount_fixture=1',
               '-smbios', 'type=1,manufacturer=RufusTest,product=RufusMountFixture',
               '-device', 'qemu-xhci,id=xhci', '-drive', 'if=none,id=stick,format=raw,file=' + str(disk),
               '-device', 'usb-storage,drive=stick,removable=on,serial=RUFUS-MOUNT-TEST']
    (base / 'qemu-command.json').write_text(json.dumps(command, indent=2) + '\n')
    with (base / 'qemu.log').open('wb') as output:
        subprocess.run(command, check=True, timeout=GUEST_TIMEOUT_SECONDS, stdout=output, stderr=subprocess.STDOUT)
    text = serial.read_text(errors='replace')
    assert 'RUFUS_MOUNTS_DONE' in text and 'RUFUS_MOUNTS_EXIT 0' in text, serial
    assert 'AddressSanitizer' not in text and 'runtime error:' not in text, serial
    cases = dict(re.findall(r'RUFUS_MOUNTS_CASE (\S+) ERROR=(\d+)', text))
    expected = set(CASES)
    if args.ntfs_3g_package:
        expected.update(driver + '-' + case for driver in ('ntfs-3g', 'lowntfs-3g')
                        for case in ('source', 'busy', 'home', 'alias', 'nested', 'system', 'stale', 'cancel', 'success'))
    assert set(cases) == expected, cases
    for index, offset in enumerate((MIB, 32 * MIB), 1):
        volume = base / 'volume.raw'
        with disk.open('rb') as source, volume.open('wb') as output:
            source.seek(offset)
            remaining = VOLUME_BYTES
            while remaining:
                chunk = source.read(min(remaining, MIB))
                assert chunk
                output.write(chunk)
                remaining -= len(chunk)
        checker = 'ntfsck' if args.ntfs_3g_package and index == 1 else 'fsck.fat'
        checked = run([checker, '-n', str(volume)], capture_output=True)
        (base / f'fsck-{index}.log').write_bytes(checked.stdout + checked.stderr)
        volume.unlink()
    inputs = [args.build / 'test_target_mounts_guest', Path('/usr/lib/libmount.so.1')]
    if args.ntfs_3g_package:
        inputs += [args.ntfs_3g_package, args.ntfs_library_package, Path('/usr/lib/libfuse.so.2'), Path('/usr/bin/mkntfs')]
    (base / 'result.json').write_text(json.dumps({'kernel': args.kernel, 'cases': cases,
        'inputs': {str(path): digest(path) for path in inputs},
        'binary_sha256': digest(args.build / 'test_target_mounts_guest'), 'disk_sha256': digest(disk)}, indent=2) + '\n')
    for name in ('root', 'initramfs'):
        shutil.rmtree(base / name)
    for name in ('usb.raw', 'vmlinuz', 'initramfs.cpio'):
        (base / name).unlink()
    print(f'{len(cases)} native mount cases passed; guest and disk removed: {base}')


if __name__ == '__main__':
    main()
