#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate a self-contained EFI tree in an ISO and boot the helper's USB output."""
import gzip
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

import prepare
from boot_linux import firmware_options
from fat_extract import add_executable, digest, run, values

BOOT_TIMEOUT_SECONDS = 120
BOOT_LOADER = Path('/usr/lib/systemd/boot/efi/systemd-bootx64.efi')
BOOT_CONFIG = 'loader/entries/rufus.conf'
SOURCE_CONFIG = ('title Rufus ISO fixture\nlinux /boot/vmlinuz\ninitrd /boot/initrd.gz\n'
                 'options console=ttyS0 panic=-1 rufus.iso_fixture=1 rufus.fixture_label=RUFUS_UEFI\n')
USB_CONFIG = ('title Rufus ISO fixture\nlinux /boot/vmlinuz\ninitrd /boot/initrd.gz\n'
              'options console=ttyS0 panic=-1 rufus.iso_fixture=1 rufus.fixture_label=RUFUS\\x20TEST\n')

AUX_SOURCE = {
    'EFI/BOOT/isolinux.cfg': 'append RUFUS_UEFI\n',
    'syslinux.cfg': '# keep original root config\n',
    'boot/grub/grub.cfg': 'set root=cd9660:/dev/iso9660/RUFUS_UEFI\n',
}
USB_CONFIGS = {
    BOOT_CONFIG: USB_CONFIG,
    'EFI/BOOT/isolinux.cfg': 'append RUFUS\\x20TEST\n',
    'EFI/BOOT/syslinux.cfg': 'append RUFUS\\x20TEST\n',
    'boot/grub/grub.cfg': 'set root=msdosfs:/dev/msdosfs/RUFUS TEST\n',
}


def make_fixture(args, base):
    # Reuse the guest's native kernel/modules; only replace its initial userspace.
    init = base / 'uefi-initramfs'
    shutil.copytree(base / 'initramfs', init, symlinks=True)
    (init / 'target').mkdir()
    for name in ('sha256sum', 'umount'):
        add_executable(init, Path('/usr/bin') / name)
    prepare.write(init, 'init', (prepare.HERE / 'guest-uefi-iso').read_text(), 0o755)
    prepare.write(init, 'etc/rufus-expected-kernel', args.kernel + '\n')
    archive = base / 'uefi-initramfs.cpio'
    names = run(['find', '.', '-print0'], cwd=init, capture_output=True).stdout
    with archive.open('wb') as output:
        run(['cpio', '--null', '-o', '-H', 'newc', '--owner=0:0'], cwd=init, input=names, stdout=output)
    tree = base / 'fixtures/UEFI'
    for name in ('EFI/BOOT', 'boot', 'loader/entries'):
        (tree / name).mkdir(parents=True, exist_ok=True)
    shutil.copy2(BOOT_LOADER, tree / 'EFI/BOOT/BOOTX64.EFI')
    shutil.copy2(base / 'vmlinuz', tree / 'boot/vmlinuz')
    with archive.open('rb') as source, (tree / 'boot/initrd.gz').open('wb') as output:
        with gzip.GzipFile(fileobj=output, mode='wb', mtime=0) as compressed:
            shutil.copyfileobj(source, compressed)
    prepare.write(tree, 'loader/loader.conf',
                  'default rufus.conf\ntimeout 0\neditor no\nauto-entries no\nauto-firmware no\n')
    prepare.write(tree, BOOT_CONFIG, SOURCE_CONFIG)
    for name, content in AUX_SOURCE.items():
        prepare.write(tree, name, content)
    prepare.write(tree, 'rufus-iso-payload', 'RUFUS-UEFI-ISO-TREE\n')
    checksums = []
    for path in sorted(tree.rglob('*')):
        if path.is_file():
            relative = str(path.relative_to(tree))
            # The booted USB verifies the expected changed configuration too.
            checksum = hashlib.sha256(USB_CONFIGS[relative].encode()).hexdigest() if relative in USB_CONFIGS else digest(path)
            checksums.append(checksum + '  ' + relative)
    for name, content in USB_CONFIGS.items():
        if not (tree / name).exists():
            checksums.append(hashlib.sha256(content.encode()).hexdigest() + '  ' + name)
    prepare.write(tree, 'payload.sha256', '\n'.join(checksums) + '\n')
    image = base / 'fixtures/UEFI.iso'
    run([args.mkisofs, '-quiet', '-R', '-J', '-input-charset', 'utf-8', '-V', 'RUFUS_UEFI',
         '-o', str(image), str(tree)], capture_output=True)
    check_source(args.build, image, base / 'source-scan.log')
    (base / 'uefi-fixture.json').write_text(json.dumps({
        'kernel': args.kernel, 'boot_loader': str(BOOT_LOADER), 'boot_loader_sha256': digest(BOOT_LOADER),
        'iso_sha256': digest(image), 'payload_checksums': checksums,
        'scope': 'Generated ISO file tree, x86_64 UEFI USB boot; no optical boot catalog or distro installer.'
    }, indent=2) + '\n')
    archive.unlink()
    shutil.rmtree(init)
    return image


