// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "image_compression.h"
#include <QByteArray>
#include <QCryptographicHash>
#include <limits>
#include <zlib.h>

// Test representation only; the actual wire association remains gzip (3).
constexpr int LzwGzipFixture = -3;
constexpr int CompressedFixtureKinds[] = {BLED_COMPRESSION_GZIP, BLED_COMPRESSION_XZ,
    BLED_COMPRESSION_BZIP2, BLED_COMPRESSION_LZMA, BLED_COMPRESSION_LZW,
    BLED_COMPRESSION_ZIP, BLED_COMPRESSION_ZSTD, LzwGzipFixture};

inline int CompressedFixtureAssociation(int compression)
{
    return compression == LzwGzipFixture ? BLED_COMPRESSION_GZIP : compression;
}

inline void PutVhdFixtureBe(QByteArray& bytes, qsizetype offset, quint64 value, unsigned width)
{
    for (unsigned index = 0; index < width; index++)
        bytes[offset + index] = char(value >> (8 * (width - 1 - index)));
}

inline QByteArray VhdFixedFixture(const QByteArray& decoded)
{
    if (decoded.isEmpty() || decoded.size() % 512 != 0) return {};
    QByteArray footer(512, '\0');
    footer.replace(0, 8, "conectix");
    PutVhdFixtureBe(footer, 8, 2, 4);
    PutVhdFixtureBe(footer, 12, 0x10000, 4);
    PutVhdFixtureBe(footer, 16, UINT64_MAX, 8);
    PutVhdFixtureBe(footer, 40, quint64(decoded.size()), 8);
    PutVhdFixtureBe(footer, 48, quint64(decoded.size()), 8);
    PutVhdFixtureBe(footer, 60, 2, 4);
    quint32 sum = 0;
    for (char byte : footer) sum += quint8(byte);
    PutVhdFixtureBe(footer, 64, ~sum, 4);
    return decoded + footer;
}

// Independent literal-only Unix-compress stream. At a nine-bit limit the code
// width never grows; dictionary entries are valid but need not be referenced.
inline QByteArray LzwLiteralFixture(const QByteArray& decoded)
{
    if (decoded.size() > (std::numeric_limits<qsizetype>::max() - 7) / 9)
        return {};
    const qsizetype bits = decoded.size() * 9;
    QByteArray packed(3 + (bits + 7) / 8, '\0');
    packed[0] = char(0x1f); packed[1] = char(0x9d); packed[2] = char(0x89);
    for (qsizetype index = 0; index < decoded.size(); index++)
    {
        const qsizetype bit = index * 9;
        const unsigned value = unsigned(quint8(decoded[index])) << (bit % 8);
        packed[3 + bit / 8] |= char(value & 255);
        packed[4 + bit / 8] |= char(value >> 8);
    }
    return packed;
}

inline void AppendZipFixture16(QByteArray& bytes, quint16 value)
{
    for (unsigned index = 0; index < 2; index++) bytes.append(char(value >> (8 * index)));
}

inline void AppendZipFixture32(QByteArray& bytes, quint32 value)
{
    for (unsigned index = 0; index < 4; index++) bytes.append(char(value >> (8 * index)));
}

// Independent single-entry ZIP container for the ordinary Qt test paths.
inline QByteArray ZipDeflateFixture(const QByteArray& decoded)
{
    if (decoded.size() > std::numeric_limits<quint32>::max()) return {};
    QByteArray packed(qsizetype(compressBound(uLong(decoded.size()))), '\0');
    uLongf packedBytes = uLongf(packed.size());
    const int encoded = compress2(reinterpret_cast<Bytef*>(packed.data()), &packedBytes,
        reinterpret_cast<const Bytef*>(decoded.constData()), uLong(decoded.size()), Z_BEST_SPEED);
    if (encoded != Z_OK || packedBytes < 6 || packedBytes - 6 > std::numeric_limits<quint32>::max()) return {};
    packed = packed.mid(2, qsizetype(packedBytes - 6)); // Remove zlib header and Adler32.
    const QByteArray name("disk.img");
    const quint32 crc = quint32(crc32(0, reinterpret_cast<const Bytef*>(decoded.constData()), uInt(decoded.size())));
    QByteArray archive;
    AppendZipFixture32(archive, 0x04034b50);
    AppendZipFixture16(archive, 20); AppendZipFixture16(archive, 0); AppendZipFixture16(archive, 8);
    AppendZipFixture16(archive, 0); AppendZipFixture16(archive, 0);
    AppendZipFixture32(archive, crc); AppendZipFixture32(archive, quint32(packed.size()));
    AppendZipFixture32(archive, quint32(decoded.size()));
    AppendZipFixture16(archive, quint16(name.size())); AppendZipFixture16(archive, 0);
    archive += name;
    archive += packed;
    const quint32 directoryOffset = quint32(archive.size());
    AppendZipFixture32(archive, 0x02014b50);
    AppendZipFixture16(archive, 20); AppendZipFixture16(archive, 20);
    AppendZipFixture16(archive, 0); AppendZipFixture16(archive, 8);
    AppendZipFixture16(archive, 0); AppendZipFixture16(archive, 0);
    AppendZipFixture32(archive, crc); AppendZipFixture32(archive, quint32(packed.size()));
    AppendZipFixture32(archive, quint32(decoded.size()));
    AppendZipFixture16(archive, quint16(name.size())); AppendZipFixture16(archive, 0); AppendZipFixture16(archive, 0);
    AppendZipFixture16(archive, 0); AppendZipFixture16(archive, 0);
    AppendZipFixture32(archive, 0); AppendZipFixture32(archive, 0);
    archive += name;
    const quint32 directorySize = quint32(archive.size()) - directoryOffset;
    AppendZipFixture32(archive, 0x06054b50);
    AppendZipFixture16(archive, 0); AppendZipFixture16(archive, 0);
    AppendZipFixture16(archive, 1); AppendZipFixture16(archive, 1);
    AppendZipFixture32(archive, directorySize); AppendZipFixture32(archive, directoryOffset);
    AppendZipFixture16(archive, 0);
    return archive;
}

