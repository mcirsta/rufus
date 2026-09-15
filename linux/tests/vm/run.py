#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run native USB qualification and boot each verified output in a separate VM."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import socket
import subprocess
import time
from boot_linux import boot_linux

USB_SIZE = 256 * 1024 * 1024


def digest(path):
    with path.open('rb') as file:
        return hashlib.file_digest(file, 'sha256').hexdigest()


def verify_usb(path, source, previous=None):
    with path.open('rb') as target, source.open('rb') as image:
        while chunk := image.read(1024 * 1024):
            if target.read(len(chunk)) != chunk:
                raise RuntimeError('Written USB range differs from the source')
        tail = previous.open('rb') if previous else None
        try:
            if tail:
                tail.seek(image.tell())
            while chunk := target.read(1024 * 1024):
                expected = tail.read(len(chunk)) if tail else bytes([0xa5]) * len(chunk)
                if chunk != expected:
                    raise RuntimeError('USB tail was modified')
        finally:
            if tail:
                tail.close()
        if target.tell() != USB_SIZE:
            raise RuntimeError('USB capacity changed')


def qmp_reply(stream):
    while True:
        reply = json.loads(stream.readline())
        if 'error' in reply:
            raise RuntimeError(reply['error'])
        if 'return' in reply:
            return reply['return']


def qmp_command(path, command, arguments):
    with socket.socket(socket.AF_UNIX) as client:
        client.settimeout(5)
        client.connect(str(path))
        stream = client.makefile('rwb', buffering=0)
        json.loads(stream.readline())
        stream.write(b'{"execute":"qmp_capabilities"}\n')
        qmp_reply(stream)
        stream.write((json.dumps({'execute': command, 'arguments': arguments}) + '\n').encode())
        return qmp_reply(stream)