def check_source(build, image, log):
    scan = run([str(build / 'iso_scan_probe'), str(image), 'rr'], capture_output=True, text=True)
    log.write_text(scan.stdout + scan.stderr)
    lines = scan.stdout.splitlines()
    for expected in ('STATUS=tree-complete', 'BOOT=0\t/EFI/BOOT/BOOTX64.EFI', 'GRUB=0', 'BOOTMGR=0',
                     'WININST=0', 'NEEDS_NTFS=0', 'DISABLE_ISO=0', 'SYMLINKS=0'):
        assert expected in lines, scan.stdout
    assert values(scan.stdout)['EFI'] != 0, scan.stdout


def check_copied(base):
    expected, actual = {}, {}
    expected_dirs, actual_dirs = set(), set()
    for root, files, directories in ((base / 'fixtures/UEFI', expected, expected_dirs),
                                     (base / 'copied', actual, actual_dirs)):
        for path in root.rglob('*'):
            assert not path.is_symlink(), path
            relative = str(path.relative_to(root))
            if path.is_dir():
                directories.add(relative)
            else:
                files[relative] = digest(path)
    assert (base / 'fixtures/UEFI' / BOOT_CONFIG).read_text() == SOURCE_CONFIG
    for name, content in AUX_SOURCE.items():
        assert (base / 'fixtures/UEFI' / name).read_text() == content
    for name, content in USB_CONFIGS.items():
        expected[name] = hashlib.sha256(content.encode()).hexdigest()
    assert actual == expected, (actual, expected)
    assert actual_dirs == expected_dirs, (actual_dirs, expected_dirs)


def boot_disk(args, base, disk, missing_payload=False):
    base.mkdir()
    original = digest(disk)
    command = ['qemu-system-x86_64', '-accel', args.accel,
               '-cpu', 'host' if args.accel == 'kvm' else 'max', '-m', '512',
               '-display', 'none', '-monitor', 'none', '-no-reboot', '-nic', 'none',
               '-serial', 'file:' + str(base / 'serial.log'), '-snapshot',
               '-smbios', 'type=1,manufacturer=RufusTest,product=RufusIsoBootFixture',
               '-device', 'qemu-xhci,id=xhci',
               '-drive', 'if=none,id=stick,format=raw,file=' + str(disk),
               '-device', 'usb-storage,drive=stick,removable=on,bootindex=1']
    command.extend(firmware_options(base, 'uefi', 'iso'))
    (base / 'qemu-command.json').write_text(json.dumps(command, indent=2) + '\n')
    environment = os.environ.copy()
    environment['TMPDIR'] = str(base)
    with (base / 'qemu.log').open('wb') as log:
        subprocess.run(command, check=True, timeout=BOOT_TIMEOUT_SECONDS, env=environment,
                       stdout=log, stderr=subprocess.STDOUT)
    lines = (base / 'serial.log').read_text(errors='replace').splitlines()
    passed = 'RUFUS_UEFI_ISO_BOOT_OK' in lines
    if missing_payload:
        assert not passed, base
        assert 'RUFUS_UEFI_ISO_MISSING_PAYLOAD' in lines, base
        assert 'RUFUS_UEFI_ISO_EXIT 1' in lines, base
    else:
        assert passed and 'RUFUS_UEFI_ISO_EXIT 0' in lines, base
    assert digest(disk) == original, 'Boot modified its verified backing image'
    return {'disk_sha256': original, 'missing_payload': missing_payload, 'passed': True}


def qualify_boot(args, base, disk, offset):
    results = {'uefi': boot_disk(args, base / 'uefi-boot', disk)}
    broken = base / 'missing-payload.raw'
    shutil.copyfile(disk, broken)
    run(['mdel', '-i', str(broken) + '@@' + str(offset), '::/rufus-iso-payload'], capture_output=True)
    results['missing-payload'] = boot_disk(args, base / 'uefi-missing-payload', broken, True)
    print('PASS: UEFI USB reached Linux userspace and checked all payload hashes; missing payload refused.', flush=True)
    return results
