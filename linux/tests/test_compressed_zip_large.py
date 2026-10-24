#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Real ZIP64 >4-GiB decode/counting, without an expanded image file."""
import errno
import pathlib
import struct
import subprocess
import sys
import tempfile
import zipfile
import zlib


def main():
    program = str(pathlib.Path(sys.argv[1]).resolve())
    size = 2**32 + 512
    prefix = bytes(510) + b'\x55\xaa'
    zeros = bytes(1024 * 1024)
    name = b'disk.img'
    extra = struct.pack('<HHQ', 1, 8, size)
    with tempfile.TemporaryDirectory(prefix='rufus-zip64-large-') as temporary:
        path = pathlib.Path(temporary) / 'disk.zip'
        encoder = zlib.compressobj(level=1, wbits=-15)
        crc = zlib.crc32(prefix)
        with path.open('w+b') as output:
            header_size = 30 + len(name) + len(extra)
            output.write(bytes(header_size))
            output.write(encoder.compress(prefix))
            for _ in range(4096):
                output.write(encoder.compress(zeros))
                crc = zlib.crc32(zeros, crc)
            output.write(encoder.flush())
            compressed = output.tell() - header_size
            directory = output.tell()
            output.write(struct.pack('<IHHHHHHIIIHHHHHII', 0x02014b50, 45, 45, 0, 8, 0, 0,
                                     crc, compressed, 0xffffffff, len(name), len(extra), 0, 0, 0, 0, 0) + name + extra)
            directory_size = output.tell() - directory
            output.write(struct.pack('<IHHHHIIH', 0x06054b50, 0, 0, 1, 1, directory_size, directory, 0))
            output.seek(0)
            output.write(struct.pack('<IHHHHHIIIHH', 0x04034b50, 45, 0, 8, 0, 0, crc,
                                     compressed, 0xffffffff, len(name), len(extra)) + name + extra)
        with zipfile.ZipFile(path) as independent:
            info = independent.infolist()[0]
            assert info.file_size == size and info.CRC == crc
            with independent.open(info) as source:
                assert source.read(512) == prefix
                count = 512
                while data := source.read(len(zeros)):
                    assert data == zeros[:len(data)]
                    count += len(data)
                assert count == size
        for maximum, expected in ((size, 0), (2**32, errno.EFBIG)):
            result = subprocess.run([program, str(maximum), 'count', str(path)], text=True,
                                    capture_output=True, check=True, timeout=180)
            row = dict(line.split(' ', 1) for line in result.stdout.splitlines())
            assert int(row['error']) == expected, row
            if expected == 0:
                assert int(row['decoded']) == size and int(row['type']) == 1
                assert row['prefix'] == prefix.hex() and int(row['marker']) == 1
                assert int(row['members']) == 1 and int(row['source']) == path.stat().st_size
            else:
                assert all(int(row[key]) == 0 for key in ('type', 'source', 'decoded', 'members', 'trailing', 'marker', 'forced'))
                assert row['prefix'] == row['sha256'] == ''
        print(f'ZIP64 large: independent complete content/CRC and native actual {size}-byte count/prefix/limit passed; no expanded image retained')


if __name__ == '__main__':
    main()
