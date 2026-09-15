#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Qualify one real polkit request for all GPT/FAT32 guest-helper operations."""
import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess

import prepare
from gpt import inspect_formatted_partition, DISK_BYTES, CASES
from fat_extract import digest, values
from test_partition_image import make_image, inspect_gpt, check_preserved

GUEST_TIMEOUT_SECONDS = 360
TOOL_TIMEOUT_SECONDS = 30


def run(command, **kwargs):
    return subprocess.run(command, check=True, timeout=TOOL_TIMEOUT_SECONDS, **kwargs)


def make_guest(args, base):
    # Reuse the installed-version package selection and real desktop session.
    prepare.ROOT_PACKAGES = prepare.ROOT_PACKAGES + ['dosfstools']
    packages = prepare.cached_packages(args.cache)
    root = base / 'root'
    root.mkdir()
    for package in packages:
        run(['bsdtar', '-xpf', str(package), '--no-same-owner', '--no-xattrs',
             '--exclude=.PKGINFO', '--exclude=.BUILDINFO', '--exclude=.MTREE', '--exclude=.INSTALL', '-C', str(root)])
    dbus_helper = root / 'usr/lib/dbus-daemon-launch-helper'
    if dbus_helper.exists():
        dbus_helper.chmod(dbus_helper.stat().st_mode | 0o400)
    prepare.configure_guest(root, args.build / 'test_usb_guest')
    binaries = ['test_gpt_guest', 'test_helper_guest']
    if args.device:
        binaries += ['rufus-prepare-helper', 'prepare_helper_probe', 'test_prepare_device_guest']
    for name in binaries:
        shutil.copy2(args.build / name, root / 'opt/rufus' / name)
    prepare.write(root, 'opt/rufus/guest-root.sh', (prepare.HERE / 'helper-root.sh').read_text(), 0o755)
    prepare.write(root, 'opt/rufus/guest-user.sh', (prepare.HERE / 'helper-user.sh').read_text(), 0o755)
    prepare.write(root, 'etc/rufus-gpt-fixture', 'RUFUS-GPT-TEST\n')
    prepare.write(root, 'usr/share/polkit-1/actions/org.rufus.TestCreate.policy', '''<?xml version="1.0"?>
<!DOCTYPE policyconfig PUBLIC "-//freedesktop//DTD PolicyKit Policy Configuration 1.0//EN"
"http://www.freedesktop.org/standards/PolicyKit/1/policyconfig.dtd">
<policyconfig>
  <action id="org.rufus.TestCreate">
    <description>Run the Rufus disposable USB helper test</description>
    <message>Authentication is required for the complete USB creation test</message>
    <defaults>
      <allow_any>auth_admin</allow_any>
      <allow_inactive>auth_admin</allow_inactive>
      <allow_active>auth_admin</allow_active>
    </defaults>
    <annotate key="org.freedesktop.policykit.exec.path">/opt/rufus/test_gpt_guest</annotate>
  </action>
</policyconfig>
''')
    if args.device:
        user_script = (prepare.HERE / 'helper-user.sh').read_text().replace('test_helper_guest', 'test_prepare_device_guest')
        prepare.write(root, 'opt/rufus/guest-user.sh', user_script, 0o755)
        prepare.write(root, 'opt/rufus/guest-root.sh', (prepare.HERE / 'device-root.sh').read_text(), 0o755)
        prepare.write(root, 'opt/rufus/slow-formatter', '''#!/bin/bash
set -eu
[[ $(< /sys/class/dmi/id/product_name) == RufusUSBFixture ]]
[[ $(< /etc/rufus-gpt-fixture) == RUFUS-GPT-TEST ]]
trap '' TERM
echo "$$ $PPID" > /run/rufus-formatter.pid
exec /usr/bin/sleep 60
''', 0o755)
        policy = (args.build / 'org.rufus.PrepareDevice.policy').read_text()
        policy = re.sub(r'(<annotate key="org.freedesktop.policykit.exec.path">)[^<]+',
                        r'\g<1>/opt/rufus/rufus-prepare-helper', policy)
        prepare.write(root, 'usr/share/polkit-1/actions/org.rufus.PrepareDevice.policy', policy)
        probe_policy = policy.replace('org.rufus.PrepareDevice', 'org.rufus.PrepareProbe')
        probe_policy = probe_policy.replace('/opt/rufus/rufus-prepare-helper', '/opt/rufus/prepare_helper_probe')
        prepare.write(root, 'usr/share/polkit-1/actions/org.rufus.PrepareProbe.policy', probe_policy)
    prepare.bootstrap(base, root, args.kernel)
    system = base / 'system.raw'
    with system.open('xb') as output:
        output.truncate(3 * 1024 * 1024 * 1024)
    run(['mkfs.ext4', '-q', '-F', '-d', str(root), str(system)])
    if args.device:
        shutil.rmtree(root)  # Reproducible package copies are now in the guest disk.
    return {str(path): digest(path) for path in packages}


