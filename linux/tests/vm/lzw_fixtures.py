# SPDX-License-Identifier: GPL-3.0-or-later
"""Independent host fixtures staged only into the guarded LZW test guest."""
import errno
import hashlib
import json
import subprocess

from lzw_reference import encode, encode_clear_tail, encode_literals


def stage_lzw_fixtures(destination, encoder):
    destination.mkdir(parents=True)
    state = 28
    raw = bytearray()
    for _ in range(1024 * 1024 + 37):
        state ^= (state << 13) & 0xffffffff
        state ^= state >> 17
        state ^= (state << 5) & 0xffffffff
        raw.append(state & 255)
    raw[510:512] = b'\x55\xaa'
    raw = bytes(raw)
    repeated = b'A' * 510 + b'\x55\xaa' + b'A' * (len(raw) - 512)
    (destination / 'repeated.img').write_bytes(repeated)
    rows = []

    def add(name, packed, output, error=0, kind='ordinary', wire_original=False):
        independent = subprocess.run([str(encoder), '-dc'], input=packed,
                                     capture_output=True, timeout=30)
        if output is None:
            assert independent.returncode != 0, name
        else:
            assert independent.returncode == 0 and independent.stdout == output, name
        path = destination / (name + '.Z')
        path.write_bytes(packed)
        rows.append({'name': name, 'file': path.name, 'error': error, 'kind': kind,
                     'wire_original': wire_original, 'stored': len(packed),
                     'stored_sha256': hashlib.sha256(packed).hexdigest(),
                     'decoded': len(raw) if wire_original else len(output) if output is not None else len(raw),
                     'sha256': hashlib.sha256(raw if wire_original or output is None else output).hexdigest()})

    for block in (True, False):
        for bits in (9, 10, 12, 16):
            add(f'{"block" if block else "nonblock"}-{bits}', encode(raw, bits, block), raw)
    clear = encode_clear_tail(raw)
    add('clear-tail', clear, raw)
    literal = encode_literals(raw)
    add('literal', literal, raw)
    add('incomplete-code', encode_literals(raw[:512], 9) + b'\x7f', raw[:512], kind='prefix')
    add('appended-code', encode_literals(raw[:512], 9) + bytes(2), raw[:512] + b'\0', kind='appended')
    add('truncated-prefix', encode_literals(raw[:513], 9)[:-1], raw[:512], kind='prefix')
    add('shorter-stream', literal[:-1], raw[:-1], error=errno.ESTALE, wire_original=True)
    changed = bytearray(raw)
    changed_index = len(raw) // 2
    changed[changed_index] ^= 1
    alternate = encode_literals(changed)
    differences = [(i, a ^ b) for i, (a, b) in enumerate(zip(literal, alternate)) if a != b]
    assert len(literal) == len(alternate) and len(differences) == 1
    flip_offset, flip_xor = differences[0]
    assert flip_xor != 0 and flip_xor & (flip_xor - 1) == 0
    add('valid-hash-change', alternate, changed, error=errno.ESTALE, wire_original=True)
    add('repeated', encode(repeated), repeated, kind='repeated')
    bad = bytearray(clear)
    bad[-2] |= 0xf0
    bad[-1] |= 0x1f
    for name, packed in (('short-header', b'\x1f\x9d'), ('bad-bits', b'\x1f\x9d\x91' + clear[3:]),
                         ('first-code', b'\x1f\x9d\x90\xff\x01'), ('late-code', bad)):
        add(name, packed, None, error=errno.EBADMSG)
    with encoder.open('rb') as source:
        encoder_sha256 = hashlib.file_digest(source, 'sha256').hexdigest()
    manifest = {'cases': rows, 'ordinary_sha256': hashlib.sha256(raw).hexdigest(),
                'flip_offset': flip_offset, 'flip_xor': flip_xor, 'decoded_flip_index': changed_index,
                'independent_decoder': str(encoder), 'independent_decoder_sha256': encoder_sha256}
    (destination / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    return manifest
