#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Create FAT32 Windows media through the real helper, then verify every SWM resource."""
import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess
import xml.etree.ElementTree as ET

import helper
import prepare
from fat_extract import digest, values
from test_partition_image import make_image, inspect_gpt, check_preserved, extent

DISK_BYTES = 16 * 1024**3
CLUSTER_BYTES = 4096
FAT_MAX_FILE_BYTES = 2**32 - 1
GUEST_TIMEOUT_SECONDS = 3600
IO_TIMEOUT_SECONDS = 600
LABEL = 'WINDOWS 11'
INSTALL_WIM = 'sources/install.wim'


def configure_guest(args, base, root):
    prepare.write(root, 'etc/rufus-wim-split-fixture', 'RUFUS-WIM-SPLIT-TEST\n')
    prepare.write(root, 'etc/rufus-wim-split-source', '/source/' + args.image.name + '\n')
    user = (prepare.HERE / 'helper-user.sh').read_text()
    user = user.replace('test_helper_guest -v1', 'test_prepare_device_guest -v1 nativeWimSplitExtraction')
    user = user.replace('export DISPLAY=', f'export QTEST_FUNCTION_TIMEOUT={GUEST_TIMEOUT_SECONDS * 1000}\nexport DISPLAY=')
    prepare.write(root, 'opt/rufus/guest-user.sh', user, 0o755)
    shutil.copy2(args.build / 'wim_split_verify', root / 'opt/rufus/wim_split_verify')
    # Packages provide Qt and the desktop. Include the build's optional native
    # libraries (hivex and the independent system wimlib) without host installs.
    for name in ('rufus-prepare-helper', 'test_prepare_device_guest', 'wim_split_verify'):
        linked = subprocess.check_output(['ldd', str(args.build / name)], text=True)
        assert 'not found' not in linked, linked
        for library in re.findall(r'(/[^\s()]+)', linked):
            library = Path(library)
            destination = root / 'usr/lib' / library.name
            if library.is_file() and not destination.exists():
                shutil.copy2(library, destination)
    kernel = Path('/usr/lib/modules') / args.kernel
    for name in ('9p', '9pnet_virtio'):
        dependencies = subprocess.check_output(['modprobe', '--show-depends', '-S', args.kernel, name], text=True)
        for line in dependencies.splitlines():
            if line.startswith('insmod '):
                module = Path(line.split()[1]).resolve()
                destination = root / 'usr/lib/modules' / args.kernel / module.relative_to(kernel)
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(module, destination)
    subprocess.run(['depmod', '-b', str(root), args.kernel], check=True)


def check_source(args, base):
    # Reuse a prior independent 7-Zip manifest only after hashing the complete
    # ISO. This avoids another extracted installation tree and WIM copy.
    original = json.loads(args.source_manifest.read_text())
    assert not original.get('privacy_customization'), 'Use an unmodified source manifest'
    assert digest(args.image) == original['sha256'], 'ISO no longer matches the reference manifest'
    command = [str(args.build / 'iso_write_check_probe'), str(args.image), str(DISK_BYTES),
               str(CLUSTER_BYTES), LABEL, 'fat32-split']
    result = subprocess.run(command, check=True, capture_output=True, text=True, timeout=IO_TIMEOUT_SECONDS)
    preflight = json.loads(result.stdout)
    assert preflight['error'] == 0 and preflight['splitInstallWim'], preflight
    assert preflight['extraPartitions'] == 0, preflight
    assert int(preflight['files']) == len(original['contents']['files']), preflight
    (base / 'preflight.json').write_text(result.stdout)
    (base / 'preflight.log').write_text(result.stderr)
    with (base / 'source-inventory.txt').open('wb') as output:
        result = subprocess.run([str(args.build / 'wim_split_source_probe'), str(args.image)],
                                check=True, stdout=output, stderr=subprocess.PIPE, timeout=IO_TIMEOUT_SECONDS)
    (base / 'source-inventory.log').write_bytes(result.stderr)
    wim_bytes = values(result.stderr.decode())['WIM_BYTES']
    assert wim_bytes > FAT_MAX_FILE_BYTES, wim_bytes
    record = {'source': str(args.image), 'sha256': original['sha256'], 'wim_bytes': wim_bytes,
              'reference_manifest': str(args.source_manifest), 'contents': original['contents'],
              'preflight': preflight, 'customization_flags': 0}
    (base / 'source.json').write_text(json.dumps(record, indent=2) + '\n')
    return record


