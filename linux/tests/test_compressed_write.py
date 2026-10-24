#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Independent compressed streams, regular-file readback and destructive-error boundaries."""
import bz2
import errno
import gzip
import hashlib
import json
import lzma
import os
from pathlib import Path
import random
import struct
import subprocess
import sys
import tempfile

from lzw_reference import encode as encode_lzw, encode_clear_tail, encode_literals

PREPARE, COPY, SYNC, VERIFY, DONE = range(5)
TRANSFER = 65536
LIMIT = 2**63 - 1
runs = 0
xz_mode = False
bzip2_mode = False
lzma_mode = False
lzw_mode = False
lzw_gzip_mode = False
zstd_mode = False
extension = "gz"


def compress(data, check=lzma.CHECK_CRC64):
    if zstd_mode:
        return subprocess.run(['zstd', '-q', '-c'], input=data, capture_output=True, check=True).stdout
    if lzw_mode:
        return encode_lzw(data)
    if xz_mode:
        return lzma.compress(data, check=check, filters=[{'id': lzma.FILTER_LZMA2, 'dict_size': 65536}])
    if bzip2_mode:
        return bz2.compress(data)
    if lzma_mode:
        return lzma.compress(data, format=lzma.FORMAT_ALONE,
                             filters=[{'id': lzma.FILTER_LZMA1, 'dict_size': 65536}])
    return gzip.compress(data, mtime=0)


def invoke(program, arguments):
    global runs
    result = subprocess.run([program, *map(str, arguments)], capture_output=True, text=True, timeout=20)
    runs += 1
    assert result.returncode == 0, (arguments, result.returncode, result.stderr)
    return json.loads(result.stdout)


def geometry(program):
    for sector in (512, 1024, 2048, 4096, 8192, 16384, 32768, 65536):
        maximum_aligned = LIMIT // sector * sector
        for size in (1, 511, 512, 513, 65535, 65536, 65537, 2**32 + 512,
                     maximum_aligned - 1, maximum_aligned, maximum_aligned + 1, LIMIT, 2**64 - 1):
            padded = (size + sector - 1) // sector * sector
            report = invoke(program, ("geometry", size, sector))
            assert report == {"error": errno.EOVERFLOW if padded > LIMIT else 0,
                              "size": 0 if padded > LIMIT else padded}, (size, sector, report)
    for size, sector in ((0, 512), (512, 0), (512, 1), (512, 256), (512, 513),
                         (512, 65535), (512, 65537), (512, 2**31)):
        assert invoke(program, ("geometry", size, sector)) == {"error": errno.EINVAL, "size": 0}


def run(program, source, target, sector=512, force=False, maximum=LIMIT, fault="none"):
    report = invoke(program, (source, target, sector, int(force), maximum, fault))
    assert (report["phase"] == DONE) == (report["error"] == 0), (fault, report)
    assert report["written"] <= report["padded"] and report["verified"] <= report["written"], report
    return report


def payload(size, marker=True):
    data = bytearray(random.Random(size + 17).randbytes(size))
    if size >= 512:
        data[510:512] = b"\x55\xaa" if marker else b"\0\0"
    return bytes(data)


def check_write(program, source, target, data, sector=512, packed=None, force=False, fault="none", tail=b""):
    packed = compress(data) if packed is None else packed
    source.write_bytes(packed)
    before = source.stat()
    padded = data + bytes(-len(data) % sector)
    target.write_bytes(b"\xa5" * len(padded) + tail)
    report = run(program, source, target, sector, force, fault=fault)
    assert report["error"] == report["sync_error"] == 0, report
    assert report["decoded"] == len(data) and report["padded"] == len(padded), report
    assert report["written"] == report["verified"] == len(padded), report
    assert report["syncs"] == (3 if fault == "eintr" else 1), report
    assert report["plan_sha256"] == hashlib.sha256(data).hexdigest(), report
    assert report["compression"] == (9 if zstd_mode else 6 if xz_mode else 5 if bzip2_mode else 4 if lzma_mode else 2 if lzw_mode and not lzw_gzip_mode else 3), report
    assert target.read_bytes() == padded + tail
    assert source.read_bytes() == packed
    after = source.stat()
    assert (before.st_ino, before.st_size, before.st_mtime_ns, before.st_ctime_ns) == (
        after.st_ino, after.st_size, after.st_mtime_ns, after.st_ctime_ns)
    if fault == "parallel":
        assert report["parallel_sha256"] == hashlib.sha256(packed).hexdigest(), report
    return report


