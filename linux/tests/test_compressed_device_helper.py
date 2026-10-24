#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Real helper/job/stream engine with link-only selection/kernel facts on owned files."""
import bz2
import errno
import gzip
import hashlib
import json
import lzma
import os
from pathlib import Path
import random
import resource
import selectors
import struct
import subprocess
import sys
import tempfile
import time

from lzw_reference import encode as encode_lzw, encode_clear_tail, encode_literals
from test_compressed_zip import archive as zip_archive, record as zip_record

PREPARE, COPY, SYNC, VERIFY, DONE = range(5)
DEVICE = os.makedev(240, 42)
runs = 0
xz_mode = False
bzip2_mode = False
lzma_mode = False
lzw_mode = False
lzw_gzip_mode = False
zip_mode = False
zstd_mode = False
extension = "gz"


def timestamp(value):
    return f"{value // 10**9}:{value % 10**9}"


def request(root, data, packed, sector, capacity, force):
    source = root / ("source." + extension)
    source.write_bytes(packed)
    info = source.stat()
    return {"operation": "compressed-write-device-v1", "path": str(root / "disk.bin"),
            "device": str(DEVICE), "size": str(capacity), "sequence": "19",
            "object": "/org/freedesktop/UDisks2/block_devices/fixture", "owner": ":1.42", "identity": ["fixture"],
            "source": {"path": str(source), "device": str(info.st_dev), "inode": str(info.st_ino),
                       "size": str(info.st_size), "modified": timestamp(info.st_mtime_ns), "changed": timestamp(info.st_ctime_ns),
                       "compression": {"type": 9 if zstd_mode else 1 if zip_mode else 6 if xz_mode else 5 if bzip2_mode else 4 if lzma_mode else 2 if lzw_mode and not lzw_gzip_mode else 3, "decoded": str(len(data)), "sector": sector,
                                       "sha256": hashlib.sha256(data).hexdigest(), "ignoreMarker": force}}}


