# SPDX-License-Identifier: GPL-3.0-or-later
"""Generated Windows metadata for native BIOS/dual FAT32 or NTFS, without Setup."""
import json
import os
import struct
import subprocess
import time
from pathlib import Path

from fat_extract import digest, run, values
from grub2_iso import inspect_ntfs
from test_mbr_image import MIB, SECTOR_BYTES, extent, inspect_mbr, trailing_extent
from test_freedos_boot import read_window, expected_records
from test_fat_format import copy_volume
from syslinux_alpine import qmp_read, qmp_command, read_screen

DISK_BYTES = 64 * MIB
CLUSTER_BYTES = 4096
FAT32_CLUSTER_BYTES = 512
LABEL = 'RUFUS_TEST'
BOOTMGR_BYTES = 0x12000
BOOT_MARKER = 'RUFUS_WINDOWS_BOOTMGR_OK cs=2000 drive=80 tail=ok'
SECOND_DISK_MARKER = 'RUFUS_WINDOWS_SECOND_DISK_OK'
MBR_PROMPT = 'Press any key to boot from USB.'
BOOT_TIMEOUT_SECONDS = 30


def make_fixture(args, base):
    tree = base / 'windows-source'
    (tree / 'sources').mkdir(parents=True)
    command = [args.build / 'wim_fixture', tree / 'sources/install.wim', 'win' + str(args.windows_version)]
    if args.windows_efi_repair:
        command += ['efi', args.build / 'uefi-handoff.efi']
    run(command)
    bootmgr = (args.build / 'windows-bootmgr.bin').read_bytes() if args.windows_boot else (
        b'Opaque BOOTMGR fixture\n' + bytes(range(256)) * 256)
    if args.windows_boot:
        assert len(bootmgr) == BOOTMGR_BYTES
    (tree / 'BoOtMgR').write_bytes(bootmgr)
    (tree / 'readme.txt').write_text('Native Windows request fixture, no Windows Setup payload\n')
    (tree / 'data').mkdir()
    (tree / 'data/copy.bin').write_bytes(bytes(range(256)) * 8192 + b'tail')
    if not args.windows_fat32 or args.windows_dual:
        efi = (args.build / 'uefi-handoff.efi').read_bytes()
        # Older BIOS and repair fixtures have only root bootmgr.efi in the tree.
        # Rufus repairs its missing fallback only for UEFI/Alt+E, not plain BIOS.
        if args.windows_version != 7 and not args.windows_efi_repair:
            loader = tree / 'EFI/BOOT/BOOTX64.EFI'
            loader.parent.mkdir(parents=True)
            loader.write_bytes(efi)
        (tree / 'bootmgr.efi').write_bytes(b'Root manager must not supply the fallback\n' if args.windows_efi_repair else efi)
    args.iso = base / 'windows-source.iso'
    command = [str(args.mkisofs), '-quiet', '-R', '-J', '-V', 'RUFUS_SOURCE', '-o', str(args.iso), str(tree)]
    if args.windows_udf:
        command.insert(1, '-udf')
    run(command)
    files = {str(p.relative_to(tree)): digest(p) for p in tree.rglob('*') if p.is_file()}
    (base / 'windows-source-manifest.json').write_text(json.dumps({
        'windows_version': args.windows_version, 'udf_bridge': args.windows_udf,
        'iso_sha256': digest(args.iso), 'files': files,
        'executable_bootmgr': args.windows_boot,
        'efi_repair': args.windows_efi_repair,
        'limit': 'Generated metadata and handoff fixture only, no Windows Setup qualification'
    }, indent=2) + '\n')
    if args.windows_efi_repair:
        assert 'EFI/BOOT/BOOTX64.EFI' not in files
        expected = dict(files)
        expected['EFI/BOOT/BOOTX64.EFI'] = digest(args.build / 'uefi-handoff.efi')
        assert expected['EFI/BOOT/BOOTX64.EFI'] != files['bootmgr.efi']
        (base / 'windows-target-manifest.json').write_text(json.dumps({'files': expected}, indent=2) + '\n')