def successes(program, root):
    source, target = root / f"source ü.{extension.upper()}", root / "target.img"
    for sector in (512, 4096, 65536):
        for size in (512, 513, 4095, 4096, 65535, 65536, 65537, 3 * TRANSFER + 37):
            for tail in (b"", b"preserved target tail" * 7):
                check_write(program, source, target, payload(size), sector, tail=tail)
    data = payload(65537)
    if xz_mode or lzma_mode:
        # Windows stops at the first stream; preserve padding/other streams/tail.
        for tail in (b"ignored trailing data", bytes(4), compress(bytes(4096))):
            check_write(program, source, target, data, 4096, packed=compress(data) + tail)
        if xz_mode:
            for kind in (lzma.CHECK_NONE, lzma.CHECK_CRC32, lzma.CHECK_SHA256):
                for sector in (512, 4096, 65536):
                    check_write(program, source, target, data, sector, packed=compress(data, kind))
        else:
            for dictionary in (4096, 65536, 1024 * 1024):
                packed = lzma.compress(data, format=lzma.FORMAT_ALONE,
                                       filters=[{'id': lzma.FILTER_LZMA1, 'dict_size': dictionary}])
                check_write(program, source, target, data, 4096, packed=packed)
                known = packed[:5] + struct.pack('<Q', len(data)) + packed[13:]
                report = check_write(program, source, target, data, 4096, packed=known)
                assert report['members'] == 1 and report['trailing'] > 0, report
                report = check_write(program, source, target, data, 4096, packed=known[:-report['trailing']])
                assert report['trailing'] == 0, report
            maximum_dictionary = known[:1] + struct.pack('<I', 2**30) + known[5:]
            check_write(program, source, target, data, 65536, packed=maximum_dictionary)
    elif zstd_mode:
        members = [b"", data[:128], data[128:512], b"", data[512:], b""]
        check_write(program, source, target, data, 4096,
                    packed=b"".join(compress(member) for member in members))
        check_write(program, source, target, data, 512,
                    packed=b"".join(compress(data[i:i + 113]) for i in range(0, len(data), 113)))
    elif lzw_mode:
        for bits in range(9, 17):
            for block in (True, False):
                for sector in (512, 4096, 65536):
                    check_write(program, source, target, data, sector, packed=encode_lzw(data, bits, block))
        # Exercise real CLEAR resets and long KwKwK strings inside the writer's
        # repeated decodes, not concatenated .Z headers or a fictitious tail.
        for data in (payload(3 * TRANSFER + 37), b'A' * 510 + b'\x55\xaa' + b'A' * 100000):
            check_write(program, source, target, data, packed=encode_clear_tail(data))
        data = payload(513)
        report = check_write(program, source, target, data[:512], packed=encode_literals(data, 9)[:-1])
        assert report['members'] == 1 and report['trailing'] == 0
        packed = encode_literals(data[:512], 9)
        check_write(program, source, target, data[:512], packed=packed + b'\x7f')
        check_write(program, source, target, data[:512] + b'\0', packed=packed + bytes(2))
    else:
        members = [b"", data[:128], data[128:512], b"", data[512:], b""]
        stream = b"".join(compress(member) for member in members)
        check_write(program, source, target, data, 4096, packed=stream + b"ignored trailing data")
        # Tiny members cross both physical sectors and our batched I/O boundary.
        stream = b"".join(bz2.compress(data[i:i + 113], compresslevel=1) if bzip2_mode
                          else compress(data[i:i + 113]) for i in range(0, len(data), 113))
        report = check_write(program, source, target, data, 512, packed=stream)
        assert report["writes"] == report["target_reads"] == 2, report
    if bzip2_mode:
        boundary = bytearray(random.Random(26).randbytes(262181))
        boundary[510:512] = b"\x55\xaa"
        first = bz2.compress(boundary[:64890])
        assert len(first) == 65535
        check_write(program, source, target, boundary, 4096, packed=first + bz2.compress(boundary[64890:]))
    check_write(program, source, target, payload(8193, marker=False), 4096, force=True)
    for fault in ("short", "eintr", "parallel"):
        check_write(program, source, target, payload(3 * TRANSFER + 37), fault=fault)


