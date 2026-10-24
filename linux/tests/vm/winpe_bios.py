# SPDX-License-Identifier: GPL-3.0-or-later
"""Original legacy records to a generated loader, on regular-file USBs only.

This does not qualify Windows Setup or connect a GUI/helper write route.
"""
import argparse
import errno
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time
import struct
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from test_mbr_image import extent, invoke as create_mbr, inspect_mbr
from test_fat_format import invoke as format_fat, copy_volume
from test_freedos_boot import read_window, expected_records
from syslinux_alpine import qmp_read, qmp_command, read_screen
from mbr import check_fat_volume

MIB = 1024 * 1024
# A small USB gives SeaBIOS a different CHS translation from mkfs.fat's 255/63.
# Use a sparse disk above the CHS limit so the original NT record's fallback
# agrees with its BPB. It occupies only its FAT tables and generated files.
DISK_BYTES = 9 * 1024 * MIB
FAT16_DISK_BYTES = 129 * MIB
CLUSTER_BYTES = 4096
SECTOR_BYTES = 512
LABEL = 'RUFUS_TEST'
BOOT_TIMEOUT_SECONDS = 15
SUCCESS_EXIT = 33
CRC_OFFSET = 0x2060
PATH_OFFSET = 0x2100
DRIVE_OFFSET = 0x2200
EXPECTED_SIF = b'[SetupData]\r\nSetupSourceDevice = "\\device\\harddisk1\\partition1"\r\nOsLoadOptions=/fastdetect\r\n'
FAT32_DRIVE_OFFSET = 64
FAT16_DRIVE_OFFSET = 36
FAT32_BACKUP_SECTOR = 6
FSINFO_HINT_OFFSET = SECTOR_BYTES + 488
FSINFO_HINT_BYTES = 8


def digest(path):
    with path.open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def disk_identity(path):
    # Hash allocated ranges plus their locations and logical length, including
    # any zero-filled allocation. This catches changed bytes/hole layout without
    # reading nine GiB of holes for every disposable snapshot boot.
    checksum = hashlib.sha256()
    size = path.stat().st_size
    checksum.update(struct.pack('<Q', size))
    allocated = 0
    with path.open('rb') as source:
        position = 0
        while position < size:
            try:
                start = os.lseek(source.fileno(), position, os.SEEK_DATA)
            except OSError as error:
                if error.errno == errno.ENXIO:
                    break
                raise
            end = min(os.lseek(source.fileno(), start, os.SEEK_HOLE), size)
            checksum.update(struct.pack('<QQ', start, end))
            source.seek(start)
            remaining = end - start
            allocated += remaining
            while remaining:
                data = source.read(min(remaining, MIB))
                assert data
                checksum.update(data)
                remaining -= len(data)
            position = end
    return {'logical_bytes': size, 'allocated_range_bytes': allocated,
            'sparse_extent_sha256': checksum.hexdigest()}


def run(command, output, name):
    command = [str(value) for value in command]
    result = subprocess.run(command, capture_output=True, timeout=30)
    (output / (name + '.log')).write_bytes(result.stdout + result.stderr)
    with (output / 'commands.jsonl').open('a') as log:
        log.write(json.dumps(command) + '\n')
    assert result.returncode == 0, (command, result.returncode, result.stdout, result.stderr)
    return result.stdout


