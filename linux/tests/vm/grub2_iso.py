# SPDX-License-Identifier: GPL-3.0-or-later
"""Pinned GRUB modules/config/kernel qualification for the existing MBR guest."""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess

import prepare
from fat_extract import digest, run, values
from test_fat_format import copy_volume
from test_mbr_image import extent, inspect_mbr, trailing_extent
from test_freedos_boot import read_window, expected_records
from syslinux_alpine import qmp_read, qmp_command, read_screen

MIB = 1024 * 1024
DISK_BYTES = 512 * MIB
SECTOR_BYTES = 512
FAT32_CLUSTER_BYTES = 4096
FAT16_CLUSTER_BYTES = 8192
NTFS_CLUSTER_BYTES = 4096
CORE_SHA256 = '15902895487c084c8f574666c31e7fa399d05b831fd61c1e3c3eee69d6576924'
NORMAL_SHA256 = 'ef4882c067ba6242690bf43746e7926d956bd7101e1cad3054337c9ea4fa6e67'
MODULES_SHA256 = 'bb36c84170d7193226777979ef0c6e826a5ff2730f94644d444c720b7dd61145'
LABEL = 'RUFUS_TEST'
SOURCE_LABEL = 'RUFUS_SOURCE'
BOOT_MARKER = 'RUFUS_GRUB_ISO_BOOT_OK'
BOOT_TIMEOUT_SECONDS = 120


def make_fixture(args, base):
    assert digest(args.grub_core) == CORE_SHA256, 'Use the checked-in signed Rufus 2.12 core'
    assert digest(args.grub_modules / 'normal.mod') == NORMAL_SHA256, 'Use the pinned Ubuntu 2.12 modules'
    module_files = {str(path.relative_to(args.grub_modules)): digest(path)
                    for path in args.grub_modules.rglob('*') if path.is_file()}
    listing = ''.join(f'{checksum}  {name}\n' for name, checksum in sorted(module_files.items()))
    assert hashlib.sha256(listing.encode()).hexdigest() == MODULES_SHA256, 'The complete module set changed'
    boot = base / 'grub-boot-fixture'
    boot.mkdir()
    root = boot / 'root'
    root.mkdir()
    (root / "lib").symlink_to("usr/lib")
    prepare.bootstrap(boot, root, args.kernel)
    filesystem = 'ntfs' if args.grub_ntfs else 'vfat'
    if args.grub_ntfs:
        kernel = Path('/usr/lib/modules') / args.kernel
        for name in ('ntfs', 'nls_utf8'):
            modules = subprocess.check_output(['modprobe', '--show-depends', '-S', args.kernel, name], text=True)
            for line in modules.splitlines():
                if line.startswith('insmod '):
                    module = Path(line.split()[1]).resolve()
                    destination = boot / 'initramfs/usr/lib/modules' / args.kernel / module.relative_to(kernel)
                    destination.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(module, destination)
        run(['depmod', '-b', str(boot / 'initramfs'), args.kernel])
    prepare.write(boot / 'initramfs', 'init', '''#!/bin/bash
set -eu
[[ $$ == 1 ]] || exit 77
export PATH=/usr/bin
mount -t devtmpfs devtmpfs /dev
mount -t proc proc /proc
mount -t sysfs sysfs /sys
[[ " $(< /proc/cmdline) " == *" root=LABEL=RUFUS_TEST "* ]]
for module in xhci_pci usb_storage sd_mod FILESYSTEM_MODULES; do modprobe "$module"; done
for attempt in {1..100}; do
    [[ -b /dev/sda1 ]] && break
    sleep .1
done
mount -t FILESYSTEM -o ro /dev/sda1 /newroot
[[ $(< /newroot/rufus-grub-image.txt) == "Rufus GRUB ISO fixture" ]]
echo "RUFUS_GRUB_ISO_BOOT_OK label=RUFUS_TEST normal=2.12"
echo o > /proc/sysrq-trigger
while :; do sleep 1; done
'''.replace('FILESYSTEM_MODULES', 'ntfs nls_utf8' if args.grub_ntfs else 'vfat nls_cp437 nls_iso8859_1')
        .replace('FILESYSTEM', filesystem), 0o755)
    init = boot / 'initramfs'
    names = subprocess.check_output(['find', '.', '-print0'], cwd=init)
    with (boot / 'initramfs.cpio').open('wb') as output:
        run(['cpio', '--null', '-o', '-H', 'newc', '--owner=0:0'], cwd=init, input=names, stdout=output)
    tree = base / 'grub-source'
    modules = tree / 'boot/grub/i386-pc'
    shutil.copytree(args.grub_modules, modules)
    shutil.copy2(boot / 'vmlinuz', tree / 'boot/vmlinuz')
    shutil.copy2(boot / 'initramfs.cpio', tree / 'boot/initramfs.cpio')
    prepare.write(tree, 'rufus-grub-image.txt', 'Rufus GRUB ISO fixture\n')
    prepare.write(tree, 'boot/grub/grub.cfg', '''insmod serial
serial --unit=0 --speed=115200
terminal_output serial
echo RUFUS_GRUB_NORMAL_CONFIG_OK
linux /boot/vmlinuz console=ttyS0,115200 panic=-1 root=LABEL=RUFUS_SOURCE
initrd /boot/initramfs.cpio
boot
''')
    if args.grub_uefi:
        loader = tree / 'EFI/BOOT/BOOTX64.EFI'
        loader.parent.mkdir(parents=True)
        shutil.copy2(args.build / 'uefi-handoff.efi', loader)
    args.iso = base / 'grub-source.iso'
    run([str(args.mkisofs), '-quiet', '-R', '-J', '-V', SOURCE_LABEL, '-o', str(args.iso), str(tree)])
    files = {str(path.relative_to(tree)): digest(path) for path in tree.rglob('*') if path.is_file()}
    (base / 'grub-source-manifest.json').write_text(json.dumps({
        'core_sha256': CORE_SHA256, 'normal_sha256': NORMAL_SHA256, 'modules_sha256': MODULES_SHA256,
        'module_source': 'https://archive.ubuntu.com/ubuntu/pool/main/g/grub2/grub-pc-bin_2.12-1ubuntu7_amd64.deb',
        'package_sha256': '5d5ee8a68cd9b75c859eb0c45ff3da5721fc6d4d8bb3673b98f6ce358db32014',
        'kernel_release': args.kernel, 'iso_sha256': digest(args.iso), 'files': files
    }, indent=2) + '\n')


