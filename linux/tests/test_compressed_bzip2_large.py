#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Count actual BZIP2 output beyond 4 GiB without storing an expanded image."""
import bz2
import errno
import pathlib
import sys
import tempfile

from test_compressed_source import probe


def main():
    program = str(pathlib.Path(sys.argv[1]).resolve())
    prefix = bytearray(512)
    prefix[510:512] = b'\x55\xaa'
    # Independently encode a 64-MiB zero stream in bounded input chunks. The
    # original BZIP2 RLE expands beyond its 900000-entry intermediate buffer.
    encoder = bz2.BZ2Compressor(9)
    packed = bytearray()
    zeros = bytes(1024 * 1024)
    for _ in range(64):
        packed += encoder.compress(zeros)
    packed += encoder.flush()
    assert len(packed) < 4096
    print('BZIP2 large: counting two real 4-GiB decode passes; no expanded file', flush=True)
    with tempfile.TemporaryDirectory(prefix='rufus-bzip2-large-') as directory:
        path = pathlib.Path(directory) / 'large.bz2'
        path.write_bytes(bz2.compress(prefix) + packed * 64)
        assert path.stat().st_size < 256 * 1024
        result = probe(program, path, 2**32 + 512, 'count', timeout=240)
        assert int(result['error']) == 0 and int(result['decoded']) == 2**32 + 512, result
        assert int(result['type']) == 5 and int(result['members']) == 65
        assert int(result['trailing']) == 0 and int(result['marker']) == 1
        assert result['prefix'] == prefix.hex()
        result = probe(program, path, 2**32, 'count', timeout=240)
        assert int(result['error']) == errno.EFBIG, result
        for name in ('type', 'source', 'decoded', 'members', 'trailing', 'marker'):
            assert int(result[name]) == 0, result
        assert result['prefix'] == result['sha256'] == ''
    print('BZIP2 large: actual 4294967808-byte count, 65 members and limit refusal passed')


if __name__ == '__main__':
    main()
