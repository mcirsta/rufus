#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Stage the CMake installation in a guest; qualify Alpine Widgets/helper jobs."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import xml.etree.ElementTree as ET

import prepare
import syslinux_alpine
from fat_extract import digest

GUEST_TIMEOUT_SECONDS = 480
SYSTEM_BYTES = 4 * 1024**3


def run(command, **kwargs):
    return subprocess.run(command, check=True, timeout=120, **kwargs)


def make_guest(args, base):
    prepare.ROOT_PACKAGES += ['dosfstools', 'mtools', 'hivex']
    packages = prepare.cached_packages(args.cache)
    root = base / 'root'
    root.mkdir()
    for package in packages:
        run(['bsdtar', '-xpf', package, '--no-same-owner', '--no-xattrs',
             '--exclude=.PKGINFO', '--exclude=.BUILDINFO', '--exclude=.MTREE',
             '--exclude=.INSTALL', '-C', root])
    dbus_helper = root / 'usr/lib/dbus-daemon-launch-helper'
    if dbus_helper.exists():
        dbus_helper.chmod(dbus_helper.stat().st_mode | 0o400)
    prepare.configure_guest(root, args.build / 'test_usb_guest')
    shutil.copy2(args.build / 'test_prepare_device_guest', root / 'opt/rufus/test_prepare_device_guest')
    run(['cmake', '--install', args.build], env=dict(os.environ, DESTDIR=str(root)))
    paths = (args.build / 'install_manifest.txt').read_text().splitlines()
    installed = {name: next(Path(path) for path in paths if Path(path).name == name)
                 for name in ('rufus-linux', 'rufus-prepare-helper', 'rufus-syslinux', 'org.rufus.PrepareDevice.policy')}
    policy = root / installed['org.rufus.PrepareDevice.policy'].relative_to('/')
    action = ET.parse(policy).find('action')
    helper_path = action.find("annotate[@key='org.freedesktop.policykit.exec.path']").text
    assert helper_path == str(installed['rufus-prepare-helper'])
    assert action.find('defaults/allow_active').text == 'auth_admin', 'Authorization must not be cached'
    assert policy.parent == root / 'usr/share/polkit-1/actions', 'Build this packaged fixture with CMAKE_INSTALL_PREFIX=/usr'
    records = {str(path): digest(root / path.relative_to('/')) for path in installed.values()}
    (base / 'installed-files.json').write_text(json.dumps(records, indent=2) + '\n')
    prepare.write(root, 'etc/rufus-installed-gui', str(installed['rufus-linux']) + '\n')
    prepare.write(root, 'etc/rufus-installed-helper', helper_path + '\n')
    prepare.write(root, 'etc/rufus-installed-fixture', 'RUFUS-INSTALLED-TEST\n')
    prepare.write(root, 'etc/rufus-gpt-fixture', 'RUFUS-GPT-TEST\n')
    prepare.write(root, 'etc/rufus-alpine-fixture', 'RUFUS-ALPINE-TEST\n')
    prepare.write(root, 'opt/rufus/guest-root.sh', (prepare.HERE / 'installed-root.sh').read_text(), 0o755)
    user = (prepare.HERE / 'helper-user.sh').read_text().replace(
        '/opt/rufus/test_helper_guest -v1',
        '/opt/rufus/test_prepare_device_guest -v1 installedGuiStarts installedSyslinuxWidgets')
    prepare.write(root, 'opt/rufus/guest-user.sh', user, 0o755)
    shutil.copy2(args.alpine, root / 'home/rufus/ALPINE.iso')
    prepare.bootstrap(base, root, args.kernel)
    with (base / 'system.raw').open('xb') as stream:
        stream.truncate(SYSTEM_BYTES)
    run(['mkfs.ext4', '-q', '-F', '-d', root, base / 'system.raw'])
    shutil.rmtree(root)
    return {str(path): digest(path) for path in packages}


