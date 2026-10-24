# SPDX-License-Identifier: GPL-3.0-or-later
"""Qualify bounded copy and the unchanged Rufus UEFI:NTFS payload on file GPTs/MBRs."""
import errno
import json
from pathlib import Path
import subprocess
import sys
import tempfile

from test_raw_write import payload, PREPARE, COPY, SYNC, VERIFY, DONE
from test_partition_image import trailing_extent

MIB = 1024 * 1024
DISK_BYTES = 64 * MIB
CLUSTER_BYTES = 4096
TRACK_SECTORS = 63
runs = 0


def invoke(probe, source, target, fault, arguments, error=0):
    global runs
    process = subprocess.run([str(probe), str(source), str(target), fault] + [str(arg) for arg in arguments],
                             capture_output=True, timeout=15)
    runs += 1
    assert process.returncode == 0, (process.returncode, process.stdout, process.stderr)
    report = json.loads(process.stdout)
    assert report['error'] == error, (fault, report, process.stderr)
    assert (report['phase'] == DONE) == (error == 0), report
    return report


def check_extent(probe, root):
    source, target = root / 'source.img', root / 'extent.img'
    data = payload(2 * MIB + 37)
    source.write_bytes(data)
    offset = 511  # Unaligned ordinary-file offsets and multi-buffer transfers.
    padding = 123
    original = b'P' * offset + b'T' * (len(data) + padding) + b'G' * padding
    faults = [('none', 0), ('short', 0), ('eintr', 0), ('write-fail', errno.ENOSPC),
              ('write-cancel', errno.ECANCELED), ('sync-fail', errno.EIO), ('corrupt', errno.EBADMSG),
              ('verify-fail', errno.EIO), ('verify-cancel', errno.ECANCELED)]
    for fault, error in faults:
        target.write_bytes(original)
        report = invoke(probe, source, target, fault, [offset, len(data) + padding], error)
        written = report['written']
        expected = bytearray(original)
        expected[offset:offset+written] = data[:written]
        if fault == 'corrupt':
            expected[offset+23] ^= 1
        assert target.read_bytes() == expected, fault
        assert report['syncs'] >= int(written != 0)
    for start, size, error in [(offset, 0, errno.EINVAL), (offset, len(data)-1, errno.ENOSPC),
                               (len(original)+1, 1, errno.ENOSPC), (offset, 2**64-1, errno.ENOSPC),
                               (2**64-1, len(data), errno.ENOSPC)]:
        target.write_bytes(original)
        report = invoke(probe, source, target, 'none', [start, size], error)
        assert report['written'] == report['syncs'] == 0
        assert target.read_bytes() == original