def source_and_target_preflight(program, root):
    source, target = root / ("source." + extension), root / "target.img"
    data = payload(513)
    packed = compress(data)
    original = b"\xa5" * 4096
    cases = [(b"", False, LIMIT, errno.ENOTSUP), (payload(511), True, LIMIT, errno.ENOTSUP),
             (payload(512, False), False, LIMIT, errno.ENOTSUP), (data, False, 512, errno.EFBIG),
             (data, False, 0, errno.EINVAL), (data, False, 2**63, errno.EINVAL)]
    for image, force, maximum, expected in cases:
        source.write_bytes(compress(image))
        target.write_bytes(original)
        report = run(program, source, target, force=force, maximum=maximum)
        assert report["error"] == expected and report["phase"] == PREPARE, report
        assert report["written"] == report["syncs"] == report["decoded"] == report["padded"] == 0, report
        assert target.read_bytes() == original
    damaged = bytearray(packed)
    if xz_mode:
        from test_compressed_xz import block_checks
        damaged[block_checks(packed, 8)[0]] ^= 1
        wrong_header = b"invalid!" + packed[8:]
        invalid_streams = (bytes(damaged), packed[:-1], wrong_header)
    elif lzma_mode:
        invalid_streams = (packed[:-8], bytes([225]) + packed[1:],
                           packed[:5] + struct.pack('<Q', len(data) + 1) + packed[13:],
                           struct.pack('<BIQ', 93, 4096, 1) + b'\0\xc0\0\0\0' + bytes(32),
                           compress(bytes(20000))[:5] + struct.pack('<Q', 20001) + compress(bytes(20000))[13:])
    elif zstd_mode:
        damaged[-1] ^= 1  # XXH64 trailer on a checked Zstd frame.
        invalid_streams = (bytes(damaged), packed[:-1], packed + b"garbage", b"\0" + packed)
    elif lzw_mode:
        invalid_streams = (b'\x1f\x9d', packed[:2] + b'\x91' + packed[3:],
                           b'\x1f\x9d\x90\xff\x01')
        late = bytearray(encode_clear_tail(data))
        late[-2] |= 0xf0
        late[-1] |= 0x1f
        invalid_streams += (bytes(late),)
    elif bzip2_mode:
        damaged[10] ^= 1  # Actual first block CRC after BZh and block magic.
        combined = bytearray(packed)
        combined[-3] ^= 1  # Entirely inside the final 32-bit combined CRC.
        empty = bytearray(bz2.compress(b""))
        empty[-1] ^= 1
        invalid_streams = (bytes(damaged), bytes(combined), packed[:-1], packed + b"BZ",
                           bytes(empty), bytes(empty) + packed, packed + empty, packed + empty + packed)
    else:
        damaged[-8] ^= 1
        invalid_streams = (bytes(damaged), packed[:-1], packed + b"\x1f\x8b")
    for stream in invalid_streams:
        source.write_bytes(stream)
        target.write_bytes(original)
        report = run(program, source, target)
        assert report["error"] == errno.EBADMSG and report["written"] == report["syncs"] == 0, report
        assert target.read_bytes() == original
    if lzw_mode and not lzw_gzip_mode:
        # The same payload named .gz uses original signature dispatch and
        # retains its filename association throughout the frozen plan.
        alias = source.with_suffix('.gz')
        alias.write_bytes(packed)
        target.write_bytes(original)
        report = run(program, alias, target)
        assert report['error'] == 0 and report['compression'] == 3, report
        assert report['written'] == report['verified'] == 1024
        assert target.read_bytes() == data + bytes(511) + original[1024:]
    source.write_bytes(packed)
    # Source payload fits in 513 bytes, but its final sector needs 1024 bytes.
    target.write_bytes(original[:513])
    report = run(program, source, target)
    assert report["error"] == errno.ENOSPC and report["written"] == report["syncs"] == 0, report
    assert target.read_bytes() == original[:513]
    for fault in ("readonly", "writeonly", "append", "direct", "path"):
        target.write_bytes(original)
        report = run(program, source, target, fault=fault)
        assert report["error"] == errno.EINVAL and report["written"] == report["syncs"] == 0, (fault, report)
        assert target.read_bytes() == original
    report = run(program, source, source)
    assert report["error"] == errno.EINVAL and source.read_bytes() == packed, report
    target.unlink()
    os.link(source, target)
    report = run(program, source, target)
    assert report["error"] == errno.EINVAL and source.read_bytes() == packed, report
    target.unlink()
    os.mkfifo(target)
    report = run(program, source, target, fault="fifo")
    assert report["error"] == errno.ENOTSUP and report["written"] == report["syncs"] == 0, report
    target.unlink()


