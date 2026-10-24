#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Large ISO/UDF fixtures for the authorized NTFS extraction guest."""
import json
from pathlib import Path
import struct
import subprocess

from fat_extract import digest, values
from test_udf_images import rename_udf_entry

MIB = 1024**2
GIB = 1024**3
FILE_BYTES = 4 * GIB + MIB + 123
DISK_BYTES = 6 * GIB
SYSTEM_BYTES = 8 * GIB
SYSTEM_IMAGE_TIMEOUT_SECONDS = 600
GUEST_TIMEOUT_SECONDS = 5400
ISO_PATH = 'sources/LargePayload.bin'
UDF_PATH = 'sources/UdfPayloadxx.bin'


def make_fixture(args, base, entries):
    tree = base / 'LARGE'
    tree.mkdir()
    for name, content in entries.items():
        path = tree / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
    (tree / 'empty-dir/nested').mkdir(parents=True)
    payload = tree / ISO_PATH
    payload.parent.mkdir()
    with payload.open('xb') as output:
        output.truncate(FILE_BYTES)
        # Distinct markers in every MiB detect wrapped/repeated blocks. The
        # final marker also verifies the non-sector-aligned end of the file.
        for offset in range(0, FILE_BYTES, MIB):
            output.seek(offset)
            output.write(struct.pack('<Q', offset) + b'Rufus large file marker\n')
        output.seek(FILE_BYTES - 32)
        output.write(b'Rufus unaligned tail'.ljust(32, b'!'))
    image = base / 'LARGE.iso'
    command = [args.mkisofs, '-quiet', '-R', '-J', '-iso-level', '3',
               '-input-charset', 'utf-8', '-V', 'LARGE']
    if args.large_file == 'udf':
        command.append('-udf')
    with (base / 'large-mkisofs.log').open('wb') as log:
        subprocess.run(command + ['-o', str(image), str(tree)], check=True,
                       stdout=log, stderr=subprocess.STDOUT, timeout=180)
    path = UDF_PATH if args.large_file == 'udf' else ISO_PATH
    if args.large_file == 'udf':
        # Only UDF sees this name. A silent switch to ISO9660 must fail readback.
        # mkisofs places this small tree's directory metadata before file data.
        rename_udf_entry(image, Path(ISO_PATH).name, Path(UDF_PATH).name, scan_bytes=MIB)
    report = {'view': args.large_file, 'path': path, 'size': FILE_BYTES,
              'sha256': digest(payload),
              'copied_bytes': FILE_BYTES + sum(len(data) for name, data in entries.items()
                                               if name != 'ldlinux.sys')}
    check_ranges(args, base, image, payload, path)
    (base / 'large-file.json').write_text(json.dumps(report, indent=2) + '\n')
    return image, report


def check_ranges(args, base, image, payload, path):
    ranges = [(0, 65536), (GIB - 4097, 65536), (2 * GIB - 4097, 65536),
              (4 * GIB - 4097, 65536), (FILE_BYTES - 65536, 65536), (FILE_BYTES, 0)]
    view = 'udf' if args.large_file == 'udf' else 'rr'
    command = [str(args.build / 'iso_file_probe'), str(image), view, '/' + path]
    for offset, count in ranges:
        command += [str(offset), str(count)]
    result = subprocess.run(command, check=True, capture_output=True, timeout=30)
    (base / 'large-ranges.log').write_bytes(result.stderr)
    expected = bytearray()
    with payload.open('rb') as source:
        for offset, count in ranges:
            source.seek(offset)
            expected.extend(source.read(count))
    assert result.stdout == expected, (view, result.stderr)
    report = values(result.stderr.decode())
    assert report['SIZE'] == FILE_BYTES and report['MAX_BLOCKS'] <= 16, report
    if view == 'udf':
        assert report['ISO_READS'] == 0, report


def check_readback(base):
    expected = json.loads((base / 'fixtures/large-file.json').read_text())
    actual = (base / 'large-file.sha256').read_text().split()
    assert actual == [expected['sha256'], './' + expected['path']], actual
    assert int((base / 'large-file.size').read_text()) == expected['size']
    return expected