def inspect_fat(base, disk, offset, size, window, legacy=False, masquerade=False, installed=True):
    (base / 'windows-fat32-boot.bin').write_bytes(window)
    expected = bytearray(expected_records(window, 32, windows=not legacy,
                                         winpe_bootmgr=legacy, masquerade_disk=masquerade))
    # Native allocation updates live FSInfo free/next-cluster hints after the
    # original PE record is installed. All executable bytes must still match.
    fsinfo_hints_offset = SECTOR_BYTES + 488
    fsinfo_hints_bytes = 8
    for copy in (0, 6 * SECTOR_BYTES):
        start = copy + fsinfo_hints_offset
        expected[start:start + fsinfo_hints_bytes] = window[start:start + fsinfo_hints_bytes]
    if installed:
        assert window == expected, 'Original Windows FAT32 executable records differ'
    else:
        assert window[3:11] == b'mkfs.fat', 'Failed root job installed unexpected boot code'
    backup = window[6 * SECTOR_BYTES:7 * SECTOR_BYTES]
    assert window[:SECTOR_BYTES] == backup, 'Primary/backup FAT32 boot sector differs'
    sector, spc = struct.unpack_from('<HB', window, 11)
    assert sector == SECTOR_BYTES and sector * spc == FAT32_CLUSTER_BYTES
    assert struct.unpack_from('<I', window, 28)[0] == offset // sector
    assert struct.unpack_from('<I', window, 32)[0] * sector == size
    assert window[71:82] == (b'NO NAME    ' if installed else LABEL.encode().ljust(11, b' '))
    assert window[64] == (0x81 if installed and masquerade else 0x80)
    volume = base / 'volume.raw'
    copy_volume(disk, volume, offset, size)
    from mbr import check_fat_volume
    check_fat_volume(base, volume, standard=installed, log='windows-image-fsck.log', label=LABEL)
    listing = run(['mdir', '-i', volume, '::'], capture_output=True, text=True).stdout
    assert LABEL in listing, listing
    (base / 'windows-image-mdir.log').write_text(listing)
    readback = base / 'windows-image-readback'
    readback.mkdir()
    run(['mcopy', '-s', '-i', volume, '::*', readback], capture_output=True)
    return readback


def inspect_written(args, base, disk):
    payload_bytes = args.boot_payload.stat().st_size if args.windows_dual and not args.windows_fat32 else 0
    cluster = FAT32_CLUSTER_BYTES if args.windows_fat32 else CLUSTER_BYTES
    filesystem = 'fat32' if args.windows_fat32 else 'ntfs'
    offset, size = extent(DISK_BYTES, cluster, uefi_payload=payload_bytes)
    with disk.open('rb') as stream:
        mbr = stream.read(SECTOR_BYTES)
    signature = struct.unpack_from('<I', mbr, 440)[0]
    inspect_mbr(disk, DISK_BYTES, cluster, file_system=32 if args.windows_fat32 else 'ntfs', signature=signature,
                boot_code=args.windows_bios, uefi_payload=payload_bytes)
    window = read_window(disk, offset)
    manifest = 'windows-target-manifest.json' if args.windows_efi_repair else 'windows-source-manifest.json'
    readback = inspect_fat(base, disk, offset, size, window) if args.windows_fat32 else inspect_ntfs(
        base, disk, offset, size, window, manifest, 'windows-image')
    expected = json.loads((base / manifest).read_text())['files']
    actual = {str(p.relative_to(readback)): digest(p) for p in readback.rglob('*') if p.is_file()}
    assert actual == expected, (actual, expected)
    if args.windows_version == 7 and not args.windows_efi_repair:
        assert 'bootmgr.efi' in actual and 'EFI/BOOT/BOOTX64.EFI' not in actual
    serial = (base / 'serial-512.log').read_text(errors='replace')
    report = values(serial.split('RUFUS_WINDOWS_IMAGE_DONE ', 1)[1].splitlines()[0])
    assert (report['ERROR'], report['OFFSET'], report['SIZE']) == (0, offset, size), report
    if not args.windows_fat32:
        assert report['BYTES'] == size, report
    assert report['COPIED'] > 0 and report['FAT'] == (32 if args.windows_fat32 else 0) and report['CLUSTER'] == cluster, report
    # Ordinary Windows has no GRUB/Syslinux secondary loader in the partition gap.
    with disk.open('rb') as stream:
        stream.seek(SECTOR_BYTES)
        assert stream.read(offset - SECTOR_BYTES) == bytes(offset - SECTOR_BYTES)
    result = {'bootstrap': args.windows_bios, 'dual': args.windows_dual, 'driver': args.ntfs_mount_driver or 'vfat',
              'filesystem': filesystem,
              'windows_version': args.windows_version, 'udf_bridge': args.windows_udf,
              'efi_repair': args.windows_efi_repair,
              'offset': offset, 'size': size, 'disk_signature': signature, 'files': len(actual),
              'filesystem_clean': True, 'original_mbr_pbr': True, 'exact_file_readback': True,
              'disk_sha256': digest(disk), 'bios_boot_qualified': False, 'efi_boot_qualified': False}
    if not args.windows_fat32:
        result['ntfs_clean'] = True
    else:
        result['original_boot_label_warning'] = True
        result['fsinfo_hints'] = [struct.unpack_from('<II', window, copy + SECTOR_BYTES + 488)
                                  for copy in (0, 6 * SECTOR_BYTES)]
    if payload_bytes:
        boot_offset, boot_size = trailing_extent(DISK_BYTES, payload_bytes)
        with disk.open('rb') as stream:
            stream.seek(boot_offset)
            assert stream.read(payload_bytes) == args.boot_payload.read_bytes()
        result.update({'boot_offset': boot_offset, 'boot_size': boot_size,
                       'uefi_payload_sha256': digest(args.boot_payload)})
    (base / 'windows-image-readback.json').write_text(json.dumps(actual, indent=2) + '\n')
    return result


