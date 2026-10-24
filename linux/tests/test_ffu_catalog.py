#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Grouped catalog signatures, frozen source identity and prewrite refusals.

OpenSSL CLI creates CMS signatures independently of the PKCS7 reader. Only the
encapsulation changes: Authenticode CTLs wrap sequence contents rather than an
OCTET STRING. All keys and target files are disposable and unprivileged.
"""
import errno
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
from ffu_fixture import BLOCK, make_ffu

CTL_OID = '1.3.6.1.4.1.311.10.1'


def der(tag, body):
    size = len(body)
    width = (size.bit_length() + 7) // 8
    length = bytes([size]) if size < 128 else bytes([128 + width]) + size.to_bytes(width, 'big')
    return bytes([tag]) + length + body


def children(data):
    result, position = [], 0
    while position < len(data):
        tag, size = data[position:position + 2]
        position += 2
        if size & 128:
            width = size & 127
            assert width and width <= 4
            size = int.from_bytes(data[position:position + width], 'big')
            position += width
        assert position + size <= len(data)
        result.append((tag, data[position:position + size]))
        position += size
    return result


def encode(parts):
    return b''.join(der(tag, body) for tag, body in parts)


def signed_parts(catalog):
    outer = children(children(catalog)[0][1])
    signed = children(children(outer[1][1])[0][1])
    return outer, signed


def replace_signed(catalog, signed):
    outer, unused = signed_parts(catalog)
    outer[1] = (0xa0, der(0x30, encode(signed)))
    return der(0x30, encode(outer))


def unsigned_catalog():
    template = (Path(__file__).parents[1] / 'platform/ffu_catalog.inc').read_text()
    prefix, suffix = template.split('static const unsigned char ffuCatalogSuffix[]')
    return bytes.fromhex(' '.join(re.findall(r'0x([0-9A-Fa-f]{2})', prefix))) + bytes(20) + bytes.fromhex(
        ' '.join(re.findall(r'0x([0-9A-Fa-f]{2})', suffix)))


def command(arguments):
    subprocess.run(arguments, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)


def make_signer(root, name, algorithm='rsa:2048'):
    key, cert = root / (name + '.key'), root / (name + '.pem')
    options = ['-newkey', algorithm] if algorithm != 'ec' else ['-newkey', 'ec', '-pkeyopt', 'ec_paramgen_curve:prime256v1']
    command(['openssl', 'req', '-x509', *options, '-nodes', '-subj', '/CN=' + name,
             '-keyout', str(key), '-out', str(cert), '-days', '1'])
    return key, cert


def sign_catalog(root, catalog, signers, digest='sha256', oid=CTL_OID, extra=None):
    unused, signed = signed_parts(catalog)
    content_info = children(signed[2][1])
    content = children(content_info[1][1])[0][1]
    source, output = root / 'content.bin', root / 'signed.der'
    source.write_bytes(content)
    args = ['openssl', 'cms', '-sign', '-binary', '-nodetach', '-nosmimecap', '-md', digest,
            '-econtent_type', oid, '-in', str(source), '-outform', 'DER', '-out', str(output)]
    for key, cert in signers:
        args += ['-signer', str(cert), '-inkey', str(key)]
    if extra is not None: args += ['-certfile', str(extra)]
    command(args)
    cms = output.read_bytes()
    unused, signed = signed_parts(cms)
    content_info = children(signed[2][1])
    assert children(content_info[1][1]) == [(4, content)]
    content_info[1] = (0xa0, der(0x30, content))
    signed[2] = (0x30, encode(content_info))
    return replace_signed(cms, signed)



def mixed_production_signers(root, production, signer):
    """Optional real catalog qualification supplied from an owner's Windows ISO."""
    test = sign_catalog(root, production, [signer])
    unused, original = signed_parts(production)
    unused, other = signed_parts(test)
    parts = list(original)
    parts[1] = (0x31, encode(list(dict.fromkeys(children(original[1][1]) + children(other[1][1])))))
    assert original[3][0] == other[3][0] == 0xa0
    parts[3] = (0xa0, encode(list(dict.fromkeys(children(original[3][1]) + children(other[3][1])))))
    genuine, disposable = original[-1][1], other[-1][1]
    cases = [(production, errno.ENOTSUP, 'genuine production catalog')]
    for first, second, error, name in (
        (genuine, disposable, errno.ENOTSUP, 'production signer precedes valid test signer'),
        (disposable, genuine, 0, 'valid test signer precedes production signer'),
        (corrupt_signature(genuine), disposable, 0, 'invalid production signer before valid test signer'),
        (corrupt_signature(disposable), genuine, errno.ENOTSUP, 'invalid test signer before production signer'),
        (corrupt_signature(genuine), corrupt_signature(disposable), errno.EKEYREJECTED, 'both signatures invalid')):
        parts[-1] = (0x31, first + second)
        cases.append((replace_signed(production, parts), error, name))
    return cases


