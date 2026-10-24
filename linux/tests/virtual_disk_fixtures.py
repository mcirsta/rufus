# SPDX-License-Identifier: GPL-3.0-or-later
"""Independent VHD/VHDX chain and log vectors, shared with the disposable guest."""
from pathlib import Path
import struct
import uuid
from test_compressed_vhd import checksum, footer
from test_compressed_vhdx import BAT_GUID, META_GUID, ITEMS, MIB, TABLE, set_checksum

VHDX_PARENT = 'a8d35f2d-b30b-454d-abf7-d3d84834ab0c'
LOCATOR_TYPE = 'b04aefb7-d19e-4a81-b789-25b8e9445913'


def vhd_fixed(raw, identifier=None):
    identifier = identifier or uuid.uuid4()
    mark = bytearray(footer(len(raw), 2, 0xffffffffffffffff))
    mark[68:84] = identifier.bytes
    struct.pack_into('>I', mark, 64, checksum(mark, 64))
    return bytes(raw) + mark, identifier


def vhd_child(raw, parent, parent_id, parent_stamp, changes=None, block=4096, identifier=None):
    changes = changes or {}
    identifier = identifier or uuid.uuid4()
    logical = bytearray(raw)
    path = str(parent).replace('/', '\\').encode('utf-16le')
    assert len(path) < 4096
    locator_bytes = (len(path) + 511) // 512 * 512
    bat_offset = 1536 + locator_bytes
    blocks = (len(raw) + block - 1) // block
    bat_bytes = (blocks * 4 + 511) // 512 * 512
    image = bytearray(bat_offset + bat_bytes)
    image[1536:1536 + len(path)] = path
    header = bytearray(1024); header[:8] = b'cxsparse'
    struct.pack_into('>QQIII', header, 8, 0xffffffffffffffff, bat_offset, 0x10000, blocks, block)
    header[40:56] = parent_id.bytes
    struct.pack_into('>I', header, 56, parent_stamp)
    name = Path(str(parent)).name.encode('utf-16be')
    header[64:64 + len(name)] = name
    header[576:580] = b'W2ru'
    struct.pack_into('>IIIQ', header, 580, locator_bytes // 512, len(path), 0, 1536)
    bitmap_bytes = ((block // 512 + 4095) // 4096) * 512
    for index in range(blocks):
        sectors = [sector for sector in changes if sector * 512 // block == index]
        physical = 0xffffffff
        if sectors:
            physical = len(image) // 512
            bitmap = bytearray(bitmap_bytes)
            payload = bytearray(block)
            for sector in sectors:
                data = changes[sector]; assert len(data) == 512
                local = sector - index * (block // 512)
                bitmap[local // 8] |= 0x80 >> (local % 8)
                payload[local * 512:(local + 1) * 512] = data
                logical[sector * 512:(sector + 1) * 512] = data
            image += bitmap + payload
        struct.pack_into('>I', image, bat_offset + index * 4, physical)
    struct.pack_into('>I', header, 36, checksum(header, 36))
    mark = bytearray(footer(len(raw), 4, 512)); mark[68:84] = identifier.bytes
    struct.pack_into('>I', mark, 64, checksum(mark, 64))
    image[:512] = mark; image[512:1536] = header; image += mark
    return image, logical, identifier


def parent_locator(parent, linkage, alternatives=None):
    pairs = [('parent_linkage', '{' + str(linkage) + '}')] + (alternatives if alternatives is not None else [('relative_path', str(parent).replace('/', '\\'))])
    data = bytearray(20 + 12 * len(pairs)); data[:16] = uuid.UUID(LOCATOR_TYPE).bytes_le
    struct.pack_into('<HH', data, 16, 0, len(pairs))
    for index, (key, value) in enumerate(pairs):
        key, value = key.encode('utf-16le'), value.encode('utf-16le')
        key_offset = len(data); data += key
        value_offset = len(data); data += value
        struct.pack_into('<IIHH', data, 20 + index * 12, key_offset, value_offset, len(key), len(value))
    return data


def vhdx(raw, parent=None, parent_id=None, changes=None, states=None, block=MIB, sector=512, identifier=None):
    changes, states = changes or {}, states or {}
    identifier = identifier or uuid.uuid4()
    logical = bytearray(raw)
    image = bytearray(4 * MIB); image[:8] = b'vhdxfile'
    for seq, start in enumerate((TABLE, 2 * TABLE), 10):
        image[start:start + 4] = b'head'
        struct.pack_into('<Q', image, start + 8, seq)
        image[start + 32:start + 48] = identifier.bytes_le
        struct.pack_into('<HHIQ', image, start + 64, 0, 1, MIB, MIB)
        set_checksum(image, start, 4096)
    for start in (3 * TABLE, 4 * TABLE):
        image[start:start + 4] = b'regi'; struct.pack_into('<I', image, start + 8, 2)
        for index, guid, offset in ((0, BAT_GUID, 3 * MIB), (1, META_GUID, 2 * MIB)):
            entry = start + 16 + 32 * index
            image[entry:entry + 16] = uuid.UUID(guid).bytes_le
            struct.pack_into('<QII', image, entry + 16, offset, MIB, 1)
        set_checksum(image, start, TABLE)
    image[2 * MIB:2 * MIB + 8] = b'metadata'
    values = [(ITEMS[0], struct.pack('<II', block, 2 if parent is not None else 0), 4),
              (ITEMS[1], struct.pack('<Q', len(raw)), 6), (ITEMS[2], uuid.uuid4().bytes_le, 6),
              (ITEMS[3], struct.pack('<I', sector), 6), (ITEMS[4], struct.pack('<I', 4096), 6)]
    if parent is not None:
        values.append((VHDX_PARENT, parent_locator(parent, parent_id), 4))
    struct.pack_into('<H', image, 2 * MIB + 10, len(values))
    for index, (guid, value, flags) in enumerate(values):
        entry = 2 * MIB + 32 + 32 * index
        image[entry:entry + 16] = uuid.UUID(guid).bytes_le
        offset = TABLE + (16 * index if index < 5 else 128)
        struct.pack_into('<IIII', image, entry + 16, offset, len(value), flags, 0)
        image[2 * MIB + offset:2 * MIB + offset + len(value)] = value
    blocks = (len(raw) + block - 1) // block
    ratio = (1 << 23) * sector // block
    bitmaps = {}
    for index in range(blocks):
        position = index + index // ratio
        initial = raw[index * block:(index + 1) * block]
        sectors = [s for s in changes if s * sector // block == index]
        state = states.get(index, 7 if parent is not None and sectors else 0 if parent is not None else 6 if any(initial) else 2)
        physical = 0
        if state in (6, 7):
            physical = len(image)
            payload = bytearray(initial.ljust(block, b'\0') if state == 6 and parent is None else bytes(block))
            for s in sectors:
                data = changes[s]; assert len(data) == sector
                local = s * sector - index * block
                payload[local:local + sector] = data
                logical[s * sector:(s + 1) * sector] = data
                bits = bitmaps.setdefault(index // ratio, bytearray(MIB))
                bit = s % (1 << 23); bits[bit // 8] |= 1 << (bit % 8)
            if parent is not None and state == 6:
                logical[index * block:(index + 1) * block] = payload[:min(block, len(raw) - index * block)]
            image += payload
        elif state in (1, 2, 3):
            logical[index * block:min((index + 1) * block, len(raw))] = bytes(min(block, len(raw) - index * block))
        struct.pack_into('<Q', image, 3 * MIB + position * 8, physical | state)
    for chunk, bitmap in bitmaps.items():
        physical = len(image); image += bitmap
        struct.pack_into('<Q', image, 3 * MIB + ((chunk + 1) * (ratio + 1) - 1) * 8, physical | 6)
    return image, logical, identifier


def log_entry(updates, sequence, tail, log_guid, flushed, last):
    descriptors = (64 + 32 * len(updates) + 4095) // 4096 * 4096
    data_count = sum(isinstance(value, (bytes, bytearray)) for _, value in updates)
    entry = bytearray(descriptors + data_count * 4096)
    entry[:4] = b'loge'
    struct.pack_into('<IIQII', entry, 8, len(entry), tail, sequence, len(updates), 0)
    entry[32:48] = log_guid.bytes_le
    struct.pack_into('<QQ', entry, 48, flushed, last)
    data_position = descriptors
    for index, (offset, value) in enumerate(updates):
        descriptor = 64 + index * 32
        if isinstance(value, int):
            entry[descriptor:descriptor + 4] = b'zero'
            struct.pack_into('<QQQ', entry, descriptor + 8, value, offset, sequence)
        else:
            assert len(value) == 4096
            entry[descriptor:descriptor + 4] = b'desc'
            entry[descriptor + 4:descriptor + 8] = value[-4:]
            entry[descriptor + 8:descriptor + 16] = value[:8]
            struct.pack_into('<QQ', entry, descriptor + 16, offset, sequence)
            entry[data_position:data_position + 4] = b'data'
            struct.pack_into('<I', entry, data_position + 4, sequence >> 32)
            entry[data_position + 8:data_position + 4092] = value[8:-4]
            struct.pack_into('<I', entry, data_position + 4092, sequence & 0xffffffff)
            data_position += 4096
    set_checksum(entry, 0, len(entry))
    return entry


def dirty_log(image, records, last=None):
    """records: [(ring offset, sequence, tail, [(file offset, data or zero length)])]."""
    image = image.copy(); guid = uuid.uuid4()
    for start in (TABLE, 2 * TABLE):
        image[start + 48:start + 64] = guid.bytes_le
        set_checksum(image, start, 4096)
    for position, sequence, tail, updates in records:
        entry = log_entry(updates, sequence, tail, guid, len(image), last or len(image))
        assert len(entry) <= MIB
        first = min(len(entry), MIB - position)
        image[MIB + position:MIB + position + first] = entry[:first]
        image[MIB:MIB + len(entry) - first] = entry[first:]
    return image
