#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Qualify one real polkit request for each GPT/filesystem guest-helper job."""
import argparse
import errno
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tarfile

import prepare
import uefi_iso
import alpine_iso
import large_iso
import windows_iso
from gpt import inspect_formatted_partition, DISK_BYTES, CASES
from fat_extract import digest, values
from test_partition_image import make_image, inspect_gpt, check_preserved, trailing_extent
from test_iso_extract import make_image as make_iso, check_tree
from test_fat_format import copy_volume
from test_ntfs_format import inspect as inspect_ntfs
from test_prepare_helper import inspect_fat16

GUEST_TIMEOUT_SECONDS = 480
TOOL_TIMEOUT_SECONDS = 30
SYSTEM_IMAGE_TIMEOUT_SECONDS = 120
NTFS_CLUSTER_BYTES = 4096
FAT16_CLUSTER_BYTES = 2048
NTFS_LABEL = 'Rufus NTFS ș😀'
UEFI_NTFS_IMAGE = Path(__file__).resolve().parents[3] / 'res/uefi/uefi-ntfs.img'
PREPARE_CASES = ('cancelAuthorization changedSequence changedIdentity fileRequestIsNotElevated '
                 'cancelActiveFormatter callerDeathCleansUp nativeSuccess mountedTargetIsRefused noAgentDoesNotRetry')
BOOT_CASES = 'cancelBootWrite callerDeathDuringBootWrite bootWriteChecks '
PREPARE_REQUESTS = 9
BOOT_REQUESTS = 5
EXTRACT_REQUESTS = 9
EXTRACT_CASES = ('changedSourceIsRefused unsupportedSourceIsRefused rootOwnedSourceIsRefused '
                 'cancelMountedExtraction callerDeathDuringExtraction mountAndCleanupFailures nativeExtraction')


def extraction_entries():
    return {'MiXeD/ș-é.txt': b'UTF-8 names', 'empty': b'', 'one': b'x',
            'boot/data.bin': bytes(range(256)) * 8192 + b'tail',
            'ldlinux.sys': b'installer owns this file', 'boot/ldlinux.sys': b'keep nested loader',
            'space dir/long name.txt': b'spaces', 'bad:name.txt': b'substitution'}


def run(command, **kwargs):
    return subprocess.run(command, check=True, timeout=TOOL_TIMEOUT_SECONDS, **kwargs)


