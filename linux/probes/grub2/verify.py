# SPDX-License-Identifier: GPL-3.0-or-later
"""Boot Rufus's unchanged GRUB2 records on disposable FAT USB images."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'linux/tests'))
sys.path.insert(0, str(ROOT / 'linux/tests/vm'))
from test_mbr_image import invoke as partition_mbr
from test_fat_format import invoke as format_fat
from test_freedos_boot import invoke as install_fat_records, expected_records, read_window
from test_grub2_core import invoke as install_core
from syslinux_alpine import qmp_read, qmp_command, read_screen, type_keys

SECTOR_BYTES = 512
MIB = 1024 * 1024
DISK_BYTES = 64 * MIB
MBR_CODE_BYTES = 432
BOOT_TIMEOUT_SECONDS = 20
CORE_SHA256 = '9a2c946704017fa8dc4e03a8a58d754d2d1607c2d2cd74f0e2920133f1192809'
BOOT_SHA256 = 'b31c4cf688e8e16ddd177b619c20b049940bab5c675f877a1aa84a15e1e6e2e6'
MARKER = 'rufus-grub-probe.txt'


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def check_payloads():
    core_path = ROOT / 'res/grub2/core.img'
    boot_path = ROOT / 'res/grub2/boot.img'
    core = core_path.read_bytes()
    boot = boot_path.read_bytes()
    assert digest(core_path) == CORE_SHA256
    assert digest(boot_path) == BOOT_SHA256
    # Rufus writes this array, not res/grub2/boot.img. Check they really agree.
    header = (ROOT / 'src/ms-sys/inc/mbr_grub2.h').read_text()
    initializer = header.split('{', 1)[1].split('}', 1)[0]
    code = bytes(int(value, 16) for value in re.findall(r'0x([0-9A-Fa-f]{2})\b', initializer))
    assert len(code) == MBR_CODE_BYTES and code == boot[:MBR_CODE_BYTES]
    assert boot[510:] == b'\x55\xaa' and boot[0x66:0x68] == b'\x90\x90'
    assert struct.unpack_from('<Q', boot, 0x5c)[0] == 1
    first, count, segment = struct.unpack_from('<QHH', core, 500)
    assert (first, segment) == (2, 0x820)
    assert count == (len(core) + SECTOR_BYTES - 1) // SECTOR_BYTES - 1
    return core_path, core, code


def wait_screen(process, work, log, expected):
    deadline = time.monotonic() + BOOT_TIMEOUT_SECONDS
    while process.poll() is None and time.monotonic() < deadline:
        screen = read_screen(process, work)
        log.write_text(screen + '\n')
        if expected in screen:
            return screen
        time.sleep(.1)
    raise AssertionError(f'GRUB did not display {expected!r}; see {log}')


def boot_disk(args, disk, work, output, marker=MARKER, filesystem='fat'):
    firmware = Path('/usr/share/qemu/bios-256k.bin')
    command = [args.qemu, '-machine', 'pc', '-accel', 'tcg', '-m', '64',
               '-nodefaults', '-device', 'VGA', '-display', 'none', '-serial', 'none',
               '-monitor', 'none', '-qmp', 'stdio', '-no-reboot', '-bios', str(firmware),
               '-drive', f'if=none,id=stick,format=raw,snapshot=on,file={disk}',
               '-device', 'qemu-xhci,id=xhci', '-device', 'usb-storage,drive=stick,bootindex=0']
    (output / 'qemu-command.json').write_text(json.dumps(command, indent=2) + '\n')
    before = digest(disk)
    with (output / 'qemu-stderr.log').open('wb') as errors:
        process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                   stderr=errors, bufsize=0, env=dict(os.environ, TMPDIR=str(work)))
        try:
            assert 'QMP' in qmp_read(process)
            qmp_command(process, 'qmp_capabilities')
            wait_screen(process, work, output / 'boot-screen.txt', 'grub rescue>')
            type_keys(process, 'set\n')
            screen = wait_screen(process, work, output / 'prefix-screen.txt',
                                 "root='hd0,msdos1'")
            assert "prefix='(hd0,msdos1)/boot/grub'" in screen
            command = 'ls (hd0,msdos1)/\n' if marker else 'ls (hd0,msdos1)\n'
            type_keys(process, command)
            wait_screen(process, work, output / 'files-screen.txt', marker or 'Filesystem is ' + filesystem)
            qmp_command(process, 'quit')
            assert process.wait(timeout=5) == 0
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=5)
            process.stdin.close()
            process.stdout.close()
    assert digest(disk) == before, 'Snapshot boot changed the backing disk'
    return {'disk_sha256': before, 'firmware_sha256': digest(firmware),
            'prefix': '(hd0,msdos1)/boot/grub', 'root': 'hd0,msdos1',
            'bare_loader': True, 'filesystem_marker': marker}


def check_refusals(probe, disk, work, core_path):
    original = digest(disk)
    for size in (0, SECTOR_BYTES - 1, MIB):
        core = work / 'invalid-core.img'
        core.write_bytes(bytes(size))
        result = subprocess.run([probe, disk, core], capture_output=True, timeout=5)
        assert result.returncode == 1, result.stderr
        assert digest(disk) == original, 'Rejected core changed target'
    # Windows's old-BIOS layout can leave only the traditional 63-sector gap,
    # too small for this 42-KiB core. Restore the test's original partition entry.
    first_lba_offset = 454
    with disk.open('r+b') as stream:
        stream.seek(first_lba_offset)
        saved_lba = stream.read(4)
        stream.seek(first_lba_offset)
        stream.write(struct.pack('<I', 63))
    small_gap = digest(disk)
    result = subprocess.run([probe, disk, core_path], capture_output=True, timeout=5)
    assert result.returncode == 1, result.stderr
    assert digest(disk) == small_gap, 'Rejected old-BIOS gap changed target'
    with disk.open('r+b') as stream:
        stream.seek(first_lba_offset)
        stream.write(saved_lba)
    assert digest(disk) == original
    print('PASS: empty, undersized, oversized and old-BIOS-gap inputs refused unchanged', flush=True)


def qualify(args, work, bits, core_path, core, code):
    output = args.output / f'fat{bits}'
    output.mkdir(exist_ok=True)
    disk = work / f'fat{bits}.img'
    with disk.open('wb') as stream:
        stream.truncate(DISK_BYTES)
    cluster = 2048 if bits == 16 else SECTOR_BYTES
    layout = partition_mbr(args.main_build / 'mbr_image_probe', disk, DISK_BYTES,
                           cluster=cluster, file_system=bits, non_bootable=not args.native,
                           boot_code='grub2' if args.native else None)
    offset, size = layout['OFFSET'], layout['SIZE']
    formatter = argparse.Namespace(probe=args.main_build / 'fat_format_probe',
                                  mkfs=args.mkfs, fat_bits=bits)
    format_fat(formatter, disk, offset, size, cluster, label='GRUB_AUDIT')
    marker = work / MARKER
    marker.write_text('Existing Rufus core.img reads this FAT volume.\n')
    env = dict(os.environ, LC_ALL='C', MTOOLSRC='/dev/null')
    subprocess.run([args.mcopy, '-i', f'{disk}@@{offset}', marker, '::/'],
                   check=True, capture_output=True, timeout=10, env=env)
    # Seed metadata/padding to demonstrate the original MBR writer preserves it.
    with disk.open('r+b') as stream:
        stream.seek(MBR_CODE_BYTES)
        stream.write(b'RUFUSPAD' + struct.pack('<I', 0x78563412))
        stream.seek(446)
        stream.write(b'\x80')
        stream.seek(SECTOR_BYTES)
        stream.write(b'G' * (offset - SECTOR_BYTES))
    before = disk.read_bytes()
    expected = bytearray(before)
    expected[:MBR_CODE_BYTES] = code
    expected[510:SECTOR_BYTES] = b'\x55\xaa'
    expected[SECTOR_BYTES:SECTOR_BYTES + len(core)] = core
    if args.native:
        install_core(args.main_build / 'grub2_core_probe', disk, core_path,
                     capacity=DISK_BYTES, bits=bits, cluster=cluster)
        window = read_window(disk, offset)
        records = expected_records(window, bits, standard=True)
        boot = argparse.Namespace(probe=args.main_build / 'freedos_boot_probe', standard=True)
        install_fat_records(boot, disk, bits, offset, size, cluster, 'GRUB_AUDIT',
                            written=SECTOR_BYTES if bits == 16 else 6 * SECTOR_BYTES)
        expected[offset:offset + len(records)] = records
    else:
        check_refusals(args.probe, disk, work, core_path)
        subprocess.run([args.probe, disk, core_path], check=True, capture_output=True, timeout=5)
    after = disk.read_bytes()
    assert after == expected, 'Write differs from the original MBR/SBR/PBR ranges'
    assert digest(core_path) == CORE_SHA256
    evidence = boot_disk(args, disk, work, output)
    evidence.update({'fat_bits': bits, 'cluster_bytes': cluster, 'offset': offset,
                     'size': size, 'core_bytes': len(core), 'core_sha256': CORE_SHA256,
                     'mbr_writer': 'SetMbrCode' if args.native else 'write_grub2_mbr',
                     'standard_pbr': args.native, 'outside_ranges_unchanged': True})
    (output / 'readback.json').write_text(json.dumps(evidence, indent=2) + '\n')
    disk.unlink()
    print(f'PASS FAT{bits}: original MBR/core -> BIOS USB boot -> prefix/root -> directory read', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('probe', type=Path)
    parser.add_argument('main_build', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--qemu', default='qemu-system-x86_64')
    parser.add_argument('--mkfs', default='mkfs.fat')
    parser.add_argument('--mcopy', default='mcopy')
    parser.add_argument('--native', action='store_true', help='Use CP21b production MBR/SBR/standard PBR adapters')
    args = parser.parse_args()
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    core_path, core, code = check_payloads()
    sources = ['res/grub2/core.img', 'res/grub2/boot.img', 'res/grub2/readme.txt',
               'res/grub2/grub2_version.h', 'src/ms-sys/br.c', 'src/ms-sys/inc/mbr_grub2.h',
               'src/format.c', 'src/rufus.c']
    probes = [args.probe]
    if args.native:
        sources += ['linux/platform/partition_image.c', 'linux/platform/partition_image.h',
                    'linux/platform/fat_boot.c', 'linux/platform/fat_boot.h']
        probes = [args.main_build / name for name in ['mbr_image_probe', 'grub2_core_probe',
                  'freedos_boot_probe', 'fat_format_probe']]
    manifest = {'source_sha256': {name: digest(ROOT / name) for name in sources},
                'qemu': subprocess.check_output([args.qemu, '--version'], text=True).strip(),
                'probe_sha256': {str(path): digest(path) for path in probes},
                'native_adapters': args.native, 'scope': 'bare FAT BIOS boot; no OS/menu qualification'}
    (args.output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    with tempfile.TemporaryDirectory(prefix='rufus-grub2-audit-') as temporary:
        for bits in (16, 32):
            qualify(args, Path(temporary), bits, core_path, core, code)
    assert all(digest(ROOT / name) == value for name, value in manifest['source_sha256'].items())
    print('All generated media removed; compact logs and hashes retained.', flush=True)


if __name__ == '__main__':
    main()