def corrupt_signature(infos):
    result = children(infos)
    fields = children(result[0][1])
    index = next(index for index, field in enumerate(fields) if field[0] == 4)
    fields[index] = (4, bytes(len(fields[index][1])))
    result[0] = (0x30, encode(fields))
    return encode(result)


def insert_catalog(packed, catalog):
    unused, unused, chunk_kib, unused, old_size, hashes_size = struct.unpack_from('<I12sIIII', packed)
    chunk = chunk_kib * 1024
    old_image = (32 + old_size + hashes_size + chunk - 1) // chunk * chunk
    hashes = packed[32 + old_size:32 + old_size + hashes_size]
    security = bytearray(packed[:32])
    struct.pack_into('<I', security, 24, len(catalog))
    security += catalog + hashes
    security += bytes(-len(security) % chunk)
    return security + packed[old_image:]


def main():
    probe, reader, writer = sys.argv[1:]
    cases = []
    with tempfile.TemporaryDirectory(prefix='rufus-catalog-') as temporary:
        root = Path(temporary)
        path, source, target = root / 'test.cat', root / 'source.ffu', root / 'target.img'
        unsigned = unsigned_catalog()
        unused, empty_usage_parts = signed_parts(unsigned)
        info = children(empty_usage_parts[2][1])
        ctl = children(children(info[1][1])[0][1])
        ctl[0] = (0x30, b'')
        info[1] = (0xa0, der(0x30, encode(ctl)))
        empty_usage_parts[2] = (0x30, encode(info))
        empty_usage = replace_signed(unsigned, empty_usage_parts)
        first, second = make_signer(root, 'test-one'), make_signer(root, 'test-two')
        accepted = [(b'', False, 'absent legacy catalog'), (unsigned, False, 'unsigned CTL'),
                    (empty_usage, False, 'CTL with empty usage list')]
        for digest in ('sha1', 'sha256', 'sha384'):
            accepted.append((sign_catalog(root, unsigned, [first], digest), True, 'RSA ' + digest))
        ec = make_signer(root, 'test-ec', 'ec')
        accepted.append((sign_catalog(root, unsigned, [ec]), True, 'ECDSA SHA256'))
        expired_cert = root / 'expired.pem'
        # The CA command supports explicit historical dates on OpenSSL 3.0 too.
        (root / 'index').write_text('')
        (root / 'serial').write_text('10\n')
        config = root / 'ca.cnf'
        config.write_text(f'[ca]\ndefault_ca=test\n[test]\ndatabase={root}/index\n'
            f'serial={root}/serial\nnew_certs_dir={root}\nprivate_key={first[0]}\n'
            f'certificate={first[1]}\ndefault_md=sha256\npolicy=policy\n'
            '[policy]\ncommonName=supplied\n')
        request = root / 'expired.csr'
        command(['openssl', 'req', '-new', '-key', str(first[0]), '-subj', '/CN=expired-test', '-out', str(request)])
        command(['openssl', 'ca', '-batch', '-selfsign', '-notext', '-config', str(config),
                 '-in', str(request), '-startdate', '20000101000000Z', '-enddate', '20010101000000Z',
                 '-out', str(expired_cert)])
        accepted.append((sign_catalog(root, unsigned, [(first[0], expired_cert)]), True, 'expired test signer accepted'))
        signed = sign_catalog(root, unsigned, [first])
        multi = sign_catalog(root, unsigned, [first, second])
        unused, parts = signed_parts(multi)
        infos = children(parts[-1][1])
        broken = children(infos[0][1])
        assert broken[-1][0] == 4
        broken[-1] = (4, broken[-1][1][:-1] + bytes([broken[-1][1][-1] ^ 1]))
        parts[-1] = (0x31, der(0x30, encode(broken)) + der(*infos[1]))
        accepted.append((replace_signed(multi, parts), True, 'first invalid signer then valid signer'))
        # An unrelated production root in the bag must not classify a test chain.
        production = (Path(__file__).parents[1] / 'platform/ffu_production_root.inc').read_text()
        root_der = bytes.fromhex(' '.join(re.findall(r'0x([0-9A-Fa-f]{2})', production)))
        extra = root / 'extra.pem'
        extra.write_bytes(subprocess.check_output(['openssl', 'x509', '-inform', 'DER', '-outform', 'PEM'], input=root_der))
        accepted.append((sign_catalog(root, unsigned, [first], extra=extra), True, 'unrelated production certificate'))
        production_cases = []
        production_path = os.environ.get('RUFUS_TEST_FFU_PRODUCTION_CAT')
        if production_path:
            production_cases = mixed_production_signers(root, Path(production_path).read_bytes(), first)
            accepted += [(catalog, True, name) for catalog, error, name in production_cases if error == 0]
        for catalog, is_signed, name in accepted:
            path.write_bytes(catalog)
            report = json.loads(subprocess.check_output([probe, str(path)], text=True))
            expected_hash = hashlib.sha256(catalog).hexdigest() if catalog else '00' * 32
            assert report == {'error': 0, 'signed': is_signed, 'sha256': expected_hash}, (name, report)
            cases.append(name)
        invalid = [(unsigned[:-1], errno.EBADMSG, 'truncated DER'),
                   (unsigned + b'\0', errno.EBADMSG, 'trailing DER'),
                   (bytes(16 * 1024 * 1024 + 1), errno.ENOTSUP, 'catalog allocation budget'),
                   (sign_catalog(root, unsigned, [first], oid='1.2.3.4'), errno.ENOTSUP, 'non-CTL signed content')]
        unused, parts = signed_parts(signed)
        infos = children(parts[-1][1]); fields = children(infos[0][1])
        fields[-1] = (4, bytes(len(fields[-1][1])))
        parts[-1] = (0x31, der(0x30, encode(fields)))
        invalid.append((replace_signed(signed, parts), errno.EKEYREJECTED, 'invalid signature'))
        unused, parts = signed_parts(signed)
        parts = [part for part in parts if part[0] != 0xa0]
        invalid.append((replace_signed(signed, parts), errno.EKEYREJECTED, 'missing signer certificate'))
        tampered = bytearray(signed)
        marker = 'HashTable.blob'.encode('utf-16le')
        tampered[tampered.index(marker)] ^= 1
        invalid.append((tampered, errno.EKEYREJECTED, 'signed content changed'))
        invalid += [(catalog, error, name) for catalog, error, name in production_cases if error != 0]
        for catalog, expected, name in invalid:
            path.write_bytes(catalog)
            report = json.loads(subprocess.check_output([probe, str(path)], text=True))
            assert report == {'error': expected, 'signed': False, 'sha256': '00' * 32}, (name, report)
            cases.append(name)
        path.write_bytes(signed)
        report = json.loads(subprocess.check_output([probe, str(path), 'cancel'], text=True))
        assert report == {'error': errno.ECANCELED, 'signed': False, 'sha256': '00' * 32}
        cases.append('cancellation before signature work')
        packed, logical, unused = make_ffu([(b'A' * BLOCK, [(0, 0)]), (b'B' * BLOCK, [(2, 0)])])
        poison = bytes([0xa5]) * len(logical)
        for catalog, unused, name in accepted:
            source.write_bytes(insert_catalog(packed, catalog))
            target.write_bytes(poison)
            result = json.loads(subprocess.check_output([writer, str(source), str(target), '512', '1', str(2**63 - 1), 'none'], text=True))
            expected = bytearray(poison); expected[:BLOCK] = b'A' * BLOCK; expected[-BLOCK:] = b'B' * BLOCK
            assert result['error'] == 0 and result['written'] == result['verified'] == 2 * BLOCK, result
            assert target.read_bytes() == expected
            cases.append('FFU write/readback: ' + name)
        for catalog, expected, name in invalid:
            source.write_bytes(insert_catalog(packed, catalog)); target.write_bytes(poison)
            listing = json.loads(subprocess.check_output([reader, 'ffu-stores', str(source)], text=True))
            assert listing['error'] == expected and not listing['stores'], (name, listing)
            result = json.loads(subprocess.check_output([writer, str(source), str(target), '512', '1', str(2**63 - 1), 'none'], text=True))
            assert result['error'] == expected and result['written'] == 0 and target.read_bytes() == poison, (name, result)
            cases.append('FFU prewrite refusal: ' + name)
        # Catalog identity is frozen even when both catalogs are valid and the
        # decoded image, hash table, sizes and prefix are identical.
        replacement = bytearray(unsigned)
        replacement[replacement.index(b'200110121227Z')] = ord('1')
        source.write_bytes(insert_catalog(packed, unsigned))
        Path(str(source) + '.replacement.ffu').write_bytes(insert_catalog(packed, replacement))
        target.write_bytes(poison)
        result = json.loads(subprocess.check_output([writer, str(source), str(target), '512', '1', str(2**63 - 1), 'preflight-layout-substitute'], text=True))
        assert result['error'] == errno.ESTALE and result['written'] == 0 and target.read_bytes() == poison, result
        cases.append('valid catalog substitution changes frozen layout')
    assert not root.exists()
    print(json.dumps({'passed': len(cases), 'cases': cases, 'temporary_media_removed': True}, indent=2))


if __name__ == '__main__':
    main()