def make_guest(args, base):
    # Reuse the installed-version package selection and real desktop session.
    prepare.ROOT_PACKAGES = prepare.ROOT_PACKAGES + ['dosfstools']
    if args.device:
        prepare.ROOT_PACKAGES += ['hivex']
    if args.ntfs:
        prepare.ROOT_PACKAGES += ['ntfsprogs-plus']
    if args.ntfs and args.extract:
        prepare.ROOT_PACKAGES += ['fuse2', 'tar']
    packages = prepare.cached_packages(args.cache)
    root = base / 'root'
    root.mkdir()
    for package in packages:
        run(['bsdtar', '-xpf', str(package), '--no-same-owner', '--no-xattrs',
             '--exclude=.PKGINFO', '--exclude=.BUILDINFO', '--exclude=.MTREE', '--exclude=.INSTALL', '-C', str(root)])
    if args.ntfs and args.extract:
        for package in (args.ntfs_3g_package, args.ntfs_library_package):
            run(['bsdtar', '-xpf', str(package), '--no-same-owner', '--no-xattrs',
                 '--exclude=.PKGINFO', '--exclude=.BUILDINFO', '--exclude=.MTREE', '--exclude=.INSTALL', '-C', str(root)])
            packages.append(package)
        driver = 'fuseblk' if args.ntfs_fallback else 'ntfs'
        prepare.write(root, 'etc/rufus-ntfs-mount', driver + '\n')
        # Only this guest wrapper injects startup failure; every other request
        # executes the packaged foreground NTFS-3G driver unchanged.
        native = root / 'usr/bin/lowntfs-3g'
        native.rename(root / 'usr/bin/lowntfs-3g-native')
        prepare.write(root, 'usr/bin/lowntfs-3g', '''#!/bin/bash
set -eu
[[ $(< /etc/rufus-ntfs-fixture) == RUFUS-NTFS-TEST ]]
if [[ -f /home/rufus/extract-fault && $(< /home/rufus/extract-fault) == mount ]]; then
    exit 5
fi
exec /usr/bin/lowntfs-3g-native "$@"
''', 0o755)
    dbus_helper = root / 'usr/lib/dbus-daemon-launch-helper'
    if dbus_helper.exists():
        dbus_helper.chmod(dbus_helper.stat().st_mode | 0o400)
    prepare.configure_guest(root, args.build / 'test_usb_guest')
    binaries = ['test_gpt_guest', 'test_helper_guest']
    if args.device:
        binaries += ['rufus-prepare-helper', 'prepare_helper_probe', 'test_prepare_device_guest']
    if args.extract:
        binaries.append('prepare_extract_probe')
    if args.ntfs:
        binaries += ['ntfs_format_probe', 'prepare_boot_probe']
        prepare.write(root, 'etc/rufus-ntfs-fixture', 'RUFUS-NTFS-TEST\n')
    for name in binaries:
        shutil.copy2(args.build / name, root / 'opt/rufus' / name)
    prepare.write(root, 'opt/rufus/guest-root.sh', (prepare.HERE / 'helper-root.sh').read_text(), 0o755)
    prepare.write(root, 'opt/rufus/guest-user.sh', (prepare.HERE / 'helper-user.sh').read_text(), 0o755)
    prepare.write(root, 'etc/rufus-gpt-fixture', 'RUFUS-GPT-TEST\n')
    if getattr(args, 'fat16', False):
        prepare.write(root, 'etc/rufus-fat16-fixture', 'RUFUS-FAT16-TEST\n')
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
        cases = EXTRACT_CASES if args.extract else PREPARE_CASES
        if args.ntfs and not args.extract:
            cases = cases.replace('nativeSuccess', BOOT_CASES + 'nativeSuccess')
        if args.uefi:
            cases = 'nativeUefiWidgets' if args.widgets else 'nativeUefiExtraction'
            if getattr(args, 'fat16', False):
                cases = EXTRACT_CASES.replace('nativeExtraction', 'nativeUefiExtraction')
        if args.alpine:
            cases = 'nativeAlpineExtraction'
        if args.large_file:
            cases = 'largeFileFatIsRefused nativeLargeExtraction'
            user_script = user_script.replace('export DISPLAY=',
                f'export QTEST_FUNCTION_TIMEOUT={large_iso.GUEST_TIMEOUT_SECONDS * 1000}\nexport DISPLAY=')
        if args.windows:
            cases = 'nativeUefiWidgets'
            user_script = user_script.replace('export DISPLAY=',
                f'export QTEST_FUNCTION_TIMEOUT={windows_iso.GUEST_TIMEOUT_SECONDS * 1000}\nexport DISPLAY=')
        user_script = user_script.replace(' -v1', ' -v1 ' + cases)
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
        if args.ntfs:
            boot_policy = probe_policy.replace('org.rufus.PrepareProbe', 'org.rufus.PrepareBootProbe')
            boot_policy = boot_policy.replace('/opt/rufus/prepare_helper_probe', '/opt/rufus/prepare_boot_probe')
            prepare.write(root, 'usr/share/polkit-1/actions/org.rufus.PrepareBootProbe.policy', boot_policy)
        if args.extract:
            extract_policy = probe_policy.replace('org.rufus.PrepareProbe', 'org.rufus.PrepareExtractProbe')
            extract_policy = extract_policy.replace('/opt/rufus/prepare_helper_probe', '/opt/rufus/prepare_extract_probe')
            prepare.write(root, 'usr/share/polkit-1/actions/org.rufus.PrepareExtractProbe.policy', extract_policy)
            prepare.write(root, 'etc/rufus-extract-fixture', 'RUFUS-EXTRACT-TEST\n')
            fixtures = base / 'fixtures'
            fixtures.mkdir()
            image = make_iso(fixtures, 'TREE', extraction_entries(), args.mkisofs, directories=['empty-dir/nested'])
            link = make_iso(fixtures, 'LINK', {'data.bin': b'data'}, args.mkisofs, special='symlink')
            shutil.copy2(image, root / 'home/rufus/TREE.iso')
            shutil.copy2(link, root / 'home/rufus/LINK.iso')
            shutil.copy2(image, root / 'opt/rufus/root-owned.iso')
            if args.large_file:
                image, report = large_iso.make_fixture(args, fixtures, extraction_entries())
                # mkfs.ext4 imports the file into the guest; avoid another host copy.
                (root / 'home/rufus/LARGE.iso').hardlink_to(image)
                prepare.write(root, 'etc/rufus-large-file-path', report['path'] + '\n')
                prepare.write(root, 'etc/rufus-large-file-bytes', str(report['copied_bytes']) + '\n')
    prepare.bootstrap(base, root, args.kernel)
    if args.ntfs:
        kernel = Path('/usr/lib/modules') / args.kernel
        modules = ['ntfs3', 'nls_utf8']
        if args.extract:
            modules += ['fuse'] if args.ntfs_fallback else ['ntfs', 'fuse']
        for name in modules:
            dependencies = run(['modprobe', '--show-depends', '-S', args.kernel, name],
                               capture_output=True, text=True).stdout
            for line in dependencies.splitlines():
                if not line.startswith('insmod '):
                    continue
                module = Path(line.split()[1]).resolve()
                destination = root / 'usr/lib/modules' / args.kernel / module.relative_to(kernel)
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(module, destination)
        run(['depmod', '-b', str(root), args.kernel])
    if args.uefi:
        image = uefi_iso.make_fixture(args, base)
        shutil.copy2(image, root / 'home/rufus/UEFI.iso')
    if args.alpine:
        alpine_iso.prepare_image(args, base, root)
    if args.windows:
        windows_iso.prepare_image(args, base, root)
    if getattr(args, 'wim_split', False):
        import wim_split
        wim_split.configure_guest(args, base, root)
    system = base / 'system.raw'
    system_bytes = large_iso.SYSTEM_BYTES if args.large_file else 3 * 1024**3
    system_timeout = large_iso.SYSTEM_IMAGE_TIMEOUT_SECONDS if args.large_file else SYSTEM_IMAGE_TIMEOUT_SECONDS
    if args.windows:
        system_bytes = windows_iso.SYSTEM_BYTES
        system_timeout = windows_iso.IO_TIMEOUT_SECONDS
    with system.open('xb') as output:
        output.truncate(system_bytes)
    subprocess.run(['mkfs.ext4', '-q', '-F', '-d', str(root), str(system)],
                   check=True, timeout=system_timeout)
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
        timeout = large_iso.GUEST_TIMEOUT_SECONDS if args.large_file else GUEST_TIMEOUT_SECONDS
        if args.windows:
            timeout = windows_iso.GUEST_TIMEOUT_SECONDS
        subprocess.run(command, check=True, timeout=timeout, stdout=log, stderr=subprocess.STDOUT)
    text = serial.read_text(errors='replace')
    for diagnostic in ('AddressSanitizer:', 'LeakSanitizer:', 'runtime error:'):
        assert diagnostic not in text, (diagnostic, serial)
    if args.device:
        requests = PREPARE_REQUESTS + (BOOT_REQUESTS if args.ntfs and not args.extract else 0)
        specific = ('RUFUS_NATIVE_EXTRACTION_PASSED REQUESTS=9', 'RUFUS_NATIVE_PRIVATE_MOUNT') if args.extract else (
            f'RUFUS_NATIVE_AUTHORIZATION_PASSED REQUESTS={requests}', 'RUFUS_NATIVE_MOUNTED_REFUSAL')
        common = ('RUFUS_HELPER_TEST_EXIT 0', 'RUFUS_HELPER_GUEST_FINISHED')
        if args.uefi:
            uefi_requests = EXTRACT_REQUESTS if getattr(args, 'fat16', False) else 1
            specific = (f'RUFUS_HELPER_AUTH_REQUESTS {uefi_requests}',
                        f'RUFUS_NATIVE_UEFI_EXTRACTION_PASSED REQUESTS={uefi_requests}')
            if getattr(args, 'fat16', False):
                specific += ('RUFUS_NATIVE_PRIVATE_MOUNT', 'RUFUS_NATIVE_UNCHANGED')
        elif args.windows:
            specific = ('RUFUS_HELPER_AUTH_REQUESTS 1', 'RUFUS_WINDOWS_CREATION_PASSED REQUESTS=1')
            if args.windows_privacy:
                specific += ('RUFUS_WINDOWS_CUSTOMIZATION_PASSED FLAGS=8 REQUESTS=1',)
        elif args.alpine:
            specific = ('RUFUS_HELPER_AUTH_REQUESTS 1', 'RUFUS_NATIVE_ALPINE_EXTRACTION_PASSED REQUESTS=1')
        elif args.large_file:
            specific = ('RUFUS_HELPER_AUTH_REQUESTS 2', 'RUFUS_NATIVE_LARGE_EXTRACTION_PASSED REQUESTS=2',
                        'RUFUS_NATIVE_PRIVATE_MOUNT', 'RUFUS_NATIVE_UNCHANGED')
        else:
            common += (f'RUFUS_HELPER_AUTH_REQUESTS {requests}', 'RUFUS_NATIVE_UNCHANGED')
        for marker in common + specific:
            assert marker in text, (marker, serial)
        if args.widgets:
            assert 'RUFUS_ISO_WIDGETS_PASSED REQUESTS=1' in text, serial
        if args.ntfs and not args.extract:
            assert 'RUFUS_NTFS_FORMATTER_MOUNTED_REFUSED' in text, serial
            assert 'RUFUS_NTFS_CLAIM_RETAINED' in text, serial
            payload_bytes = UEFI_NTFS_IMAGE.stat().st_size
            for error, written, verified in ((errno.ECANCELED, 4096, 0), (errno.EIO, payload_bytes, 0),
                                             (errno.EBADMSG, payload_bytes, 0), (0, payload_bytes, payload_bytes)):
                report = f'ERROR={error} WRITTEN={written} VERIFIED={verified}'
                assert 'RUFUS_BOOT_BOUNDARIES_PASSED ' + report in text, (report, serial)
            assert 'RUFUS_BOOT_TEMPORARIES_REMOVED' in text, serial
        if args.ntfs and args.extract:
            driver = 'fuseblk' if args.ntfs_fallback else 'ntfs'
            for marker in ('RUFUS_NTFS_EXTRACTION_READBACK', 'RUFUS_BOOT_TEMPORARIES_REMOVED',
                           'RUFUS_NTFS_READBACK_WRITE_PROTECTED',
                           'RUFUS_NTFS_MOUNT ' + driver, 'RUFUS_NTFS_DAEMONS_REAPED'):
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
    parser.add_argument('--ntfs', action='store_true', help='Prepare NTFS and write bundled UEFI:NTFS with one authorized helper')
    parser.add_argument('--fat16', action='store_true', help='Use FAT16; --uefi includes copy faults and generated-image boot')
    parser.add_argument('--ntfs-fallback', action='store_true', help='Expect NTFS-3G on a kernel without the new NTFS driver')
    parser.add_argument('--ntfs-3g-package', type=Path, help='NTFS-3G package to unpack only in the disposable guest')
    parser.add_argument('--ntfs-library-package', type=Path, help='Matching libntfs-3g package for that guest')
    parser.add_argument('--extract', action='store_true', help='Qualify ISO extraction in the authorized device job')
    parser.add_argument('--large-file', choices=['iso', 'udf'], help='Extract a generated file beyond 4 GiB; requires --ntfs --extract')
    parser.add_argument('--uefi', action='store_true', help='Create and boot the generated UEFI ISO tree in one authorized job')
    parser.add_argument('--widgets', action='store_true', help='Use application START for the generated UEFI creation job')
    parser.add_argument('--alpine', type=Path, help='Create and boot the pinned Alpine standard ISO')
    parser.add_argument('--windows', type=Path, help='Create and independently verify a real Windows 11 ISO through Widgets')
    parser.add_argument('--windows-privacy', action='store_true', help='Select only Rufus\'s skip-privacy-questions customization; requires --windows')
    parser.add_argument('--mkisofs', default='mkisofs')
    parser.add_argument('--mcopy', default='/usr/bin/mcopy')
    parser.add_argument('--sfdisk', default='/usr/bin/sfdisk')
    parser.add_argument('--fsck', default='/usr/bin/fsck.fat')
    parser.add_argument('--mdir', default='/usr/bin/mdir')
    parser.add_argument('--ntfsinfo', default='/usr/bin/ntfsinfo')
    parser.add_argument('--ntfslabel', default='/usr/bin/ntfslabel')
    parser.add_argument('--ntfsck', default='/usr/bin/ntfsck')
    parser.add_argument('output', type=Path, help='New output directory; must not exist')
    args = parser.parse_args()
    if args.windows_privacy and not args.windows:
        parser.error('--windows-privacy requires --windows')
    if args.windows:
        if args.uefi or args.alpine or args.large_file:
            parser.error('--windows is a separate creation fixture')
        args.windows = args.windows.resolve()
        if not args.windows.is_file():
            parser.error('Provide a Windows 11 ISO file')
        args.ntfs = args.extract = args.widgets = True
    if args.large_file and not (args.ntfs and args.extract):
        parser.error('--large-file requires --ntfs --extract')
    fat_boot_fixture = args.uefi or bool(args.alpine)
    if args.ntfs and fat_boot_fixture:
        parser.error('--ntfs cannot be combined with --uefi or --alpine')
    if args.ntfs_fallback and not (args.ntfs and args.extract):
        parser.error('--ntfs-fallback requires --ntfs --extract')
    if args.ntfs and args.extract:
        for package in (args.ntfs_3g_package, args.ntfs_library_package):
            if package is None or not package.is_file():
                parser.error('NTFS extraction tests need both NTFS-3G guest packages')
    if args.widgets and not (args.uefi or args.windows):
        parser.error('--widgets requires --uefi or --windows')
    if args.uefi and args.alpine:
        parser.error('Select either the generated UEFI fixture or Alpine')
    if args.alpine:
        args.alpine = args.alpine.resolve()
        if not args.alpine.is_file() or digest(args.alpine) != alpine_iso.IMAGE_SHA256:
            parser.error('Provide the pinned Alpine standard ' + alpine_iso.VERSION + ' x86-64 ISO')
    args.extract = args.extract or args.uefi or bool(args.alpine)
    if args.fat16 and (args.ntfs or args.alpine or args.widgets):
        parser.error('--fat16 supports preparation, --extract, or generated --uefi without --widgets')
    args.device = args.device or args.extract or args.ntfs or args.fat16
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
    if args.extract and not (args.build / 'prepare_extract_probe').is_file():
        parser.error('Build prepare_extract_probe first')
    if args.large_file and not (args.build / 'iso_file_probe').is_file():
        parser.error('Build iso_file_probe first')
    if args.ntfs:
        for name in ('ntfs_format_probe', 'prepare_boot_probe'):
            if not (args.build / name).is_file():
                parser.error('Build ' + name + ' first')
    if args.uefi and not (args.build / 'iso_scan_probe').is_file():
        parser.error('Build iso_scan_probe first')
    if args.alpine and not (args.build / 'iso_extract_probe').is_file():
        parser.error('Build iso_extract_probe first')
    base.mkdir()
    (base / 'options.json').write_text(json.dumps(vars(args), default=str, indent=2) + '\n')
    packages = make_guest(args, base)
    qualify_guest(args, base, packages)


