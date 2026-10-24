#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""One batched corpus for parent resolution, immutable log replay and shared DD."""
import errno
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import uuid
from virtual_disk_fixtures import parent_locator, vhd_fixed, vhd_child, vhdx, dirty_log, MIB, TABLE, set_checksum, checksum
from test_compressed_vhdx import invoke
from test_compressed_vhd import dynamic

STAMP = 1000000000
checks = []


def checked(probe, path, logical=None, expected=0, label=''):
    before = {p: (p.stat(), hashlib.sha256(p.read_bytes()).digest()) for p in path.parent.glob('*') if p.is_file() and not p.is_symlink() and os.access(p, os.R_OK)}
    report = invoke(probe, path, 2**63 - 1)
    assert int(report['error']) == expected, (label, path.name, report)
    if logical is not None:
        assert report['sha256'] == hashlib.sha256(logical).hexdigest(), (label, report)
        assert int(report['decoded']) == len(logical), (label, report)
    else:
        assert int(report['decoded']) == 0, (label, report)
    for p, (info, digest) in before.items():
        after = p.stat()
        assert (info.st_ino, info.st_size, info.st_mtime_ns, info.st_ctime_ns) == (after.st_ino, after.st_size, after.st_mtime_ns, after.st_ctime_ns)
        assert hashlib.sha256(p.read_bytes()).digest() == digest
    checks.append(label or path.name)


def retained(probe, path, mode, expected):
    result = subprocess.check_output([probe, str(path), mode], text=True, timeout=30).split()
    assert list(map(int, result)) == [expected, 0], (path, mode, result)
    checks.append('held graph ' + mode)


def write(writer, path, raw):
    target = path.with_suffix('.target')
    target.write_bytes(b'\xa5' * (len(raw) + 4096))
    result = subprocess.check_output([writer, str(path), 'write', str(target)], text=True, timeout=60).split()
    assert list(map(int, result)) == [0, 0], result
    assert target.read_bytes() == raw + b'\xa5' * 4096
    target.unlink(); checks.append('write ' + path.name)


def vhd_stamp(path):
    os.utime(path, (STAMP, STAMP))
    return STAMP - 946684800


def relocated_parents(probe, held, root):
    raw = bytearray(b'P' * 16384); raw[510:512] = b'\x55\xaa'
    parent, identifier = vhd_fixed(raw)
    base = root / 'relocated-parent.vhd'; base.write_bytes(parent)
    stamp = vhd_stamp(base)
    locator = r'C:\old\parent.vhd'
    child, logical, _ = vhd_child(raw, locator, identifier, stamp, {2: b'M' * 512})
    path = root / 'relocated-child.vhd'; path.write_bytes(child)
    mapping = path.with_name(path.name + '.parents')
    mapping.write_text(locator + '\t' + str(base) + '\n')
    checked(probe, path, logical, label='explicit VHD drive locator relocation')
    write(held, path, logical)
    mapping.write_text(locator + '\t' + str(base) + '\n' + locator + '\t' + str(base) + '\n')
    checked(probe, path, expected=errno.EBADMSG, label='duplicate relocation rejected')
    mapping.write_text('@parent:\t' + str(base) + '\n')
    absolute = bytearray(child); absolute[1088:1092] = b'W2ku'
    struct.pack_into('>I', absolute, 548, checksum(absolute[512:1536], 36))
    path.write_bytes(absolute)
    checked(probe, path, logical, label='VHD absolute locator with explicit parent selection')
    mapping.unlink(); mapping.symlink_to(base)
    checked(probe, path, expected=errno.ELOOP, label='relocation symlink rejected')
    mapping.unlink()
    raw = bytearray(b'Q' * MIB); raw[510:512] = b'\x55\xaa'
    parent, _, identifier = vhdx(raw)
    base = root / 'relocated-parent.vhdx'; base.write_bytes(parent)
    win32 = r'\\?\C:\old\parent.vhdx'
    volume = r'\\?\Volume{01234567-89ab-cdef-0123-456789abcdef}\parent.vhdx'
    for key, locator in [('volume_path', volume), ('absolute_win32_path', win32)]:
        child, logical, _ = vhdx(raw, 'missing.vhdx', identifier)
        metadata = parent_locator(None, identifier, [(key, locator)])
        start = 2 * MIB + TABLE + 128
        child[start:start + len(metadata)] = metadata
        struct.pack_into('<I', child, 2 * MIB + 32 + 32 * 5 + 20, len(metadata))
        path = root / ('relocated-' + key + '.vhdx'); path.write_bytes(child)
        mapping = path.with_name(path.name + '.parents'); mapping.write_text(locator + '\t' + str(base) + '\n')
        checked(probe, path, logical, label='explicit VHDX ' + key)
        write(held, path, logical)
    child, logical, _ = vhdx(raw, base.name, uuid.uuid4())
    metadata = parent_locator(base.name, uuid.uuid4(), [('parent_linkage2', '{' + str(identifier) + '}'), ('relative_path', base.name)])
    child[start:start + len(metadata)] = metadata
    struct.pack_into('<I', child, 2 * MIB + 32 + 32 * 5 + 20, len(metadata))
    path = root / 'dual-linkage.vhdx'; path.write_bytes(child)
    checked(probe, path, logical, label='VHDX merge-time alternate linkage')
    write(held, path, logical)


