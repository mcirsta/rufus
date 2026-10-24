# SPDX-License-Identifier: GPL-3.0-or-later
"""Bounded original GRUB SBR writes; all targets are disposable regular files."""
import argparse
import errno
import hashlib
from pathlib import Path
import re
import subprocess
import tempfile

from test_mbr_image import inspect_mbr, invoke as partition_mbr

MIB = 1024 * 1024
SECTOR = 512
DISK_SIZE = 4 * MIB
CORE_OFFSET = SECTOR
PARTITION_OFFSET = MIB
BOOT_CODES = ['grub2', 'rufus', 'win7', 'rufus-masquerade']
provider = "grub2"
runs = 0


def invoke(probe, image, core, capacity=DISK_SIZE, bits=32, cluster=512,
           fault='none', error=0, written=None, boot_code='grub2'):
    global runs
    if written is None:
        written = core.stat().st_size - (SECTOR if provider == "grub4dos" else 0) if error == 0 else 0
    command = [probe, image, core, str(capacity), str(bits), str(cluster), fault, boot_code]
    if provider == "grub4dos":
        command.append(provider)
    process = subprocess.run(command, capture_output=True, timeout=10)
    assert process.returncode == 0, (process.returncode, process.stdout, process.stderr)
    values = {key.decode(): int(value) for key, value in re.findall(rb'([A-Z_]+)=(-?\d+)', process.stdout)}
    assert values['ERROR'] == error and values['BYTES'] == written, (boot_code, bits, fault, values, process.stderr)
    expected_position = 0 if image == Path('/dev/null') else 17
    assert values['FDS'] == 0 and values['OFFSET'] == expected_position and values['FLAGS'] == 1, values
    assert values['SYNCS'] == (2 if fault == 'sync-eintr' else int(written != 0)), values
    assert values['SYNC'] == (errno.ENOSPC if 'sync-fail' in fault else 0), values
    runs += 1
    return values


def check_case(args, root, bits, boot_code):
    disk = root / 'grub.img'
    disk.write_bytes(b'G' * DISK_SIZE)
    # New disk with no existing table. Leave the embedding gap and data seeded.
    with disk.open('r+b') as stream:
        stream.write(bytes(SECTOR))
    partition_mbr(args.mbr_probe, disk, DISK_SIZE, file_system=bits, boot_code=boot_code)
    inspect_mbr(disk, DISK_SIZE, file_system=bits, boot_code=boot_code)
    # Original GRUB writers leave some bytes outside their bootstrap. Windows
    # records use all 440 bytes; seeding that range would corrupt their code.
    if boot_code in ('grub2', 'grub4dos'):
        with disk.open('r+b') as stream:
            stream.seek(423 if boot_code == 'grub4dos' else 432)
            stream.write(b'RUFUSPAD')
    before = disk.read_bytes()
    core = root / 'core.img'
    resource = args.core.read_bytes()
    original = resource[SECTOR:] if args.grub4dos else resource
    core.write_bytes(resource)
    faults = [('none', 0, len(original)), ('short-write', 0, len(original)),
              ('short-read', 0, len(original)), ('write-eintr', 0, len(original)),
              ('read-eintr', 0, len(original)), ('sync-eintr', 0, len(original)),
              ('read-fail', errno.EIO, 0), ('write-fail', errno.EIO, 0), ('write-zero', errno.EIO, 0),
              ('partial-fail', errno.EIO, 19), ('partial-sync-fail', errno.EIO, 19),
              ('sync-fail', errno.ENOSPC, len(original)), ('verify-fail', errno.EIO, len(original)),
              ('verify-eof', errno.EIO, len(original)), ('verify-corrupt', errno.EBADMSG, len(original)),
              ('cancel-before', errno.ECANCELED, 0), ('cancel-partial', errno.ECANCELED, 19),
              ('cancel-sync', errno.ECANCELED, len(original)), ('resize', errno.ESTALE, len(original)),
              ('mbr-changed', errno.EBADMSG, len(original))]
    for fault, error, written in faults:
        disk.write_bytes(before)
        invoke(args.probe, disk, core, bits=bits, fault=fault, error=error, written=written,
               boot_code=boot_code)
        expected = bytearray(before)
        expected[CORE_OFFSET:CORE_OFFSET + written] = original[:written]
        if fault == 'resize':
            expected.extend(bytes(SECTOR))
        if fault == 'mbr-changed':
            expected[454] ^= 1
        assert disk.read_bytes() == expected, fault
        assert core.read_bytes() == resource, fault

    if args.grub4dos:
        # Windows rejects a resource with no bytes beyond its MBR sector.
        for size in [0, SECTOR - 1, SECTOR]:
            core.write_bytes(bytes(size))
            disk.write_bytes(before)
            invoke(args.probe, disk, core, bits=bits, error=errno.EINVAL, boot_code=boot_code)
            assert disk.read_bytes() == before, size
        core.write_bytes(resource)
        if boot_code in ('rufus', 'win7'):
            # WinPE publishes the original masqueraded MBR after private unmount.
            # Reuse that operation and prove it preserves the independent SBR.
            disk.write_bytes(before)
            invoke(args.probe, disk, core, bits=bits, boot_code=boot_code)
            installed = disk.read_bytes()
            partition_mbr(args.mbr_probe, disk, DISK_SIZE, file_system=bits,
                          boot_code='rufus-masquerade')
            inspect_mbr(disk, DISK_SIZE, file_system=bits, boot_code='rufus-masquerade')
            assert disk.read_bytes()[SECTOR:] == installed[SECTOR:]
            assert disk.read_bytes()[CORE_OFFSET:CORE_OFFSET + len(original)] == original

    for fault, error in [('readonly', errno.EINVAL), ('append', errno.EINVAL), ('direct', errno.EINVAL),
                         ('null-options', errno.EINVAL), ('null-result', errno.EINVAL), ('null-core', errno.EINVAL),
                         ('overflow', errno.ENOSPC), ('wrong-id', errno.EBADMSG), ('wrong-code', errno.EBADMSG),
                         ('block-request', errno.ENOTSUP), ('sector', errno.ENOTSUP)]:
        disk.write_bytes(before)
        invoke(args.probe, disk, core, bits=bits, fault=fault, error=error, boot_code=boot_code)
        assert disk.read_bytes() == before, fault
    for capacity in [DISK_SIZE - SECTOR, DISK_SIZE + SECTOR]:
        invoke(args.probe, disk, core, capacity=capacity, bits=bits, error=errno.ESTALE,
               boot_code=boot_code)
        assert disk.read_bytes() == before

    # Corrupt the code/table before entry; no core bytes may be written.
    for position in [100, 440, 444, 446, 450, 454, 458, 470, 510]:
        damaged = bytearray(before)
        damaged[position] ^= 1
        disk.write_bytes(damaged)
        invoke(args.probe, disk, core, bits=bits, error=errno.EBADMSG, boot_code=boot_code)
        assert disk.read_bytes() == damaged, position

    # Accepting multiple record choices must not weaken the selected-record
    # check. Rufus and its masqueraded variant differ only in the active byte.
    disk.write_bytes(before)
    for other in BOOT_CODES:
        if other == boot_code:
            continue
        invoke(args.probe, disk, core, bits=bits, error=errno.EBADMSG, boot_code=other)
        assert disk.read_bytes() == before, (boot_code, other)

    # Exact embedding-gap boundary and multiple transfer chunks. These arbitrary
    # bytes test placement only, not GRUB compatibility or bootability.
    gap = PARTITION_OFFSET - CORE_OFFSET
    minimum = 1 if args.grub4dos else SECTOR
    for size in [0, 1, SECTOR - 1, SECTOR, 2 * 64 * 1024 + 1, gap, gap + 1]:
        payload = bytes(index % 251 for index in range(size))
        core.write_bytes(b'P' * SECTOR + payload if args.grub4dos else payload)
        disk.write_bytes(before)
        error = errno.EINVAL if size < minimum else errno.ENOSPC if size > gap else 0
        invoke(args.probe, disk, core, bits=bits, error=error, boot_code=boot_code)
        expected = bytearray(before)
        if error == 0:
            expected[CORE_OFFSET:CORE_OFFSET + size] = payload
        assert disk.read_bytes() == expected, size
        if size != 2 * 64 * 1024 + 1:
            continue
        for fault, error, written in [('cancel-chunk', errno.ECANCELED, 64 * 1024),
                                      ('verify-late-corrupt', errno.EBADMSG, size)]:
            disk.write_bytes(before)
            invoke(args.probe, disk, core, bits=bits, fault=fault, error=error, written=written,
                   boot_code=boot_code)
            expected = bytearray(before)
            expected[CORE_OFFSET:CORE_OFFSET + written] = payload[:written]
            assert disk.read_bytes() == expected, fault


