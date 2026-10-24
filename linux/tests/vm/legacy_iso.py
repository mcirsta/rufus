# SPDX-License-Identifier: GPL-3.0-or-later
"""Finite B4 sources, five native jobs and three distinct firmware handoffs."""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

import prepare
from boot_linux import firmware_options
from fat_extract import digest
from test_freedos_boot import original_array


def stage_fixture(args, base, init):
    repo = Path(__file__).resolve().parents[3]
    efi = (args.build / 'uefi-handoff.efi').read_bytes()
    (init / 'original.efi').write_bytes(efi)
    (init / 'usr/bin/sh').symlink_to('bash')
    flags = (args.build / 'build.ninja').read_text()
    for definition, name in [('RUFUS_NATIVE_SYSLINUX_PATH', 'rufus-syslinux'),
                             ('RUFUS_NATIVE_SYSLINUX4_PATH', 'rufus-syslinux4')]:
        path = re.search(definition + r'=\\"([^"\\]+)', flags)[1]
        target = init / path.lstrip('/')
        target.parent.mkdir(parents=True, exist_ok=True)
        if target != init / 'usr/bin' / name:
            target.symlink_to('/usr/bin/' + name)
    for name in ('fat16_0x0', 'fat16_0x3e', 'fat32kos_0x52','ntfs_0x0','ntfs_0x54'):
        (init / (name + '.bin')).write_bytes(original_array('br_' + name + '.h'))
    for name in ('rufus','syslinux','kolibri'):
        (init / ('mbr-' + name + '.bin')).write_bytes(original_array('mbr_' + name + '.h'))
    (init / 'original-mboot.c32').write_bytes((repo / 'res/syslinux/mboot.c32').read_bytes())
    multiboot, kolibri = base / 'multiboot.bin', base / 'kolibri-loader.bin'
    for source, output in [('legacy_iso_multiboot.asm', multiboot), ('legacy_iso_kolibri.asm', kolibri)]:
        subprocess.run(['nasm', '-f', 'bin', str(prepare.HERE / source), '-o', str(output)],
                       check=True, capture_output=True)
    manifest = []
    for number in range(5):
        tree = base / ('legacy-source-' + str(number))
        tree.mkdir()
        files = {'payload.txt': b'B4 source payload retained byte for byte\n'}
        generated = {}
        if number in (0,4):
            loader = bytearray(b'Legacy setup loader fixture\n' + bytes(0x2400))
            loader[0x2060:0x2062] = b'\x74\x03'
            path = b'\\minint\\txtsetup.sif\x00'
            loader[0x2100:0x2100 + len(path)] = path
            path = b'rdisk(0)\x00'
            loader[0x2200:0x2200 + len(path)] = path
            files.update({'i386/setupldr.bin': bytes(loader),
                          'i386/ntdetect.com': b'B4 generated detector\r\n',
                          'i386/txtsetup.sif': b'[SetupData]\r\nOsLoadOptions=/fastdetect\r\n'})
            loader[0x2060:0x2062] = b'\xeb\x1a'
            path = b'\\i386\\txtsetup.sif\x00'
            loader[0x2100:0x2100 + len(path)] = path
            loader[0x2206] = ord('1')
            generated = {'BOOTMGR': bytes(loader), 'ntdetect.com': files['i386/ntdetect.com'],
                         'TXTSETUP.SIF': b'[SetupData]\r\nSetupSourceDevice = "\\device\\harddisk1\\partition1"\r\nOsLoadOptions=/fastdetect\r\n'}
            if number == 4:
                files['EFI/BOOT/BOOTX64.EFI'] = efi
        elif number == 1:
            (tree / 'sources').mkdir()
            subprocess.run([str(args.build / 'wim_fixture'), str(tree / 'sources/install.wim'), 'unknown'],
                           check=True, capture_output=True)
            files.update({'BOOTMGR': b'B4 normalized-unknown installer boot manager\n',
                          'bootmgr.efi': efi, 'EFI/BOOT/BOOTX64.EFI': efi})
        elif number == 2:
            files.update({'loader/setupldr.sys': multiboot.read_bytes(),
                          'freeldr.sys': multiboot.read_bytes()})
            generated = {'mboot.c32': (repo / 'res/syslinux/mboot.c32').read_bytes()}
        else:
            files.update({'kolibri.img': b'B4 Kolibri source marker\n',
                          'HD_Load/USB_Boot/MTLD_F32': kolibri.read_bytes()})
            generated = {'MTLD_F32': kolibri.read_bytes()}
        for name, data in files.items():
            path = tree / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        files = {str(path.relative_to(tree)): path.read_bytes() for path in tree.rglob('*') if path.is_file()}
        iso = init / ('legacy-' + str(number) + '.iso')
        subprocess.run([str(args.mkisofs), '-quiet', '-R', '-J', '-V', 'B4SOURCE', '-o', str(iso), str(tree)],
                       check=True, capture_output=True)
        expected = dict(files)
        expected.update(generated)
        manifest.append({'job': number, 'iso_sha256': digest(iso),
                         'files': {name: hashlib.sha256(data).hexdigest() for name, data in expected.items()}})
    prepare.write(init, 'legacy-iso-manifest.json', json.dumps(manifest, indent=2) + '\n')
    (base / 'source-corpus.json').write_text(json.dumps(manifest, indent=2) + '\n')
    prepare.write(init, 'etc/rufus-legacy-iso-fixture', 'legacy-iso\n')
    prepare.write(init, 'etc/rufus-format-helper-fixture', 'format-helper\n')
    shutil.copy2(args.boot_payload,init/'uefi-ntfs.img')
    (init / 'tmp').mkdir(mode=0o1777, exist_ok=True)
    (init / 'tmp').chmod(0o1777)