inline void AppendVtsiFixture64(QByteArray& bytes, quint64 value)
{
    for (unsigned index = 0; index < 8; index++) bytes.append(char(value >> (8 * index)));
}

inline void PutVtsiFixtureValue(QByteArray& bytes, qsizetype offset, quint64 value, unsigned width)
{
    for (unsigned index = 0; index < width; index++) bytes[offset + index] = char(value >> (8 * index));
}

inline quint32 VtsiFixtureChecksum(const QByteArray& bytes)
{
    quint32 sum = 0;
    for (char byte : bytes) sum += quint8(byte);
    return ~sum;
}

// Two ordered sectors/regions with a zero logical gap. This fixture is
// independent of the production VTSI encoder and layout-hash implementation.
inline QByteArray VtsiSparseFixture(const QByteArray& logical, qsizetype firstBytes,
    qsizetype secondOffset, qsizetype secondBytes, QByteArray& layoutHash)
{
    constexpr qsizetype sectorBytes = 512;
    const bool aligned = logical.size() % sectorBytes == 0 && firstBytes % sectorBytes == 0
        && secondOffset % sectorBytes == 0 && secondBytes % sectorBytes == 0;
    const bool ordered = firstBytes > 0 && firstBytes < secondOffset && secondBytes > 0
        && secondOffset + secondBytes <= logical.size();
    if (!aligned || !ordered) return {};
    const QByteArray first = logical.left(firstBytes);
    const QByteArray second = logical.mid(secondOffset, secondBytes);
    QByteArray table;
    AppendVtsiFixture64(table, 0); AppendVtsiFixture64(table, quint64(firstBytes / sectorBytes));
    AppendVtsiFixture64(table, 0xdeadbeef);
    AppendVtsiFixture64(table, quint64(secondOffset / sectorBytes));
    AppendVtsiFixture64(table, quint64(secondBytes / sectorBytes));
    AppendVtsiFixture64(table, 0);
    QByteArray map;
    AppendVtsiFixture64(map, 0); AppendVtsiFixture64(map, 0); AppendVtsiFixture64(map, quint64(firstBytes));
    AppendVtsiFixture64(map, quint64(secondOffset)); AppendVtsiFixture64(map, quint64(firstBytes));
    AppendVtsiFixture64(map, quint64(secondBytes));
    AppendVtsiFixture64(map, quint64(logical.size())); AppendVtsiFixture64(map, quint64(firstBytes + secondBytes));
    layoutHash = QCryptographicHash::hash(map, QCryptographicHash::Sha256);
    QByteArray footer(sectorBytes, '\0');
    footer.replace(0, 8, QByteArray("VENTOY\0\0", 8));
    PutVtsiFixtureValue(footer, 8, 1, 2);
    PutVtsiFixtureValue(footer, 12, quint64(logical.size()), 8);
    PutVtsiFixtureValue(footer, 20, 0x12345678, 4);
    PutVtsiFixtureValue(footer, 28, 2, 4);
    PutVtsiFixtureValue(footer, 32, VtsiFixtureChecksum(table), 4);
    PutVtsiFixtureValue(footer, 36, quint64(firstBytes + secondBytes), 8);
    PutVtsiFixtureValue(footer, 24, VtsiFixtureChecksum(footer), 4);
    table.append(QByteArray((sectorBytes - table.size() % sectorBytes) % sectorBytes, '\0'));
    return first + second + table + footer;
}
