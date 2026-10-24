#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Grouped partial-FFU validation, sparse update and malformed-source corpus."""
import errno
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zlib

from ffu_fixture import BLOCK, make_ffu
from test_compressed_ffu import check
from test_image_save import gpt_image


def write_case(writer, root, packed, initial, expected, error=0, sector=512, fault='none'):
    source, target = root / 'update.ffu', root / 'target.img'
    source.write_bytes(packed)
    target.write_bytes(initial)
    run = subprocess.run([writer, str(source), str(target), str(sector), '0', str(len(initial)), fault],
                         check=True, capture_output=True, text=True, timeout=30)
    report = json.loads(run.stdout)
    assert report['error'] == error, (report, run.stderr)
    assert source.read_bytes() == packed
    assert target.read_bytes() == expected, (report, run.stderr)
    if error == 0:
        assert report['written'] == report['verified'] != 0 and report['phase'] == 4, report
        assert report['syncs'] == (3 if fault == 'eintr' else 1), report
    else:
        assert report['written'] == report['verified'] == report['syncs'] == 0, report
        assert report['phase'] == 0, report
    return report


def metadata_hashes(image, offsets):
    for index, start in enumerate(range(offsets['image'], offsets['data'], BLOCK)):
        image[32 + 32 * index:32 + 32 * (index + 1)] = hashlib.sha256(image[start:start + BLOCK]).digest()


