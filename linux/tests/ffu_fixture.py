#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Independent FFU serializer from the published structures; test use only."""
import hashlib
import struct

BLOCK = 131072


def xpress_literals(data):
    return b''.join(bytes(4) + data[i:i + 32] for i in range(0, len(data), 32))


def lznt1_literals(data):
    return b''.join(struct.pack('<H', 0x3000 | (len(data[i:i + 4096]) - 1)) + data[i:i + 4096]
                    for i in range(0, len(data), 4096))


def huffman_literals(data):
    # A complete canonical 9-bit tree for all 512 symbols; little-endian words,
    # high-bit-first codes, and the 16-bit lookahead padding specified by MS-XCA.
    result = bytearray()
    for start in range(0, len(data), 65536):
        result += bytes([0x99]) * 256
        bits = count = 0
        for byte in data[start:start + 65536]:
            bits = (bits << 9) | byte; count += 9
            if count >= 16:
                count -= 16; result += struct.pack('<H', bits >> count)
                bits &= (1 << count) - 1
        if count: result += struct.pack('<H', bits << (16 - count))
        result += bytes(2)
    return bytes(result)


def make_ffu(payloads, disk_blocks=8, version=1, algorithm=0, sector=512, block=BLOCK,
             extended=False, disk_bytes=None, partial=False, validations=(), manifest=None):
    """payloads: (decoded data, [(access method, block index)], optional packed data)."""
    disk_bytes = disk_blocks * block if disk_bytes is None else disk_bytes
    if manifest is None:
        manifest = (f'[FullFlash]\r\nOSVersion=10.0.22621.0\r\n\r\n[Store]\r\n'
                    f'MinSectorCount = {disk_bytes // sector}\r\nSectorSize = {sector}\r\n').encode()
    image_header = struct.pack('<I12sII', 28 if extended else 24, b'ImageFlash  ', len(manifest), block // 1024)
    if extended:
        image_header += struct.pack('<I', 1)
    image = image_header + manifest
    if extended:
        strings = (b'Fixture', b'Family', b'Product', b'1', b'SKU', b'Board', b'Model')
        image += struct.pack('<7I', *(len(value) for value in strings)) + b''.join(strings)
    image += bytes(-len(image) % block)
    descriptors = bytearray()
    chunks = []
    logical = bytearray(disk_bytes)
    for entry in payloads:
        data, locations = entry[:2]
        assert len(data) % block == 0
        count = len(data) // block
        packed = entry[2] if len(entry) == 3 else (lznt1_literals(data) if algorithm == 2 else huffman_literals(data) if algorithm == 4 else xpress_literals(data) if algorithm else data)
        descriptors += struct.pack('<II', len(locations), count)
        if version == 3:
            descriptors += struct.pack('<I', len(packed))
        for method, index in locations:
            descriptors += struct.pack('<II', method, index)
            start = index * block if method == 0 else disk_bytes - (index + 1) * block
            logical[start:start + count * block] = data
        chunks.append((data, packed))
    validation = b''.join(struct.pack('<III', index, offset, len(data)) + data
                          for index, offset, data in validations)
    store = struct.pack('<IHHHH192s11I', int(partial), 2 if version == 2 else 1, 0,
                        3 if version == 3 else 2, 0, bytes(192), block,
                        len(payloads), len(descriptors), len(validations), len(validation), 0, 0, 0, 0, 0, len(payloads))
    if version == 3:
        store += struct.pack('<I', algorithm)
    elif version == 2:
        path = 'VenHw(Fixture)'.encode('utf-16le')
        store += struct.pack('<HHQH', 1, 1, sum(len(packed) for data, packed in chunks), len(path) // 2) + path
    validation_in_store = len(store)
    descriptor_in_store = len(store) + len(validation)
    store += validation + descriptors
    store += bytes(-len(store) % block)
    metadata = image + store
    hashes = b''.join(hashlib.sha256(metadata[i:i + block]).digest() for i in range(0, len(metadata), block))
    hashes += b''.join(hashlib.sha256(data[i:i + block]).digest()
                       for data, packed in chunks for i in range(0, len(data), block))
    security = struct.pack('<I12sIIII', 32, b'SignedImage ', block // 1024, 0x800c, 0, len(hashes)) + hashes
    security += bytes(-len(security) % block)
    offsets = {'image': len(security), 'store': len(security) + len(image),
               'validation': len(security) + len(image) + validation_in_store,
               'descriptors': len(security) + len(image) + descriptor_in_store,
               'data': len(security) + len(metadata), 'hash': 32}
    result = security + metadata + b''.join(packed for data, packed in chunks)
    return bytearray(result), bytes(logical), offsets


def make_multistore(stores, block=BLOCK):
    """Combine independently encoded V2 store tables and payloads in FFU order."""
    images = []
    logicals = []
    manifest = b'[FullFlash]\r\nOSVersion=10.0.22621.0\r\n'
    for index, definition in enumerate(stores, 1):
        options = dict(definition)
        entries = options.pop('payloads')
        packed, logical, offsets = make_ffu(entries, version=2, block=block, **options)
        logicals.append(logical)
        manifest += (f'\r\n[Store]\r\nMinSectorCount={len(logical) // options.get("sector", 512)}\r\n'
                     f'SectorSize={options.get("sector", 512)}\r\n').encode()
        table = packed[offsets['store']:offsets['data']]
        struct.pack_into('<HH', table, 248, len(stores), index)
        images.append((table, packed[offsets['data']:], offsets))
    image = struct.pack('<I12sII', 24, b'ImageFlash  ', len(manifest), block // 1024) + manifest
    image += bytes(-len(image) % block)
    metadata = image + b''.join(table for table, payload, offsets in images)
    payload = b''.join(payload for table, payload, offsets in images)
    hashed = metadata + payload
    hashes = b''.join(hashlib.sha256(hashed[i:i + block]).digest() for i in range(0, len(hashed), block))
    security = struct.pack('<I12sIIII', 32, b'SignedImage ', block // 1024, 0x800c, 0, len(hashes)) + hashes
    security += bytes(-len(security) % block)
    result = bytearray(security + hashed)
    store_offsets, payload_offsets = [], []
    position = len(security) + len(image)
    data = len(security) + len(metadata)
    for table, payload, unused in images:
        store_offsets.append(position)
        payload_offsets.append(data)
        position += len(table)
        data += len(payload)
    return result, logicals, {'image': len(security), 'stores': store_offsets,
                             'data': payload_offsets, 'hash': 32, 'metadata_end': len(security) + len(metadata)}
