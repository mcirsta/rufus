#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Small Linux-only runtime validation batch; no Windows application runs."""
import hashlib
from pathlib import Path
import shutil
import subprocess

# Nonresident NTFS data avoids resident copies in metadata/journal records.
PAYLOAD = b'RUFUS_RUNTIME_POISON_CONTROL_0123456789\n' + bytes(range(256)) * 32


def stage_fixture(args, base, init):
    from prepare import write
    source = base / 'runtime-source'
    entries = {'efi/boot/bootx64.efi': (args.build / 'uefi-handoff.efi').read_bytes(),
               'isolinux/isolinux.cfg': b'append root=LABEL=SOURCE\n',
               'payload/安装.txt': 'Native Unicode validation\n'.encode(),
               'control.txt': PAYLOAD, 'known-bad.txt': b'Keep an existing wrong checksum\n'}
    source.mkdir()
    for name, content in entries.items():
        path = source / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
    for name, manifest in [('listed', 'md5sum.txt'), ('uppercase', 'MD5SUMS'), ('plain', None), ('conflict', None)]:
        # Each preceding filesystem leaves stale data after quick formatting.
        # Give only the final NTFS image the exact control sequence.
        entries['control.txt'] = PAYLOAD if name == 'plain' else name.encode() + PAYLOAD[1:]
        (source / 'control.txt').write_bytes(entries['control.txt'])
        rows = []
        for path, content in entries.items():
            md5 = '0' * 32 if name == 'uppercase' and path == 'known-bad.txt' else hashlib.md5(content).hexdigest()
            rows.append(md5 + '  ./' + path + '\n')
        if manifest:
            (source / manifest).write_text('# preserve source comments\r\n' + ''.join(rows))
        if name == 'conflict':
            (source / 'efi/boot/bootx64_original.efi').write_bytes(b'existing backup')
        subprocess.run([str(args.mkisofs), '-quiet', '-R', '-J', '-joliet-long', '-V', 'SOURCE',
                        '-o', str(init / (name + '.iso')), str(source)], check=True)
        if manifest:
            (source / manifest).unlink()
    shutil.copy2(args.build / 'uefi-handoff.efi', init / 'original.efi')
    shutil.copy2(Path(__file__).resolve().parents[3] / 'res/md5/bootx64.efi', init / 'validator.efi')
    write(init, 'etc/rufus-runtime-validation-fixture', 'runtime-validation\n')
    write(init, 'etc/rufus-format-helper-fixture', 'format-helper\n')
    (init / 'tmp').mkdir(mode=0o1777, exist_ok=True)
    (init / 'tmp').chmod(0o1777)


def qualify_corruption(args, base, disk):
    """Change one payload byte and observe failure without accepting continue."""
    import json
    import mmap
    import os
    from boot_linux import firmware_options
    from fat_extract import digest
    output = base / 'runtime-corrupt-uefi'
    output.mkdir()
    original = digest(disk)
    poisoned = base / 'runtime-corrupt.raw'
    shutil.copyfile(disk, poisoned)
    try:
        with poisoned.open('r+b') as stream:
            with mmap.mmap(stream.fileno(), 0) as content:
                offset = content.find(PAYLOAD)
                assert offset >= 0 and content.find(PAYLOAD, offset + 1) == -1, ('Unique control payload', offset, content.find(PAYLOAD, offset + 1))
                content[offset] ^= 1
                content.flush()
        command = ['qemu-system-x86_64', '-accel', args.accel, '-m', '256', '-nic', 'none',
                   '-display', 'none', '-monitor', 'none', '-no-reboot', '-snapshot',
                   '-serial', 'file:' + str(output / 'serial.log'),
                   '-device', 'isa-debug-exit,iobase=0xf4,iosize=0x04', '-device', 'qemu-xhci,id=xhci',
                   '-drive', 'if=none,id=stick,format=raw,file=' + str(poisoned),
                   '-device', 'usb-storage,drive=stick,removable=on,bootindex=1']
        command.extend(firmware_options(output, 'uefi', 'syslinux'))
        (output / 'qemu-command.json').write_text(json.dumps(command, indent=2) + '\n')
        timed_out = False
        with (output / 'qemu.log').open('wb') as log:
            try:
                subprocess.run(command, timeout=45, env=dict(os.environ, TMPDIR=str(output)),
                               stdout=log, stderr=subprocess.STDOUT, check=False)
            except subprocess.TimeoutExpired:
                timed_out = True
        serial = (output / 'serial.log').read_text(errors='replace')
        assert timed_out and 'RUFUS_SYSLINUX_UEFI_OK' not in serial, 'Corrupt media must not chainload automatically'
        assert 'Continue with boot?' in serial and '1 failed' in serial and 'control.txt' in serial, serial
        assert digest(disk) == original, 'Corruption control changed the original successful media'
        return {'one_payload_byte_changed': True, 'one_failure_reported': True,
                'continue_prompt_unanswered': True, 'original_loader_not_started': True}
    finally:
        poisoned.unlink(missing_ok=True)
        for firmware in output.glob('*.fd'):
            firmware.unlink()
