# SPDX-License-Identifier: GPL-3.0-or-later
"""Real Alpine BIOS/UEFI qualification for the production Syslinux helper."""
from contextlib import ExitStack
import hashlib
import json
import os
import re
from pathlib import Path
import shutil
import select
import struct
import subprocess
import sys
import time

import alpine_iso
from boot_linux import firmware_options
from fat_extract import digest, run
from test_mbr_image import extent, inspect_mbr

DISK_BYTES = 512 * 1024 * 1024
CLUSTER_BYTES = 4096
BOOT_TIMEOUT_SECONDS = 180
VGA_TEXT_ADDRESS = 0xb8000
VGA_COLUMNS = 80
VGA_ROWS = 25


def prepare_source(args, base):
    assert digest(args.alpine) == alpine_iso.IMAGE_SHA256, 'Pinned Alpine ISO differs'
    args.iso = args.alpine
    tree = base / 'alpine-source-tree'
    tree.mkdir()
    run(['bsdtar', '-xf', args.iso, '--no-same-owner', '--no-xattrs', '-C', tree])
    contents = alpine_iso.tree_contents(tree)
    records = alpine_iso.add_boot_images(args.iso, tree, contents)
    module = tree / 'boot/syslinux/ldlinux.c32'
    source = {'url': alpine_iso.IMAGE_URL, 'sha256': alpine_iso.IMAGE_SHA256,
              'version': alpine_iso.VERSION, 'contents': contents, 'virtual_boot_images': records,
              'source_companion_bytes': module.stat().st_size, 'source_companion_sha256': digest(module),
              'bios_config': (tree / 'boot/syslinux/syslinux.cfg').read_text()}
    (base / 'alpine-source.json').write_text(json.dumps(source, indent=2) + '\n')
    # Reference files may be read-only in the ISO. Only this generated tree is removed.
    tree.chmod(0o700)
    for path in tree.rglob('*'):
        if path.is_dir():
            path.chmod(0o700)
    shutil.rmtree(tree)


def check_disk(args, base, disk):
    sys.path.append(str(Path(__file__).resolve().parents[2] / 'probes/syslinux'))
    from verify import check_volume
    with disk.open('rb') as stream:
        stream.seek(440)
        signature = struct.unpack('<I', stream.read(4))[0]
    inspect_mbr(disk, DISK_BYTES, CLUSTER_BYTES, signature=signature)
    offset, size = extent(DISK_BYTES, CLUSTER_BYTES)
    source = json.loads((base / 'alpine-source.json').read_text())
    files = source['contents']['files'].copy()
    assert 'syslinux.cfg' not in files and 'ldlinux.c32' not in files and 'ldlinux.sys' not in files
    files['syslinux.cfg'] = hashlib.sha256(alpine_iso.ROOT_CONFIG.encode()).hexdigest()
    files['ldlinux.c32'] = digest(args.upstream / 'bios/com32/elflink/ldlinux/ldlinux.c32')
    env = dict(os.environ, LC_ALL='C', MTOOLSRC='/dev/null', TMPDIR=str(base))
    image_spec = f'{disk}@@{offset}'
    listing = run(['mdir', '-a', '-b', '-/', '-i', image_spec, '::/'], env=env, capture_output=True, text=True)
    (base / 'fat-files.txt').write_text(listing.stdout)
    actual = {name.removeprefix('::/') for name in listing.stdout.splitlines() if not name.endswith('/')}
    assert actual == set(files) | {'ldlinux.sys'}, (actual - set(files), set(files) - actual)
    readback = base / 'readback'
    for name, expected_hash in files.items():
        # mtools applies DOS-path globbing even without a shell.
        literal = name.replace('[', '[[]').replace('?', '[?]').replace('*', '[*]')
        run(['mcopy', '-o', '-i', image_spec, '::/' + literal, readback], env=env, capture_output=True)
        assert digest(readback) == expected_hash, name
    readback.unlink()
    with disk.open('rb') as stream:
        stream.seek(512)
        assert stream.read(offset - 512) == bytes(offset - 512), 'Changed alignment gap'
        stream.seek(offset + 6 * 512)
        original_boot = stream.read(512)
    check_volume(disk, base, offset, size, original_boot, env)
    print(f'PASS: {len(files)} Alpine files/configs/native companion independently verified.', flush=True)
    return {'files': files, 'disk_sha256': digest(disk), 'offset': offset, 'size': size}


