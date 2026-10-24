# SPDX-License-Identifier: GPL-3.0-or-later
"""Small independent Unix-compress fixture encoder; never used by production."""


class CodeWriter:
    def __init__(self, bits=16, block=True):
        self.bits = bits
        self.block = block
        self.width = 9
        self.next_code = 257 if block else 256
        self.first = True
        self.codes = []
        self.output = bytearray((0x1f, 0x9d, bits | (0x80 if block else 0)))

    def flush(self, padded):
        if not self.codes:
            return
        value = sum(code << (i * self.width) for i, code in enumerate(self.codes))
        count = self.width if padded else (len(self.codes) * self.width + 7) // 8
        self.output += value.to_bytes(count, 'little')
        self.codes.clear()

    def emit(self, code):
        if self.width < self.bits and self.next_code >= 1 << self.width:
            self.flush(True)
            self.width += 1
        self.codes.append(code)
        if len(self.codes) == 8:
            self.flush(False)
        if self.block and code == 256:
            if self.codes:
                self.flush(True)
            self.width = 9
            self.next_code = 256
        elif self.first:
            self.first = False
        elif self.next_code < 1 << self.bits:
            self.next_code += 1

    def finish(self):
        if self.codes:
            self.flush(False)
        return bytes(self.output)


def codes(data, bits=16, block=True):
    table = {bytes((byte,)): byte for byte in range(256)}
    next_code = 257 if block else 256
    phrase = b''
    for byte in data:
        symbol = bytes((byte,))
        longer = phrase + symbol
        if longer in table:
            phrase = longer
        else:
            yield table[phrase]
            if next_code < 1 << bits:
                table[longer] = next_code
                next_code += 1
            phrase = symbol
    if phrase:
        yield table[phrase]


def encode(data, bits=16, block=True):
    writer = CodeWriter(bits, block)
    for code in codes(data, bits, block):
        writer.emit(code)
    return writer.finish()


def encode_literals(data, bits=16, block=True):
    """Valid uncompressed codes, with a size independent of literal values."""
    writer = CodeWriter(bits, block)
    for byte in data:
        writer.emit(byte)
    return writer.finish()


def encode_clear_tail(data, tail=37):
    """End with CLEAR and a known nine-bit literal group for late code faults."""
    assert 0 < tail <= len(data)
    writer = CodeWriter()
    for code in codes(data[:-tail]):
        writer.emit(code)
    writer.emit(256)
    for byte in data[-tail:]:
        writer.emit(byte)
    return writer.finish()