def run_guest(args, base, disk):
    command = ['qemu-system-x86_64', '-accel', args.accel,
               '-cpu', 'host' if args.accel == 'kvm' else 'max', '-m', '4096',
               '-display', 'none', '-monitor', 'none', '-serial', 'file:' + str(base / 'serial.log'),
               '-no-reboot', '-nic', 'none', '-kernel', str(base / 'vmlinuz'),
               '-initrd', str(base / 'initramfs.cpio'),
               '-append', 'root=/dev/vda rw console=ttyS0 rufus.usb_fixture=1',
               '-smbios', 'type=1,manufacturer=RufusTest,product=RufusUSBFixture',
               '-drive', 'format=raw,if=virtio,file=' + str(base / 'system.raw'),
               '-device', 'qemu-xhci,id=xhci',
               '-drive', 'if=none,id=stick,format=raw,file=' + str(disk),
               '-device', 'usb-storage,drive=stick,removable=on,serial=RUFUS-GPT-TEST',
               '-virtfs', 'local,path=' + str(args.image.parent) +
               ',mount_tag=source,security_model=none,readonly=on']
    (base / 'qemu-command.json').write_text(json.dumps(command, indent=2) + '\n')
    with (base / 'qemu.log').open('wb') as log:
        subprocess.run(command, check=True, timeout=GUEST_TIMEOUT_SECONDS, stdout=log, stderr=subprocess.STDOUT)


def read_inventory(path):
    lines = path.read_text().splitlines()
    assert lines[0].startswith('IMAGES ') and lines[1].startswith('XML '), path
    images = int(lines[0].split()[1])
    xml = ET.fromstring(bytes.fromhex(lines[1][4:]))
    # libxml may change whitespace when writing a part. Preserve element order,
    # all attributes and actual values while ignoring indentation only.
    for element in xml.iter():
        if element.text is not None and not element.text.strip():
            element.text = None
        if element.tail is not None and not element.tail.strip():
            element.tail = None
    canonical = ET.canonicalize(ET.tostring(xml, encoding='unicode'))
    resources = sorted(lines[2:])
    assert sum(line.startswith('M ') for line in resources) == images, path
    return images, canonical, resources