def boot_written_usb(args, base, disk):
    before = digest(disk)
    serial = base / 'grub-image-bios-serial.log'
    firmware = Path('/usr/share/qemu/bios-256k.bin')
    command = ['qemu-system-x86_64', '-accel', args.accel, '-cpu', 'host' if args.accel == 'kvm' else 'max',
               '-m', '512', '-display', 'none', '-monitor', 'none', '-no-reboot', '-nic', 'none',
               '-serial', 'file:' + str(serial), '-qmp', 'stdio', '-bios', str(firmware), '-snapshot',
               '-device', 'qemu-xhci,id=xhci', '-drive', f'if=none,id=stick,format=raw,file={disk}',
               '-device', 'usb-storage,drive=stick,removable=on,bootindex=0']
    (base / 'grub-image-bios-command.json').write_text(json.dumps(command, indent=2) + '\n')
    with (base / 'grub-image-bios-qemu.log').open('wb') as log:
        environment = dict(os.environ, TMPDIR=str(base))
        process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log, env=environment)
        try:
            assert 'QMP' in qmp_read(process)
            qmp_command(process, 'qmp_capabilities')
            try:
                assert process.wait(timeout=BOOT_TIMEOUT_SECONDS) == 0
            except subprocess.TimeoutExpired:
                (base / 'grub-image-bios-screen.txt').write_text(read_screen(process, base) + '\n')
                raise
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=5)
            process.stdin.close()
            process.stdout.close()
            (base / 'vga.bin').unlink(missing_ok=True)
    text = serial.read_text(errors='replace').replace('\r', '')
    assert 'RUFUS_GRUB_NORMAL_CONFIG_OK' in text, serial
    assert BOOT_MARKER + ' label=RUFUS_TEST normal=2.12' in text.splitlines(), serial
    assert digest(disk) == before, 'Snapshot boot changed backing media'
    return {'normal_config': True, 'linux_userspace': True, 'disk_sha256': before,
            'firmware_sha256': digest(firmware), 'snapshot_unchanged': True}