def qmp_read(process):
    ready, unused_write, unused_error = select.select([process.stdout], [], [], 5)
    if not ready:
        raise RuntimeError('QMP response timed out')
    line = process.stdout.readline()
    if not line:
        raise RuntimeError('QMP pipe closed')
    return json.loads(line)


def qmp_command(process, name, arguments=None):
    request = {'execute': name}
    if arguments is not None:
        request['arguments'] = arguments
    process.stdin.write(json.dumps(request).encode() + b'\n')
    process.stdin.flush()
    while True:
        response = qmp_read(process)
        if 'error' in response:
            raise RuntimeError(str(response['error']))
        if 'return' in response:
            return response['return']


def type_keys(stream, text, hold_time_ms=1, key_interval_seconds=.012):
    # US keyboard scancodes in the disposable guest, with no clipboard/guest agent.
    punctuation = {' ': 'spc', '\n': 'ret', ',': 'comma', ':': 'shift-semicolon', '=': 'equal',
                   '(': 'shift-9', ')': 'shift-0', '/': 'slash', '.': 'dot', '%': 'shift-5', '-': 'minus'}
    for character in text:
        if character.isascii() and character.isalnum():
            code = ('shift-' if character.isupper() else '') + character.lower()
        else:
            code = punctuation[character]
        keys = [{'type': 'qcode', 'data': part} for part in code.split('-')]
        qmp_command(stream, 'send-key', {'keys': keys, 'hold-time': hold_time_ms})
        time.sleep(key_interval_seconds)


def read_screen(stream, output):
    memory = output / 'vga.bin'
    qmp_command(stream, 'pmemsave', {'val': VGA_TEXT_ADDRESS, 'size': VGA_COLUMNS * VGA_ROWS * 2,
                                    'filename': str(memory)})
    text = memory.read_bytes()[::2].decode('cp437')
    return '\n'.join(text[index:index + VGA_COLUMNS].rstrip() for index in range(0, len(text), VGA_COLUMNS))


def guest_checks(base):
    files = json.loads((base / 'alpine-source.json').read_text())['contents']['files']
    boot_files = ['.alpine-release', 'boot/vmlinuz-lts', 'boot/initramfs-lts',
                  'boot/modloop-lts', 'boot/syslinux/syslinux.cfg', 'boot/syslinux/ldlinux.c32']
    checksums = ' '.join("'" + files[name] + '  ' + name + "'" for name in boot_files)
    # Check unchanged media and actual BIOS userspace. Nothing is added to the ISO
    # or written USB, and no kernel/initrd is passed directly to this boot VM.
    return ("(set -e; test ! -d /sys/firmware/efi; "
            "test \"$(cat /etc/alpine-release)\" = '" + alpine_iso.VERSION + "'; "
            "test \"$(uname -m)\" = x86_64; "
            "media=$(awk '$1 == \"/dev/sda1\" && $3 == \"vfat\" {print $2; exit}' /proc/mounts); "
            "test -n \"$media\"; cd \"$media\"; printf '%s\\n' " + checksums + " | sha256sum -c -; "
            "grep -q ' /.modloop squashfs ' /proc/mounts; "
            "echo RUFUS_ALPINE_BIOS_BOOT_OK); "
            "echo RUFUS_ALPINE_EXIT $?; poweroff\r")


