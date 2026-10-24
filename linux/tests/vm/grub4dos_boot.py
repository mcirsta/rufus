# SPDX-License-Identifier: GPL-3.0-or-later
"""Execute original GRUB4DOS from helper-created standalone or ISO USB media."""
import json
import os
import re
import shutil
import subprocess
import zlib

from boot_linux import firmware_options
from fat_extract import digest
from freedos_boot import wait_for_screen
from syslinux_alpine import qmp_read, qmp_command, type_keys

BOOT_TIMEOUT_SECONDS = 30
COMMAND_TIMEOUT_SECONDS = 10
FAILURE_CONTROL_SECONDS = 8
KEY_HOLD_MILLISECONDS = 40
KEY_INTERVAL_SECONDS = .08
MARKER = 'RUFUSGRUB4DOSOK'
LOADER_SHA256 = '19ba57329be745ceae46354c5d411b7d8efec6802cfd7f6fee986df4b763f6fe'


def qualify_boot(args, base, disk, loader, timeout=BOOT_TIMEOUT_SECONDS):
    output = base / 'grub4dos-bios'
    output.mkdir()
    before = digest(disk)
    assert digest(loader) == LOADER_SHA256, 'Standalone loader differs from original Rufus fixture'
    crc = f'{zlib.crc32(loader.read_bytes()):08x}'
    command = ['qemu-system-x86_64', '-accel', args.accel,
               '-cpu', 'host' if args.accel == 'kvm' else 'max', '-m', '64', '-vga', 'std',
               '-display', 'none', '-monitor', 'none', '-serial', 'none', '-no-reboot', '-nic', 'none',
               '-qmp', 'stdio', '-snapshot', '-device', 'qemu-xhci,id=xhci',
               '-drive', 'if=none,id=stick,format=raw,file=' + str(disk),
               '-device', 'usb-storage,drive=stick,removable=on,bootindex=1']
    command.extend(firmware_options(output, 'bios', 'grub4dos'))
    (output / 'qemu-command.json').write_text(json.dumps(command, indent=2) + '\n')
    checks = ['root', 'ls /', 'crc32 /grldr', 'echo ' + MARKER]
    if args.grub4dos_image:
        checks.extend(['crc32 /menu.lst', 'echo %RUFUSISO%', 'cat /image-marker'])
    (output / 'commands.txt').write_text('\n'.join(checks) + '\n')
    with (output / 'qemu.log').open('wb') as log:
        process = subprocess.Popen(command, env=dict(os.environ, TMPDIR=str(output)),
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log, bufsize=0)
        try:
            assert 'QMP' in qmp_read(process)
            qmp_command(process, 'qmp_capabilities')
            prompt = wait_for_screen(process, output, r'^grub>\s*$', timeout, 'prompt')
            assert re.search(r'GRUB4DOS\s+0\.4\.6a\s+2024-02-26', prompt), prompt
            filesystem = 'ntfs' if args.grub4dos_ntfs else f'fat{args.fat_bits}'
            partition_type = '07' if args.grub4dos_ntfs else '0E' if args.fat_bits == 16 else '0C'
            root = rf'\(hd0,0\) Filesystem type is {filesystem}, partition type 0x{partition_type}'
            patterns = [root, r'\bgrldr\b', '^' + crc + '$', '^' + MARKER + '$']
            names = ['root', 'directory', 'crc', 'command']
            if args.grub4dos_image:
                menu = (args.iso.parent / 'grub4dos-source/menu.lst').read_bytes()
                patterns.extend(['^' + f'{zlib.crc32(menu):08x}' + '$',
                                 '^RUFUSGRUB4DOSIMAGE$', '^RUFUSGRUB4DOSIMAGE$'])
                names.extend(['menu-crc', 'menu-executed', 'image-marker'])
            for text, pattern, name in zip(checks, patterns, names):
                type_keys(process, text + '\n', KEY_HOLD_MILLISECONDS, KEY_INTERVAL_SECONDS)
                # Require a distinct command response and the next prompt, not
                # the input echo. CRC reads all loader bytes through GRUB4DOS FAT.
                expected = r'^grub> ' + re.escape(text) + r'\n[\s\S]*?' + pattern + r'[\s\S]*?^grub>\s*$'
                wait_for_screen(process, output, expected, COMMAND_TIMEOUT_SECONDS, name)
            qmp_command(process, 'quit')
            assert process.wait(timeout=5) == 0, 'GRUB4DOS BIOS VM did not exit cleanly'
        finally:
            if process.poll() is None:
                try:
                    qmp_command(process, 'screendump', {'filename': str(output / 'stopped.png'), 'format': 'png'})
                except (OSError, RuntimeError, ValueError):
                    pass  # Preserve failure evidence without preventing cleanup.
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
            process.stdin.close()
            process.stdout.close()
    assert digest(disk) == before, 'GRUB4DOS snapshot boot changed the verified backing disk'
    result = {'firmware': 'bios', 'filesystem': 'NTFS' if args.grub4dos_ntfs else f'FAT{args.fat_bits}', 'disk_sha256': before,
              'loader_sha256': LOADER_SHA256, 'loader_crc32': crc,
              'version': '0.4.6a 2024-02-26', 'command_output': MARKER, 'passed': True}
    result['image_menu_executed'] = args.grub4dos_image
    (output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print('PASS: helper-created GRUB4DOS USB reached its prompt, read the loader and executed ECHO under BIOS.', flush=True)
    return result


def check_missing_loader(args, base, disk, loader, offset, size):
    control = base / 'grub4dos-missing-loader'
    control.mkdir()
    missing = control / 'missing.raw'
    shutil.copyfile(disk, missing)
    try:
        if args.grub4dos_ntfs:
            from test_fat_format import copy_volume
            volume = control / 'missing-volume.raw'
            assert offset <= missing.stat().st_size and size <= missing.stat().st_size - offset
            copy_volume(missing, volume, offset, size)
            subprocess.run([args.build / 'ntfs_file_fixture', 'remove', volume, 'grldr'],
                           check=True, capture_output=True, timeout=15)
            absent = subprocess.run([args.build / 'ntfs_file_fixture', 'get', volume, 'grldr'],
                                    capture_output=True, timeout=15)
            assert absent.returncode == 67 and b'No such file' in absent.stderr
            subprocess.run(['7z', 'l', volume], check=True, capture_output=True, timeout=15)
            with volume.open('rb') as source, missing.open('r+b') as target:
                target.seek(offset)
                shutil.copyfileobj(source, target, 1024 * 1024)
                target.flush()
                os.fsync(target.fileno())
            volume.unlink()
        else:
            subprocess.run(['mdel', '-i', f'{missing}@@{offset}', '::/grldr'],
                           check=True, capture_output=True, timeout=10)
        before = digest(missing)
        try:
            qualify_boot(args, control, missing, loader, timeout=FAILURE_CONTROL_SECONDS)
        except RuntimeError as error:
            assert str(error) == 'BIOS guest did not reach prompt', error
        else:
            raise AssertionError('Boot qualification accepted media without grldr')
        screens = (control / 'grub4dos-bios/prompt-screens.txt').read_text()
        assert 'Cannot find GRLDR in all drives.' in screens, 'Expected original missing-loader error'
        assert digest(missing) == before, 'Failure-control boot changed its backing disk'
        result = {'missing_loader_rejected': True, 'mutated_disk_sha256': before}
        (control / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
        return result
    finally:
        (control / 'missing-volume.raw').unlink(missing_ok=True)
        missing.unlink(missing_ok=True)