def inspect_ntfs(base, disk, offset, size, window, manifest_name="grub-source-manifest.json", prefix="grub-image"):
    from test_ntfs_boot import original_code
    code_offset = 0x54
    tail = original_code('br_ntfs_0x54.h')
    window = window[:code_offset + len(tail)]
    assert window[:11] == original_code('br_ntfs_0x0.h')
    assert window[code_offset:] == tail
    with disk.open('rb') as stream:
        stream.seek(offset + size - SECTOR_BYTES)
        backup = stream.read(SECTOR_BYTES)
    assert window[11:code_offset] == backup[11:code_offset]
    assert window[:SECTOR_BYTES] != backup
    volume = base / 'volume.raw'
    copy_volume(disk, volume, offset, size)
    label = run(['ntfslabel', volume], capture_output=True, text=True).stdout.strip()
    assert label == LABEL
    info = run(['ntfsinfo', '-m', volume], capture_output=True, text=True).stdout
    (base / (prefix + '-ntfsinfo.log')).write_text(info)
    assert 'Volume Version: 3.1' in info and 'Volume Flags: 0x0000' in info, info
    sector, spc = struct.unpack_from('<HB', window, 11)
    sectors = struct.unpack_from('<Q', window, 40)[0]
    assert sector == SECTOR_BYTES and sector * spc == NTFS_CLUSTER_BYTES
    assert struct.unpack_from('<I', window, 28)[0] == offset // sector
    assert sectors * sector == size - sector
    readback = base / ('grub-readback' if prefix == 'grub-image' else prefix + '-readback')
    readback.mkdir()
    manifest = json.loads((base / manifest_name).read_text())['files']
    listing = run(['7z', 'l', '-slt', volume], capture_output=True, text=True).stdout
    (base / (prefix + '-ntfs-files.log')).write_text(listing)
    files = set()
    for record in listing.split('\n\n'):
        fields = dict(line.split(' = ', 1) for line in record.splitlines() if ' = ' in line)
        name = fields.get('Path', '')
        if fields.get('Folder') == '-' and not name.startswith('[SYSTEM]/'):
            files.add(name)
    assert files == set(manifest), (files - set(manifest), set(manifest) - files)
    run(['7z', 'x', '-y', '-o' + str(readback), volume, *manifest], capture_output=True)
    return readback