def check_alternate_mbr(args, root, bits):
    disk = root / 'unsupported.img'
    opposite = 'grub2' if args.grub4dos else 'grub4dos'
    for boot_code in ['syslinux', 'zero', opposite]:
        disk.write_bytes(bytes(SECTOR) + b'G' * (DISK_SIZE - SECTOR))
        partition_mbr(args.mbr_probe, disk, DISK_SIZE, file_system=bits,
                      non_bootable=boot_code == 'zero',
                      boot_code=None if boot_code == 'zero' else boot_code)
        before = disk.read_bytes()
        # ReactOS may retain its initial Syslinux MBR for an independent GRUB
        # SBR. Other providers and a nonbootable data MBR remain unsupported.
        supported = boot_code == 'syslinux'
        invoke(args.probe, disk, args.core, bits=bits, error=0 if supported else errno.ENOTSUP,
               boot_code=boot_code)
        expected = bytearray(before)
        if supported:
            payload = args.core.read_bytes()[SECTOR:] if args.grub4dos else args.core.read_bytes()
            expected[CORE_OFFSET:CORE_OFFSET + len(payload)] = payload
        assert disk.read_bytes() == expected, boot_code


def main():
    global provider, BOOT_CODES
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('probe', type=Path)
    parser.add_argument('mbr_probe', type=Path)
    parser.add_argument('core', type=Path)
    parser.add_argument('--grub4dos', action='store_true')
    args = parser.parse_args()
    if args.grub4dos:
        provider = "grub4dos"
        BOOT_CODES = ["grub4dos", "rufus", "win7", "rufus-masquerade"]
    original = hashlib.sha256(args.core.read_bytes()).digest()
    with tempfile.TemporaryDirectory(prefix='rufus-grub2-core-') as temporary:
        for bits in [16, 32, 'ntfs']:
            for boot_code in BOOT_CODES:
                check_case(args, Path(temporary), bits, boot_code)
            check_alternate_mbr(args, Path(temporary), bits)
        invoke(args.probe, Path('/dev/null'), args.core, error=errno.ENOTSUP,
               boot_code=provider)
        if args.grub4dos:
            for file_system in ['exfat', 'ext2', 'ext3']:
                partition_mbr(args.mbr_probe, Path(temporary) / 'absent', 256 * MIB,
                              operation='plan', file_system=file_system, boot_code=provider,
                              error=errno.ENOTSUP, started=False)
    assert hashlib.sha256(args.core.read_bytes()).digest() == original
    print(f'{runs} {provider} SBR write/refusal/fault cases passed; fixtures removed.')


if __name__ == '__main__':
    main()