def run_guest(base, output, accel, linux_images):
    system = output / 'system.raw'
    usb = output / 'usb.raw'
    # Every run receives its own files. Never attach a block node or a shared host tree.
    shutil.copyfile(base / 'system.raw', system)
    with usb.open('xb') as file:
        for _ in range(USB_SIZE // (1024 * 1024)):
            file.write(bytes([0xa5]) * (1024 * 1024))
    source = base / 'boot.img'
    serial = output / 'serial.log'
    qmp = output / 'qmp.sock'
    command = ['qemu-system-x86_64', '-accel', accel, '-cpu', 'host' if accel == 'kvm' else 'max',
               '-m', '2048', '-smp', '2', '-display', 'none', '-monitor', 'none',
               '-serial', 'file:' + str(serial), '-no-reboot', '-nic', 'none',
               '-qmp', 'unix:' + str(qmp) + ',server=on,wait=off',
               '-kernel', str(base / 'vmlinuz'), '-initrd', str(base / 'initramfs.cpio'),
               '-append', 'root=/dev/vda rw console=ttyS0 rufus.usb_fixture=1',
               '-smbios', 'type=1,manufacturer=RufusTest,product=RufusUSBFixture',
               '-drive', 'file=' + str(system) + ',format=raw,if=virtio',
               '-device', 'qemu-xhci,id=xhci',
               # Explicit block nodes survive device_del, preserving the same medium for replug.
               '-object', 'throttle-group,id=usb-throttle,x-bps-write=2097152',
               '-blockdev', json.dumps({'driver': 'file', 'node-name': 'usb-file', 'filename': str(usb)}),
               '-blockdev', 'driver=raw,node-name=usb-raw,file=usb-file',
               '-blockdev', 'driver=throttle,node-name=stick,throttle-group=usb-throttle,file=usb-raw',
               '-device', 'usb-storage,id=usb,drive=stick,removable=on,serial=RUFUS-TEST-USB']
    (output / 'qemu-command.json').write_text(json.dumps(command, indent=2) + '\n')
    copied = removed = reattached = False
    with (output / 'qemu.log').open('wb') as log:
        process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + 360
        try:
            while process.poll() is None:
                if time.monotonic() >= deadline:
                    raise TimeoutError('Native guest did not finish in 360 seconds')
                text = serial.read_text(errors='replace') if serial.exists() else ''
                if not copied and 'RUFUS_NATIVE_PARTITION_DISCOVERED' in text:
                    # Capture the successful, flushed output before later fault cases.
                    verify_usb(usb, source)
                    shutil.copyfile(usb, output / 'verified-usb.raw')
                    copied = True
                    print('Native write verified; saved boot-test disk.', flush=True)
                if not removed and 'RUFUS_REMOVE_USB_NOW' in text:
                    qmp_command(qmp, 'device_del', {'id': 'usb'})
                    removed = True
                    print('Unplugged the emulated USB during a write.', flush=True)
                if not reattached and 'RUFUS_REATTACH_USB_NOW' in text:
                    qmp_command(qmp, 'device_add', {'driver': 'usb-storage', 'id': 'usb',
                                'drive': 'stick', 'removable': True, 'serial': 'RUFUS-TEST-USB'})
                    reattached = True
                    print('Reattached the same emulated USB.', flush=True)
                time.sleep(.05)
            if process.returncode != 0:
                raise RuntimeError('QEMU failed; see ' + str(output / 'qemu.log'))
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
    for name in ['authentication.png', 'complete.png', 'mount-error']:
        subprocess.run(['debugfs', '-R', 'dump /home/rufus/' + name + ' ' + str(output / name),
                        str(system)], check=True, stdout=subprocess.DEVNULL)
    text = serial.read_text(errors='replace')
    required = ['RUFUS_NATIVE_AUTH_PROMPT', 'RUFUS_NATIVE_AUTH_DISMISSED_NO_WRITE',
                'RUFUS_NATIVE_WRITE_VERIFIED', 'RUFUS_NATIVE_PARTITION_DISCOVERED',
                'RUFUS_NATIVE_EXCLUSIVE_AND_CANCELLED', 'RUFUS_NATIVE_REMOVAL_FAILED_CLEANLY',
                'RUFUS_NATIVE_REATTACH_REJECTED_STALE_PLAN',
                'Totals: %d passed, 0 failed' % (6 + len(linux_images)), 'RUFUS_GUI_EXIT 0', 'RUFUS_GUEST_FINISHED']
    for record in linux_images:
        required.append('RUFUS_OPENWRT_WRITTEN_' + record['firmware'])
    if not copied or not removed or not reattached or any(marker not in text for marker in required):
        raise RuntimeError('Native qualification failed; see ' + str(serial))
    verify_usb(output / 'verified-usb.raw', source)
    previous = None
    for record in linux_images:
        name = 'openwrt-' + record['firmware'] + '.raw'
        subprocess.run(['debugfs', '-R', 'dump /home/rufus/' + name + ' ' + str(output / name),
                        str(system)], check=True, stdout=subprocess.DEVNULL)
        verify_usb(output / name, base / record['file'], previous)
        previous = output / name


def boot_output(output, accel):
    serial = output / 'boot.log'
    disk = output / 'verified-usb.raw'
    before = digest(disk)
    bios = Path('/usr/share/qemu/bios-256k.bin')
    shutil.copyfile(bios, output / 'bios.bin')
    (output / 'firmware.json').write_text(json.dumps({
        'source': str(bios), 'sha256': digest(output / 'bios.bin'),
        'qemu': subprocess.check_output(['qemu-system-x86_64', '--version'], text=True).strip()
    }, indent=2) + '\n')
    command = ['qemu-system-x86_64', '-accel', accel, '-cpu', 'host' if accel == 'kvm' else 'max',
               '-m', '128', '-display', 'none', '-monitor', 'none', '-serial', 'file:' + str(serial),
               '-nic', 'none', '-no-reboot', '-boot', 'strict=on',
               '-bios', str(output / 'bios.bin'),
               '-device', 'isa-debug-exit,iobase=0xf4,iosize=0x04', '-device', 'qemu-xhci,id=xhci',
               '-drive', 'if=none,id=stick,file=' + str(disk) + ',format=raw,readonly=on',
               '-device', 'usb-storage,drive=stick,removable=on,bootindex=1']
    (output / 'boot-command.json').write_text(json.dumps(command, indent=2) + '\n')
    with (output / 'boot-qemu.log').open('wb') as log:
        result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=30)
    if result.returncode != 33 or b'RUFUS_RAW_BOOT_OK\r\n' not in serial.read_bytes():
        raise RuntimeError('BIOS fixture did not boot; see ' + str(serial))
    if before != digest(disk):
        raise RuntimeError('Boot test modified the verified disk')
    print('PASS: the verified USB output boots the BIOS fixture.', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('guest', type=Path)
    parser.add_argument('output', type=Path, help='New result directory (must not exist)')
    parser.add_argument('--accel', choices=['kvm', 'tcg'], default='kvm')
    args = parser.parse_args()
    base, output = args.guest.resolve(), args.output.resolve()
    # QEMU -drive and the debugfs command parser use separators in path arguments.
    for path in [base, output]:
        if any(char in str(path) for char in ',\n\r\t "'):
            parser.error('Fixture paths must not contain whitespace, quotes or commas')
    manifest = json.loads((base / 'manifest.json').read_text())
    linux_images = manifest.get('linux_images', [])
    for name in ['system.raw', 'vmlinuz', 'initramfs.cpio', 'boot.img'] + [item['file'] for item in linux_images]:
        expected = next(item['sha256'] for item in manifest['files'] if item['file'] == name)
        if not (base / name).is_file() or digest(base / name) != expected:
            parser.error('Prepared guest file changed: ' + name)
    output.mkdir()
    shutil.copy2(base / 'manifest.json', output / 'manifest.json')
    run_guest(base, output, args.accel, linux_images)
    boot_output(output, args.accel)
    for record in linux_images:
        boot_linux(output, record['firmware'], args.accel)
    print('PASS: native authentication, write/read-back, exclusivity, cancellation, reattachment and boot checks.', flush=True)


if __name__ == '__main__':
    main()