def inspect_and_boot(args, base, disk):
    cluster = NTFS_CLUSTER_BYTES if args.grub_ntfs else FAT16_CLUSTER_BYTES if args.fat_bits == 16 else FAT32_CLUSTER_BYTES
    payload_bytes = args.boot_payload.stat().st_size if args.grub_uefi else 0
    offset, size = extent(DISK_BYTES, cluster, uefi_payload=payload_bytes)
    with disk.open('rb') as stream:
        first = stream.read(SECTOR_BYTES)
        stream.seek(SECTOR_BYTES)
        core = stream.read(args.grub_core.stat().st_size)
    signature = struct.unpack_from('<I', first, 440)[0]
    filesystem = 'ntfs' if args.grub_ntfs else args.fat_bits
    inspect_mbr(disk, DISK_BYTES, cluster, file_system=filesystem, signature=signature, boot_code='grub2',
                uefi_payload=payload_bytes)
    assert core == args.grub_core.read_bytes()
    window = read_window(disk, offset)
    if not args.grub_ntfs:
        assert window == expected_records(window, args.fat_bits, standard=True)
    report = values(re.search(r'RUFUS_GRUB2_IMAGE_DONE ([^\r\n]+)',
                             (base / 'serial-512.log').read_text(errors='replace')).group(1))
    assert (report['FAT'], report['CLUSTER']) == (0 if args.grub_ntfs else args.fat_bits, cluster)
    assert (report['OFFSET'], report['SIZE']) == (offset, size)
    if not args.grub_ntfs and args.fat_bits == 16:
        sector, spc, reserved, fats, entries, short_total = struct.unpack_from('<HBHBHH', window, 11)
        fat_sectors = struct.unpack_from('<H', window, 22)[0]
        total = short_total or struct.unpack_from('<I', window, 32)[0]
        assert sector == SECTOR_BYTES and spc * sector == cluster
        assert reserved > 0 and fats == 2 and entries >= 512 and fat_sectors > 0
        assert window[54:62] == b'FAT16   ' and window[510:512] == b'\x55\xaa'
        assert struct.unpack_from('<I', window, 28)[0] == offset // sector
        assert struct.unpack_from('<I', window, 39)[0] == 0x1234abcd
        assert window[43:54] == b'NO NAME    '
        metadata_sectors = reserved + fats * fat_sectors + entries * 32 // sector
        clusters = (total - metadata_sectors) // spc
        assert 4085 <= clusters <= 65524
        assert 0 <= size - total * sector < 63 * sector

    assert report['ERROR'] == 0 and report['COPIED'] > 0
    if args.grub_ntfs:
        readback = inspect_ntfs(base, disk, offset, size, window)
    else:
        volume = base / 'volume.raw'
        copy_volume(disk, volume, offset, size)
        checked = subprocess.run(['fsck.fat', '-n', str(volume)], capture_output=True, timeout=20)
        (base / 'grub-image-fsck.log').write_bytes(checked.stdout + checked.stderr)
        # Rufus's standard PBR says NO NAME; the formatted root retains the selected label.
        assert checked.returncode == 1 and not checked.stderr, checked
        lines = checked.stdout.decode().splitlines()
        assert lines[1:5] == [f"There is no label in boot sector, but there is volume label '{LABEL}' stored in root directory",
            "  Auto-copying volume label from root directory to boot sector.", "", "Leaving filesystem unchanged."], lines
        assert len(lines) == 6 and re.fullmatch(re.escape(str(volume)) + r": \d+ files, \d+/\d+ clusters", lines[5]), lines
        readback = base / 'grub-readback'
        readback.mkdir()
        env = dict(os.environ, LC_ALL='C', MTOOLSRC='/dev/null')
        run(['mcopy', '-s', '-i', f'{disk}@@{offset}', '::*', str(readback)], env=env)
    manifest = json.loads((base / 'grub-source-manifest.json').read_text())['files']
    actual = {str(path.relative_to(readback)): digest(path) for path in readback.rglob('*') if path.is_file()}
    expected = dict(manifest)
    config = (base / 'grub-source/boot/grub/grub.cfg').read_bytes().replace(SOURCE_LABEL.encode(), LABEL.encode())
    expected['boot/grub/grub.cfg'] = hashlib.sha256(config).hexdigest()
    assert actual == expected, (set(actual) - set(expected), set(expected) - set(actual))
    (base / 'grub-image-readback.json').write_text(json.dumps(actual, indent=2) + '\n')
    result = {'filesystem': filesystem, 'fat_bits': 0 if args.grub_ntfs else args.fat_bits,
            'ntfs_driver': args.grub_ntfs, 'cluster_bytes': cluster, 'offset': offset, 'size': size, 'files': len(actual), 'core_sha256': digest(args.grub_core),
            'label_patch': True, 'bios_boot': boot_written_usb(args, base, disk)}
    if args.grub_uefi:
        from mbr import boot_syslinux_uefi
        boot_offset, boot_size = trailing_extent(DISK_BYTES, payload_bytes)
        with disk.open('rb') as stream:
            stream.seek(boot_offset)
            assert stream.read(payload_bytes) == args.boot_payload.read_bytes()
        boot_syslinux_uefi(args, base / 'uefi-boot', disk)
        result.update({'uefi_boot': True, 'boot_offset': boot_offset, 'boot_size': boot_size,
                       'uefi_payload_sha256': digest(args.boot_payload),
                       'efi_fixture_sha256': digest(args.build / 'uefi-handoff.efi')})
    return result
