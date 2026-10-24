#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Independent ZIP archives, first-entry semantics, bounds and source contracts."""
import errno
import bz2
import hashlib
import io
import lzma
import pathlib
import random
import struct
import subprocess
import sys
import tempfile
import zipfile
import zlib


def packed_zip(data, method=8, name='disk.img', comment=b'', extra=b''):
    output = io.BytesIO()
    with zipfile.ZipFile(output, 'w', compression=method) as archive:
        info = zipfile.ZipInfo(name)
        info.compress_type = method
        info.extra = extra
        archive.writestr(info, data)
        archive.comment = comment
    return output.getvalue()


def record(data, method=8, name=b'disk.img', flags=None, extra=b'', size_delta=0):
    if flags is None:
        flags = 2 if method == 14 else 0
    if method == 8:
        encoder = zlib.compressobj(wbits=-15)
        packed = encoder.compress(data) + encoder.flush()
    elif method == 12:
        packed = bz2.compress(data)
    elif method == 14:
        encoder = zipfile.LZMACompressor()
        packed = encoder.compress(data) + encoder.flush()
    elif method == 95:
        packed = lzma.compress(data, format=lzma.FORMAT_XZ)
    else:
        packed = data
    crc = zlib.crc32(data)
    size = len(data) + size_delta
    local = struct.pack('<IHHHHHIIIHH', 0x04034b50, 20, flags, method, 0, 0,
                        0 if flags & 8 else crc, 0 if flags & 8 else len(packed),
                        0 if flags & 8 else size, len(name), len(extra)) + name + extra + packed
    if flags & 8:
        local += struct.pack('<IIII', 0x08074b50, crc, len(packed), size)
    return {'local': local, 'name': name, 'flags': flags, 'method': method,
            'crc': crc, 'packed': len(packed), 'size': size, 'attributes': 0}


def archive(rows, order=None, zip64=False):
    body, central = bytearray(), []
    for row in rows:
        offset = len(body)
        body.extend(row['local'])
        extra = struct.pack('<HHQQQ', 1, 24, row['size'], row['packed'], offset) if zip64 else b''
        central.append(struct.pack('<IHHHHHHIIIHHHHHII', 0x02014b50, 45 if zip64 else 20,
                                   45 if zip64 else 20, row['flags'], row['method'], 0, 0,
                                   row['crc'], 0xffffffff if zip64 else row['packed'],
                                   0xffffffff if zip64 else row['size'], len(row['name']),
                                   len(extra), 0, 0, 0, row['attributes'],
                                   0xffffffff if zip64 else offset) + row['name'] + extra)
    directory = b''.join(central[index] for index in (range(len(rows)) if order is None else order))
    start = len(body)
    body.extend(directory)
    if zip64:
        end = len(body)
        body.extend(struct.pack('<IQHHIIQQQQ', 0x06064b50, 44, 45, 45, 0, 0,
                                len(rows), len(rows), len(directory), start))
        body.extend(struct.pack('<IIQI', 0x07064b50, 0, end, 1))
    end_count = min(len(rows), 0xffff) if zip64 else len(rows)
    body.extend(struct.pack('<IHHHHIIH', 0x06054b50, 0, 0, end_count, end_count,
                            len(directory), 0xffffffff if zip64 else start, 0))
    return bytes(body)


