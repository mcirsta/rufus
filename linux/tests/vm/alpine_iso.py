#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Qualify the pinned Alpine standard ISO through the native extraction helper."""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import socket
import struct
import subprocess
import time

from boot_linux import firmware_options
from fat_extract import digest, run, values

VERSION = '3.22.5'
IMAGE_NAME = 'alpine-standard-3.22.5-x86_64.iso'
IMAGE_URL = 'https://dl-cdn.alpinelinux.org/alpine/v3.22/releases/x86_64/' + IMAGE_NAME
IMAGE_SHA256 = '8844f664dffbf50964ba33fe67658dd5d61c8f02d88e4febd17cf2c4334983c4'
DISK_BYTES = 512 * 1024 * 1024
BOOT_TIMEOUT_SECONDS = 180
ISO_SECTOR_BYTES = 2048
VIRTUAL_SECTOR_BYTES = 512
BOOT_FILES = ('.alpine-release', 'efi/boot/bootx64.efi', 'boot/grub/grub.cfg',
              'boot/vmlinuz-lts', 'boot/initramfs-lts', 'boot/modloop-lts')
ROOT_CONFIG = ('DEFAULT loadconfig\n\nLABEL loadconfig\n'
               '  CONFIG /boot/syslinux/syslinux.cfg\n  APPEND /boot/syslinux/\n')


def tree_contents(root):
    files, directories = {}, []
    for path in sorted(root.rglob('*')):
        assert not path.is_symlink(), path
        relative = str(path.relative_to(root))
        if path.is_dir():
            directories.append(relative)
        else:
            files[relative] = digest(path)
    return {'files': files, 'directories': directories}


def add_boot_images(image, tree, contents):
    # Rufus's bundled reader exposes virtual El Torito files which bsdtar omits.
    # Read the pinned ISO's two catalog entries independently to verify their bytes.
    catalog = (tree / 'boot/syslinux/boot.cat').read_bytes()
    records = []
    with image.open('rb') as source:
        for index, offset in enumerate((32, 96)):
            assert catalog[offset:offset + 2] == b'\x88\x00'  # Bootable, no emulation.
            sectors, lba = struct.unpack_from('<HI', catalog, offset + 6)
            size = sectors * VIRTUAL_SECTOR_BYTES
            source.seek(lba * ISO_SECTOR_BYTES)
            data = source.read(size)
            assert len(data) == size and size > 0
            name = f'[boot]/{index}-boot-noemul.img'
            contents['files'][name] = hashlib.sha256(data).hexdigest()
            records.append({'file': name, 'lba': lba, 'bytes': size})
    contents['directories'].append('[boot]')
    contents['directories'].sort()
    return records


def prepare_image(args, base, root):
    if not args.alpine.is_file() or digest(args.alpine) != IMAGE_SHA256:
        raise RuntimeError('Missing or changed pinned Alpine ISO: ' + str(args.alpine))
    expected = base / 'alpine-source-tree'
    expected.mkdir()
    run(['bsdtar', '-xf', str(args.alpine), '--no-same-owner', '--no-xattrs', '-C', str(expected)])
    contents = tree_contents(expected)
    boot_images = add_boot_images(args.alpine, expected, contents)
    for name in BOOT_FILES:
        assert name in contents['files'], name
    scan = run([str(args.build / 'iso_extract_probe'), str(args.alpine), str(base / 'unused'),
                'rr', 'preflight'], capture_output=True, text=True)
    (base / 'source-preflight.log').write_text(scan.stdout + scan.stderr)
    report = values(scan.stdout)
    assert report['ERROR'] == 0 and report['FDS'] == 0, scan
    assert report['FILES'] == len(contents['files']) and report['WRITES'] == 0, scan
    with args.alpine.open('rb') as source:
        source.seek(16 * ISO_SECTOR_BYTES + 40)  # ISO9660 primary volume descriptor's label.
        label = source.read(32).decode('ascii').rstrip()
    record = {'version': VERSION, 'url': IMAGE_URL, 'sha256': IMAGE_SHA256,
              'iso_label': label, 'target_label': 'RUFUS TEST', 'contents': contents,
              'virtual_boot_images': boot_images,
              'grub_config': (expected / 'boot/grub/grub.cfg').read_text(),
              'generated_syslinux_config': ROOT_CONFIG,
              'preflight': report}
    assert label != record['target_label']
    (base / 'alpine-source.json').write_text(json.dumps(record, indent=2) + '\n')
    shutil.copy2(args.alpine, root / 'home/rufus/ALPINE.iso')
    (root / 'etc/rufus-alpine-fixture').write_text('RUFUS-ALPINE-TEST\n')
    # ISO directory modes can be read-only; discard only this unpacked reference.
    expected.chmod(0o700)
    for directory in expected.rglob('*'):
        if directory.is_dir():
            directory.chmod(0o700)
    shutil.rmtree(expected)