def boot_bios(args, base, disk, choice):
    output = base / ('windows-bios-' + choice)
    output.mkdir()
    before = digest(disk)
    firmware = Path('/usr/share/qemu/bios-256k.bin')
    serial = output / 'serial.log'
    command = ['qemu-system-x86_64', '-accel', args.accel,
               '-cpu', 'host' if args.accel == 'kvm' else 'max', '-m', '256',
               '-display', 'none', '-monitor', 'none', '-no-reboot', '-nic', 'none',
               '-serial', 'file:' + str(serial), '-qmp', 'stdio', '-S',
               '-bios', str(firmware), '-snapshot',
               '-device', 'isa-debug-exit,iobase=0xf4,iosize=0x04',
               '-device', 'qemu-xhci,id=xhci',
               '-drive', f'if=none,id=stick,format=raw,file={disk}',
               '-device', 'usb-storage,drive=stick,removable=on,bootindex=1']
    second = output / 'second.raw'
    if choice != 'usb-only':
        with second.open('xb') as target:
            target.write((args.build / 'windows-second-disk.bin').read_bytes())
            target.truncate(4 * MIB)
        second_hash = digest(second)
        command += ['-drive', f'if=none,id=second,format=raw,file={second}',
                    '-device', 'ide-hd,drive=second,bootindex=2']
    (output / 'qemu-command.json').write_text(json.dumps(command, indent=2) + '\n')
    prompt_seen = False
    start = time.monotonic()
    try:
        with (output / 'qemu.log').open('wb') as log:
            process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                       stderr=log, env=dict(os.environ, TMPDIR=str(output)), bufsize=0)
            # qmp_read uses select on the pipe. Buffered read-ahead can hide a
            # queued response until the guest exits, missing the timed prompt.
            try:
                assert 'QMP' in qmp_read(process)
                qmp_command(process, 'qmp_capabilities')
                qmp_command(process, 'cont')
                if choice != 'usb-only':
                    deadline = start + BOOT_TIMEOUT_SECONDS
                    while process.poll() is None and time.monotonic() < deadline:
                        screen = read_screen(process, output)
                        if MBR_PROMPT in screen:
                            (output / 'prompt.txt').write_text(screen + '\n')
                            prompt_seen = True
                            if choice == 'press-key':
                                qmp_command(process, 'send-key', {'keys': [{'type': 'qcode', 'data': 'spc'}],
                                                                  'hold-time': 100})
                            break
                        time.sleep(.05)
                    assert prompt_seen, 'Original Rufus MBR prompt was not observed'
                exit_code = process.wait(timeout=BOOT_TIMEOUT_SECONDS)
                assert exit_code == 33, (choice, exit_code, serial.read_text(errors='replace'))
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait(timeout=5)
                process.stdin.close()
                process.stdout.close()
        text = serial.read_text(errors='replace').replace('\r', '').splitlines()
        expected = SECOND_DISK_MARKER if choice == 'timeout' else BOOT_MARKER
        assert expected in text, (choice, text)
        other = BOOT_MARKER if choice == 'timeout' else SECOND_DISK_MARKER
        assert other not in text
        assert digest(disk) == before, 'BIOS snapshot boot changed backing USB'
        if second.exists():
            assert digest(second) == second_hash, 'BIOS snapshot boot changed second disk'
        return {'choice': choice, 'prompt_observed': prompt_seen, 'marker': expected,
                'firmware_sha256': digest(firmware), 'disk_sha256': before,
                'snapshot_unchanged': True, 'duration_seconds': round(time.monotonic() - start, 2)}
    finally:
        second.unlink(missing_ok=True)
        (output / 'vga.bin').unlink(missing_ok=True)


def qualify_boot(args, base, disk):
    bios = []
    if not args.windows_efi_repair:
        bios = [boot_bios(args, base, disk, 'usb-only')]
        if args.windows_bios == 'rufus':
            bios += [boot_bios(args, base, disk, 'press-key'), boot_bios(args, base, disk, 'timeout')]
    result = {'bios': bios, 'windows_setup_qualified': False}
    if args.windows_dual:
        # Reuse the x64 application oracle; FAT boots directly from its main volume.
        from mbr import boot_syslinux_uefi
        output = base / 'windows-uefi'
        boot_syslinux_uefi(args, output, disk, native_fat=args.windows_fat32)
        result['uefi'] = {'original_payload_banner': not args.windows_fat32, 'efi_application': True,
                          'snapshot_unchanged': True}
    return result