def check_readback(args, base, disk, segments, source):
    # Always export evidence before assertions or removal of the guest disk.
    names = ('authorization.log', 'helper-authentication.png', 'created-disk.checksum',
             'fat-check.log', 'windows-files.sha256z', 'windows-directories', 'split-parts.txt',
             'split-inventory.txt', 'split-verify.log', 'private-mount-info')
    for name in names:
        with (base / name).open('wb') as output:
            subprocess.run(['debugfs', '-R', 'cat /home/rufus/' + name, str(base / 'system.raw')],
                           check=True, stdout=output, stderr=subprocess.PIPE, timeout=30)
    serial = (base / 'serial.log').read_text(errors='replace')
    required = ('RUFUS_HELPER_TEST_EXIT 0', 'RUFUS_HELPER_GUEST_FINISHED', 'RUFUS_HELPER_AUTH_REQUESTS 1',
                'RUFUS_WIM_SPLIT_PASSED REQUESTS=1', 'RUFUS_NATIVE_PRIVATE_MOUNT',
                'RUFUS_WIM_SPLIT_READBACK_WRITE_PROTECTED', 'RUFUS_WIM_SPLIT_READBACK_PASSED')
    for marker in required:
        assert marker in serial, (marker, base / 'serial.log')
    for diagnostic in ('AddressSanitizer:', 'LeakSanitizer:', 'runtime error:', 'SUPERVISOR_FAILED'):
        assert diagnostic not in serial, diagnostic
    completion = re.search(r'RUFUS_WIM_SPLIT_PASSED REQUESTS=1 BYTES=(\d+)', serial)
    assert completion is not None, 'Missing logical copied-byte count'
    copied_bytes = int(completion[1])
    assert copied_bytes == int(source['preflight']['bytes']), copied_bytes
    assert (base / 'authorization.log').read_bytes().count(b'string "org.rufus.Prepare') == 1
    assert (base / 'helper-authentication.png').read_bytes().startswith(b'\x89PNG\r\n\x1a\n')
    expected_inventory = read_inventory(base / 'source-inventory.txt')
    actual_inventory = read_inventory(base / 'split-inventory.txt')
    assert actual_inventory == expected_inventory, 'Source and split WIM inventories differ'
    parts = dict(line.rsplit(' ', 1) for line in (base / 'split-parts.txt').read_text().splitlines())
    parts = {name: int(size) for name, size in parts.items()}
    assert len(parts) > 1, parts
    expected_names = {'sources/install.swm'} | {f'sources/install{number}.swm' for number in range(2, len(parts) + 1)}
    assert set(parts) == expected_names, parts
    assert all(0 < size <= FAT_MAX_FILE_BYTES for size in parts.values()), parts
    files = {}
    for entry in (base / 'windows-files.sha256z').read_bytes().split(b'\0'):
        if entry:
            assert entry[64:66] == b'  ' and len(entry) > 66, entry
            name = entry[66:].decode('utf-8')
            assert name not in files, name
            files[name] = entry[:64].decode('ascii')
    expected_files = dict(source['contents']['files'])
    del expected_files[INSTALL_WIM]
    assert files == expected_files, 'Ordinary ISO file hashes differ'
    directories = sorted(name.decode() for name in (base / 'windows-directories').read_bytes().split(b'\0') if name)
    assert directories == source['contents']['directories'], 'Output directories differ'
    preflight = source['preflight']
    actual_bytes = int(preflight['bytes']) - source['wim_bytes'] + sum(parts.values())
    assert actual_bytes <= int(preflight['requiredBytes']), (actual_bytes, preflight)
    inspect_gpt(args, disk, DISK_BYTES, CLUSTER_BYTES)
    offset, length = extent(DISK_BYTES, CLUSTER_BYTES)
    check_preserved(disk, DISK_BYTES, segments, (offset, length))
    with disk.open('rb') as raw:
        raw.seek(offset)
        boot = raw.read(512)
    assert boot[82:90] == b'FAT32   ' and boot[71:82] == LABEL.encode().ljust(11), boot
    return {'passed': True, 'customization_flags': 0, 'authorization_requests': 1,
            'source_sha256': source['sha256'], 'ordinary_files': len(files), 'parts': parts,
            'logical_source_bytes': copied_bytes, 'actual_bytes': actual_bytes,
            'required_bytes': int(preflight['requiredBytes']),
            'images': expected_inventory[0], 'resources': len(expected_inventory[2]),
            'independent_verification': 'system wimlib verifies every image and resource on read-only FAT32',
            'source_matches_split': True, 'sync_cleanup_and_helper_exit': True}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--kernel', required=True)
    parser.add_argument('--cache', type=Path, default=Path('/var/cache/pacman/pkg'))
    parser.add_argument('--accel', choices=['kvm', 'tcg'], default='kvm')
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--source-manifest', type=Path, required=True,
                        help='Prior windows_iso.py independent unmodified-source manifest; ISO hash must match')
    parser.add_argument('--sfdisk', default='sfdisk')
    parser.add_argument('--keep-usb-for-boot', action='store_true',
                        help='Retain only the verified USB for the immediately following boot checkpoint')
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    args.build, args.image = args.build.resolve(), args.image.resolve(strict=True)
    base = args.output.resolve()
    assert not base.exists(), 'Choose a new output directory'
    assert args.image.is_file() and args.image.stat().st_uid == 1000, args.image
    for path in (base, args.image):
        assert not any(character in str(path) for character in (',', '\n', '\r')), path
    assert shutil.disk_usage(base.parent).free > 24 * 1024**3, 'Need 24 GiB temporary free space'
    base.mkdir()
    source = check_source(args, base)
    args.device, args.wim_split = True, True
    args.ntfs = args.extract = args.uefi = args.widgets = args.windows_privacy = False
    args.alpine = args.large_file = args.windows = None
    print('Building disposable guest; source ISO stays on its read-only share.', flush=True)
    packages = helper.make_guest(args, base)
    inputs = {name: digest(args.build / name) for name in
              ('rufus-prepare-helper', 'test_prepare_device_guest', 'wim_split_verify',
               'wim_split_source_probe', 'iso_write_check_probe')}
    (base / 'inputs.json').write_text(json.dumps({'packages': packages, 'binaries': inputs}, indent=2) + '\n')
    disk, segments = make_image(base, 'usb.raw', DISK_BYTES, CLUSTER_BYTES)
    print('Running one authorized FAT32 creation and independent split-WIM verification.', flush=True)
    run_guest(args, base, disk)
    report = check_readback(args, base, disk, segments, source)
    # The share is read-only at QEMU and guest levels; confirm the host input
    # hash still matches before declaring the checkpoint successful.
    assert digest(args.image) == source['sha256'], 'Source ISO changed during the test'
    (base / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    for name in ('system.raw', 'initramfs.cpio', 'vmlinuz'):
        (base / name).unlink()
    shutil.rmtree(base / 'initramfs')
    if args.keep_usb_for_boot:
        retention = {'path': str(disk), 'reason': 'Immediately following Windows Setup UEFI boot checkpoint',
                     'cleanup': 'Remove after saving boot evidence; do not retain for another installation trial'}
        (base / 'retained-usb.json').write_text(json.dumps(retention, indent=2) + '\n')
        print('PASS: real Windows WIM split verified; only the USB is retained for the next boot check.', flush=True)
    else:
        disk.unlink()
        print('PASS: real Windows WIM split verified; disposable disks removed.', flush=True)


if __name__ == '__main__':
    main()