def check_payload(probe, partition_probe, bundled, root, mbr_probe=None, file_system="ntfs"):
    source, target = root / 'uefi.img', root / 'disk.img'
    data = bundled.read_bytes()
    cluster = 128 * 1024 if file_system == "exfat" else CLUSTER_BYTES
    source.write_bytes(data)
    original = bytearray(b'G' * DISK_BYTES)
    target.write_bytes(original)
    if mbr_probe:
        from test_mbr_image import invoke as prepare_mbr, trailing_extent as mbr_trailing_extent
        # Full preparation also removes the seeded data under both partition prefixes.
        prepare_mbr(mbr_probe, target, DISK_BYTES, cluster, operation="prepare", file_system=file_system,
                    uefi_payload=len(data))
        offset, extent_size = mbr_trailing_extent(DISK_BYTES, len(data))
    else:
        subprocess.run([str(partition_probe), str(target), str(DISK_BYTES), str(cluster),
                        str(TRACK_SECTORS), 'none', str(len(data))], capture_output=True, check=True, timeout=15)
        offset, extent_size = trailing_extent(DISK_BYTES, len(data))
    original = target.read_bytes()
    assert extent_size > len(data)  # Track padding must survive, just as in Windows.
    mode = f'uefi-{file_system}-mbr' if mbr_probe else 'uefi-ntfs'
    arguments = [mode, DISK_BYTES, cluster, TRACK_SECTORS, len(data)]
    faults = [('none', 0, DONE), ('short', 0, DONE), ('eintr', 0, DONE),
              ('cancel-before', errno.ECANCELED, PREPARE), ('readonly', errno.EINVAL, PREPARE),
              ('append', errno.EINVAL, PREPARE), ('block-request', errno.ENOTSUP, PREPARE),
              ('zero-write', errno.EIO, COPY), ('write-fail', errno.ENOSPC, COPY),
              ('write-cancel', errno.ECANCELED, COPY), ('sync-fail', errno.EIO, SYNC),
              ('double-fail', errno.ENOSPC, COPY), ('sync-cancel', errno.ECANCELED, SYNC),
              ('verify-fail', errno.EIO, VERIFY), ('corrupt', errno.EBADMSG, VERIFY),
              ('source-change', errno.ESTALE, SYNC)]
    for fault, error, phase in faults:
        source.write_bytes(data)
        target.write_bytes(original)
        report = invoke(probe, source, target, fault, arguments, error)
        assert report['phase'] == phase, report
        written = report['written']
        expected = bytearray(original)
        expected[offset:offset+written] = data[:written]
        if fault == 'corrupt':
            expected[offset+23] ^= 1
        assert target.read_bytes() == expected, fault
        assert report['syncs'] >= int(written != 0), report
        if not error:
            assert report['written'] == report['verified'] == len(data), report
    source.write_bytes(data)
    damage_cases = [('active', 446), ('boot-type', 466), ('boot-offset', 470), ('unused', 478)] if mbr_probe else [
        ('primary', 512), ('backup', DISK_BYTES-512), ('entries', 1024)]
    for name, damage in damage_cases:
        damaged = bytearray(original)
        damaged[damage] ^= 1
        target.write_bytes(damaged)
        report = invoke(probe, source, target, 'none', arguments, errno.EBADMSG)
        assert report['written'] == report['syncs'] == 0
        assert target.read_bytes() == damaged, name
    for index, value, error in [(1, DISK_BYTES+512, errno.ESTALE), (4, len(data)+512, errno.EINVAL),
                                (4, 0, errno.EINVAL), (3, 32, errno.EBADMSG)]:
        request = arguments.copy()
        request[index] = value
        target.write_bytes(original)
        report = invoke(probe, source, target, 'none', request, error)
        assert report['written'] == report['syncs'] == 0
        assert target.read_bytes() == original
    # A complete payload cannot hide a changed partition table after the write.
    target.write_bytes(original)
    report = invoke(probe, source, target, 'mbr-change' if mbr_probe else 'gpt-change', arguments, errno.EBADMSG)
    assert report['phase'] == VERIFY and report['verified'] == len(data), report
    target.write_bytes(original)
    # Truncation makes payload readback reach EOF before the final size check.
    report = invoke(probe, source, target, 'target-resize', arguments, errno.EIO)
    assert report['written'] == len(data), report
    target.write_bytes(original)
    report = invoke(probe, source, target, 'none', arguments)
    assert target.read_bytes() == original[:offset] + data + original[offset+len(data):]
    if mbr_probe:
        prepare_mbr(mbr_probe, target, DISK_BYTES, cluster, file_system=file_system, non_bootable=True)
    else:
        subprocess.run([str(partition_probe), str(target), str(DISK_BYTES), str(cluster),
                        str(TRACK_SECTORS), 'none', '0'], capture_output=True, check=True, timeout=15)
    single_partition = target.read_bytes()
    report = invoke(probe, source, target, 'none', arguments, errno.EBADMSG)
    assert report['written'] == report['syncs'] == 0 and target.read_bytes() == single_partition


def main():
    if len(sys.argv) not in (4, 5):
        raise SystemExit('usage: test_uefi_ntfs.py RAW_PROBE GPT_PROBE BUNDLED_IMAGE [MBR_PROBE]')
    with tempfile.TemporaryDirectory(prefix='rufus-uefi-ntfs-') as temporary:
        root = Path(temporary)
        if len(sys.argv) == 5:
            for file_system in ("ntfs", "exfat"):
                check_payload(sys.argv[1], sys.argv[2], Path(sys.argv[3]), root, sys.argv[4], file_system)
        else:
            check_extent(sys.argv[1], root)
            check_payload(sys.argv[1], sys.argv[2], Path(sys.argv[3]), root)
    print(f'{runs} bounded image/UEFI:NTFS cases passed; complete outside-extent preservation checked.')


if __name__ == '__main__':
    main()
