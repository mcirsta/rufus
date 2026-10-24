#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Grouped device-table and raw optical capture qualification on owned files."""
import errno
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib


def invoke(command):
    result = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=45, check=True)
    return json.loads(result.stdout.splitlines()[-1])


def table(path):
    return json.loads(subprocess.check_output(['sfdisk', '--json', str(path)], text=True))['partitiontable']


def check_gpt(data):
    for offset in (512, len(data) - 512):
        header = bytearray(data[offset:offset + 92])
        expected = struct.unpack_from('<I', header, 16)[0]
        struct.pack_into('<I', header, 16, 0)
        assert zlib.crc32(header) == expected
        location, count, size, checksum = struct.unpack_from('<QIII', header, 72)
        assert zlib.crc32(data[location * 512:location * 512 + count * size]) == checksum


def check_tables(probe, root, checks):
    for kind in ('gpt', 'dos', 'dos-logical'):
        image = root / (kind + '.raw')
        with image.open('wb') as output:
            output.truncate(64 * 1024 * 1024)
        label = 'gpt' if kind == 'gpt' else 'dos'
        layout = 'label: ' + label + '\nlabel-id: ' + ('{01234567-89ab-cdef-0123-456789abcdef}' if kind == 'gpt' else '0x12345678') + '\nunit: sectors\n'
        layout += 'start=2048,size=16384,type=' + ('U' if kind == 'gpt' else 'ef') + '\n'
        layout += 'start=18432,size=16384,type=' + ('L' if kind == 'gpt' else '83') + '\n'
        if kind == 'dos-logical':
            mbr = bytearray(512); struct.pack_into('<I', mbr, 440, 0x12345678)
            mbr[450] = 5; struct.pack_into('<II', mbr, 454, 2048, 65536)
            mbr[466] = 0x83; struct.pack_into('<II', mbr, 470, 67584, 16384); mbr[510:512] = b'\x55\xaa'
            ebr = bytearray(512); ebr[450] = 0xef; struct.pack_into('<II', ebr, 454, 2048, 16384); ebr[510:512] = b'\x55\xaa'
            with image.open('r+b') as output:
                output.write(mbr); output.seek(2048 * 512); output.write(ebr)
        else:
            subprocess.run(['sfdisk', '--no-reread', '--no-tell-kernel', str(image)], input=layout, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True)
        with image.open('r+b') as output:
            output.write(bytes(range(220)) * 2)  # Unrelated executable boot code.

        original = image.read_bytes()
        if kind == 'gpt':
            damaged = bytearray(original); damaged[512 + 16] ^= 1; image.write_bytes(damaged)
            invalid = invoke([probe, str(image), 'accept'])
            assert invalid['error'] == errno.EBADMSG and not invalid['started'], invalid
            assert image.read_bytes() == damaged
            image.write_bytes(original); checks.append('GPT damaged header refused without repair')
        refused = invoke([probe, str(image), 'refuse'])
        assert refused['error'] == errno.EIO and not refused['started'], refused
        assert image.read_bytes() == original
        result = invoke([probe, str(image), 'accept'])
        assert result['error'] == result['sync'] == result['close'] == 0 and result['started'] and not result['restore'], result
        changed = image.read_bytes()
        if kind == 'gpt':
            check_gpt(changed)
            primary = struct.unpack_from('<Q', original, 512 + 72)[0] * 512
            backup = struct.unpack_from('<Q', original, len(original) - 512 + 72)[0] * 512
            allowed = set(range(primary, primary + 16)) | set(range(backup, backup + 16))
            allowed.update(range(512 + 16, 512 + 20)); allowed.update(range(512 + 88, 512 + 92))
            allowed.update(range(len(original) - 512 + 16, len(original) - 512 + 20)); allowed.update(range(len(original) - 512 + 88, len(original) - 512 + 92))
        else:
            allowed = {450 if kind == 'dos' else 2048 * 512 + 450}
        differences = {i for i, values in enumerate(zip(original, changed)) if values[0] != values[1]}
        assert differences and differences <= allowed, (kind, sorted(differences - allowed)[:30])
        assert table(image)['partitions'][1]['type'].lower() == ('0fc63daf-8483-4772-8e79-3d69d8477de4' if kind == 'gpt' else '83')
        restored = invoke([probe, str(image), 'accept', result['guid']])
        assert restored['error'] == 0 and restored['restore'], restored
        assert image.read_bytes() == original
        missing = invoke([probe, str(image), 'accept', '{aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee}'])
        # An ESP already exists, so conversion must choose it ahead of restore records.
        assert missing['error'] == 0 and not missing['restore'], missing
        checks.extend([kind + ' prewrite record failure', kind + ' preserved unrelated bytes', kind + ' GUID restore', kind + ' ESP precedence'])


def check_capture(probe, root, checks):
    source = root / 'optical.raw'
    payload = bytes(range(256)) * 8192
    source.write_bytes(payload)
    expected = {'read-fail': errno.EIO, 'write-fail': errno.ENOSPC, 'sync-fail': errno.EIO,
                'cancel-copy': errno.ECANCELED, 'cancel-verify': errno.ECANCELED, 'corrupt': errno.EBADMSG, 'mutate': errno.ESTALE}
    for mode in ('normal', 'short', 'read-fail', 'write-fail', 'sync-fail', 'cancel-copy', 'cancel-verify', 'corrupt', 'mutate'):
        source.write_bytes(payload)
        output = root / ('disc-' + mode + '.iso')
        output.write_bytes(b'previous image')
        report = invoke([probe, mode, str(source), str(output), '2048', str(len(payload))])
        assert report['error'] == expected.get(mode, 0), (mode, report)
        if mode in ('normal', 'short'):
            assert output.read_bytes() == payload
            assert report['sha256'] == hashlib.sha256(payload).hexdigest()
        else:
            assert output.read_bytes() == b'previous image'
        assert not list(root.glob('.rufus-save-*'))
        checks.append('optical raw ' + mode)


def main():
    operations, capture = sys.argv[1:]
    assert shutil.which('sfdisk'), 'sfdisk fixture provider required'
    checks = []
    with tempfile.TemporaryDirectory(prefix='rufus-device-source-') as directory:
        root = Path(directory)
        check_tables(operations, root, checks)
        check_capture(capture, root, checks)
    print(json.dumps({'passed': len(checks), 'checks': checks}))


if __name__ == '__main__':
    main()