def errors_and_frozen_plans(program, root):
    source, target = root / ("source." + extension), root / "target.img"
    data = payload(3 * TRANSFER + 37)
    # BZIP2 decodes a complete BWT block before producing it. Separate members
    # require fresh source I/O after each committed 64-KiB target transfer.
    packed = (b"".join(bz2.compress(data[i:i + TRANSFER], compresslevel=1)
                       for i in range(0, len(data), TRANSFER)) if bzip2_mode else compress(data))
    if lzw_mode:
        packed = encode_clear_tail(data)
    padded = data + bytes(-len(data) % 512)
    original = b"\xa5" * (len(padded) + 147)
    before_write = [(name, errno.ESTALE) for name in ("plan-hash", "plan-size-small", "plan-size-large", "plan-source",
                    "plan-tail", "plan-members", "plan-prefix", "plan-compression", "source-before-change")]
    before_write += [(name, errno.EINVAL) for name in ("plan-marker", "plan-sector", "plan-write-size", "plan-prefix-size", "plan-compression-unsupported")]
    before_write += [("preflight-read", errno.EIO), ("preflight-corrupt", errno.EBADMSG),
                     ("preflight-cancel", errno.ECANCELED), ("cancel-before", errno.ECANCELED)]
    for fault, expected in before_write:
        source.write_bytes(packed)
        target.write_bytes(original)
        report = run(program, source, target, fault=fault)
        assert report["error"] == expected and report["phase"] == PREPARE, (fault, report)
        assert report["written"] == report["verified"] == report["syncs"] == 0, (fault, report)
        assert target.read_bytes() == original
    cases = (("copy-read-before", errno.EIO, COPY, 0, 0),
             ("source-copy-change", errno.ESTALE, COPY, 0, 0),
             ("read-fail", errno.EIO, COPY, TRANSFER, 0),
             ("early-eof", errno.EIO, COPY, TRANSFER, 0),
             ("read-cancel", errno.ECANCELED, COPY, TRANSFER, 0),
             ("copy-corrupt", errno.EBADMSG, COPY, 3 * TRANSFER, 0),
             ("zero-write", errno.EIO, COPY, 0, 0),
             ("write-fail", errno.ENOSPC, COPY, 17, 0),
             ("write-cancel", errno.ECANCELED, COPY, 17, 0),
             ("cancel-sync-fail", errno.ECANCELED, COPY, 17, 0),
             ("double-fail", errno.ENOSPC, COPY, 17, 0),
             ("final-zero-write", errno.EIO, COPY, 3 * TRANSFER, 0),
             ("final-write-fail", errno.ENOSPC, COPY, 3 * TRANSFER + 17, 0),
             ("cancel-last-write", errno.ECANCELED, SYNC, len(padded), 0),
             ("sync-fail", errno.EIO, SYNC, len(padded), 0),
             ("sync-cancel", errno.ECANCELED, SYNC, len(padded), 0),
             ("source-sync-change", errno.ESTALE, SYNC, len(padded), 0),
             ("target-sync-resize", errno.ESTALE, SYNC, len(padded), 0),
             ("verify-fail", errno.EIO, VERIFY, len(padded), 0),
             ("verify-eof", errno.EIO, VERIFY, len(padded), 0),
             ("verify-cancel", errno.ECANCELED, VERIFY, len(padded), TRANSFER),
             ("corrupt", errno.EBADMSG, VERIFY, len(padded), 0),
             ("padding-corrupt", errno.EBADMSG, VERIFY, len(padded), 3 * TRANSFER),
             ("source-verify-change", errno.ESTALE, VERIFY, len(padded), 3 * TRANSFER),
             ("target-verify-resize", errno.ESTALE, VERIFY, len(padded), len(padded)),
             ("target-verify-append", errno.EINVAL, VERIFY, len(padded), len(padded)))
    for fault, expected, phase, written, verified in cases:
        source.write_bytes(packed)
        target.write_bytes(original)
        report = run(program, source, target, fault=fault)
        assert report["error"] == expected and report["phase"] == phase, (fault, report)
        assert report["written"] == written and report["verified"] == verified, (fault, report)
        assert report["syncs"] == int(written != 0), (fault, report)
        assert report["sync_error"] == (errno.EIO if fault in ("sync-fail", "double-fail", "cancel-sync-fail") else 0), (fault, report)
        expected_disk = bytearray(padded[:written] + original[written:])
        if fault == "corrupt":
            expected_disk[23] ^= 1
        if fault == "padding-corrupt":
            expected_disk[len(data)] ^= 1
        if fault in ("target-sync-resize", "target-verify-resize"):
            expected_disk += bytes(512)
        assert target.read_bytes() == expected_disk, (fault, report)
        assert source.read_bytes() == packed
    baseline = check_write(program, source, target, data, packed=packed)
    for index in range(1, baseline["allocations"] + 1):
        source.write_bytes(packed)
        target.write_bytes(original)
        report = run(program, source, target, fault=f"alloc-{index}")
        assert report["error"] == errno.ENOMEM, report
        assert report["syncs"] == int(report["written"] != 0) and report["sync_error"] == 0, report
        assert target.read_bytes() == padded[:report["written"]] + original[report["written"]:], report