def boot_bios(args, base, disk, name, marker):
    output = base / name
    output.mkdir()
    original = digest(disk)
    command = ['qemu-system-x86_64', '-accel', args.accel, '-cpu', 'max', '-m', '128',
               '-display', 'none', '-monitor', 'none', '-no-reboot', '-nic', 'none', '-snapshot',
               '-serial', 'file:' + str(output / 'serial.log'),
               '-device', 'isa-debug-exit,iobase=0xf4,iosize=0x04', '-device', 'qemu-xhci,id=xhci',
               '-drive', 'if=none,id=stick,format=raw,file=' + str(disk),
               '-device', 'usb-storage,drive=stick,removable=on,bootindex=1']
    command += firmware_options(output, 'bios', name)
    (output / 'qemu-command.json').write_text(json.dumps(command, indent=2) + '\n')
    try:
        with (output / 'qemu.log').open('wb') as log:
            result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=60,
                                    env=dict(os.environ, TMPDIR=str(output)))
        serial = (output / 'serial.log').read_text(errors='replace')
        assert result.returncode == 33 and marker in serial, (result.returncode, serial)
        assert digest(disk) == original, 'Snapshot firmware boot changed backing media'
    finally:
        for path in output.glob('*.fd'):
            path.unlink()
    return {'marker': marker, 'original_boot_records': True, 'backing_unchanged': True,
            'operating_system_executed': False}


def qualify_boot(args, base, disk):
    from mbr import boot_syslinux_uefi
    boot_syslinux_uefi(args, base / 'windows-fat16-uefi', base / 'legacy-windows.raw', native_fat=True)
    reactos = boot_bios(args, base, base / 'legacy-reactos.raw', 'reactos-iso-bios',
                       'RUFUS_REACTOS_ISO_MULTIBOOT_OK')
    kolibri = boot_bios(args, base, disk, 'kolibri-iso-bios', 'RUFUS_KOLIBRI_ISO_BIOS_OK tail=ok')
    return {'windows_fat16_native_efi': True, 'reactos': reactos, 'kolibri': kolibri,
            'fat16_standard_pbr_intentionally_not_bootmgr': True}
