# SPDX-License-Identifier: GPL-3.0-or-later
"""One bootable mapped-parent source for the combined B2 guest gate."""
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import uuid
from virtual_disk_fixtures import vhdx, parent_locator, MIB, TABLE


def stage(base, init):
    size = 64 * MIB
    partition = base / 'mapped-partition.raw'
    subprocess.run(['mkfs.fat', '-F', '32', '-n', 'B2RESTORE', '-C', str(partition), str((size - MIB) // 1024)],
                   check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    marker = base / 'mapped-marker.txt'; marker.write_bytes(b'B2 mapped parent device restore\n')
    subprocess.run(['mcopy', '-i', str(partition), str(marker), '::marker.txt'], check=True)
    raw = bytearray(size)
    raw[MIB:] = partition.read_bytes()
    boot = bytearray.fromhex('fa fc 31 c0 8e d8 be 00 00 ac 84 c0 74 04 e6 e9 eb f7 b0 10 e6 f4 f4 eb fd')
    struct.pack_into('<H', boot, 7, 0x7c00 + len(boot))
    boot += b'RUFUS_B2_MAPPED_BOOT\x00'
    raw[:len(boot)] = boot
    struct.pack_into('<I', raw, 440, 0x12345678)
    raw[446] = 0x80; raw[450] = 0x0c
    struct.pack_into('<II', raw, 454, 2048, (size - MIB) // 512)
    raw[510:512] = b'\x55\xaa'
    parent, _, identifier = vhdx(raw)
    child, _, _ = vhdx(raw, 'missing-parent.vhdx', uuid.uuid4())
    metadata = parent_locator(None, uuid.uuid4(), [('parent_linkage2', '{' + str(identifier) + '}'),
        ('absolute_win32_path', r'\\?\C:\old\parent.vhdx')])
    offset = 2 * MIB + TABLE + 128
    child[offset:offset + len(metadata)] = metadata
    struct.pack_into('<I', child, 2 * MIB + 32 + 32 * 5 + 20, len(metadata))
    (init / 'mapped-parent.vhdx').write_bytes(parent)
    (init / 'mapped-child.vhdx').write_bytes(child)
    (init / 'mapped-child.vhdx.parents').write_text('@parent:\t/mapped-parent.vhdx\n')
    expected = hashlib.sha256(raw).hexdigest()
    (init / 'etc/rufus-mapped-expected').write_text(expected + '\n')
    (base / 'mapped-source.json').write_text(json.dumps({'size': size, 'sha256': expected, 'boot_marker': 'RUFUS_B2_MAPPED_BOOT'}) + '\n')
    partition.unlink(); marker.unlink()


def boot(args, base, disk):
    log = base / 'mapped-boot.log'
    command = ['qemu-system-x86_64', '-accel', args.accel, '-m', '64', '-display', 'none', '-monitor', 'none',
        '-serial', 'none', '-nic', 'none', '-debugcon', 'file:' + str(log), '-global', 'isa-debugcon.iobase=0xe9',
        '-device', 'isa-debug-exit,iobase=0xf4,iosize=0x04', '-device', 'qemu-xhci,id=xhci',
        '-drive', 'if=none,id=stick,format=raw,readonly=on,file=' + str(disk),
        '-device', 'usb-storage,drive=stick,removable=on,bootindex=0', '-no-reboot']
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
    assert result.returncode == 33 and b'RUFUS_B2_MAPPED_BOOT' in log.read_bytes(), (result.returncode, result.stdout)
    return {'bios_handoff': True, 'whole_disk_sha256': True, 'read_only_boot': True}
