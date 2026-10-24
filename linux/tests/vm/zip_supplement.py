#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Independent supplementary ZIP inputs for the combined disposable guest."""
import hashlib
import json
import shutil
import struct
import warnings
import zipfile


def stage_fixture(base, init):
    valid = base / 'supplement.zip'
    entries = [('drivers/', b''), ('keep.txt', b'ZIP FIRST\n'),
               ('drivers/fișiere.txt', b'Unicode supplement\n'),
               ('keep.txt', b'ZIP REPLACEMENT\n'), ('empty.txt', b'')]
    with warnings.catch_warnings():
        warnings.simplefilter('ignore', UserWarning)
        with zipfile.ZipFile(valid, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
            for name, data in entries: archive.writestr(name, data)
    packed = bytearray(valid.read_bytes())
    with zipfile.ZipFile(valid) as archive:
        member = archive.infolist()[2]
        changed_crc = member.CRC ^ 1
        struct.pack_into('<I', packed, member.header_offset + 14, changed_crc)
    position = packed.index(b'PK\x01\x02')
    for unused in range(2): position += 46 + sum(struct.unpack_from('<3H', packed, position + 28))
    struct.pack_into('<I', packed, position + 16, changed_crc)
    broken = base / 'bad-supplement.zip'; broken.write_bytes(packed)
    assert zipfile.ZipFile(valid).testzip() is None
    assert zipfile.ZipFile(broken).testzip() == 'drivers/fișiere.txt'
    for source in (valid, broken): shutil.copy2(source, init / source.name)
    for name, data in entries:
        path = base / 'expected' / name
        if name.endswith('/'): path.mkdir(parents=True, exist_ok=True)
        else:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
    manifest = {'entries': len(entries), 'files': 4, 'unpacked': sum(len(data) for name, data in entries),
                'sha256': {path.name: hashlib.sha256(path.read_bytes()).hexdigest() for path in (valid, broken)}}
    (base / 'archive-inputs.json').write_text(json.dumps(manifest, indent=2) + '\n')