def run_guest(args, base, disk):
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
               '-drive', 'if=none,id=stick,format=raw,file=' + str(disk),
               '-device', 'usb-storage,drive=stick,removable=on,serial=RUFUS-GPT-TEST']
    (base / 'qemu-command.json').write_text(json.dumps(command, indent=2) + '\n')
    with (base / 'qemu.log').open('wb') as log:
        subprocess.run(command, check=True, timeout=GUEST_TIMEOUT_SECONDS, stdout=log, stderr=subprocess.STDOUT)
    text = serial.read_text(errors='replace')
    for diagnostic in ('AddressSanitizer:', 'LeakSanitizer:', 'runtime error:'):
        assert diagnostic not in text, (diagnostic, serial)
    if args.device:
        for marker in ('RUFUS_HELPER_TEST_EXIT 0', 'RUFUS_HELPER_AUTH_REQUESTS 9',
                       'RUFUS_NATIVE_AUTHORIZATION_PASSED REQUESTS=9', 'RUFUS_NATIVE_UNCHANGED',
                       'RUFUS_NATIVE_MOUNTED_REFUSAL',
                       'RUFUS_HELPER_GUEST_FINISHED'):
            assert marker in text, (marker, serial)
        match = re.search(r'RUFUS_NATIVE_PREPARED ([^\r\n]+)', text)
        assert match, serial
        return {'success': values(match[1])}
    for marker in ('RUFUS_HELPER_TEST_EXIT 0', 'RUFUS_HELPER_AUTH_REQUESTS 3',
                   'RUFUS_HELPER_DISMISSED REQUESTS=1', 'RUFUS_HELPER_APPROVED REQUESTS=1',
                   'RUFUS_HELPER_NO_AGENT REQUESTS=1', 'RUFUS_HELPER_GUEST_FINISHED'):
        assert marker in text, (marker, serial)
    # Services also replay messages in journalctl at shutdown; keep one report per name.
    reports = {name: values(report) for name, report in re.findall(r'RUFUS_GPT_CASE (\S+) ([^\r\n]+)', text)}
    assert set(reports) == set(CASES), reports
    return reports


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--kernel', required=True)
    parser.add_argument('--cache', type=Path, default=Path('/var/cache/pacman/pkg'))
    parser.add_argument('--accel', choices=['kvm', 'tcg'], default='kvm')
    parser.add_argument('--device', action='store_true', help='Qualify the production device helper and Qt controller')
    parser.add_argument('--sfdisk', default='/usr/bin/sfdisk')
    parser.add_argument('--fsck', default='/usr/bin/fsck.fat')
    parser.add_argument('--mdir', default='/usr/bin/mdir')
    parser.add_argument('output', type=Path, help='New output directory; must not exist')
    args = parser.parse_args()
    args.build = args.build.resolve()
    base = args.output.resolve()
    if any(char in str(base) for char in ',\n\r'):
        parser.error('Output path must not contain QEMU option separators')
    for name in ('test_helper_guest', 'test_gpt_guest', 'test_usb_guest', 'check_usb_mount'):
        if not (args.build / name).is_file():
            parser.error('Build RUFUS_BUILD_VM_TEST=ON first: ' + name)
    if args.device:
        for name in ('rufus-prepare-helper', 'prepare_helper_probe', 'test_prepare_device_guest', 'org.rufus.PrepareDevice.policy'):
            if not (args.build / name).is_file():
                parser.error('Build the native helper targets first: ' + name)
    base.mkdir()
    packages = make_guest(args, base)
    disk, segments = make_image(base, 'usb.raw', DISK_BYTES)
    inputs = [base / name for name in ('vmlinuz', 'initramfs.cpio', 'system.raw', 'usb.raw')]
    inputs += [args.build / name for name in ('test_helper_guest', 'test_gpt_guest')]
    if args.device:
        inputs += [args.build / name for name in ('rufus-prepare-helper', 'prepare_helper_probe', 'test_prepare_device_guest')]
    manifest = {'kernel': args.kernel, 'accel': args.accel, 'packages': packages,
                'inputs': {str(path): digest(path) for path in inputs}}
    (base / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print('Running native helper authorization guest:', base, flush=True)
    reports = run_guest(args, base, disk)
    inspect_gpt(args, disk, DISK_BYTES, 512)
    partition = reports['success']
    check_preserved(disk, DISK_BYTES, segments, (partition['OFFSET'], partition['SIZE']))
    inspect_formatted_partition(args, base, disk, partition)
    for name in ('authorization.log', 'helper-authentication.png'):
        with (base / name).open('wb') as output:
            run(['debugfs', '-R', 'cat /home/rufus/' + name, str(base / 'system.raw')], stdout=output, stderr=subprocess.PIPE)
    action = b'string "org.rufus.Prepare' if args.device else b'string "org.rufus.TestCreate"'
    assert (base / 'authorization.log').read_bytes().count(action) == (9 if args.device else 3)
    assert (base / 'helper-authentication.png').read_bytes().startswith(b'\x89PNG\r\n\x1a\n')
    for path in inputs:
        if path.name not in ('system.raw', 'usb.raw'):
            assert digest(path) == manifest['inputs'][str(path)], path
    (base / 'results.json').write_text(json.dumps(reports, indent=2) + '\n')
    print('PASS: native authorization scenarios and independent GPT/FAT32 disk readback passed.', flush=True)


if __name__ == '__main__':
    main()