def prepare_root(args, work, output, uses_minint, family='i386'):
    root = work / 'root'
    source = root / ('minint' if uses_minint else 'i386')
    source.mkdir(parents=True)
    setup = root / 'amd64' if family == 'amd64' else source
    setup.mkdir(exist_ok=True)
    loader = source / 'setupldr.bin'
    drive = '0x80' if uses_minint else '0x81'
    assembly = Path(__file__).with_name('windows_bootmgr.asm')
    run(['nasm', '-f', 'bin', '-DEXPECTED_DRIVE=' + drive, assembly, '-o', loader], output, 'nasm')
    data = bytearray(loader.read_bytes())
    data[CRC_OFFSET:CRC_OFFSET + 2] = b'\x74\x03'
    marker = b'\\minint\\txtsetup.sif\x00'
    data[PATH_OFFSET:PATH_OFFSET + len(marker)] = marker
    marker = b'rdisk(0)\x00'
    data[DRIVE_OFFSET:DRIVE_OFFSET + len(marker)] = marker
    loader.write_bytes(data)
    (source / 'ntdetect.com').write_bytes(b'generated detector fixture\r\n')
    sif = setup / 'txtsetup.sif'
    options = b'/fastdetect /minint' if uses_minint else b'/fastdetect'
    sif.write_bytes(b'[SetupData]\r\nOsLoadOptions=' + options + b'\r\n')
    originals = {str(path.relative_to(root)): digest(path)
                 for path in [loader, source / 'ntdetect.com', sif]}
    mask = '0x1c0' if uses_minint else '0x23' if family == 'amd64' else '0x7'
    result = run([args.build / 'winpe_prepare_probe', root, mask, int(uses_minint)], output, 'prepare')
    values = {key.decode(): int(value) for key, value in re.findall(rb'([A-Z_]+)=(\d+)', result)}
    assert values == {'ERROR': 0, 'CLOSE': 0, 'MASQUERADE': int(not uses_minint), 'COPY_SIF': int(not uses_minint)}
    expected = bytearray(data)
    if not uses_minint:
        expected[CRC_OFFSET:CRC_OFFSET + 2] = b'\xeb\x1a'
        replacement = ('\\' + family + '\\txtsetup.sif\x00').encode()
        expected[PATH_OFFSET:PATH_OFFSET + len(replacement)] = replacement
        expected[DRIVE_OFFSET + 6] = ord('1')
        assert (root / 'TXTSETUP.SIF').read_bytes() == EXPECTED_SIF
    else:
        assert not (root / 'TXTSETUP.SIF').exists()
    assert (root / 'BOOTMGR').read_bytes() == expected
    assert (root / 'ntdetect.com').read_bytes() == (source / 'ntdetect.com').read_bytes()
    assert originals == {name: digest(root / name) for name in originals}
    (output / 'source-readback.json').write_text(json.dumps(originals, indent=2) + '\n')
    return root