def observe_boot(process, output, checks, serial_input, serial_output):
    deadline = time.monotonic() + BOOT_TIMEOUT_SECONDS
    assert 'QMP' in qmp_read(process)
    qmp_command(process, 'qmp_capabilities')
    selected = logged_in = sent = False
    received = b''
    with (output / 'serial.log').open('wb') as serial:
        while process.poll() is None and time.monotonic() < deadline:
            if not selected:
                screen = read_screen(process, output)
                (output / 'screen.txt').write_text(screen)
                if 'SYSLINUX 6.04' in screen and 'boot:' in screen:
                    # Use the distribution entry; only console routing changes.
                    type_keys(process, 'lts console=ttyS0,115200\n')
                    selected = True
            try:
                data = os.read(serial_output, 65536)
            except BlockingIOError:
                data = b''
            serial.write(data)
            serial.flush()
            received = (received + data)[-2 * 1024 * 1024:]
            console = received.replace(b'\r', b'')
            if not logged_in and re.search(rb'(?:^|\n)localhost login: ', console):
                os.write(serial_input, b'root\r')
                logged_in = True
            if logged_in and not sent and re.search(rb'(?:^|\n)localhost:~# ', console):
                for character in checks.encode():
                    os.write(serial_input, bytes([character]))
                    time.sleep(.005)
                sent = True
            if b'RUFUS_ALPINE_EXIT 0' in console.split(b'\n'):
                process.wait(timeout=max(1, deadline - time.monotonic()))
                break
            time.sleep(.1)
    assert selected and logged_in and sent, 'Alpine BIOS console did not complete its checks'
    assert process.poll() == 0, 'Alpine BIOS boot did not finish'


def qualify_boot(args, base, disk):
    output = base / 'alpine-bios'
    output.mkdir()
    before = digest(disk)
    command = ['qemu-system-x86_64', '-accel', args.accel,
               '-cpu', 'host' if args.accel == 'kvm' else 'max', '-m', '1024', '-vga', 'std',
               '-display', 'none', '-monitor', 'none', '-no-reboot', '-nic', 'none',
               '-qmp', 'stdio',
               '-chardev', 'pipe,id=console,path=' + str(output / 'console'),
               '-serial', 'chardev:console', '-snapshot',
               '-device', 'qemu-xhci,id=xhci',
               '-drive', 'if=none,id=stick,format=raw,file=' + str(disk),
               '-device', 'usb-storage,drive=stick,removable=on,bootindex=1']
    command.extend(firmware_options(output, 'bios', 'alpine'))
    (output / 'qemu-command.json').write_text(json.dumps(command, indent=2) + '\n')
    checks = guest_checks(base)
    (output / 'guest-check.sh').write_text(checks)
    env = dict(os.environ, TMPDIR=str(output))
    with ExitStack() as resources, (output / 'qemu.log').open('wb') as log:
        descriptors = []
        for suffix in ('in', 'out'):
            path = output / ('console.' + suffix)
            os.mkfifo(path, 0o600)
            resources.callback(path.unlink, missing_ok=True)
            fd = os.open(path, os.O_RDWR | os.O_NONBLOCK | os.O_CLOEXEC)
            resources.callback(os.close, fd)
            descriptors.append(fd)
        serial_input, serial_output = descriptors
        process = subprocess.Popen(command, env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                   stderr=log, bufsize=0)
        try:
            observe_boot(process, output, checks, serial_input, serial_output)
        finally:
            if process.poll() is None:
                try:
                    qmp_command(process, 'screendump', {'filename': str(output / 'stopped.ppm')})
                except (OSError, RuntimeError, ValueError):
                    pass  # Failure evidence must never prevent VM cleanup.
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
            process.stdin.close()
            process.stdout.close()
    lines = (output / 'serial.log').read_bytes().replace(b'\r', b'').split(b'\n')
    assert b'RUFUS_ALPINE_BIOS_BOOT_OK' in lines and b'RUFUS_ALPINE_EXIT 0' in lines, lines
    assert digest(disk) == before, 'BIOS boot changed the verified backing image'
    print('PASS: unchanged Alpine ISO booted from the created Syslinux USB to BIOS userspace and verified modloop/media.', flush=True)
    return {'version': alpine_iso.VERSION, 'firmware': 'bios', 'disk_sha256': before, 'passed': True}


def qualify_disk(args, base, disk):
    result = check_disk(args, base, disk)
    result['boot'] = qualify_boot(args, base, disk)
    result['uefi_boot'] = alpine_iso.qualify_boot(args, base, disk)
    return result
