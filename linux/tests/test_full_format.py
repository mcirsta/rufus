#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""One full/quick format corpus using dirty regular files and native tools."""
import errno
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
from test_fat_format import fake_formatter

MIB = 1024 * 1024


def run(command, expected=0):
    result = subprocess.run([str(item) for item in command], capture_output=True, timeout=30)
    assert result.returncode == 0, (command, result.stdout, result.stderr)
    report = {key.decode(): int(value) for key, value in re.findall(rb'([A-Z_]+)=(-?\d+)', result.stdout)}
    assert report['ERROR'] == expected, (command, report, result.stderr)
    assert report['FDS'] == 0 and report['FLAGS'] == 1 and report['OFFSET'] == 17, report
    assert report['CHILD'] == -1 and report['CHILD_ERRNO'] == errno.ECHILD, report
    if expected == 0:
        assert report['STARTED'] == 1 and report['EXIT'] == report['SYNC'] == 0, report
    return report


def dirty(path, size):
    with path.open('wb') as output:
        for unused in range(size // MIB): output.write(b'\xa5' * MIB)
        output.write(b'\xa5' * (size % MIB))


def exfat_capability(probe, formatter):
    """Use the standalone exFAT corpus's library/help gates and verify refusal."""
    assert formatter and os.path.isabs(str(formatter)) and os.access(formatter, os.X_OK), formatter
    library = subprocess.run([str(probe), '--check-library'], capture_output=True, timeout=15)
    assert library.returncode in (0, 77), (library.returncode, library.stdout, library.stderr)
    unavailable = 'libblkid' if library.returncode == 77 else None
    if unavailable is None:
        help_result = subprocess.run([str(formatter), '--help'], capture_output=True, timeout=15)
        help_text = help_result.stdout + help_result.stderr
        required = (b'--check-written', b'--no-discard', b'--partition-table')
        if not all(option in help_text for option in required):
            unavailable = 'mkfs.exfat'
    if unavailable is not None:
        preflight = subprocess.run([str(probe), '--tools', str(formatter), 'none'],
                                   capture_output=True, timeout=15)
        assert preflight.returncode == 0, (preflight.returncode, preflight.stdout, preflight.stderr)
        report = {key.decode(): int(value) for key, value in re.findall(rb'([A-Z_]+)=(-?\d+)', preflight.stdout)}
        assert report['ERROR'] == errno.ENOTSUP and report['PROBLEM'] == 1, (report, preflight.stderr)
        assert report['FDS'] == 0 and report['CHILD'] == -1 and report['CHILD_ERRNO'] == errno.ECHILD, report
        print(f'exFAT capability preflight refusal verified: unsupported {unavailable}', flush=True)
    return unavailable


def main():
    fat, ntfs, exfat, ext, zero = sys.argv[1:]
    tools = {name: shutil.which(name) for name in ('mkfs.fat', 'mkntfs', 'mkfs.exfat', 'mke2fs', 'debugfs', 'e2fsck', 'fsck.fat')}
    assert all(tools.values()), tools
    exfat_unavailable = exfat_capability(exfat, tools['mkfs.exfat'])
    cases = []
    with tempfile.TemporaryDirectory(prefix='rufus-full-format-') as scratch:
        root = Path(scratch); target = root / 'volume.raw'
        if exfat_unavailable is not None:
            cases.append('exFAT unsupported ' + exfat_unavailable + ': tools preflight refuses before formatting')
            if exfat_unavailable == 'libblkid':
                for full in (False, True):
                    dirty(target, 64 * MIB)
                    before = target.read_bytes()
                    command = [exfat, tools['mkfs.exfat'], target, 64 * MIB, 4096, 'FULLTEST', 'none']
                    if full: command.append('full')
                    refused = run(command, errno.ENOTSUP)
                    assert refused['STARTED'] == refused['SYNCS'] == refused['BYTES'] == 0, refused
                    assert target.read_bytes() == before
                    cases.append(f'exFAT {"full" if full else "quick"}: old library refuses without writes')
        for fs in ('fat16', 'fat32', 'ntfs', 'exfat', 'ext2', 'ext3', 'ext4'):
            if fs == 'exfat' and exfat_unavailable is not None:
                continue
            size = (32 if fs == 'fat16' else 64) * MIB
            for full in (False, True):
                offset = MIB if fs.startswith('fat') else 0
                dirty(target, size + 2 * offset)
                if fs.startswith('fat'):
                    command = [fat, tools['mkfs.fat'], target, offset, size, 2048 if fs == 'fat16' else 512, 'FULLTEST', 'none', fs[3:]]
                elif fs == 'ntfs': command = [ntfs, tools['mkntfs'], target, MIB, size, 4096, 'FULLTEST', 'none']
                elif fs == 'exfat': command = [exfat, tools['mkfs.exfat'], target, size, 4096, 'FULLTEST', 'none']
                else: command = [ext, tools['mke2fs'], target, size, fs, 'FULLTEST', 'none', 'persistence']
                if full: command.append('full')
                trace = root / 'arguments.json'
                native = command[1]
                wrapper = fake_formatter(root, 'record-arguments', "import json; open(" + repr(str(trace))
                    + ", 'w').write(json.dumps(sys.argv[1:])); os.execv(" + repr(native) + ", [" + repr(native) + "] + sys.argv[1:])")
                command[1] = wrapper
                report = run(command)
                arguments = json.loads(trace.read_text())
                if fs == 'ntfs':
                    assert ('-Q' in arguments) == (not full)
                    if full: assert '--full' in arguments
                if fs == 'exfat': assert ('-f' in arguments) == full
                if fs in ('ext3', 'ext4'):
                    assert 'lazy_journal_init=' + str(int(not full)) in arguments[arguments.index('-E') + 1]

                with target.open('rb') as volume:
                    if offset:
                        assert volume.read(offset) == b'\xa5' * offset
                        volume.seek(offset + size); assert volume.read() == b'\xa5' * offset
                    volume.seek(offset + size * 3 // 4 + MIB)
                    sample = volume.read(4096)
                    expected = bytes(4096) if full and not fs.startswith('ext') else b'\xa5' * 4096
                    assert sample == expected, (fs, full, sample[:32])
                if fs.startswith('ext'):
                    checked = subprocess.run([tools['e2fsck'], '-fn', str(target)], capture_output=True)
                    assert checked.returncode == 0, checked.stdout + checked.stderr
                    if fs != 'ext2':
                        journal = root / 'journal'
                        subprocess.run([tools['debugfs'], '-R', 'dump <8> ' + str(journal), str(target)], capture_output=True, check=True)
                        data = journal.read_bytes()
                        assert len(data) >= MIB
                        if full: assert data[1024:] == bytes(len(data) - 1024), fs
                        # Native allocation may also zero a quick journal; the explicit
                        # lazy-init choice is checked in the captured arguments below.
                        journal.unlink()
                cases.append(f'{fs} {"full" if full else "quick"}: dirty free-data/journal and extent readback')
        for fault, expected_error in (('none', 0), ('short', 0), ('eintr', 0),
                ('write-error', errno.ENOSPC), ('write-cancel', errno.ECANCELED),
                ('read-error', errno.EIO), ('sync-error', errno.EIO), ('verify-mismatch', errno.EBADMSG)):
            prefix, length, suffix = MIB + 512, MIB + 512, 4096
            target.write_bytes(b'P' * prefix + b'D' * length + b'S' * suffix)
            result = json.loads(subprocess.check_output([zero, str(target), '1', fault, str(prefix), str(length)], text=True))
            assert result['error'] == expected_error, (fault, result)
            actual = target.read_bytes()
            assert actual[:prefix] == b'P' * prefix and actual[prefix + length:] == b'S' * suffix
            if expected_error == 0:
                assert actual[prefix:prefix + length] == bytes(length)
                assert result['written'] == result['verified'] == length and result['format_progress'] == 90
            cases.append('bounded format preclear ' + fault)
        for quick_default in (False, True):
            dirty(target, 64 * MIB)
            before = target.read_bytes()
            help_text = '--quick Perform a quick format' + ('(default)' if quick_default else '')
            body = ("if '--help' in sys.argv:\n    print(" + repr(help_text) + "); sys.exit(0)\n"
                "assert '-Q' not in sys.argv and '--full' not in sys.argv\n"
                "os.execv(" + repr(tools['mkntfs']) + ", [" + repr(tools['mkntfs']) + ", '--full'] + sys.argv[1:])")
            provider = fake_formatter(root, 'provider-default', body)
            run([ntfs, provider, target, MIB, 64 * MIB, 4096, 'FULLTEST', 'none', 'full'],
                errno.ENOTSUP if quick_default else 0)
            if quick_default: assert target.read_bytes() == before
            else:
                with target.open('rb') as volume:
                    volume.seek(49 * MIB); assert volume.read(4096) == bytes(4096)
            cases.append('NTFS refuses unsupported quick default' if quick_default else 'NTFS legacy full-default capability adapter')
        # Exact odd FAT extents include the final sector despite mkfs taking KiB.
        size = 64 * MIB + 512; dirty(target, size + 2 * MIB)
        run([fat, tools['mkfs.fat'], target, MIB, size, 512, 'FULLTEST', 'none', '32', 'full'])
        with target.open('rb') as stream:
            stream.seek(MIB + size - 512); assert stream.read(512) == bytes(512)
            assert stream.read() == b'\xa5' * MIB
        cases.append('full FAT32 exact odd-sector extent and guard')
        for full in (False, True):
            dirty(target, 32 * MIB)
            program = fake_formatter(root, 'slow-native', "time.sleep(0.4); os.execv(" + repr(tools['mkfs.fat']) + ", [" + repr(tools['mkfs.fat']) + "] + sys.argv[1:])")
            command = [fat, program, target, 0, 32 * MIB, 2048, 'FULLTEST', 'timeout', '16']
            if full: command.append('full')
            run(command, 0 if full else errno.ETIMEDOUT)
            cases.append('full ignores old absolute deadline' if full else 'quick retains old absolute deadline')
        dirty(target, 32 * MIB)
        blocked = fake_formatter(root, 'cancel-full', 'signal.signal(signal.SIGTERM, signal.SIG_IGN); time.sleep(10)')
        report = run([fat, blocked, target, 0, 32 * MIB, 2048, 'FULLTEST', 'cancel-running', '16', 'full'], errno.ECANCELED)
        assert report['SYNCS'] >= 1 and report['SIGNAL'] != 0, report
        cases.append('full cancellation kills, reaps and synchronizes formatter')
        for fault, error in (('cancel-before', errno.ECANCELED), ('readonly', errno.EINVAL), ('sync-fail', errno.EIO)):
            dirty(target, 32 * MIB)
            run([fat, tools['mkfs.fat'], target, 0, 32 * MIB, 2048, 'FULLTEST', fault, '16', 'full'], error)
            cases.append('full FAT ' + fault)
    print(json.dumps({'passed': len(cases), 'cases': cases, 'temporary_media_removed': True}, indent=2))


if __name__ == '__main__': main()
