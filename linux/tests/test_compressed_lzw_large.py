#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Count real Unix-compress output beyond 4 GiB without an expanded image."""
import errno
import pathlib
import subprocess
import sys
import tempfile

from lzw_reference import CodeWriter, codes
from test_compressed_source import probe


def main():
    program = str(pathlib.Path(sys.argv[1]).resolve())
    prefix = bytearray(512)
    prefix[510:512] = b'\x55\xaa'
    writer = CodeWriter()
    for code in codes(prefix):
        writer.emit(code)
    # Reset the dictionary between independent one-MiB segments, preserving
    # one continuous stream. Keep only the short code list and encoded bytes.
    segment = list(codes(bytes(1024 * 1024)))
    for _ in range(4096):
        writer.emit(256)
        for code in segment:
            writer.emit(code)
    packed = writer.finish()
    assert len(packed) < 12 * 1024 * 1024
    print('LZW large: counting 4294967808 actual output bytes; no expanded file', flush=True)
    with tempfile.TemporaryDirectory(prefix='rufus-lzw-large-') as directory:
        path = pathlib.Path(directory) / 'large.Z'
        path.write_bytes(packed)
        independent = subprocess.Popen(['gzip', '-dc', str(path)], stdout=subprocess.PIPE)
        assert independent.stdout.read(512) == prefix
        counted = 512
        zeros = bytes(1024 * 1024)
        while True:
            chunk = independent.stdout.read(len(zeros))
            if not chunk:
                break
            assert chunk == zeros[:len(chunk)]
            counted += len(chunk)
        independent.stdout.close()
        assert independent.wait(timeout=30) == 0 and counted == 2**32 + 512
        print('LZW large: independent gzip content/count passed', flush=True)
        result = probe(program, path, 2**32 + 512, 'count', timeout=600)
        assert int(result['error']) == 0 and int(result['decoded']) == 2**32 + 512, result
        assert int(result['type']) == 2 and int(result['members']) == 1
        assert int(result['trailing']) == 0 and int(result['marker']) == 1
        assert result['prefix'] == prefix.hex()
        result = probe(program, path, 65536, 'count')
        assert int(result['error']) == errno.EFBIG, result
        for name in ('type', 'source', 'decoded', 'members', 'trailing', 'marker'):
            assert int(result[name]) == 0, result
        assert result['prefix'] == result['sha256'] == ''
    print('LZW large: actual 4294967808-byte count and caller-limit refusal passed')


if __name__ == '__main__':
    main()