def main():
    probe, held = sys.argv[1:]
    writer = held
    with tempfile.TemporaryDirectory(prefix='rufus-chain-') as directory:
        root = Path(directory)
        raw = bytearray(b'A' * 16384); raw[510:512] = b'\x55\xaa'
        parent, pid = vhd_fixed(raw); base = root / 'parent-\u03b1.vhd'; base.write_bytes(parent)
        stamp = vhd_stamp(base)
        child, logical, cid = vhd_child(raw, base.name, pid, stamp, {0: b'B' * 512, 3: b'C' * 512, 31: b'D' * 512})
        path = root / 'child.vhd'; path.write_bytes(child)
        checked(probe, path, logical, label='VHD bitmap inheritance Unicode')
        retained(held, path, 'none', 0); write(writer, path, logical)
        stamp2 = vhd_stamp(path)
        grand, output, _ = vhd_child(logical, path.name, cid, stamp2, {3: b'E' * 512, 20: b'F' * 512}, block=8192)
        top = root / 'grand.vhd'; top.write_bytes(grand)
        checked(probe, top, output, label='VHD multi-level different block sizes')
        missing, _, _ = vhd_child(raw, 'missing.vhd', pid, stamp)
        top.write_bytes(missing); checked(probe, top, expected=errno.ENOENT, label='missing VHD parent')
        for offset, value, error, label in [(552, b'\x11' * 16, errno.ESTALE, 'parent UUID'), (568, struct.pack('>I', 1), errno.ESTALE, 'parent timestamp'), (1092, struct.pack('>I', 0), errno.EBADMSG, 'locator extent'), (1096, struct.pack('>I', 70000), errno.EBADMSG, 'locator length')]:
            changed = child.copy(); changed[offset:offset + len(value)] = value
            struct.pack_into('>I', changed, 548, checksum(changed[512:1536], 36))
            top.write_bytes(changed); checked(probe, top, expected=error, label=label)
        top.write_bytes(child)
        base.chmod(0); checked(probe, top, expected=errno.EACCES, label='unreadable parent'); base.chmod(0o600)
        base.unlink(); base.symlink_to('child.vhd'); checked(probe, top, expected=errno.ELOOP, label='symlink parent'); base.unlink(); base.write_bytes(parent); vhd_stamp(base)
        # A self-referencing chain is refused before recursively interpreting its header.
        cycle, _, _ = vhd_child(raw, top.name, pid, stamp); top.write_bytes(cycle)
        checked(probe, top, expected=errno.ELOOP, label='parent cycle')
        fallback, fallback_output, _ = vhd_child(raw, 'absent/' + base.name, pid, stamp)
        top.write_bytes(fallback); checked(probe, top, fallback_output, label='VHD missing locator falls back to parent name')
        absolute, _, _ = vhd_child(raw, 'C:\\images\\parent.vhd', pid, stamp)
        top.write_bytes(absolute); checked(probe, top, expected=errno.ENOTSUP, label='Windows absolute parent path')
        top.write_bytes(child); retained(held, top, 'mutate', errno.ESTALE); vhd_stamp(base)
        retained(held, top, 'replace', errno.ESTALE); base.unlink(); base.write_bytes(parent); vhd_stamp(base)
        # Dynamic parent has sparse holes and clear bitmap bits despite stored bytes.
        sparse = bytearray(8192); sparse[:512] = b'A' * 512; sparse[1536:2048] = b'B' * 512
        dyn = root / 'dynamic.vhd'; dyn.write_bytes(dynamic(sparse)); dstamp = vhd_stamp(dyn)
        parent_id = uuid.UUID(int=0)
        dchild, dout, _ = vhd_child(sparse, dyn.name, parent_id, dstamp, {5: b'C' * 512})
        top.write_bytes(dchild); checked(probe, top, dout, label='VHD sparse dynamic parent')
        # Windows relative separators with a parent-directory component.
        nested = root / 'nested'; nested.mkdir(); nested_top = nested / 'child.vhd'
        relative, rout, _ = vhd_child(sparse, '../dynamic.vhd', parent_id, dstamp)
        nested_top.write_bytes(relative); checked(probe, nested_top, rout, label='relative parent directory')
        # Retained graph bound includes the top source; failure closes every opened parent.
        previous, previous_id, content = dyn, parent_id, sparse
        for index in range(32):
            stamp = vhd_stamp(previous)
            chain, _, ident = vhd_child(content, previous.name, previous_id, stamp)
            entry = root / ('depth-%02d.vhd' % index); entry.write_bytes(chain)
            previous, previous_id = entry, ident
        retained(held, previous, 'none', errno.ELOOP)
        for entry in root.glob('depth-*.vhd'): entry.unlink()
        # VHDX: partial present sectors, explicitly zeroed blocks, full overrides and two levels.
        raw = bytearray(b'P' * (3 * MIB + 4096)); raw[510:512] = b'\x55\xaa'
        parent, output, pid = vhdx(raw); base = root / 'parent.vhdx'; base.write_bytes(parent)
        changes = {0: b'X' * 512, 7: b'Y' * 512, 2048: b'Z' * 512, 6144: b'Q' * 512}
        child, logical, cid = vhdx(raw, base.name, pid, changes, {1: 6, 2: 2})
        path = root / 'child.vhdx'; path.write_bytes(child)
        checked(probe, path, logical, label='VHDX partial full zero trailing bitmap'); write(writer, path, logical)
        retained(held, path, 'none', 0); retained(held, path, 'cancel', errno.ECANCELED)
        grand, final, _ = vhdx(logical, path.name, cid, {5: b'G' * 512}, block=2 * MIB)
        top = root / 'grand.vhdx'; top.write_bytes(grand); checked(probe, top, final, label='VHDX multi-level block geometry')
        bad, _, _ = vhdx(raw, base.name, uuid.uuid4()); top.write_bytes(bad); checked(probe, top, expected=errno.ESTALE, label='VHDX linkage')
        bad, _, _ = vhdx(raw, 'missing.vhdx', pid); top.write_bytes(bad); checked(probe, top, expected=errno.ENOENT, label='VHDX missing')
        for offset, fmt, value, error, label in [(3 * MIB, '<Q', 7, errno.EBADMSG, 'partial without bitmap'), (2 * MIB + TABLE + 48, '<I', 4096, errno.EBADMSG, 'logical sector mismatch'), (2 * MIB + TABLE + 128, '<B', 0, errno.ENOTSUP, 'locator type'), (2 * MIB + TABLE + 128 + 20, '<I', 1, errno.EBADMSG, 'locator bounds')]:
            bad = child.copy(); struct.pack_into(fmt, bad, offset, value); top.write_bytes(bad)
            checked(probe, top, expected=error, label=label)
        # A parent may itself need log replay, and its original log remains dirty.
        dirty_parent = dirty_log(parent, [(0, 30, 0, [(4 * MIB, b'H' * 4096)])])
        base.write_bytes(dirty_parent)
        inherited = bytearray(raw); inherited[:4096] = b'H' * 4096
        inherited_child, _, _ = vhdx(raw, base.name, pid)
        top.write_bytes(inherited_child); checked(probe, top, inherited, label='dirty parent replay before child inheritance')
        base.write_bytes(parent)
        # 4K logical sectors exercise LSB bitmap ownership and an unallocated final block.
        raw4 = bytes(b'K' * (2 * MIB)); p4, _, id4 = vhdx(raw4, sector=4096); base4 = root / 'parent4.vhdx'; base4.write_bytes(p4)
        c4, o4, _ = vhdx(raw4, base4.name, id4, {0: b'W' * 4096, 255: b'J' * 4096}, sector=4096)
        top.write_bytes(c4); checked(probe, top, o4, label='4096 logical sectors')
        # Log replay independent of parent inheritance; payload before replay is deliberately stale.
        clean, logical, _ = vhdx(raw)
        payload = 4 * MIB
        vectors = [([(0, 5, 0, [(payload, b'R' * 4096)])], {0: b'R' * 4096}, 'data descriptor'),
                   ([(0, 5, 0, [(payload, 12288), (payload + 4096, b'S' * 4096)])], {0: bytes(4096), 4096: b'S' * 4096, 8192: bytes(4096)}, 'zero range split'),
                   ([(0, 5, 0, [(payload, b'R' * 4096)]), (8192, 6, 0, [(payload, b'T' * 4096)])], {0: b'T' * 4096}, 'latest overlapping entry'),
                   ([(MIB - 4096, 5, MIB - 4096, [(payload, b'U' * 4096)]), (4096, 6, MIB - 4096, [(payload + 4096, b'V' * 4096)])], {0: b'U' * 4096, 4096: b'V' * 4096}, 'circular wrap'),
                   ([(0, 5, 0, []), (4096, 8, 4096, [(payload, b'N' * 4096)])], {0: b'N' * 4096}, 'sequence gap new chain'),
                   ([(0, 5, 8192, []), (4096, 10, 4096, [(payload, b'L' * 4096)])], {0: b'L' * 4096}, 'invalid chain does not hide later entry')]
        for records, patches, label in vectors:
            dirty = dirty_log(clean, records); result = bytearray(logical)
            for offset, data in patches.items(): result[offset:offset + len(data)] = data
            top.write_bytes(dirty); checked(probe, top, result, label='log ' + label)
        # Replay is applied before metadata and BAT interpretation and can virtually extend a truncated write.
        extended = clean[:4 * MIB]
        bat = bytearray(clean[3 * MIB:3 * MIB + 4096]); struct.pack_into('<Q', bat, 0, 4 * MIB | 6)
        for i in range(1, 4): struct.pack_into('<Q', bat, 8 * i, 2)
        metadata = clean[2 * MIB + TABLE:2 * MIB + TABLE + 4096]
        updates = [(3 * MIB, bat), (2 * MIB + TABLE, metadata), (4 * MIB, b'E' * 4096)]
        dirty = dirty_log(extended, [(0, 20, 0, updates)], last=5 * MIB)
        result = bytearray(len(raw)); result[:4096] = b'E' * 4096
        top.write_bytes(dirty); checked(probe, top, result, label='log virtual extension BAT metadata')
        write(writer, top, result)
        # Combine parent and dirty child replay.
        dirty = dirty_log(child, [(0, 2, 0, [(4 * MIB, b'D' * 4096)])]); result = bytearray(logical)
        # child first block bitmap owns only sectors0,7; logged bytes change those only.
        result = bytearray(vhdx(raw, base.name, pid, changes, {1: 6, 2: 2})[1])
        result[:512] = b'D' * 512; result[7 * 512:8 * 512] = b'D' * 512
        top.write_bytes(dirty); checked(probe, top, result, label='dirty differencing child'); write(writer, top, result)
        valid = dirty_log(clean, [(0, 5, 0, [(payload, b'R' * 4096)])])
        corruptions = [(MIB + 4, '<I', 0, False, 'CRC'), (MIB + 32, '<B', 0, True, 'foreign GUID'),
                       (MIB + 12, '<I', 123, True, 'unaligned tail'), (MIB + 12, '<I', 16384, True, 'tail absent'),
                       (MIB + 16, '<Q', 0, True, 'zero sequence'), (MIB + 24, '<I', 100000, True, 'descriptor overflow'),
                       (MIB + 48, '<Q', len(clean) + MIB, True, 'truncated file'), (MIB + 80, '<Q', 65536, True, 'header target'),
                       (MIB + 80, '<Q', MIB, True, 'log target'), (MIB + 88, '<Q', 10, True, 'descriptor sequence'),
                       (MIB + 4096 + 4092, '<I', 1, True, 'torn data sector')]
        for offset, fmt, value, recalc, label in corruptions:
            bad = valid.copy(); struct.pack_into(fmt, bad, offset, value)
            if recalc: set_checksum(bad, MIB, 8192)
            top.write_bytes(bad); checked(probe, top, expected=errno.EBADMSG, label='invalid log ' + label)
        bad = valid.copy()
        for offset in (TABLE, 2 * TABLE): struct.pack_into('<H', bad, offset + 64, 1); set_checksum(bad, offset, 4096)
        top.write_bytes(bad); checked(probe, top, expected=errno.ENOTSUP, label='unknown log version')
        top.write_bytes(valid); retained(held, top, 'cancel', errno.ECANCELED)
        relocated_parents(probe, held, root)
        # Independent qemu-img replay on a disposable COPY, with original still dirty and unchanged.
        oracle = shutil.which('qemu-img')
        if oracle:
            top.write_bytes(valid); copy = root / 'oracle.vhdx'; copy.write_bytes(valid); out = root / 'oracle.raw'
            subprocess.run([oracle, 'check', '-r', 'all', '-f', 'vhdx', str(copy)], check=True, capture_output=True)
            subprocess.run([oracle, 'convert', '-f', 'vhdx', '-O', 'raw', str(copy), str(out)], check=True, capture_output=True)
            result = bytearray(raw); result[:4096] = b'R' * 4096
            assert out.read_bytes() == result
            assert top.read_bytes() == valid; checks.append('independent QEMU log repair copy')
    print(json.dumps({'passed': len(checks), 'checks': checks}, indent=2))


if __name__ == '__main__':
    main()