def check_copied(base):
    expected = json.loads((base / 'alpine-source.json').read_text())['contents']
    assert 'syslinux.cfg' not in expected['files']
    expected['files']['syslinux.cfg'] = hashlib.sha256(ROOT_CONFIG.encode()).hexdigest()
    assert tree_contents(base / 'copied') == expected, 'Alpine source files or generated root config differ'


def boot_command(base):
    files = json.loads((base / 'alpine-source.json').read_text())['contents']['files']
    checksums = ' '.join("'" + files[name] + '  ' + name + "'" for name in BOOT_FILES)
    # Inspect the actual mounted USB and distribution; no test file is installed in it.
    return ("(set -e; test -d /sys/firmware/efi; "
            "test \"$(cat /etc/alpine-release)\" = '" + VERSION + "'; test \"$(uname -m)\" = x86_64; "
            "media=$(awk '$1 == \"/dev/sda1\" && $3 == \"vfat\" {print $2; exit}' /proc/mounts); "
            "test -n \"$media\"; cd \"$media\"; printf '%s\\n' " + checksums + " | sha256sum -c -; "
            "grep -q ' /.modloop squashfs ' /proc/mounts; "
            "printf '\\nRUFUS_ALPINE_BOOT_OK version=" + VERSION + " arch=x86_64 firmware=uefi\\n'); "
            "result=$?; echo RUFUS_ALPINE_EXIT $result; poweroff\r")


def read_console(process, path, log, command):
    deadline = time.monotonic() + BOOT_TIMEOUT_SECONDS
    while not path.exists():
        if process.poll() is not None or time.monotonic() >= deadline:
            raise RuntimeError('Alpine VM did not open its serial console')
        time.sleep(.05)
    expected = ('RUFUS_ALPINE_BOOT_OK version=' + VERSION + ' arch=x86_64 firmware=uefi').encode()
    logged_in = sent = passed = exited = False
    received = b''
    with socket.socket(socket.AF_UNIX) as client:
        client.settimeout(.25)
        client.connect(str(path))
        while time.monotonic() < deadline:
            try:
                data = client.recv(65536)
            except socket.timeout:
                if process.poll() is not None:
                    break
                data = None
            if data == b'':
                break
            if data:
                log.write(data)
                log.flush()
                received = (received + data)[-2 * 1024 * 1024:]
            console = received.replace(b'\r', b'')
            if not logged_in and re.search(rb'(?:^|\n)localhost login: ', console):
                client.sendall(b'root\r')
                logged_in = True
            if logged_in and not sent and re.search(rb'(?:^|\n)localhost:~# ', console):
                for byte in command.encode():
                    client.sendall(bytes([byte]))
                    time.sleep(.005)
                sent = True
            lines = console.split(b'\n')
            passed = passed or expected in lines
            exited = exited or b'RUFUS_ALPINE_EXIT 0' in lines
    if not passed or not exited:
        raise RuntimeError('Alpine did not confirm its release, UEFI, USB payload and mounted modloop')
    if process.wait(timeout=15) != 0:
        raise RuntimeError('Alpine VM did not shut down successfully')


def qualify_boot(args, base, disk):
    output = base / 'alpine-boot'
    output.mkdir()
    before = digest(disk)
    path = output / 'serial.sock'
    command = ['qemu-system-x86_64', '-accel', args.accel,
               '-cpu', 'host' if args.accel == 'kvm' else 'max', '-m', '1024',
               '-display', 'none', '-monitor', 'none', '-no-reboot', '-nic', 'none',
               '-serial', 'unix:' + str(path) + ',server=on,wait=on', '-snapshot',
               '-device', 'qemu-xhci,id=xhci',
               '-drive', 'if=none,id=stick,format=raw,file=' + str(disk),
               '-device', 'usb-storage,drive=stick,removable=on,bootindex=1']
    command.extend(firmware_options(output, 'uefi', 'alpine'))
    (output / 'qemu-command.json').write_text(json.dumps(command, indent=2) + '\n')
    checks = boot_command(base)
    (output / 'guest-check.sh').write_text(checks.rstrip('\r') + '\n')
    environment = os.environ.copy()
    environment['TMPDIR'] = str(output)
    with (output / 'qemu.log').open('wb') as errors, (output / 'serial.log').open('wb') as log:
        process = subprocess.Popen(command, env=environment, stdout=errors, stderr=subprocess.STDOUT)
        try:
            read_console(process, path, log, checks)
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
    assert digest(disk) == before, 'Alpine boot modified the verified backing image'
    print('PASS: Alpine ' + VERSION + ' booted from the extracted USB with unchanged boot files.', flush=True)
    return {'version': VERSION, 'firmware': 'uefi', 'disk_sha256': before, 'passed': True}