def decoded_hash_substitution(program, root):
    source, target = root / ('source.' + extension), root / 'target.img'
    data = payload(3 * TRANSFER + 37)
    packed = encode_literals(data) if lzw_mode else compress(data)
    # Keep every frozen stream fact identical. Only decoded bytes outside the
    # first sector (and therefore their SHA256) change, with no format checksum.
    changed = bytearray(data)
    if lzw_mode:
        changed[65550] ^= 1
        alternate = encode_literals(changed)
        assert len(alternate) == len(packed)
    else:
        for value in range(1, 256):
            changed[65550] = data[65550] ^ value
            alternate = compress(changed)
            if len(alternate) == len(packed):
                break
        else:
            raise AssertionError('No equal-length independent LZMA substitution fixture')
    replacement = Path(str(source) + '.replacement.' + extension)
    replacement.write_bytes(alternate)
    if lzw_mode:
        independent = subprocess.run(['gzip', '-dc'], input=alternate, capture_output=True, check=True)
        assert independent.stdout == changed
    else:
        assert lzma.decompress(alternate, format=lzma.FORMAT_ALONE) == changed
    padded = data + bytes(-len(data) % 512)
    original = b'\xa5' * (len(padded) + 147)
    for fault, error, phase, written, verified in (
            ('preflight-substitute', errno.ESTALE, PREPARE, 0, 0),
            ('copy-substitute', errno.ESTALE, COPY, 3 * TRANSFER, 0),
            ('verify-substitute', errno.EBADMSG, VERIFY, len(padded), TRANSFER)):
        source.write_bytes(packed)
        target.write_bytes(original)
        report = run(program, source, target, fault=fault)
        assert (report['error'], report['phase'], report['written'], report['verified']) == (
            error, phase, written, verified), (fault, report)
        assert report['syncs'] == int(written != 0) and report['sync_error'] == 0, report
        expected = bytes(changed) if fault == 'copy-substitute' else padded
        assert target.read_bytes() == expected[:written] + original[written:], (fault, report)
        assert source.read_bytes() == packed and replacement.read_bytes() == alternate


def main():
    global xz_mode, bzip2_mode, lzma_mode, lzw_mode, lzw_gzip_mode, zstd_mode, extension
    xz_mode = len(sys.argv) == 3 and sys.argv[2] == "--xz"
    bzip2_mode = len(sys.argv) == 3 and sys.argv[2] == "--bzip2"
    lzma_mode = len(sys.argv) == 3 and sys.argv[2] == "--lzma"
    lzw_gzip_mode = len(sys.argv) == 3 and sys.argv[2] == "--lzw-gzip"
    lzw_mode = lzw_gzip_mode or len(sys.argv) == 3 and sys.argv[2] == "--lzw"
    zstd_mode = len(sys.argv) == 3 and sys.argv[2] == "--zstd"
    assert len(sys.argv) == (3 if xz_mode or bzip2_mode or lzma_mode or lzw_mode or zstd_mode else 2)
    extension = "zst" if zstd_mode else "xz" if xz_mode else "bz2" if bzip2_mode else "lzma" if lzma_mode else "Z" if lzw_mode and not lzw_gzip_mode else "gz"
    program = str(Path(sys.argv[1]).resolve())
    if not xz_mode and not bzip2_mode and not lzma_mode and not lzw_mode and not zstd_mode:
        geometry(program)
    with tempfile.TemporaryDirectory(prefix="rufus-compressed-write-") as directory:
        root = Path(directory)
        successes(program, root)
        source_and_target_preflight(program, root)
        errors_and_frozen_plans(program, root)
        if lzma_mode or lzw_mode:
            decoded_hash_substitution(program, root)
            print(f"{'LZW' if lzw_mode else 'LZMA'} hash substitutions: identical size/prefix/member/tail facts; preflight/copy SHA and verify readback refusals passed")
    print(f"{'Zstd writer' if zstd_mode else 'XZ writer' if xz_mode else 'BZIP2 writer' if bzip2_mode else 'LZMA writer' if lzma_mode else 'LZW writer' if lzw_mode else 'Compressed writer'}: {runs} geometry/plan/write/flush/readback cases passed; disposable regular files only")


if __name__ == "__main__":
    main()