def run_guest(args, base):
    serial = base / 'serial.log'
    command = ['qemu-system-x86_64', '-accel', args.accel,
               '-cpu', 'host' if args.accel == 'kvm' else 'max', '-m', '2048',
               '-display', 'none', '-monitor', 'none', '-serial', 'file:' + str(serial),
               '-no-reboot', '-nic', 'none', '-kernel', str(base / 'vmlinuz'),
               '-initrd', str(base / 'initramfs.cpio'),
               '-append', 'root=/dev/vda rw console=ttyS0 rufus.usb_fixture=1',
               '-smbios', 'type=1,manufacturer=RufusTest,product=RufusUSBFixture',
               '-drive', 'format=raw,if=virtio,file=' + str(base / 'system.raw'),
               '-device', 'qemu-xhci,id=xhci',
               '-drive', 'if=none,id=stick,format=raw,file=' + str(base / 'usb.raw'),
               '-device', 'usb-storage,drive=stick,removable=on,serial=RUFUS-GPT-TEST']
    (base / 'qemu-command.json').write_text(json.dumps(command, indent=2) + '\n')
    try:
        with (base / 'qemu.log').open('wb') as log:
            subprocess.run(command, check=True, timeout=GUEST_TIMEOUT_SECONDS, stdout=log, stderr=subprocess.STDOUT)
    finally:
        # Preserve evidence even when the test fails, before disposing of disks.
        for name in ('authorization.log', 'helper-authentication.png', 'installed-gui.png',
                     'installed-cancelled.png', 'installed-complete.png'):
            with (base / name).open('wb') as output:
                run(['debugfs', '-R', 'cat /home/rufus/' + name, base / 'system.raw'],
                    stdout=output, stderr=subprocess.PIPE)
    text = serial.read_text(errors='replace')
    for marker in ('RUFUS_INSTALLED_GUI_STARTED UID=1000 REQUESTS=0',
                   'RUFUS_INSTALLED_CANCELLED REQUESTS=1', 'RUFUS_INSTALLED_CANCEL_CLEANUP_OK',
                   'RUFUS_INSTALLED_CREATED REQUESTS=1', 'RUFUS_INSTALLED_RUNTIME_PASSED REQUESTS=2'):
        assert marker in text, (marker, serial)
    for error in ('FAIL!', 'QFATAL', 'AddressSanitizer:', 'LeakSanitizer:', 'runtime error:'):
        assert error not in text, (error, serial)
    assert (base / 'authorization.log').read_bytes().count(b'string "org.rufus.PrepareDevice"') == 2


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--kernel', required=True)
    parser.add_argument('--cache', type=Path, default=Path('/var/cache/pacman/pkg'))
    parser.add_argument('--alpine', type=Path, required=True)
    parser.add_argument('--upstream', type=Path, required=True)
    parser.add_argument('--accel', choices=('kvm', 'tcg'), default='kvm')
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    args.build = args.build.resolve()
    args.alpine = args.alpine.resolve()
    args.upstream = args.upstream.resolve()
    base = args.output.resolve()
    if any(char in str(base) for char in ',\n\r'):
        parser.error('Output path must not contain QEMU option separators')
    base.mkdir()
    (base / 'options.json').write_text(json.dumps(vars(args), default=str, indent=2) + '\n')
    try:
        syslinux_alpine.prepare_source(args, base)
        packages = make_guest(args, base)
        inputs = [args.alpine, args.build / 'rufus-linux', args.build / 'rufus-prepare-helper',
                  args.build / 'test_prepare_device_guest', args.build / 'syslinux/rufus-syslinux']
        manifest = {'packages': packages, 'inputs': {str(path): digest(path) for path in inputs}}
        (base / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
        with (base / 'usb.raw').open('xb') as stream:
            stream.truncate(syslinux_alpine.DISK_BYTES)
        print('Running installed runtime guest:', base, flush=True)
        run_guest(args, base)
        result = syslinux_alpine.qualify_disk(args, base, base / 'usb.raw')
        for path in inputs:
            assert digest(path) == manifest['inputs'][str(path)], path
        (base / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
        print('PASS: staged GUI startup, real Widgets cancellation/creation, one authorization per job, readback and BIOS/UEFI boots.', flush=True)
    finally:
        for name in ('root', 'initramfs', 'alpine-source-tree'):
            if (base / name).exists():
                shutil.rmtree(base / name)
        for name in ('system.raw', 'usb.raw', 'vmlinuz', 'initramfs.cpio', 'check.img', 'readback'):
            (base / name).unlink(missing_ok=True)


if __name__ == '__main__':
    main()