def make_disk(args, work, output, root, provider, alias, masquerade, patch_drive,
              bits=32, records_last=False):
    disk = work / 'usb.raw'
    disk_bytes = FAT16_DISK_BYTES if bits == 16 else DISK_BYTES
    with disk.open('xb') as target:
        target.truncate(disk_bytes)
    code = 'rufus-masquerade' if masquerade else 'rufus'
    report = create_mbr(str(args.build / 'mbr_image_probe'), str(disk), disk_bytes, cluster=CLUSTER_BYTES,
                        boot_code=code, file_system=bits)
    if bits == 16:
        # Leave the rest unpartitioned, but keep the same BIOS CHS translation
        # as mkfs.fat (255/63). The disk remains sparse. Plan/MBR validation
        # above uses the small filesystem extent, not the enlarged fixture.
        with disk.open('r+b') as target:
            target.truncate(DISK_BYTES)
    offset, size = extent(disk_bytes, CLUSTER_BYTES)
    inspect_mbr(disk, disk_bytes, cluster=CLUSTER_BYTES, boot_code=code, file_system=bits)
    formatter = SimpleNamespace(probe=args.build / 'fat_format_probe', mkfs='/usr/bin/mkfs.fat', fat_bits=bits)
    format_fat(formatter, disk, offset=offset, size=size, cluster=CLUSTER_BYTES, label=LABEL)
    files = {path.name: path for path in root.iterdir() if path.is_file()}
    if alias:
        files['NTLDR'] = root / 'BOOTMGR'
    if records_last:
        copy_root(disk, offset, files, output)
    before = read_window(disk, offset)
    run([args.build / 'freedos_boot_probe', disk, offset, size, CLUSTER_BYTES,
         LABEL, bits, 'none', provider], output, 'records')
    adapted_winpe = provider in ('winpe-bootmgr-i386', 'winpe-bootmgr-amd64', 'winpe-bootmgr-minint')
    expected = expected_records(before, bits, winpe=provider == 'winpe', windows=provider == 'windows',
                                winpe_bootmgr=adapted_winpe, masquerade_disk=masquerade)
    assert read_window(disk, offset) == expected
    hint_refresh = bits == 32 and provider == 'windows' and not records_last
    if hint_refresh:
        # The original PE array reaches five bytes into FSInfo allocation hints,
        # advertising zero free clusters to mcopy. Restore the formatter's known
        # valid hints for this mount-free fixture; actual boot code stays exact.
        # This is not evidence for a production mount/remount metadata lifecycle.
        expected = bytearray(expected)
        with disk.open('r+b') as target:
            for base in (0, FAT32_BACKUP_SECTOR * SECTOR_BYTES):
                hint = base + FSINFO_HINT_OFFSET
                data = before[hint:hint + FSINFO_HINT_BYTES]
                target.seek(offset + hint)
                target.write(data)
                expected[hint:hint + FSINFO_HINT_BYTES] = data
        assert read_window(disk, offset) == expected
    if patch_drive:
        # Diagnostic fixture only: unchanged original code, BPB drive 0x81 in
        # the primary/available backup to follow the MBR mapping. Never a route.
        expected = bytearray(expected)
        drive_offset = FAT16_DRIVE_OFFSET if bits == 16 else FAT32_DRIVE_OFFSET
        copies = (0,) if bits == 16 else (0, FAT32_BACKUP_SECTOR * SECTOR_BYTES)
        with disk.open('r+b') as target:
            for base in copies:
                target.seek(offset + base + drive_offset)
                target.write(b'\x81')
                expected[base + drive_offset] = 0x81
        assert read_window(disk, offset) == expected
    if not records_last:
        copy_root(disk, offset, files, output)
    after = read_window(disk, offset)
    expected = bytearray(expected)
    if bits == 32 and not records_last:
        for base in (0, FAT32_BACKUP_SECTOR * SECTOR_BYTES):
            hint = base + FSINFO_HINT_OFFSET
            expected[hint:hint + FSINFO_HINT_BYTES] = after[hint:hint + FSINFO_HINT_BYTES]
    assert after == expected, 'File copy changed boot code/BPB outside allocation hints'
    inspect_mbr(disk, disk_bytes, cluster=CLUSTER_BYTES, boot_code=code, file_system=bits)
    volume = work / 'volume.raw'
    copy_volume(disk, volume, offset, size)
    damaged_hints = records_last and bits == 32 and provider == 'windows'
    if damaged_hints:
        check_overwritten_hints(volume, output)
    else:
        check_fat_volume(output, volume, standard=True, label=LABEL)
    volume.unlink()
    with disk.open('rb') as target:
        os.fsync(target.fileno())
    (output / 'disk-readback.json').write_text(json.dumps({
        'mbr': report, 'files': {name: digest(path) for name, path in files.items()},
        'disk_identity': disk_identity(disk),
        'original_records': not patch_drive and not hint_refresh and not adapted_winpe,
        'native_winpe_adaptation': adapted_winpe,
        'filesystem_clean': not damaged_hints, 'fsinfo_fixture_refresh': hint_refresh,
        'fat_bits': bits, 'records_after_copy': records_last, 'partition_layout_bytes': disk_bytes,
        'alias_test_only': alias, 'bpb_drive_test_only': patch_drive,
        'original_boot_code': True}, indent=2) + '\n')
    return disk


def copy_root(disk, offset, files, output):
    for name, path in files.items():
        run(['mcopy', '-i', str(disk) + '@@' + str(offset), path, '::' + name], output, 'copy-' + name)
        actual = run(['mtype', '-i', str(disk) + '@@' + str(offset), '::' + name], output, 'read-' + name)
        assert actual == path.read_bytes(), name
        # Binary readback is hashed; keep logs small.
        (output / ('read-' + name + '.log')).write_text(hashlib.sha256(actual).hexdigest() + '\n')