def main():
    source_probe, writer = sys.argv[1:]
    cases = []
    with tempfile.TemporaryDirectory(prefix='rufus-ffu-partial-') as temporary:
        root = Path(temporary)
        base = bytes(range(256)) * (8 * BLOCK // 256)
        # Both comparisons overlap bytes replaced by the update. Readback must
        # verify new bytes rather than demand that these old bytes still exist.
        payloads = [(b'P' * BLOCK, [(0, 0)]), (b'Q' * BLOCK, [(0, 2)]), (b'R' * BLOCK, [(2, 0)])]
        for sector in (512, 4096):
            for version, algorithm in ((1, 0), (2, 0), (3, 0), (3, 1), (3, 2), (3, 3), (3, 4)):
                validations = [(0, 7, base[7:519]), (2 * BLOCK // sector, 11, base[2 * BLOCK + 11:3 * BLOCK + 27])]
                packed, logical, offsets = make_ffu(payloads, partial=True, validations=validations,
                                                   version=version, algorithm=algorithm, sector=sector)
                check(source_probe, root / 'parse.ffu', packed, logical)
                expected = bytearray(base)
                for start, data in ((0, b'P' * BLOCK), (2 * BLOCK, b'Q' * BLOCK), (7 * BLOCK, b'R' * BLOCK)):
                    expected[start:start + len(data)] = data
                report = write_case(writer, root, packed, base, bytes(expected), sector=sector)
                assert report['written'] == 3 * BLOCK, report
                cases.append(f'partial V{version}/{algorithm} sector {sector}: overlap, multibatch compare, sparse preservation, readback')
                wrong = bytearray(base); wrong[2 * BLOCK + 11] ^= 1
                write_case(writer, root, packed, bytes(wrong), bytes(wrong), errno.ESTALE, sector)
                cases.append(f'partial V{version}/{algorithm} sector {sector}: late base mismatch, no writes')
                # End-relative data goes to the physical target end. Omitted
                # bytes, including its middle and old tail, retain their base.
                larger = base + b'T' * (2 * BLOCK)
                expected_larger = bytearray(larger)
                expected_larger[:BLOCK] = b'P' * BLOCK
                expected_larger[2 * BLOCK:3 * BLOCK] = b'Q' * BLOCK
                expected_larger[-BLOCK:] = b'R' * BLOCK
                write_case(writer, root, packed, larger, bytes(expected_larger), sector=sector)
                cases.append(f'partial V{version}/{algorithm} sector {sector}: larger-target end-relative placement')
        # A real partial update can omit the MBR entirely. FFU recognition on
        # Windows recognizes a validated FFU container without an MBR marker.
        payloads = [(b'U' * BLOCK, [(0, 4)])]
        packed, logical, offsets = make_ffu(payloads, partial=True, validations=[(0, 0, base[:512])])
        expected = bytearray(base); expected[4 * BLOCK:5 * BLOCK] = b'U' * BLOCK
        write_case(writer, root, packed, base, bytes(expected))
        cases.append('partial with no MBR payload or boot marker')
        for fault, error in (('base-read-fail', errno.EIO), ('base-eof', errno.EIO),
                             ('base-cancel', errno.ECANCELED), ('base-source-change', errno.ESTALE)):
            write_case(writer, root, packed, base, base, error, fault=fault)
            cases.append(f'base validation {fault}, no writes')
        for fault in ('eintr', 'short'):
            write_case(writer, root, packed, base, bytes(expected), fault=fault)
            cases.append(f'base validation {fault} I/O completes exactly')
        write_case(writer, root, packed, base, base + bytes(512), errno.ESTALE, fault='base-target-resize')
        cases.append('target capacity changes during base validation, no writes')
        packed_empty, unused, unused_offsets = make_ffu(payloads, partial=True)
        write_case(writer, root, packed_empty, base, bytes(expected))
        cases.append('partial with no declared constraints')
        full, unused, unused_offsets = make_ffu(payloads)
        write_case(writer, root, full, base, bytes(expected))
        cases.append('full validated FFU without an MBR marker, matching Windows dispatch')
        # Partial GPT bytes can already describe the destination capacity;
        # the manifest minimum is not a captured geometry to relocate again.
        gpt = gpt_image(size=16 * 1024 * 1024)
        changed = bytearray(gpt)
        for start in (512, len(gpt) - 512):
            changed[start + 56:start + 72] = bytes(range(16))
            struct.pack_into('<I', changed, start + 16, 0)
            struct.pack_into('<I', changed, start + 16, zlib.crc32(changed[start:start + 92]))
        gpt_update, unused, unused_offsets = make_ffu(
            [(bytes(changed[:BLOCK]), [(0, 0)]), (bytes(changed[-BLOCK:]), [(2, 0)])],
            disk_bytes=8 * 1024 * 1024, partial=True, validations=[(1, 0, gpt[512:1024])])
        write_case(writer, root, gpt_update, gpt, bytes(changed))
        cases.append('partial preserves declared GPT updates at a larger existing base capacity')
        # Count/length/offset mutations are independently rehashed so each case
        # exercises the validation parser rather than only the metadata checksum.
        for label, where, fmt, value in (
                ('validation count exceeds region', offsets['store'] + 216, '<I', 2),
                ('validation missing length', offsets['store'] + 220, '<I', 0),
                ('validation trailing byte', offsets['store'] + 220, '<I', 525),
                ('validation data exceeds region', offsets['validation'] + 8, '<I', 513),
                ('validation sector outside manifest', offsets['validation'], '<I', 0xffffffff),
                ('validation byte offset outside manifest', offsets['validation'] + 4, '<I', 0xffffffff),
                ('full update with base validations', offsets['store'], '<I', 0)):
            bad = packed.copy(); struct.pack_into(fmt, bad, where, value)
            metadata_hashes(bad, offsets)
            check(source_probe, root / 'bad.ffu', bad, expected=errno.EBADMSG)
            write_case(writer, root, bad, base, base, errno.EBADMSG)
            cases.append(label)
        stale = packed.copy(); stale[offsets['validation'] + 12] ^= 1
        check(source_probe, root / 'bad.ffu', stale, expected=errno.EBADMSG)
        write_case(writer, root, stale, base, base, errno.EBADMSG)
        cases.append('validation byte integrity failure before writes')
    print(json.dumps({'cases': cases, 'count': len(cases), 'temporary_media_removed': True}, indent=2))


if __name__ == '__main__':
    main()
