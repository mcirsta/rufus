#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""One capture/export corpus, including an actual file beyond the 4-GiB boundary."""
import argparse
import binascii
import errno
import json
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import time


def capture(probe, encoder, source, output, mode='success', error=0):
    output.unlink(missing_ok=True)
    output.write_bytes(b'OLD BACKUP')
    run = subprocess.run([str(probe), mode, str(source), str(output), '512', '0', str(encoder)],
                         capture_output=True, text=True, timeout=180)
    assert run.returncode == 0, (run.returncode, run.stderr)
    report = json.loads(run.stdout)
    assert report['error'] == error, (mode, report, run.stderr)
    if error:
        assert not report['committed'] and output.read_bytes() == (b'RACED' if mode == 'destination-race' else b'OLD BACKUP'), report
    else:
        assert report['committed'] and len(report['sha256']) == 64, report
        with output.open('rb') as file:
            file.seek(16 * 2048)
            recognition = file.read(32 * 2048)
        assert [recognition[i * 2048 + 1:i * 2048 + 6] for i in range(3)] == [b'BEA01', b'NSR02', b'TEA01']
        assert b'CD001' not in recognition and b'EL TORITO' not in recognition
    assert not list(output.parent.glob('.rufus-image-*'))
    return report


def cancel_encoder(probe, root):
    source = root / 'tree'; source.mkdir()
    (source / 'data').write_bytes(b'encoder cancellation')
    output = root / 'cancelled.iso'; output.write_bytes(b'OLD BACKUP')
    pidfile = root / 'encoder.pid'
    encoder = root / 'hold-encoder'
    encoder.write_text('#!/bin/sh\nprintf "%s" "$$" > "' + str(pidfile) + '"\nexec /usr/bin/sleep 30\n')
    encoder.chmod(0o700)
    process = subprocess.Popen([str(probe), 'success', str(source), str(output), '512', '0', str(encoder)],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    child = None
    try:
        deadline = time.monotonic() + 5
        while not pidfile.exists() or not pidfile.read_text():
            assert time.monotonic() < deadline and process.poll() is None
            time.sleep(.01)
        child = int(pidfile.read_text())
        os.kill(process.pid, signal.SIGUSR1)
        stdout, stderr = process.communicate(timeout=10)
        report = json.loads(stdout)
        assert process.returncode == 0 and report['error'] == errno.ECANCELED and not report['committed'], (report, stderr)
        assert output.read_bytes() == b'OLD BACKUP' and not list(root.glob('.rufus-image-*'))
        try:
            os.kill(child, 0)
        except ProcessLookupError:
            child = None
        else:
            raise AssertionError('Encoder remained alive after cancellation')
    finally:
        if process.poll() is None:
            process.kill(); process.wait()
        if child is not None:
            try:
                os.kill(child, signal.SIGKILL)
            except ProcessLookupError:
                pass
    print('ISO encoder cancellation: child exited, old backup retained, temporary image removed')


def genisoimage_111(encoder):
    result = subprocess.run([str(encoder), '-version'], capture_output=True, text=True, timeout=10)
    # The distribution's mkisofs symlink prints a compatibility preamble before
    # its genuine genisoimage version. Do not infer the provider from argv[0].
    return any(line.startswith('genisoimage 1.1.11 ') for line in (result.stdout + result.stderr).splitlines())


def check_genisoimage_semicolon(encoder, tree, root, original, replacement):
    # This provider rewrites semicolons even in its UDF tree. Verify the actual
    # valid FID independently before expecting native export to refuse it.
    image = root / 'provider-renamed.iso'
    subprocess.run([str(encoder), '-quiet', '-D', '-udf', '-iso-level', '3',
                    '-input-charset', 'utf-8', '-V', 'SOURCE', '-o', str(image), str(tree)],
                   check=True, capture_output=True, timeout=15)
    data = image.read_bytes()
    names = []
    for offset in range(0, len(data) - 38, 4):
        tag = data[offset:offset + 16]
        if tag[:2] != b'\x01\x01' or ((sum(tag) - tag[4]) & 255) != tag[4]:
            continue
        crc_size = int.from_bytes(tag[10:12], 'little')
        if offset + 16 + crc_size > len(data):
            continue
        if binascii.crc_hqx(data[offset + 16:offset + 16 + crc_size], 0) != int.from_bytes(tag[8:10], 'little'):
            continue
        size = data[offset + 19]
        start = offset + 38 + int.from_bytes(data[offset + 36:offset + 38], 'little')
        encoded = data[start:start + size]
        if encoded[:1] == b'\x08':
            names.append(encoded[1:].decode('latin-1'))
        elif encoded[:1] == b'\x10':
            names.append(encoded[1:].decode('utf-16-be'))
    assert replacement in names and original not in names, names
    image.unlink()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('probe', type=Path)
    parser.add_argument('encoder', type=Path)
    parser.add_argument("--cancel-only", action="store_true")
    parser.add_argument("--small-only", action="store_true")
    args = parser.parse_args()
    checks = 0
    with tempfile.TemporaryDirectory(prefix='rufus-iso-export-', dir='/tmp') as temporary:
        root = Path(temporary)
        if args.cancel_only:
            cancel_encoder(args.probe, root)
            return
        limited_encoder = genisoimage_111(args.encoder)
        cancellation = root / 'encoder-cancel'; cancellation.mkdir()
        cancel_encoder(args.probe, cancellation)
        checks += 1
        tree = root / 'source tree' 
        tree.mkdir()
        entries = {'empty': b'', '.hidden/.keep': b'hidden', 'ldlinux.sys': b'must be retained',
                   'șΩ filename [x].txt': bytes(range(256)) * 257,
                   '/'.join(['deep directory 0123456789'] * 12) + '/long name.txt': b'long path'}
        for name, data in entries.items():
            path = tree / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        (tree / 'empty directory').mkdir()
        output = root / 'saved.iso'
        report = capture(args.probe, args.encoder, tree, output)
        assert report['bytes'] == sum(map(len, entries.values()))
        checks += 1
        original = 'șΩ filename [x];.txt'
        safe_name = tree / 'șΩ filename [x].txt'
        unusual_name = tree / original
        safe_name.rename(unusual_name)
        if limited_encoder:
            check_genisoimage_semicolon(args.encoder, tree, root, original, original.replace(';', '_'))
        capture(args.probe, args.encoder, tree, output, error=errno.EBADMSG if limited_encoder else 0)
        unusual_name.rename(safe_name)
        checks += 1
        for mode, error in (('short', 0), ('cancel-copy', errno.ECANCELED), ('cancel-verify', errno.ECANCELED),
                            ('sync-fail', errno.EIO), ('destination-race', errno.ESTALE),
                            ('directory-sync-fail', errno.EIO)):
            capture(args.probe, args.encoder, tree, output, mode, error)
            checks += 1
        link = tree / 'link'
        link.symlink_to('/etc/passwd')
        capture(args.probe, args.encoder, tree, output, error=errno.ENOTSUP)
        link.unlink()
        os.mkfifo(link)
        capture(args.probe, args.encoder, tree, output, error=errno.ENOTSUP)
        link.unlink()
        capture(args.probe, root / 'missing', tree, output, error=errno.ENOENT)
        broken = root / 'broken'
        broken.write_text('#!/bin/sh\nexit 1\n')
        broken.chmod(0o700)
        capture(args.probe, broken, tree, output, error=errno.EIO)
        checks += 4
        # An empty tree must still retain its label and complete verification.
        empty = root / 'empty'; empty.mkdir()
        capture(args.probe, args.encoder, empty, output)
        checks += 1
        if args.small_only:
            print(f'ISO export: {checks} focused file-tree/cancellation cases passed')
            return
        # Real >4-GiB UDF payload; source holes save fixture allocation. Output
        # is fully encoded/read/hash-verified, never just a faked size field.
        large = tree / 'larger than 4 GiB.bin'
        size = (1 << 32) + 65537
        with large.open('wb') as file:
            file.truncate(size)
            file.seek((1 << 32) - 11); file.write(b'boundary-payload-crossing-four-gib')
            file.seek(size - 7); file.write(b'THE END')
        report = capture(args.probe, args.encoder, tree, output, error=errno.EIO if limited_encoder else 0)
        if not limited_encoder:
            assert report['bytes'] == size + sum(map(len, entries.values()))
            assert output.stat().st_size > 1 << 32
        checks += 1
        large.unlink(); output.unlink()
    large_result = 'genisoimage 1.1.11 semicolon rewriting and >4-GiB refusal preserve the old backup' if limited_encoder else 'semicolon names and actual >4-GiB UDF-only output verified'
    print(f'ISO export: {checks} batched cases; hidden/Unicode/empty/deep paths verified; {large_result}')


if __name__ == '__main__':
    main()