def check_overwritten_hints(volume, output):
    # Audit the unmodified PE writer after copying, without repairing its
    # allocation hints. Boot success alone must not hide invalid metadata.
    checked = subprocess.run(['fsck.fat', '-n', str(volume)], capture_output=True, timeout=20)
    (output / 'fsck.log').write_bytes(checked.stdout + checked.stderr)
    assert checked.returncode == 1 and not checked.stderr, checked
    lines = checked.stdout.decode().splitlines()
    assert lines[1:3] == [f"There is no label in boot sector, but there is volume label '{LABEL}' stored in root directory",
                         '  Auto-copying volume label from root directory to boot sector.'], lines
    assert re.fullmatch(r'Free cluster summary wrong \(0 vs\. really \d+\)', lines[3]), lines
    assert lines[4:7] == ['  Auto-correcting.', '', 'Leaving filesystem unchanged.'], lines
    assert len(lines) == 8 and re.fullmatch(re.escape(str(volume)) + r': \d+ files, \d+/\d+ clusters', lines[7]), lines


def boot(disk, output, expected_drive, expected_error):
    before = disk_identity(disk)
    serial = output / 'serial.log'
    firmware = Path('/usr/share/qemu/bios-256k.bin')
    command = ['qemu-system-x86_64', '-accel', 'tcg', '-cpu', 'max', '-m', '128',
               '-display', 'none', '-monitor', 'none', '-no-reboot', '-nic', 'none',
               '-serial', 'file:' + str(serial), '-qmp', 'stdio', '-S',
               '-bios', str(firmware), '-snapshot',
               '-device', 'isa-debug-exit,iobase=0xf4,iosize=0x04',
               '-device', 'qemu-xhci,id=xhci',
               '-drive', f'if=none,id=stick,format=raw,file={disk}',
               '-device', 'usb-storage,drive=stick,removable=on,bootindex=1']
    (output / 'qemu-command.json').write_text(json.dumps(command, indent=2) + '\n')
    start = time.monotonic()
    error_seen = False
    with (output / 'qemu.log').open('wb') as log:
        process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                   stderr=log, env=dict(os.environ, TMPDIR=str(output)), bufsize=0)
        try:
            assert 'QMP' in qmp_read(process)
            qmp_command(process, 'qmp_capabilities')
            qmp_command(process, 'cont')
            if expected_error:
                deadline = start + BOOT_TIMEOUT_SECONDS
                screen = ''
                while process.poll() is None and time.monotonic() < deadline:
                    screen = read_screen(process, output)
                    (output / 'screen.txt').write_text(screen + '\n')
                    if expected_error in screen:
                        error_seen = True
                        break
                    time.sleep(.05)
                assert error_seen, ('Original error not observed', expected_error, screen)
                assert process.poll() is None, 'Failed-loader guest unexpectedly exited'
            else:
                code = process.wait(timeout=BOOT_TIMEOUT_SECONDS)
                assert code == SUCCESS_EXIT, (code, serial.read_text(errors='replace'))
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=5)
            process.stdin.close()
            process.stdout.close()
            (output / 'vga.bin').unlink(missing_ok=True)
    text = serial.read_text(errors='replace').replace('\r', '').splitlines()
    expected = f'RUFUS_WINDOWS_BOOTMGR_OK cs=2000 drive={expected_drive:02x} tail=ok'
    assert text == ([] if expected_error else [expected]), text
    assert disk_identity(disk) == before, 'Snapshot boot changed backing file'
    return {'observed_error': expected_error if error_seen else None,
            'loader_marker': None if expected_error else expected,
            'snapshot_unchanged': True, 'firmware_sha256': digest(firmware),
            'duration_seconds': round(time.monotonic() - start, 2)}


