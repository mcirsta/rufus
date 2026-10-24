# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise the actual one-request helper on disposable ordinary files."""
import argparse
import errno
import fcntl
import hashlib
import json
import os
from pathlib import Path
import subprocess
import struct
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).parent / 'vm'))
from gpt import inspect_formatted_partition
from test_partition_image import make_image, inspect_gpt, check_preserved
from test_fat_format import copy_volume

DISK_BYTES = 64 * 1024 * 1024
PROCESS_TIMEOUT_SECONDS = 10


def digest(path):
    with path.open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def request_for(path):
    info = path.stat()
    return {'operation': 'prepare-image-v1', 'path': str(path), 'device': str(info.st_dev),
            'inode': str(info.st_ino), 'size': str(info.st_size), 'cluster': '512',
            'volume': str(0x1234abcd), 'label': 'RUFUS TEST'}


def start(program, request, **environment):
    env = dict(os.environ, **environment)
    process = subprocess.Popen([str(program)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, env=env)
    message = request if isinstance(request, bytes) else json.dumps(request).encode() + b'\n'
    process.stdin.write(message)
    process.stdin.flush()
    return process


def finish(process, error, started):
    try:
        process.wait(timeout=PROCESS_TIMEOUT_SECONDS)
    except subprocess.TimeoutExpired:
        process.stdin.close()
        process.wait(timeout=PROCESS_TIMEOUT_SECONDS)
        raise
    output, diagnostics = process.communicate()
    assert not diagnostics, diagnostics.decode(errors='replace')
    messages = [json.loads(line) for line in output.splitlines()]
    assert messages[0] == {'event': 'ready'}, messages
    results = [message for message in messages if message.get('event') == 'result']
    assert len(results) == 1 and messages[-1] == results[0], messages
    result = results[0]
    assert result['error'] == error and result['started'] == started, result
    assert process.returncode == (0 if error == 0 else 1), (process.returncode, messages)
    return result


def wait_for_formatter(marker):
    deadline = time.monotonic() + PROCESS_TIMEOUT_SECONDS
    while time.monotonic() < deadline:
        if marker.exists():
            text = marker.read_text()
            if text:
                return int(text)
        time.sleep(0.01)
    raise AssertionError('Formatter did not start')


def inspect_fat16(args, root, image, offset, size, cluster, empty=True):
    with image.open('rb') as source:
        source.seek(offset)
        boot = source.read(512)
    assert boot[54:62] == b'FAT16   ' and boot[510:] == b'\x55\xaa'
    assert struct.unpack_from('<H', boot, 11)[0] == 512 and boot[13] * 512 == cluster
    assert struct.unpack_from('<I', boot, 28)[0] == offset // 512
    assert struct.unpack_from('<I', boot, 39)[0] == 0x1234abcd
    assert boot[43:54] == b'RUFUS TEST '
    volume = root / 'volume.img'
    copy_volume(image, volume, offset, size)
    checked = subprocess.run([args.fsck, '-n', str(volume)], capture_output=True, timeout=15)
    assert checked.returncode == 0, checked.stdout + checked.stderr
    listing = subprocess.run([args.mdir, '-i', f'{image}@@{offset}', '::'], capture_output=True, timeout=10)
    assert listing.returncode == 0, listing.stdout + listing.stderr
    if empty:
        assert b'No files' in listing.stdout, listing.stdout + listing.stderr
    volume.unlink()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('helper', type=Path)
    parser.add_argument('--probe', type=Path, required=True)
    parser.add_argument('--sfdisk', required=True)
    parser.add_argument('--fsck', required=True)
    parser.add_argument('--mdir', required=True)
    parser.add_argument('--fat16', action='store_true')
    args = parser.parse_args()
    cluster = 2048 if args.fat16 else 512
    with tempfile.TemporaryDirectory(prefix='rufus-prepare-helper-') as temporary:
        root = Path(temporary)
        image, segments = make_image(root, 'native.img', DISK_BYTES, cluster)
        request = request_for(image)
        if args.fat16:
            request.update(operation='prepare-fat16-image-v1', cluster=str(cluster))
        result = finish(start(args.helper, request), 0, True)
        offset, size = int(result['offset']), int(result['size'])
        inspect_gpt(args, image, DISK_BYTES, cluster)
        check_preserved(image, DISK_BYTES, segments, (offset, size))
        if args.fat16:
            inspect_fat16(args, root, image, offset, size, cluster)
        else:
            inspect_formatted_partition(args, root, image, {'OFFSET': offset, 'SIZE': size})
        original = digest(image)

        # Reject unsupported operations and invalid settings before GPT writes.
        invalid = [({'operation': 'prepare-device-v1'}, errno.EPROTO),
                   ({'operation': 'prepare-fat16-device-v1'}, errno.EPROTO),
                   ({'operation': 'prepare-exfat-image-v1'}, errno.EPROTO),
                   ({'source': {}}, errno.EPROTO),
                   ({'windows': {}}, errno.EPROTO),
                   ({'mkfs': '/bin/false'}, errno.EPROTO),
                   ({'device': str(int(request['device']) + 1)}, errno.ESTALE),
                   ({'inode': str(int(request['inode']) + 1)}, errno.ESTALE),
                   ({'size': str(DISK_BYTES + 512)}, errno.ESTALE),
                   ({'cluster': '32768'}, errno.ENOTSUP),
                   ({'cluster': '1536'}, errno.EINVAL),
                   ({'cluster': str(1 << 32)}, errno.EPROTO),
                   ({'cluster': 512}, errno.EPROTO),
                   ({'size': '-1'}, errno.EPROTO),
                   ({'label': 'lowercase'}, errno.EINVAL),
                   ({'label': 'TOO LONG LABEL'}, errno.EINVAL),
                   ({'label': 'BAD\x00LABEL'}, errno.EPROTO),
                   ({'path': 'relative.img'}, errno.EPROTO)]
        for changes, expected in invalid:
            finish(start(args.helper, dict(request, **changes)), expected, False)
            assert digest(image) == original, changes
        for payload, expected in ((b'{' + b' ' * 4096, errno.EMSGSIZE),
                                  (b'[]\n', errno.EPROTO), (b'{}\n{}\n', errno.EPROTO)):
            finish(start(args.helper, payload), expected, False)
            assert digest(image) == original

        link = root / 'link.img'
        link.symlink_to(image)
        finish(start(args.helper, dict(request, path=str(link))), errno.ELOOP, False)
        with image.open('rb+') as held:
            fcntl.flock(held, fcntl.LOCK_EX | fcntl.LOCK_NB)
            finish(start(args.helper, request), errno.EWOULDBLOCK, False)
        assert digest(image) == original

        incomplete = start(args.helper, b'{"operation":')
        incomplete.stdin.close()
        incomplete.stdin = None
        finish(incomplete, errno.ECANCELED, False)
        assert digest(image) == original

        # Split a request across actual pipe writes, then qualify another complete run.
        prefix = json.dumps({'operation': request['operation']})[:-1].encode() + b','
        process = start(args.helper, prefix)
        remainder = json.dumps(request).split(',', 1)[1].encode() + b'\n'
        process.stdin.write(remainder)
        process.stdin.flush()
        finish(process, 0, True)

        # The production parser never accepts a formatter override. Only this
        # separately linked probe redirects posix_spawn to a controlled child.
        marker = root / 'formatter.pid'
        formatter = root / 'blocking-formatter'
        formatter.write_text('#!/usr/bin/python3\nimport os, signal, time\n'
                             'from pathlib import Path\n'
                             'signal.signal(signal.SIGTERM, signal.SIG_IGN)\n'
                             'Path(os.environ["RUFUS_TEST_PID"]).write_text(str(os.getpid()))\n'
                             'time.sleep(60)\n')
        formatter.chmod(0o700)
        process = start(args.probe, request, RUFUS_TEST_FORMATTER=str(formatter), RUFUS_TEST_PID=str(marker))
        child = wait_for_formatter(marker)
        process.stdin.close()  # Same EOF produced when a caller exits.
        process.stdin = None
        result = finish(process, errno.ECANCELED, True)
        assert result['syncError'] == 0 and result['filesystemBytes'] == '0', result
        assert not Path(f'/proc/{child}').exists(), 'Cancelled formatter was not reaped'
        with image.open('rb+') as held:
            fcntl.flock(held, fcntl.LOCK_EX | fcntl.LOCK_NB)

        result = finish(start(args.probe, request, RUFUS_TEST_SYNC_ERROR='1'), errno.EIO, True)
        assert result['syncError'] == errno.EIO and result['filesystemBytes'] == '0', result
        result = finish(start(args.probe, request, RUFUS_TEST_SYNC_AFTER_FORMAT='1'), errno.EIO, True)
        assert result['syncError'] == errno.EIO, result
        formatter.write_text('#!/usr/bin/python3\nimport sys\nsys.exit(7)\n')
        finish(start(args.probe, request, RUFUS_TEST_FORMATTER=str(formatter)), errno.EIO, True)
        # Start with an unformatted volume so a false-success formatter cannot
        # pass readback using the filesystem written by an earlier case.
        with image.open('r+b') as target:
            target.seek(offset)
            target.write(bytes(512))
        formatter.write_text('#!/usr/bin/python3\n')
        finish(start(args.probe, request, RUFUS_TEST_FORMATTER=str(formatter)), errno.EBADMSG, True)
        finish(start(args.helper, request), 0, True)
        inspect_gpt(args, image, DISK_BYTES, cluster)
        check_preserved(image, DISK_BYTES, segments, (offset, size))
        if args.fat16:
            inspect_fat16(args, root, image, offset, size, cluster)
    print(f'PASS: helper process, native GPT/FAT{16 if args.fat16 else 32} readback, request rejection, EOF cancellation, '
          'child reaping, flush failure and recovery; disposable files only.')


if __name__ == '__main__':
    main()
