#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Count actual LZMA output beyond 4 GiB without storing an expanded image."""
import errno
import lzma
import pathlib
import struct
import sys
import tempfile

from test_compressed_source import probe


def main():
    program = str(pathlib.Path(sys.argv[1]).resolve())
    prefix = bytearray(512)
    prefix[510:512] = b'\x55\xaa'
    print('LZMA large: encoding/counting actual >4-GiB output; no expanded file', flush=True)
    encoder = lzma.LZMACompressor(format=lzma.FORMAT_ALONE,
                                 filters=[{'id': lzma.FILTER_LZMA1, 'dict_size': 4096,
                                           'mode': lzma.MODE_FAST, 'mf': lzma.MF_HC3}])
    zeros = bytes(1024 * 1024)
    with tempfile.TemporaryDirectory(prefix='rufus-lzma-large-') as directory:
        path = pathlib.Path(directory) / 'large.lzma'
        with path.open('wb') as file:
            file.write(encoder.compress(prefix))
            for _ in range(4096):
                file.write(encoder.compress(zeros))
            file.write(encoder.flush())
        assert path.stat().st_size < 1024 * 1024
        result = probe(program, path, 2**32 + 512, 'count', timeout=300)
        assert int(result['error']) == 0 and int(result['decoded']) == 2**32 + 512, result
        assert int(result['type']) == 4 and int(result['members']) == 1
        assert 0 <= int(result['trailing']) <= 1 and int(result['marker']) == 1
        assert result['prefix'] == prefix.hex()
        # A known-size header above 4 GiB remains a promise rather than an
        # output-count shortcut. Actually decode until the separate 4-GiB limit.
        with path.open('r+b') as file:
            file.seek(5)
            file.write(struct.pack('<Q', 2**32 + 512))
        result = probe(program, path, 2**32, 'count', timeout=300)
        assert int(result['error']) == errno.EFBIG, result
        for name in ('type', 'source', 'decoded', 'members', 'trailing', 'marker'):
            assert int(result[name]) == 0, result
        assert result['prefix'] == result['sha256'] == ''
    print('LZMA large: actual 4294967808-byte count and known-size 4-GiB limit refusal passed')


if __name__ == '__main__':
    main()