def main():
    # name, provider, alias, /minint, masquerading, diagnostic BPB correction,
    # expected original diagnostic. No Windows loader is present in any case.
    cases = [('nt-stock-masquerade', 'winpe', False, False, True, False, 'Disk error'),
             ('nt-stock-drive80', 'winpe', False, False, False, False, 'NTLDR is missing'),
             ('nt-drive81', 'winpe', False, False, True, True, 'NTLDR is missing'),
             ('nt-ntldr-alias', 'winpe', True, False, True, True, None),
             ('pe-bootmgr', 'windows', False, False, True, True, None),
             ('pe-minint', 'windows', False, True, False, False, None)]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--case', choices=[case[0] for case in cases])
    mode.add_argument('--audit-fat', action='store_true',
                        help='Audit FAT16 and FAT32 BOOTMGR records after file copy, without FSInfo repair')
    mode.add_argument('--winpe-bootmgr', action='store_true',
                      help='Qualify native WinPE FAT32 BOOTMGR records, without fixture drive/hint repair')
    args = parser.parse_args()
    args.build = args.build.resolve()
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    results = []
    if args.winpe_bootmgr:
        for family, records_last in (('i386', True), ('amd64', True), ('minint', True), ('i386', False)):
            uses_minint = family == 'minint'
            name = family if records_last else family + '-records-first'
            output = args.output / name
            output.mkdir()
            with tempfile.TemporaryDirectory(prefix='rufus-winpe-bootmgr-bios-') as temporary:
                work = Path(temporary)
                root = prepare_root(args, work, output, uses_minint, family)
                disk = make_disk(args, work, output, root, 'winpe-bootmgr-' + family,
                                 False, not uses_minint, False, records_last=records_last)
                result = boot(disk, output, 0x80 if uses_minint else 0x81, None)
                result.update({'case': name, 'native_winpe_adaptation': True,
                               'diagnostic_bpb_drive': False, 'diagnostic_alias': False,
                               'fsinfo_fixture_refresh': False, 'records_after_copy': records_last})
                results.append(result)
                print('PASS', name, result, flush=True)
        cases = []
    if args.audit_fat:
        # name, bits, family, /minint, masquerade, diagnostic drive correction,
        # expected original error. No NTLDR/DOS aliases are generated.
        audit = [('fat16-i386', 16, 'i386', False, True, True, 'Invalid system disk'),
                 ('fat16-minint', 16, 'i386', True, False, False, 'Invalid system disk'),
                 ('pe-stock-i386', 32, 'i386', False, True, False, 'Disk error'),
                 ('pe-final-i386', 32, 'i386', False, True, True, None),
                 ('pe-final-amd64', 32, 'amd64', False, True, True, None),
                 ('pe-final-minint', 32, 'i386', True, False, False, None)]
        for name, bits, family, uses_minint, masquerade, patch_drive, expected_error in audit:
            output = args.output / name
            output.mkdir()
            provider = 'winpe' if bits == 16 else 'windows'
            with tempfile.TemporaryDirectory(prefix='rufus-winpe-fat-audit-') as temporary:
                work = Path(temporary)
                root = prepare_root(args, work, output, uses_minint, family)
                disk = make_disk(args, work, output, root, provider, False, masquerade,
                                 patch_drive, bits, records_last=True)
                result = boot(disk, output, 0x81 if masquerade else 0x80, expected_error)
                result.update({'case': name, 'fat_bits': bits, 'family': family, 'uses_minint': uses_minint,
                               'masquerade': masquerade, 'diagnostic_bpb_drive': patch_drive,
                               'records_after_copy': True, 'diagnostic_alias': False})
                results.append(result)
                print('PASS', name, result, flush=True)
        cases = []
    for name, provider, alias, uses_minint, masquerade, patch_drive, expected_error in cases:
        if args.case and args.case != name:
            continue
        output = args.output / name
        output.mkdir()
        with tempfile.TemporaryDirectory(prefix='rufus-winpe-bios-') as temporary:
            work = Path(temporary)
            root = prepare_root(args, work, output, uses_minint)
            disk = make_disk(args, work, output, root, provider, alias, masquerade, patch_drive)
            result = boot(disk, output, 0x81 if masquerade else 0x80, expected_error)
            result.update({'case': name, 'provider': provider, 'uses_minint': uses_minint,
                           'masquerade': masquerade, 'diagnostic_bpb_drive': patch_drive,
                           'diagnostic_alias': alias})
            results.append(result)
            print('PASS', name, result, flush=True)
    (args.output / 'result.json').write_text(json.dumps({
        'cases': results, 'windows_setup_qualified': False, 'production_route_enabled': False,
        'temporary_disks_removed': True}, indent=2) + '\n')


if __name__ == '__main__':
    main()
