#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Boot a written OpenWrt USB snapshot to its userspace shell, then power off."""
import json
from pathlib import Path
import re
import shutil
import socket
import subprocess
import time

from openwrt import VERSION, digest


def firmware_options(output, firmware, prefix='openwrt'):
    if firmware == 'bios':
        sources = [Path('/usr/share/qemu/bios-256k.bin')]
    else:
        sources = [Path('/usr/share/edk2/x64/OVMF_CODE.4m.fd'),
                   Path('/usr/share/edk2/x64/OVMF_VARS.4m.fd')]
    records = []
    for source in sources:
        target = output / (firmware + '-' + source.name)
        shutil.copyfile(source, target)
        records.append({'source': str(source), 'file': target.name, 'sha256': digest(target)})
    (output / (prefix + '-' + firmware + '-firmware.json')).write_text(json.dumps({
        'files': records,
        'qemu': subprocess.check_output(['qemu-system-x86_64', '--version'], text=True).strip()
    }, indent=2) + '\n')
    if firmware == 'bios':
        return ['-bios', str(output / records[0]['file'])]
    return ['-machine', 'q35', '-drive', 'if=pflash,format=raw,unit=0,readonly=on,file=' + str(output / records[0]['file']),
            '-drive', 'if=pflash,format=raw,unit=1,file=' + str(output / records[1]['file'])]


def read_console(process, path, log, firmware):
    deadline = time.monotonic() + 120
    while not path.exists():
        if process.poll() is not None or time.monotonic() >= deadline:
            raise RuntimeError('OpenWrt VM did not open its serial console')
        time.sleep(.05)
    expected = ('RUFUS_OPENWRT_BOOT_OK version=' + VERSION + ' arch=x86_64 firmware=' + firmware).encode()
    command = ('. /etc/openwrt_release; fw=bios; if [ -d /sys/firmware/efi ]; then fw=uefi; fi; '
               'printf "\\nRUFUS_OPENWRT_BOOT_OK version=%s arch=%s firmware=%s\\n" '
               '"$DISTRIB_RELEASE" "$(uname -m)" "$fw"; poweroff\r')
    sent = passed = False
    next_enter = 0
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
            if not sent and re.search(rb'(?:^|\n)root@OpenWrt:~# ', received):
                # Pace terminal input instead of bursting a line into the emulated UART.
                for byte in command.encode():
                    client.sendall(bytes([byte]))
                    time.sleep(.005)
                sent = True
            if not sent and b'Please press Enter to activate this console' in received and time.monotonic() >= next_enter:
                # Request fresh prompts until startup has set the configured hostname.
                client.sendall(b'\r')
                next_enter = time.monotonic() + 1
            # Match an output line, not the terminal's echo of the shell command.
            if expected in received.replace(b'\r', b'').split(b'\n'):
                passed = True
        if not passed:
            raise RuntimeError('OpenWrt did not confirm its release, architecture and ' + firmware + ' boot mode')
    if process.wait(timeout=15) != 0:
        raise RuntimeError('OpenWrt VM did not shut down successfully')


def boot_linux(output, firmware, accel):
    disk = output / ('openwrt-' + firmware + '.raw')
    before = digest(disk)
    path = output / ('openwrt-' + firmware + '.sock')
    command = ['qemu-system-x86_64', '-accel', accel, '-cpu', 'host' if accel == 'kvm' else 'max',
               '-m', '512', '-display', 'none', '-monitor', 'none', '-no-reboot', '-nic', 'none',
               '-serial', 'unix:' + str(path) + ',server=on,wait=on', '-snapshot',
               '-device', 'qemu-xhci,id=xhci',
               '-drive', 'if=none,id=stick,file=' + str(disk) + ',format=raw',
               '-device', 'usb-storage,drive=stick,removable=on,bootindex=1']
    command.extend(firmware_options(output, firmware))
    (output / ('openwrt-' + firmware + '-command.json')).write_text(json.dumps(command, indent=2) + '\n')
    with (output / ('openwrt-' + firmware + '-qemu.log')).open('wb') as errors, \
            (output / ('openwrt-' + firmware + '-boot.log')).open('wb') as log:
        process = subprocess.Popen(command, stdout=errors, stderr=subprocess.STDOUT)
        try:
            read_console(process, path, log, firmware)
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
    if digest(disk) != before:
        raise RuntimeError('OpenWrt boot modified the verified backing image')
    print('PASS: OpenWrt ' + VERSION + ' reached userspace through ' + firmware + ' USB boot.', flush=True)