def run(program, root, packet, mode, sector, decoded):
    global runs
    env = os.environ.copy()
    caller = os.getuid() + 1 if mode == "source-owner" else os.getuid()
    env.update(PKEXEC_UID=str(caller), RUFUS_TEST_COMPRESSED_ROOT=str(root), RUFUS_TEST_COMPRESSED_MODE=mode,
               RUFUS_TEST_COMPRESSED_SECTOR=str(sector), RUFUS_TEST_COMPRESSED_DECODED=str(decoded))
    process = subprocess.Popen([program, "--device"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, env=env, bufsize=0)
    buffers = {"stdout": b"", "stderr": b""}
    frames, diagnostics = [], []
    sent = cancelled = False
    selector = selectors.DefaultSelector()
    selector.register(process.stdout, selectors.EVENT_READ, "stdout")
    selector.register(process.stderr, selectors.EVENT_READ, "stderr")
    deadline = time.monotonic() + 20
    try:
        while selector.get_map():
            assert time.monotonic() < deadline, (mode, frames, diagnostics)
            for key, _ in selector.select(0.1):
                kind = key.data
                chunk = os.read(key.fileobj.fileno(), 8192)
                if not chunk:
                    selector.unregister(key.fileobj)
                    continue
                buffers[kind] += chunk
                while b"\n" in buffers[kind]:
                    line, buffers[kind] = buffers[kind].split(b"\n", 1)
                    if kind == "stderr":
                        diagnostics.append(line.decode(errors="replace"))
                        continue
                    frame = json.loads(line)
                    frames.append(frame)
                    if frame.get("event") == "ready":
                        assert not sent, frames
                        process.stdin.write(json.dumps(packet, separators=(",", ":")).encode() + b"\n")
                        process.stdin.flush()
                        sent = True
                    if frame.get("event") == "raw-progress" and mode.startswith("cancel") and not cancelled:
                        copying = frame["phase"] == COPY and int(frame["processed"]) > 0
                        syncing = frame["phase"] == SYNC
                        if (mode == "cancel-sync" and syncing) or (mode != "cancel-sync" and copying):
                            process.stdin.close()
                            cancelled = True
        process.wait(timeout=5)
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
        if not process.stdin.closed:
            process.stdin.close()
        selector.close()
    runs += 1
    results = [frame for frame in frames if frame.get("event") == "raw-result"]
    assert len(results) == 1, (mode, process.returncode, frames, diagnostics)
    result = results[0]
    assert process.returncode == (0 if result["error"] == 0 else 1), (mode, result, diagnostics)
    assert sum(frame == {"event": "phase", "phase": "checking"} for frame in frames) == 1, frames
    progress = [frame for frame in frames if frame.get("event") == "raw-progress"]
    padded = (decoded + sector - 1) // sector * sector
    for frame in progress:
        assert COPY <= frame["phase"] <= VERIFY, (mode, frame)  # No PREPARE leakage.
        expected = 0 if frame["phase"] == SYNC else padded
        assert int(frame["total"]) == expected and int(frame["processed"]) <= expected, (mode, frame)
    if mode.startswith("cancel"):
        assert cancelled, (mode, frames, diagnostics)
    return result, diagnostics


def check(program, root, mode="success", sector=512, data=None, force=False, capacity=1024 * 1024, check_kind=lzma.CHECK_CRC64, zip_method=8):
    if data is None:
        data = bytearray(random.Random(17).randbytes(3 * 65536 + 37))
        data[510:512] = b"\x55\xaa"
        data = bytes(data)
    if mode == "legacy-raw":
        packed = data
    elif zip_mode:
        packed = zip_archive([zip_record(data, zip_method)])
    elif zstd_mode:
        packed = subprocess.run(["zstd", "-q", "-c"], input=data, capture_output=True, check=True).stdout
    elif xz_mode:
        packed = lzma.compress(data, check=check_kind, filters=[{'id': lzma.FILTER_LZMA2, 'dict_size': 65536}])
    elif bzip2_mode:
        packed = bz2.compress(data)
    elif lzma_mode:
        dictionary = 4096 if mode == 'dictionary-4096' else 65536
        packed = lzma.compress(data, format=lzma.FORMAT_ALONE,
                               filters=[{'id': lzma.FILTER_LZMA1, 'dict_size': dictionary}])
    elif lzw_mode:
        bits = 9 if mode == 'nine-bit' else 16
        packed = encode_lzw(data, bits, mode != 'non-block')
    else:
        packed = gzip.compress(data, mtime=0)
    if mode == "trailing-stream":
        packed += lzma.compress(bytes(4096))
    if mode == "bad-crc":
        damaged = bytearray(packed)
        if zip_mode:
            assert damaged[:4] == b"PK\x03\x04"
            damaged[14] ^= 1
        elif xz_mode:
            from test_compressed_xz import block_checks
            size = {lzma.CHECK_CRC32: 4, lzma.CHECK_CRC64: 8, lzma.CHECK_SHA256: 32}[check_kind]
            damaged[block_checks(packed, size)[0]] ^= 1
        elif zstd_mode:
            damaged[-1] ^= 1
        elif bzip2_mode:
            damaged[10] ^= 1
        else:
            damaged[-8] ^= 1
        packed = bytes(damaged)
    if mode == "bad-combined-crc":
        damaged = bytearray(packed)
        damaged[-3] ^= 1
        packed = bytes(damaged)
    if mode.startswith("empty-crc-"):
        empty = bytearray(bz2.compress(b""))
        empty[-1] ^= 1
        prefix = packed if mode in {"empty-crc-last", "empty-crc-middle"} else b""
        suffix = packed if mode in {"empty-crc-first", "empty-crc-middle"} else b""
        packed = prefix + empty + suffix
    if mode == "concatenated":
        if zstd_mode:
            packed = b"".join(subprocess.run(["zstd", "-q", "-c"], input=member,
                                             capture_output=True, check=True).stdout
                              for member in (b"", data[:123], data[123:], b""))
        else:
            packed = bz2.compress(b"") + bz2.compress(data[:123], compresslevel=1) + bz2.compress(data[123:]) + bz2.compress(b"")
    if mode == "bad-tail":
        packed += b"not a Zstd frame"
    if mode == "trailing-data":
        packed += b"ignored non-BZ tail"
    if lzma_mode:
        if mode in {'known-size', 'known-size-no-eos', 'bad-size', 'large-known-size'}:
            size = len(data)
            if mode == 'bad-size':
                size += 1
            if mode == 'large-known-size':
                size = 2**32 + 512
            packed = packed[:5] + struct.pack('<Q', size) + packed[13:]
        if mode == 'known-size-no-eos':
            packed = packed[:-5]
            try:
                lzma.decompress(packed, format=lzma.FORMAT_ALONE)
            except lzma.LZMAError:
                pass
            else:
                raise AssertionError('Known-prefix fixture must lack complete EOS')
        if mode == 'truncated-eos':
            packed = packed[:-8]
        if mode == 'invalid-history':
            packed = struct.pack('<BIQ', 93, 4096, 1) + b'\0\xc0\0\0\0' + bytes(32)
        if mode == 'known-size-overshoot':
            packed = lzma.compress(bytes(20000), format=lzma.FORMAT_ALONE,
                                   filters=[{'id': lzma.FILTER_LZMA1, 'dict_size': 4096}])
            packed = packed[:5] + struct.pack('<Q', 4097) + packed[13:]
        if mode == 'unsupported-dictionary':
            packed = packed[:1] + struct.pack('<I', 2**30 + 1) + packed[5:]
        if mode == 'second-lzma-stream':
            packed += lzma.compress(bytes(4096), format=lzma.FORMAT_ALONE)
    if lzw_mode:
        if mode in {'clear-tail', 'late-bad-code', 'shorter-stream'}:
            packed = encode_clear_tail(data)
        if mode == 'late-bad-code':
            changed = bytearray(packed)
            changed[-2] |= 0xf0
            changed[-1] |= 0x1f
            packed = bytes(changed)
        if mode == 'bad-code':
            packed = b'\x1f\x9d\x90\xff\x01'
        if mode == 'bad-bits':
            packed = packed[:2] + b'\x91' + packed[3:]
        if mode == 'short-header':
            packed = packed[:2]
        if mode == 'shorter-stream':
            packed = packed[:-1]
        if mode == 'incomplete-code':
            # The final eight bits cannot complete another nine-bit code.
            packed = encode_literals(data, 9) + b'\x7f'
    packet = request(root, data, packed, sector, capacity, force)
    wire = packet["source"]["compression"]
    if mode == "legacy-raw":
        packet["operation"] = "raw-write-device-v1"
        del packet["source"]["compression"]
    if mode == "bad-sha": wire["sha256"] = "00" * 32
    if mode == "small-decoded": wire["decoded"] = str(len(data) - 1)
    if mode == "large-decoded": wire["decoded"] = str(len(data) + 1)
    if mode == "wrong-inode": packet["source"]["inode"] = str(int(packet["source"]["inode"]) + 1)
    if mode == "wrong-timestamp": packet["source"]["modified"] = "1:0"
    target = root / "disk.bin"
    original = b"\xa5" * capacity
    target.write_bytes(original)
    report, diagnostics = run(program, root, packet, mode, sector, int(wire["decoded"]))
    stages = [line.removeprefix("COMPRESSED_FIXTURE ") for line in diagnostics if line.startswith("COMPRESSED_FIXTURE ")]
    error = report["error"]
    written, verified = int(report["written"]), int(report["verified"])
    padded = data + bytes(-len(data) % sector)
    successes = {"success", "protected", "legacy-raw", "trailing-stream", "concatenated", "trailing-data",
                 "known-size", "known-size-no-eos", "dictionary-4096", "second-lzma-stream",
                 "nine-bit", "non-block", "clear-tail", "incomplete-code"}
    preflight = {"bad-crc": errno.EBADMSG, "bad-sha": errno.ESTALE, "small-decoded": errno.ESTALE,
                 "large-decoded": errno.ESTALE, "wrong-inode": errno.ESTALE, "wrong-timestamp": errno.ESTALE,
                 "source-owner": errno.EACCES, "missing-marker": errno.ENOTSUP}
    preflight["bad-tail"] = errno.EBADMSG
    preflight.update({name: errno.EBADMSG for name in ("bad-combined-crc", "empty-crc-only", "empty-crc-first", "empty-crc-last", "empty-crc-middle")})
    preflight.update({name: errno.EBADMSG for name in ('bad-size', 'large-known-size', 'truncated-eos', 'invalid-history', 'known-size-overshoot')})
    preflight['unsupported-dictionary'] = errno.ENOTSUP
    preflight.update({name: errno.EBADMSG for name in ('bad-code', 'late-bad-code', 'bad-bits', 'short-header')})
    preflight['shorter-stream'] = errno.ESTALE
    before_write = {"snapshot-error": errno.ESTALE, "claim-error": errno.EBUSY, "kernel-error": errno.EIO,
                    "readonly": errno.EROFS, "mount-mismatch": errno.ESTALE, "unmount-error": errno.EBUSY,
                    "native-protection-error": errno.EBUSY, "preflight-close-fail": errno.EIO,
                    "boundary-zero-device": errno.EINVAL, "boundary-zero-capacity": errno.EINVAL,
                    "boundary-wrong-device": errno.ESTALE, "boundary-wrong-capacity": errno.ESTALE,
                    "boundary-regular": errno.ENOTSUP, "boundary-source-device": errno.EINVAL,
                    "boundary-sector": errno.ESTALE, "boundary-capacity": errno.ENOSPC, "boundary-append": errno.EINVAL}
    after_write = {"sync-error": errno.EIO, "verify-error": errno.EIO, "corrupt": errno.EBADMSG,
                   "padding-corrupt": errno.EBADMSG, "sector-change": errno.ESTALE, "capacity-change": errno.ESTALE,
                   "readonly-change": errno.EROFS, "final-snapshot-error": errno.ESTALE,
                   "target-close-fail": errno.EIO, "source-close-fail": errno.EIO, "home-close-fail": errno.EIO,
                   "target-close-eintr": errno.EINTR}
    if mode in successes:
        assert error == report["syncError"] == 0 and report["phase"] == DONE, (mode, report, diagnostics)
        assert written == verified == len(padded), (mode, report)
    elif mode in preflight:
        assert error == preflight[mode] and report["phase"] == PREPARE, (mode, report, diagnostics)
        assert written == verified == 0 and "snapshot" not in stages and "claim" not in stages, (mode, stages)
    elif mode in before_write:
        assert error == before_write[mode] and report["phase"] == PREPARE and written == verified == 0, (mode, report, diagnostics)
    elif mode in after_write:
        assert error == after_write[mode] and written == len(padded), (mode, report, diagnostics)
    elif mode in {"cancel", "cancel-close", "cancel-sync-fail", "write-error"}:
        expected = errno.EIO if mode == "cancel-close" else errno.ENOSPC if mode == "write-error" else errno.ECANCELED
        assert error == expected and written == 17 and verified == 0, (mode, report, diagnostics)
    elif mode == "cancel-sync":
        assert error == errno.ECANCELED and written == len(padded) and verified == 0, (mode, report, diagnostics)
    else:
        raise AssertionError(mode)
    assert report["syncError"] == (errno.EIO if mode in {"sync-error", "cancel-sync-fail"} else 0), (mode, report)
    assert stages.count("sync") == int(written != 0), (mode, stages)
    if "claim" in stages:
        assert stages.count("claim") == stages.count("close-target") == 1, (mode, stages)
        assert stages.index("claim") < stages.index("close-target") < stages.index("close-source"), stages
    assert stages.count("source-open") == stages.count("close-source") == 1, (mode, stages)
    if "home" in stages:
        assert stages.count("home") == stages.count("close-home") == 1, (mode, stages)
    if mode == "protected":
        assert stages.index("unmount") < stages.index("close-preflight") < stages.index("claim"), stages
    expected = bytearray(padded[:written] + original[written:])
    if mode == "corrupt": expected[23] ^= 1
    if mode == "padding-corrupt": expected[len(data)] ^= 1
    assert target.read_bytes() == expected, (mode, report)
    assert (root / ("source." + extension)).read_bytes() == packed


def main():
    assert os.getuid() != 0, "Run this regular-file fixture as an ordinary user"
    # Failure logs suffice for this owned fixture; do not leave system-managed
    # core files outside the temporary directory on a deliberate assertion.
    _, hard = resource.getrlimit(resource.RLIMIT_CORE)
    resource.setrlimit(resource.RLIMIT_CORE, (0, hard))
    global xz_mode, bzip2_mode, lzma_mode, lzw_mode, lzw_gzip_mode, zip_mode, zstd_mode, extension
    zip_mode = len(sys.argv) == 3 and sys.argv[2] == "--zip"
    zstd_mode = len(sys.argv) == 3 and sys.argv[2] == "--zstd"
    xz_mode = len(sys.argv) == 3 and sys.argv[2] == "--xz"
    bzip2_mode = len(sys.argv) == 3 and sys.argv[2] == "--bzip2"
    lzma_mode = len(sys.argv) == 3 and sys.argv[2] == "--lzma"
    lzw_gzip_mode = len(sys.argv) == 3 and sys.argv[2] == "--lzw-gzip"
    lzw_mode = lzw_gzip_mode or len(sys.argv) == 3 and sys.argv[2] == "--lzw"
    assert len(sys.argv) == (3 if zip_mode or zstd_mode or xz_mode or bzip2_mode or lzma_mode or lzw_mode else 2)
    extension = "zst" if zstd_mode else "zip" if zip_mode else "xz" if xz_mode else "bz2" if bzip2_mode else "lzma" if lzma_mode else "Z" if lzw_mode and not lzw_gzip_mode else "gz"
    program = str(Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix="rufus-compressed-device-") as directory:
        root = Path(directory)
        for sector in (512, 4096, 65536):
            check(program, root, sector=sector)
        # Stored gzip can be larger than the exact target while decoded data fits.
        data = bytearray(random.Random(9).randbytes(4096))
        data[510:512] = b"\x55\xaa"
        check(program, root, data=bytes(data), capacity=4096)
        raw = bytearray(random.Random(11).randbytes(65536))
        raw[510:512] = b"\x55\xaa"
        check(program, root, "legacy-raw", data=bytes(raw))
        format_failure = 'bad-code' if lzw_mode else 'bad-size' if lzma_mode else 'bad-crc'
        for mode in ("protected", format_failure, "bad-sha", "small-decoded", "large-decoded", "wrong-inode", "wrong-timestamp",
                     "source-owner", "snapshot-error", "claim-error", "kernel-error", "readonly", "mount-mismatch", "unmount-error",
                     "native-protection-error", "preflight-close-fail", "boundary-zero-device", "boundary-zero-capacity",
                     "boundary-wrong-device", "boundary-wrong-capacity", "boundary-regular", "boundary-source-device", "boundary-sector",
                     "boundary-capacity", "boundary-append", "write-error", "sync-error", "verify-error", "corrupt", "padding-corrupt",
                     "sector-change", "capacity-change", "readonly-change", "final-snapshot-error", "target-close-fail", "source-close-fail",
                     "home-close-fail", "target-close-eintr", "cancel", "cancel-close", "cancel-sync-fail", "cancel-sync"):
            check(program, root, mode)
        no_marker = bytes(8193)
        check(program, root, "missing-marker", data=no_marker)
        check(program, root, data=no_marker, force=True)
        if bzip2_mode:
            for mode in ("concatenated", "trailing-data", "bad-combined-crc", "empty-crc-only", "empty-crc-first", "empty-crc-last", "empty-crc-middle"):
                check(program, root, mode)
        if xz_mode:
            for kind in (lzma.CHECK_CRC32, lzma.CHECK_SHA256):
                check(program, root, check_kind=kind)
                check(program, root, "bad-crc", check_kind=kind)
            check(program, root, check_kind=lzma.CHECK_NONE)
            check(program, root, "trailing-stream")
        if lzma_mode:
            for mode in ('known-size', 'known-size-no-eos', 'dictionary-4096', 'trailing-stream', 'trailing-data', 'second-lzma-stream',
                         'large-known-size', 'truncated-eos', 'invalid-history', 'known-size-overshoot', 'unsupported-dictionary'):
                check(program, root, mode)
        if lzw_mode:
            for mode in ('nine-bit', 'non-block', 'clear-tail', 'late-bad-code', 'bad-bits', 'short-header', 'shorter-stream'):
                check(program, root, mode)
            check(program, root, 'incomplete-code', data=data)
        if zip_mode:
            for method in (0, 12, 14, 95):
                for sector in (512, 4096, 65536):
                    check(program, root, sector=sector, zip_method=method)
        if zstd_mode:
            check(program, root, "concatenated")
            check(program, root, "bad-tail")
    print(f"{'Zstd device helper' if zstd_mode else 'ZIP device helper' if zip_mode else 'XZ device helper' if xz_mode else 'BZIP2 device helper' if bzip2_mode else 'LZMA device helper' if lzma_mode else 'LZW device helper' if lzw_mode else 'Compressed device helper'}: {runs} native job/wire/claim/kernel/cleanup cases passed; link-only kernel facts, regular files only")


if __name__ == "__main__":
    main()