def qualify_guest(args, base, packages):
    disk_bytes = alpine_iso.DISK_BYTES if args.alpine else DISK_BYTES
    if args.large_file:
        disk_bytes = large_iso.DISK_BYTES
    if args.windows:
        disk_bytes = windows_iso.DISK_BYTES
    cluster = NTFS_CLUSTER_BYTES if args.ntfs else (FAT16_CLUSTER_BYTES if args.fat16 else 512)
    payload = UEFI_NTFS_IMAGE.stat().st_size if args.ntfs else 0
    disk, segments = make_image(base, 'usb.raw', disk_bytes, cluster, payload=payload)
    inputs = [base / name for name in ('vmlinuz', 'initramfs.cpio', 'system.raw', 'usb.raw')]
    inputs += [args.build / name for name in ('test_helper_guest', 'test_gpt_guest')]
    if args.device:
        inputs += [args.build / name for name in ('rufus-prepare-helper', 'prepare_helper_probe', 'test_prepare_device_guest')]
    if args.extract:
        inputs.append(args.build / 'prepare_extract_probe')
        inputs += list((base / 'fixtures').glob('*.iso'))
    if args.large_file:
        inputs += [args.build / 'iso_file_probe', base / 'fixtures/large-file.json']
    if args.ntfs:
        inputs += [args.build / 'ntfs_format_probe', args.build / 'prepare_boot_probe', UEFI_NTFS_IMAGE]
    if args.uefi:
        inputs += [args.build / 'iso_scan_probe', uefi_iso.BOOT_LOADER]
    if args.alpine:
        inputs += [args.alpine, args.build / 'iso_extract_probe']
    if args.windows:
        inputs += [args.windows, args.build / 'iso_write_check_probe', base / 'windows-source.json']
    manifest = {'kernel': args.kernel, 'accel': args.accel, 'packages': packages,
                'filesystem': 'NTFS' if args.ntfs else ('FAT16' if args.fat16 else 'FAT32'),
                'inputs': {str(path): digest(path) for path in inputs}}
    (base / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print('Running native helper authorization guest:', base, flush=True)
    reports = run_guest(args, base, disk)
    inspect_guest(args, base, disk, segments, manifest, reports)


def inspect_guest(args, base, disk, segments, manifest, reports):
    disk_bytes = disk.stat().st_size
    fat16 = getattr(args, 'fat16', False)
    cluster = NTFS_CLUSTER_BYTES if args.ntfs else (FAT16_CLUSTER_BYTES if fat16 else 512)
    payload = UEFI_NTFS_IMAGE.stat().st_size if args.ntfs else 0
    inputs = [Path(path) for path in manifest['inputs']]
    inspect_gpt(args, disk, disk_bytes, cluster, payload=payload)
    partition = reports['success']
    if args.ntfs:
        boot_offset, boot_size = trailing_extent(disk_bytes, payload)
        segments.append((boot_offset, UEFI_NTFS_IMAGE.read_bytes()))
        reports['boot_payload'] = {'offset': boot_offset, 'partition_size': boot_size,
                                   'bytes': payload, 'sha256': digest(UEFI_NTFS_IMAGE)}
    check_preserved(disk, disk_bytes, segments, (partition['OFFSET'], partition['SIZE']))
    if args.ntfs:
        volume = base / 'volume.raw'
        copy_volume(disk, volume, partition['OFFSET'], partition['SIZE'])
        with volume.open('rb') as source:
            serial = struct.unpack_from('<Q', source.read(512), 72)[0]
        tools = argparse.Namespace(info=args.ntfsinfo, label=args.ntfslabel, fsck=args.ntfsck)
        inspect_ntfs(tools, volume, partition['SIZE'], partition['OFFSET'], cluster,
                     windows_iso.LABEL if args.windows else NTFS_LABEL, {'VOLUME_ID': serial})
        partition['VOLUME_ID'] = serial
        if args.large_file or args.windows:
            # The original usb.raw retains identical partition bytes for review.
            volume.unlink()
    elif fat16:
        inspect_fat16(args, base, disk, partition['OFFSET'], partition['SIZE'], cluster, empty=not args.extract)
    else:
        inspect_formatted_partition(args, base, disk, partition, empty=not args.extract)
    if args.windows:
        windows_iso.check_copied(base)
        with (base / 'created-disk.checksum').open('wb') as output:
            run(['debugfs', '-R', 'cat /home/rufus/created-disk.checksum', str(base / 'system.raw')],
                stdout=output, stderr=subprocess.PIPE)
        expected_disk = (base / 'created-disk.checksum').read_text().split()[0]
        assert digest(disk) == expected_disk, 'Created Windows media changed during independent readback'
    elif args.extract:
        copied = base / 'copied'
        copied.mkdir()
        if args.ntfs:
            archive = base / 'ntfs-tree.tar'
            with archive.open('wb') as output:
                run(['debugfs', '-R', 'cat /home/rufus/ntfs-tree.tar', str(base / 'system.raw')],
                    stdout=output, stderr=subprocess.PIPE)
            with tarfile.open(archive) as tree:
                tree.extractall(copied, filter='data')
        else:
            run([args.mcopy, '-s', '-i', f"{disk}@@{partition['OFFSET']}", '::*', str(copied)])
        if args.uefi:
            uefi_iso.check_copied(base)
        elif args.alpine:
            alpine_iso.check_copied(base)
        else:
            expected = extraction_entries()
            del expected['ldlinux.sys']
            expected['bad_name.txt'] = expected.pop('bad:name.txt')
            empty = ['empty-dir/nested', 'sources'] if args.large_file else ['empty-dir/nested']
            check_tree(copied, expected, empty)
    if args.large_file:
        for name in ('large-file.sha256', 'large-file.size'):
            with (base / name).open('wb') as output:
                run(['debugfs', '-R', 'cat /home/rufus/' + name, str(base / 'system.raw')],
                    stdout=output, stderr=subprocess.PIPE)
        reports['large_file'] = large_iso.check_readback(base)
    screenshots = ('iso-confirmation.png', 'iso-complete.png') if args.widgets else ()
    if args.windows:
        screenshots += ('windows-options.png',)
    for name in ('authorization.log', 'helper-authentication.png') + screenshots:
        with (base / name).open('wb') as output:
            run(['debugfs', '-R', 'cat /home/rufus/' + name, str(base / 'system.raw')], stdout=output, stderr=subprocess.PIPE)
    action = b'string "org.rufus.Prepare' if args.device else b'string "org.rufus.TestCreate"'
    boot_fixture = args.uefi or bool(args.alpine) or bool(args.windows)
    device_requests = PREPARE_REQUESTS + (BOOT_REQUESTS if args.ntfs and not args.extract else 0)
    authorizations = 1 if boot_fixture else (device_requests if args.device else 3)
    if fat16 and args.uefi:
        authorizations = EXTRACT_REQUESTS
    if args.large_file:
        authorizations = 2
    assert (base / 'authorization.log').read_bytes().count(action) == authorizations
    assert (base / 'helper-authentication.png').read_bytes().startswith(b'\x89PNG\r\n\x1a\n')
    for path in inputs:
        if path.name not in ('system.raw', 'usb.raw'):
            assert digest(path) == manifest['inputs'][str(path)], path
    if args.uefi:
        reports['boot'] = uefi_iso.qualify_boot(args, base, disk, partition['OFFSET'])
    if args.alpine:
        reports['boot'] = alpine_iso.qualify_boot(args, base, disk)
    (base / 'results.json').write_text(json.dumps(reports, indent=2) + '\n')
    filesystem = 'NTFS' if args.ntfs else ('FAT16' if fat16 else 'FAT32')
    print(f'PASS: native authorization scenarios and independent GPT/{filesystem} disk readback passed.', flush=True)


if __name__ == '__main__':
    main()