def main():
    program, contract = (str(pathlib.Path(arg).resolve()) for arg in sys.argv[1:3])
    cases = 0
    rng = random.Random(29)
    with tempfile.TemporaryDirectory(prefix='rufus-zip-source-') as temporary:
        directory = pathlib.Path(temporary)
        path = directory / 'image ü.ZIP'

        def run(content, maximum):
            path.write_bytes(content)
            before = path.stat()
            result = subprocess.run([program, str(maximum), 'hash', str(path)], text=True,
                                    capture_output=True, check=True, timeout=30)
            after = path.stat()
            assert (before.st_ino, before.st_size, before.st_mtime_ns, before.st_ctime_ns) == (
                after.st_ino, after.st_size, after.st_mtime_ns, after.st_ctime_ns)
            return dict(line.split(' ', 1) for line in result.stdout.splitlines())

        def check(content, data, maximum=None, independent=True, members=1):
            nonlocal cases
            with zipfile.ZipFile(io.BytesIO(content)) as oracle:
                first = oracle.infolist()[0]
                if independent:
                    if first.compress_type == 95:
                        decoder = lzma.LZMADecompressor(format=lzma.FORMAT_XZ)
                        filename_bytes, extra_bytes = struct.unpack_from('<HH', content, first.header_offset + 26)
                        start = first.header_offset + 30 + filename_bytes + extra_bytes
                        assert decoder.decompress(content[start:start + first.compress_size]) == data
                        assert decoder.eof
                    else:
                        assert oracle.read(first) == data
            local_name, local_extra = struct.unpack_from('<HH', content, first.header_offset + 26)
            data_offset = first.header_offset + 30 + local_name + local_extra
            consumed = first.compress_size
            if first.compress_type == 8:
                decoder = zlib.decompressobj(-15)
                assert decoder.decompress(content[data_offset:data_offset + first.compress_size]) == data
                assert decoder.eof
                consumed -= len(decoder.unused_data)
            elif first.compress_type == 95:
                decoder = lzma.LZMADecompressor(format=lzma.FORMAT_XZ)
                assert decoder.decompress(content[data_offset:data_offset + first.compress_size]) == data
                assert decoder.eof
                consumed -= len(decoder.unused_data)
            result = run(content, max(1, len(data)) if maximum is None else maximum)
            assert int(result['error']) == 0, result
            assert int(result['type']) == 1 and int(result['source']) == len(content)
            assert int(result['decoded']) == len(data) and int(result['members']) == members
            assert result['prefix'] == data[:512].hex()
            assert result['sha256'] == hashlib.sha256(data).hexdigest()
            marker = len(data) >= 512 and data[510:512] == b'\x55\xaa'
            assert int(result['marker']) == marker
            assert int(result['forced']) == (1 if marker else 2 if len(data) >= 512 else 0)
            trailing = int(result['trailing'])
            expected_tail = len(content) - data_offset - consumed
            if first.compress_type == 14:
                # The retained original range decoder can stop before the
                # encoder's last zero byte after its EOS marker.
                unused = trailing - expected_tail
                assert 0 <= unused <= 5
                assert content[data_offset + consumed - unused:data_offset + consumed] == bytes(unused)
            else:
                assert trailing == expected_tail
            cases += 1

        def reject(content, error=errno.EBADMSG, maximum=8 * 1024**3):
            nonlocal cases
            result = run(content, maximum)
            assert int(result['error']) == error, (error, result)
            assert all(int(result[name]) == 0 for name in ('type', 'source', 'decoded', 'members', 'trailing', 'marker', 'forced'))
            assert result['prefix'] == result['sha256'] == ''
            cases += 1

        for method in (0, 8, 12, 14):
            for size in (0, 1, 511, 512, 513, 65535, 65536, 65537, 196645):
                data = bytearray(rng.randbytes(size))
                if size >= 512:
                    data[510:512] = b'\x55\xaa'
                check(packed_zip(data, method), data)
            data = rng.randbytes(8193)
            for name in ('disk.img', '../disk.img', '/disk.img', 'a' * 4095, 'dísk.img'):
                check(packed_zip(data, method, name=name), data)
            reject(packed_zip(data, method, name='a' * 4096))
            for length in (1, 4096, 65000, 65514):
                check(packed_zip(data, method, comment=b'c' * length), data)
            # Original end-record scan reads 52 bytes from this incomplete
            # appended ZIP64 signature. The native scan keeps the complete CDE.
            check(packed_zip(data, method) + b'PK\x06\x06' + bytes(16), data)
            reject(packed_zip(data, method, comment=b'c' * 65515))
            for extra in (struct.pack('<HH', 0x1234, 0), struct.pack('<HH', 0x5455, 5) + b'\x01' + bytes(4),
                          struct.pack('<HH', 0x1234, 65531) + bytes(65531)):
                check(packed_zip(data, method, extra=extra), data)
            descriptor_flags = 8 | (2 if method == 14 else 0)
            check(archive([record(data, method, flags=descriptor_flags)]), data)
            check(archive([record(data, method)], zip64=True), data)
            if method == 8:
                for tail in (b'ignored', zlib.compress(b'later data')):
                    row = record(data)
                    row['packed'] += len(tail)
                    local = bytearray(row['local'])
                    struct.pack_into('<I', local, 18, row['packed'])
                    row['local'] = bytes(local) + tail
                    check(archive([row]), data)
            first, second = record(data, method), record(b'B' * 4096, method, b'chosen.img')
            check(archive([first, second], [1, 0]), b'B' * 4096)
            folder = record(b'', 0, b'folder/')
            folder['attributes'] = 0x10
            check(archive([folder, first]), b'')
            check(packed_zip(packed_zip(data), 0), packed_zip(data))
            # A bad stored CRC was ignored by the original copy helper. Native
            # publication must validate it, just as deflate already does.
            base = bytearray(packed_zip(data, method))
            base[14] ^= 1
            reject(base)
            for maximum in (1, 511, 512, len(data) - 1):
                reject(packed_zip(data, method), errno.EFBIG, maximum)
            reject(archive([record(data, method, flags=1)]), errno.ENOTSUP)
            for delta in (-1, 1):
                content = archive([record(data, method, size_delta=delta)])
                if method in (8, 12, 14):
                    # Compressed methods publish actual CRC-valid output even
                    # when declared length differs. Freeze that actual count.
                    check(content, data, independent=False)
                else:
                    reject(content)
            # ZIP64 local fields can contain either one or both 64-bit sizes.
            for sentinels in ((True, False), (False, True), (True, True)):
                row = record(data, method)
                fields = (struct.pack('<Q', len(data)) if sentinels[0] else b'') + (
                    struct.pack('<Q', row['packed']) if sentinels[1] else b'')
                extra = struct.pack('<HH', 1, len(fields)) + fields
                local = bytearray(row['local'][:30])
                struct.pack_into('<I', local, 18, 0xffffffff if sentinels[1] else row['packed'])
                struct.pack_into('<I', local, 22, 0xffffffff if sentinels[0] else len(data))
                struct.pack_into('<H', local, 28, len(extra))
                row['local'] = bytes(local) + row['name'] + extra + row['local'][30 + len(row['name']):]
                check(archive([row]), data)
            for extra in (b'x', b'xx', b'xxx', b'\x34\x12\xff\xff', b'\x01\x00\x00\x00' * 2):
                reject(archive([record(data, method, extra=extra)]))
        data = rng.randbytes(8193)
        valid = packed_zip(data)
        central = valid.index(b'PK\x01\x02')
        for cut in (*range(30), 38, central - 1, central, central + 4, len(valid) - 22, len(valid) - 1):
            reject(valid[:cut])
        for offset in (0, central, central + 42):
            bad = bytearray(valid)
            bad[offset] ^= 0x80
            reject(bad)
        for offset in (len(valid) - 6, central + 42):
            bad = bytearray(valid)
            struct.pack_into('<I', bad, offset, 0xfffffffe)
            reject(bad)
        bad = bytearray(valid)
        struct.pack_into('<I', bad, 18, len(valid))
        reject(bad)
        versioned = bytearray(packed_zip(data, 14))
        versioned[30 + len(b'disk.img')] = 4
        check(versioned, data)
        bad = bytearray(packed_zip(data, 14))
        bad[30 + len(b'disk.img') + 2] = 6
        reject(bad, errno.ENOTSUP)
        # The ZIP specification also permits method 14 without EOS when flag
        # bit 1 is clear. Remove the final marker bytes after verifying that
        # Python's independent LZMA1 decoder still produces the full payload.
        no_eos_data = b'abc' * 3000
        no_eos = record(no_eos_data, 14, flags=0)
        no_eos['local'] = no_eos['local'][:-6]
        no_eos['packed'] -= 6
        local = bytearray(no_eos['local'])
        struct.pack_into('<I', local, 18, no_eos['packed'])
        no_eos['local'] = bytes(local)
        decoder = zipfile.LZMADecompressor()
        assert decoder.decompress(no_eos['local'][30 + len(no_eos['name']):]) == no_eos_data
        assert not decoder.eof
        check(archive([no_eos]), no_eos_data, independent=False)
        incomplete = record(no_eos_data, 14, flags=0)
        incomplete['local'] = incomplete['local'][:-7]
        incomplete['packed'] -= 7
        local = bytearray(incomplete['local'])
        struct.pack_into('<I', local, 18, incomplete['packed'])
        incomplete['local'] = bytes(local)
        reject(archive([incomplete]))
        for method in (9, 99):
            reject(archive([record(data, method)]), errno.ENOTSUP)
        first_data, second_data = rng.randbytes(8193), rng.randbytes(4097)
        joined = first_data + second_data
        streams = bz2.compress(first_data) + bz2.compress(second_data)
        assert bz2.decompress(streams) == joined
        joined_row = record(joined, 12)
        joined_row['local'] = joined_row['local'][:30 + len(joined_row['name'])] + streams
        joined_row['packed'] = len(streams)
        local = bytearray(joined_row['local'])
        struct.pack_into('<I', local, 18, joined_row['packed'])
        joined_row['local'] = bytes(local)
        check(archive([joined_row]), joined, independent=False, members=2)
        for size in (0, 1, 511, 512, 513, 65535, 65536, 65537, 196645):
            data = bytearray(rng.randbytes(size))
            if size >= 512:
                data[510:512] = b'\x55\xaa'
            check(archive([record(data, 95)]), data)
        data = rng.randbytes(8193)
        for mode in (lzma.CHECK_NONE, lzma.CHECK_CRC32, lzma.CHECK_CRC64, lzma.CHECK_SHA256):
            row = record(data, 95)
            packed = lzma.compress(data, format=lzma.FORMAT_XZ, check=mode)
            row['local'] = row['local'][:30 + len(row['name'])] + packed
            row['packed'] = len(packed)
            local = bytearray(row['local'])
            struct.pack_into('<I', local, 18, len(packed))
            row['local'] = bytes(local)
            check(archive([row]), data)
        check(archive([record(data, 95)], zip64=True), data)
        check(archive([record(data, 95, flags=8)]), data)
        first, chosen = record(data, 95), record(b'B' * 4096, 95, b'chosen.img')
        check(archive([first, chosen], [1, 0]), b'B' * 4096)
        row = record(data, 95)
        row['packed'] += 7
        local = bytearray(row['local'])
        struct.pack_into('<I', local, 18, row['packed'])
        row['local'] = bytes(local) + b'ignored'
        check(archive([row]), data)
        for delta in (-1, 1):
            check(archive([record(data, 95, size_delta=delta)]), data)
        for maximum in (1, 511, len(data) - 1):
            reject(archive([record(data, 95)]), errno.EFBIG, maximum)
        bad = bytearray(archive([record(data, 95)]))
        bad[14] ^= 1
        reject(bad)
        truncated = record(data, 95)
        truncated['local'] = truncated['local'][:-8]
        truncated['packed'] -= 8
        reject(archive([truncated]))
        bad = bytearray(valid)
        struct.pack_into('<H', bad, len(valid) - 18, 1)
        reject(bad, errno.ENOTSUP)
        # Deterministic incompressible contracts share the existing ownership,
        # allocation, cancellation, I/O, consumer and concurrent-reader harness.
        state = 27
        raw = bytearray()
        for _ in range(3 * 65536 + 37):
            state ^= (state << 13) & 0xffffffff
            state ^= state >> 17
            state ^= (state << 5) & 0xffffffff
            raw.append(state & 0xff)
        raw[510:512] = b'\x55\xaa'
        for method in (0, 8, 12, 14, 95):
            fixture = directory / f'contract-{method}.zip'
            fixture.write_bytes(archive([record(raw, method)]) if method == 95 else packed_zip(raw, method))
            result = subprocess.run([contract, str(fixture)], text=True, capture_output=True, timeout=30)
            assert result.returncode == 0, result.stdout + result.stderr
            print(result.stdout.strip(), flush=True)
        raw = bytes((i * 37 + i // 251) & 255 for i in range(3 * 65536 + 37))
        raw = raw[:510] + b'\x55\xaa' + raw[512:]
        fixture = directory / 'contract-repeated.zip'
        fixture.write_bytes(packed_zip(raw))
        result = subprocess.run([contract, str(fixture), '--zip-repeated'], text=True, capture_output=True, timeout=30)
        assert result.returncode == 0, result.stdout + result.stderr
        print(result.stdout.strip(), flush=True)
        print(f'ZIP source: {cases} independent archive/selection/integrity/bounds cases passed; methods0/8/12/14/95', flush=True)


if __name__ == '__main__':
    main()
