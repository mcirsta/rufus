// SPDX-License-Identifier: GPL-3.0-or-later
#include "image_save.h"
#include "iso_export.h"
#include "compressed_source.h"
#include "compressed_ffu.h"
#include "ffu_codec.h"
#include "hash_native.h"
#include "rufus_reporting.h"

#include <QDateTime>
#include <QFile>
#include <QUuid>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <zlib.h>

namespace rufus
{
namespace
{
constexpr quint64 mib = 1024 * 1024;
constexpr size_t transferBytes = 65536;
constexpr quint64 vhdxBlockBytes = 32 * mib;

#include "ffu_catalog.inc"

bool IsZeroByte(char byte) { return byte == 0; }
bool IsManifestNameByte(char byte) { return byte >= 32 && byte <= 126; }

quint64 alignBytes(quint64 value, quint64 alignment)
{
    return value + (alignment - value % alignment) % alignment;
}

void put32(char* data, quint32 value)
{
    for (unsigned i = 0; i < 4; i++) data[i] = char(value >> (8 * i));
}

void put64(char* data, quint64 value)
{
    for (unsigned i = 0; i < 8; i++) data[i] = char(value >> (8 * i));
}

quint32 get32(const char* data)
{
    const auto* p = reinterpret_cast<const unsigned char*>(data);
    return quint32(p[0]) | quint32(p[1]) << 8 | quint32(p[2]) << 16 | quint32(p[3]) << 24;
}

quint64 get64(const char* data)
{
    return get32(data) | quint64(get32(data + 4)) << 32;
}

void big32(char* data, quint32 value)
{
    for (unsigned i = 0; i < 4; i++) data[i] = char(value >> (8 * (3 - i)));
}

void big64(char* data, quint64 value)
{
    for (unsigned i = 0; i < 8; i++) data[i] = char(value >> (8 * (7 - i)));
}

QByteArray guid(const char* text)
{
    const QByteArray rfc = QUuid(QLatin1String(text)).toRfc4122();
    QByteArray disk = rfc;
    std::reverse(disk.begin(), disk.begin() + 4);
    std::reverse(disk.begin() + 4, disk.begin() + 6);
    std::reverse(disk.begin() + 6, disk.begin() + 8);
    return disk;
}

void crc32c(QByteArray& data)
{
    put32(data.data() + 4, 0);
    quint32 crc = UINT32_MAX;
    for (unsigned char byte : data) {
        crc ^= byte;
        for (unsigned bit = 0; bit < 8; bit++) crc = (crc >> 1) ^ (0x82f63b78u & (0u - (crc & 1)));
    }
    put32(data.data() + 4, ~crc);
}

int positioned(int fd, void* buffer, size_t size, quint64 offset, bool write)
{
    char* bytes = static_cast<char*>(buffer);
    while (size != 0) {
        if (IsRufusJobCancelled()) return ECANCELED;
        const size_t batch = qMin(size, transferBytes);
        const ssize_t count = write ? pwrite(fd, bytes, batch, off_t(offset)) : pread(fd, bytes, batch, off_t(offset));
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return count == 0 ? EIO : errno;
        bytes += count;
        size -= size_t(count);
        offset += quint64(count);
    }
    return 0;
}

int writeBytes(int fd, const QByteArray& bytes, quint64 offset)
{
    return positioned(fd, const_cast<char*>(bytes.constData()), size_t(bytes.size()), offset, true);
}

QByteArray digest(const char* data, size_t bytes, int type = HASH_SHA256)
{
    HASH_CONTEXT hash;
    hash_init[type](&hash);
    hash_write[type](&hash, reinterpret_cast<const uint8_t*>(data), bytes);
    hash_final[type](&hash);
    return QByteArray(reinterpret_cast<const char*>(hash.buf), type == HASH_SHA1 ? 20 : 32);
}

struct SaveState
{
    int source, output;
    quint64 size, copied = 0;
    unsigned sector;
    ImageSaveProgress progress;
    void* context;
    HASH_CONTEXT hash;

    int read(QByteArray& bytes, quint64 offset, size_t count, size_t skip = 0)
    {
        if (skip > count || offset + skip != copied) return EINVAL;
        const int error = positioned(source, bytes.data(), count, offset, false);
        if (error != 0) return error;
        hash_write[HASH_SHA256](&hash, reinterpret_cast<const uint8_t*>(bytes.constData()) + skip, count - skip);
        copied += count - skip;
        if (progress) progress(context, ImageSavePhase::Copy, copied, size);
        return 0;
    }
};

int checkSource(int fd, const struct stat& initial, quint64 size, unsigned sector, quint64 sequence)
{
    struct stat current;
    if (fstat(fd, &current) != 0) return errno;
    if (current.st_dev != initial.st_dev || current.st_ino != initial.st_ino || current.st_rdev != initial.st_rdev)
        return ESTALE;
    if (S_ISREG(current.st_mode)) {
        const bool sameTimes = current.st_mtim.tv_sec == initial.st_mtim.tv_sec && current.st_mtim.tv_nsec == initial.st_mtim.tv_nsec
            && current.st_ctim.tv_sec == initial.st_ctim.tv_sec && current.st_ctim.tv_nsec == initial.st_ctim.tv_nsec;
        return current.st_size == off_t(size) && sameTimes ? 0 : ESTALE;
    }
    uint64_t capacity = 0, diskSequence = 0;
    int logicalSector = 0;
    if (ioctl(fd, BLKGETSIZE64, &capacity) != 0 || ioctl(fd, BLKGETDISKSEQ, &diskSequence) != 0
        || ioctl(fd, BLKSSZGET, &logicalSector) != 0) return errno;
    return capacity == size && logicalSector == int(sector) && diskSequence == sequence ? 0 : ESTALE;
}

int saveVhd(SaveState& state)
{
    QByteArray buffer(transferBytes, '\0');
    for (quint64 offset = 0; offset < state.size; offset += quint64(buffer.size())) {
        const size_t bytes = size_t(qMin(quint64(buffer.size()), state.size - offset));
        int error = state.read(buffer, offset, bytes);
        if (error == 0) error = positioned(state.output, buffer.data(), bytes, offset, true);
        if (error != 0) return error;
    }
    QByteArray footer(512, '\0');
    memcpy(footer.data(), "conectix", 8);
    big32(footer.data() + 8, 2);
    big32(footer.data() + 12, 0x10000);
    big64(footer.data() + 16, UINT64_MAX);
    big32(footer.data() + 24, quint32(qMax(qint64(0), QDateTime::currentSecsSinceEpoch() - 946684800)));
    memcpy(footer.data() + 28, "RufL", 4);
    big32(footer.data() + 32, 0x10000);
    memcpy(footer.data() + 36, "Wi2k", 4);
    big64(footer.data() + 40, state.size);
    big64(footer.data() + 48, state.size);
    quint64 sectors = qMin(state.size / 512, quint64(65535) * 16 * 255);
    unsigned tracks = 17, heads = qMax(4u, unsigned((sectors / tracks + 1023) / 1024));
    if (heads > 16 || sectors / tracks >= quint64(heads) * 1024) { tracks = 31; heads = 16; }
    if (sectors / tracks >= quint64(heads) * 1024) { tracks = 63; heads = 16; }
    if (sectors >= quint64(65535) * 16 * 63) { tracks = 255; heads = 16; }
    const unsigned cylinders = unsigned(sectors / (tracks * heads));
    footer[56] = char(cylinders >> 8); footer[57] = char(cylinders);
    footer[58] = char(heads); footer[59] = char(tracks);
    big32(footer.data() + 60, 2);
    memcpy(footer.data() + 68, QUuid::createUuid().toRfc4122().constData(), 16);
    quint32 sum = 0;
    for (unsigned char byte : footer) sum += byte;
    big32(footer.data() + 64, ~sum);
    return writeBytes(state.output, footer, state.size);
}

int saveVhdx(SaveState& state)
{
    const quint64 blocks = (state.size + vhdxBlockBytes - 1) / vhdxBlockBytes;
    const quint64 ratio = (quint64(1) << 23) * state.sector / vhdxBlockBytes;
    const quint64 entries = blocks + (blocks - 1) / ratio;
    const quint64 batBytes = alignBytes(entries * 8, mib);
    const quint64 dataOffset = 3 * mib + batBytes;
    QByteArray signature(65536, '\0'); memcpy(signature.data(), "vhdxfile", 8);
    int error = writeBytes(state.output, signature, 0);
    QByteArray header(4096, '\0'); memcpy(header.data(), "head", 4);
    memcpy(header.data() + 16, QUuid::createUuid().toRfc4122().constData(), 16);
    memcpy(header.data() + 32, QUuid::createUuid().toRfc4122().constData(), 16);
    header[66] = 1; put32(header.data() + 68, mib); put64(header.data() + 72, mib);
    for (unsigned i = 0; error == 0 && i < 2; i++) {
        put64(header.data() + 8, i + 1); crc32c(header);
        error = writeBytes(state.output, header, quint64(i + 1) * 65536);
    }
    QByteArray regions(65536, '\0'); memcpy(regions.data(), "regi", 4); put32(regions.data() + 8, 2);
    memcpy(regions.data() + 16, guid("2dc27766-f623-4200-9d64-115e9bfd4a08").constData(), 16);
    put64(regions.data() + 32, 3 * mib); put32(regions.data() + 40, quint32(batBytes)); put32(regions.data() + 44, 1);
    memcpy(regions.data() + 48, guid("8b7ca206-4790-4b9a-b8fe-575f050f886e").constData(), 16);
    put64(regions.data() + 64, 2 * mib); put32(regions.data() + 72, mib); put32(regions.data() + 76, 1);
    crc32c(regions);
    for (unsigned i = 3; error == 0 && i <= 4; i++) error = writeBytes(state.output, regions, quint64(i) * 65536);
    QByteArray metadata(mib, '\0'); memcpy(metadata.data(), "metadata", 8); metadata[10] = 5;
    const char* items[] = {"caa16737-fa36-4d43-b3b6-33f0aa44e76b", "2fa54224-cd1b-4876-b211-5dbed83bf4b8",
        "beca12ab-b2e6-4523-93ef-c309e000c746", "8141bf1d-a96f-4709-ba47-f233a8faab5f", "cda348c7-445d-4471-9cc9-e9885251c556"};
    const unsigned lengths[] = {8, 8, 16, 4, 4};
    for (unsigned i = 0; i < 5; i++) {
        char* entry = metadata.data() + 32 + i * 32;
        memcpy(entry, guid(items[i]).constData(), 16);
        put32(entry + 16, 65536 + i * 16); put32(entry + 20, lengths[i]); put32(entry + 24, i == 0 ? 4 : 6);
    }
    put32(metadata.data() + 65536, quint32(vhdxBlockBytes));
    put64(metadata.data() + 65552, state.size);
    memcpy(metadata.data() + 65568, QUuid::createUuid().toRfc4122().constData(), 16);
    put32(metadata.data() + 65584, state.sector); put32(metadata.data() + 65600, state.sector);
    if (error == 0) error = writeBytes(state.output, metadata, 2 * mib);
    QByteArray bat(qsizetype(batBytes), '\0');
    QByteArray buffer(qsizetype(vhdxBlockBytes), '\0');
    quint64 allocated = 0;
    for (quint64 block = 0; error == 0 && block < blocks; block++) {
        buffer.fill('\0');
        const size_t bytes = size_t(qMin(vhdxBlockBytes, state.size - block * vhdxBlockBytes));
        error = state.read(buffer, block * vhdxBlockBytes, bytes);
        if (error != 0) break;
        const bool zero = std::all_of(buffer.cbegin(), buffer.cbegin() + qsizetype(bytes), IsZeroByte);
        if (zero) put64(bat.data() + (block + block / ratio) * 8, 2);
        else {
            const quint64 physical = dataOffset + allocated++ * vhdxBlockBytes;
            put64(bat.data() + (block + block / ratio) * 8, physical | 6);
            error = positioned(state.output, buffer.data(), bytes, physical, true);
        }
    }
    if (error == 0) error = writeBytes(state.output, bat, 3 * mib);
    if (error == 0 && ftruncate(state.output, off_t(dataOffset + allocated * vhdxBlockBytes)) != 0) error = errno;
    return error;
}

QString gptGuid(const char* data)
{
    QByteArray rfc(data, 16);
    std::reverse(rfc.begin(), rfc.begin() + 4);
    std::reverse(rfc.begin() + 4, rfc.begin() + 6);
    std::reverse(rfc.begin() + 6, rfc.begin() + 8);
    return QUuid::fromRfc4122(rfc).toString().toUpper();
}

int ffuManifest(int source, quint64 size, unsigned sector, QByteArray& manifest)
{
    QByteArray header(sector, '\0');
    int error = positioned(source, header.data(), sector, sector, false);
    if (error != 0) return error;
    if (memcmp(header.constBegin(), "EFI PART", 8) != 0) return ENOTSUP;
    const quint32 headerBytes = get32(header.constBegin() + 12), expectedCrc = get32(header.constBegin() + 16);
    if (headerBytes < 92 || headerBytes > sector || get64(header.constBegin() + 24) != 1
        || get64(header.constBegin() + 32) != size / sector - 1) return ENOTSUP;
    put32(header.data() + 16, 0);
    if (quint32(crc32(0, reinterpret_cast<const Bytef*>(header.constBegin()), headerBytes)) != expectedCrc) return EBADMSG;
    const quint64 tableSector = get64(header.constBegin() + 72);
    const quint32 count = get32(header.constBegin() + 80), entryBytes = get32(header.constBegin() + 84);
    const quint64 tableBytes = quint64(count) * entryBytes;
    if (count == 0 || entryBytes < 128 || entryBytes % 128 != 0 || tableBytes > FFU_GPT_TABLE_BUDGET) return ENOTSUP;
    if (tableSector > size / sector || tableBytes > size - tableSector * sector) return EBADMSG;
    const quint64 firstUsable = get64(header.constBegin() + 40), lastUsable = get64(header.constBegin() + 48);
    const quint64 tableSectors = (tableBytes + sector - 1) / sector;
    const bool usableRange = firstUsable > 1 && firstUsable <= lastUsable && lastUsable < size / sector - 1;
    const bool tableFits = tableSector >= 2 && tableSector < firstUsable && tableSectors <= firstUsable - tableSector;
    if (!usableRange || !tableFits) return EBADMSG;
    QByteArray table(qsizetype(tableBytes), '\0');
    error = positioned(source, table.data(), size_t(tableBytes), tableSector * sector, false);
    if (error != 0) return error;
    if (quint32(crc32(0, reinterpret_cast<const Bytef*>(table.constBegin()), uInt(tableBytes))) != get32(header.constBegin() + 88)) return EBADMSG;
    manifest = "[FullFlash]\r\nOSVersion=10.0.0.0\r\nDescription=Created by Rufus Linux\r\n\r\n"
        "[StoragePool]\r\nName=OSPool\r\n\r\n[Store]\r\nOnlyAllocateDefinedGptEntries=False\r\n"
        "StoreId={5A585BAE-900B-41B5-B736-A4CECFFC34B4}\r\nIsMainOSStore=False\r\nStoreType=Default\r\n"
        "MinSectorCount=" + QByteArray::number(size / sector) + "\r\nSectorSize=" + QByteArray::number(sector) + "\r\n\r\n";
    QList<QPair<quint64, quint64>> extents;
    for (quint32 i = 0; i < count; i++) {
        const char* entry = table.constBegin() + quint64(i) * entryBytes;
        if (std::all_of(entry, entry + 16, IsZeroByte)) continue;
        const quint64 first = get64(entry + 32), last = get64(entry + 40);
        if (first < firstUsable || last > lastUsable || first > last) return EBADMSG;
        extents.append({first, last});
        char16_t characters[36]; qsizetype length = 0;
        bool manifestName = true;
        for (unsigned j = 56; j < 128; j += 2) {
            const unsigned low = static_cast<unsigned char>(entry[j]), high = static_cast<unsigned char>(entry[j + 1]);
            const char16_t value = char16_t(low | high << 8);
            if (value == 0) break;
            // Names are optional metadata; opaque GPT bytes are captured exactly.
            if (value < 32 || value == 127) manifestName = false;
            characters[length++] = value;
        }
        for (qsizetype j = 0; j < length; j++) {
            const char16_t value = characters[j];
            if (value >= 0xd800 && value <= 0xdbff) {
                if (++j == length || characters[j] < 0xdc00 || characters[j] > 0xdfff) manifestName = false;
            } else if (value >= 0xdc00 && value <= 0xdfff) manifestName = false;
        }
        const QByteArray name = QString::fromUtf16(characters, length).toUtf8();
        const QByteArray sectors = QByteArray::number(last - first + 1);
        manifest += "[Partition]\r\n";
        // Name is optional in a full-flash manifest. Keep this metadata ASCII,
        // while the captured GPT payload retains every original Unicode name.
        if (manifestName && !name.isEmpty() && std::all_of(name.cbegin(), name.cend(), IsManifestNameByte))
            manifest += "Name=" + name + "\r\n";
        manifest += "Type=" + gptGuid(entry).toLatin1()
            + "\r\nUsedSectors=" + sectors + "\r\nTotalSectors=" + sectors + "\r\nRequiredToFlash=True\r\n\r\n";
        if (manifest.size() > FFU_MAX_MANIFEST_BYTES) return ENOTSUP;
    }
    std::sort(extents.begin(), extents.end());
    for (qsizetype i = 1; i < extents.size(); i++) if (extents[i].first <= extents[i - 1].second) return EBADMSG;
    return 0;
}

int saveFfu(SaveState& state)
{
    QByteArray manifest;
    int error = ffuManifest(state.source, state.size, state.sector, manifest);
    if (error != 0) return error;
    const quint64 catalogBytes = sizeof(ffuCatalogPrefix) + 20 + sizeof(ffuCatalogSuffix);
    FFU_CAPTURE_GEOMETRY geometry = {};
    error = PlanFfuCapture(state.size, catalogBytes, quint64(manifest.size()), &geometry);
    if (error != 0) return error;
    const quint32 blocks = geometry.blocks, chunkBytes = geometry.block_bytes;
    const quint64 metadataBytes = geometry.metadata_bytes, hashBytes = geometry.hash_bytes;
    const quint64 securityBytes = geometry.security_bytes;
    const bool partialTail = state.size % chunkBytes != 0;
    QByteArray metadata(qsizetype(metadataBytes), '\0');
    put32(metadata.data(), 24); memcpy(metadata.data() + 4, "ImageFlash  ", 12);
    put32(metadata.data() + 16, quint32(manifest.size())); put32(metadata.data() + 20, chunkBytes / 1024);
    memcpy(metadata.data() + 24, manifest.constData(), size_t(manifest.size()));
    char* store = metadata.data() + geometry.image_bytes;
    store[4] = 1; store[8] = 3;
    put32(store + 204, chunkBytes); put32(store + 208, blocks); put32(store + 212, blocks * 20);
    put32(store + 248, 3); // COMPRESSION_FORMAT_XPRESS, accepted by desktop DISM.
    put32(store + 244, blocks);
    // The descriptor's stored length is known only after compression. Reserve
    // metadata hashes first, then fill them after every descriptor is frozen.
    QByteArray hashes(qsizetype(metadataBytes / chunkBytes * 32), '\0');
    hashes.reserve(qsizetype(hashBytes));
    QByteArray buffer(chunkBytes, '\0');
    QByteArray packed(qsizetype(FfuXpressCapacity(chunkBytes)), '\0');
    quint64 outputOffset = securityBytes + metadataBytes;
    for (quint32 block = 0; block < blocks; block++) {
        const bool tail = partialTail && block == blocks - 1;
        const quint64 offset = tail ? state.size - chunkBytes : quint64(block) * chunkBytes;
        const size_t skip = tail ? size_t(chunkBytes - state.size % chunkBytes) : 0;
        error = state.read(buffer, offset, chunkBytes, skip);
        size_t stored = 0;
        if (error == 0) error = EncodeFfuXpress(reinterpret_cast<const uint8_t*>(buffer.constData()), chunkBytes,
            reinterpret_cast<uint8_t*>(packed.data()), size_t(packed.size()), &stored);
        if (error == 0) error = positioned(state.output, packed.data(), stored, outputOffset, true);
        if (error != 0) return error;
        char* descriptor = store + 252 + quint64(block) * 20;
        put32(descriptor, 1); put32(descriptor + 4, 1); put32(descriptor + 8, quint32(stored));
        put32(descriptor + 12, tail ? 2 : 0); put32(descriptor + 16, tail ? 0 : block);
        outputOffset += stored;
        hashes.append(digest(buffer.constData(), chunkBytes));
    }
    for (quint64 offset = 0; offset < metadataBytes; offset += chunkBytes)
        memcpy(hashes.data() + offset / chunkBytes * 32, digest(metadata.constData() + offset, chunkBytes).constData(), 32);
    QByteArray freshManifest;
    error = ffuManifest(state.source, state.size, state.sector, freshManifest);
    if (error != 0 || freshManifest != manifest) return error != 0 ? error : ESTALE;
    QByteArray security(qsizetype(securityBytes), '\0');
    put32(security.data(), 32); memcpy(security.data() + 4, "SignedImage ", 12);
    put32(security.data() + 16, chunkBytes / 1024); put32(security.data() + 20, 0x800c);
    put32(security.data() + 24, quint32(catalogBytes)); put32(security.data() + 28, quint32(hashBytes));
    char* catalog = security.data() + 32;
    memcpy(catalog, ffuCatalogPrefix, sizeof(ffuCatalogPrefix));
    memcpy(catalog + sizeof(ffuCatalogPrefix), digest(hashes.constData(), size_t(hashes.size()), HASH_SHA1).constData(), 20);
    memcpy(catalog + sizeof(ffuCatalogPrefix) + 20, ffuCatalogSuffix, sizeof(ffuCatalogSuffix));
    memcpy(catalog + catalogBytes, hashes.constData(), size_t(hashes.size()));
    error = writeBytes(state.output, security, 0);
    if (error == 0) error = writeBytes(state.output, metadata, securityBytes);
    return error;
}

struct VerifyState
{
    HASH_CONTEXT hash;
    quint64 bytes = 0, total;
    ImageSaveProgress progress;
    void* context;
};

int verifyDecoded(void* context, const uint8_t* bytes, size_t size, uint64_t offset)
{
    auto* state = static_cast<VerifyState*>(context);
    if (offset != state->bytes || state->bytes > state->total || size > state->total - state->bytes) return EFBIG;
    hash_write[HASH_SHA256](&state->hash, bytes, size);
    state->bytes += size;
    if (state->progress) state->progress(state->context, ImageSavePhase::VerifyImage, state->bytes, state->total);
    return 0;
}
}

int ImageSaveMaximumBytes(quint64 size, unsigned sector, int format, quint64& maximum)
{
    maximum = 0;
    if (format == ImageSaveIso) {
        if (size == 0 || size > (quint64(64) << 40) || sector != 2048 || size % sector != 0) return ENOTSUP;
        maximum = size; return 0;
    }
    if (size == 0 || size > (quint64(64) << 40) || (sector != 512 && sector != 4096) || size % sector != 0) return ENOTSUP;
    if (format == IMG_COMPRESSION_VHD) {
        if (sector != 512 || size > quint64(2040) * 1024 * mib) return ENOTSUP;
        maximum = size + 512;
    } else if (format == IMG_COMPRESSION_VHDX) {
        const quint64 blocks = (size + vhdxBlockBytes - 1) / vhdxBlockBytes;
        maximum = alignBytes(size, vhdxBlockBytes) + 3 * mib + alignBytes((blocks + blocks / 128) * 8, mib);
    } else if (format == IMG_COMPRESSION_FFU) {
        FFU_CAPTURE_GEOMETRY geometry = {};
        const quint64 catalogBytes = sizeof(ffuCatalogPrefix) + 20 + sizeof(ffuCatalogSuffix);
        // Reserve the largest accepted manifest before opening the source.
        const int error = PlanFfuCapture(size, catalogBytes, FFU_MAX_MANIFEST_BYTES, &geometry);
        if (error != 0) return error;
        maximum = geometry.file_bytes;
    } else return EINVAL;
    return 0;
}

int CheckImageSaveDevice(int descriptor, quint64 device, quint64 size, quint64 sequence, unsigned& sector)
{
    sector = 0;
    struct stat info;
    if (fstat(descriptor, &info) != 0) return errno;
    const int flags = fcntl(descriptor, F_GETFL);
    if (flags < 0) return errno;
    if (!S_ISBLK(info.st_mode) || quint64(info.st_rdev) != device || (flags & O_ACCMODE) != O_RDONLY
        || (flags & (O_PATH | O_DIRECT)) != 0) return ESTALE;
    uint64_t capacity = 0, diskSequence = 0;
    int logicalSector = 0;
    if (ioctl(descriptor, BLKGETSIZE64, &capacity) != 0 || ioctl(descriptor, BLKGETDISKSEQ, &diskSequence) != 0
        || ioctl(descriptor, BLKSSZGET, &logicalSector) != 0) return errno;
    if (capacity != size || sequence == 0 || sequence != diskSequence) return ESTALE;
    if (logicalSector != 512 && logicalSector != 4096 && logicalSector != 2048) return ENOTSUP;
    sector = unsigned(logicalSector);
    return 0;
}

int SaveDeviceImage(int source, quint64 size, unsigned sector, int output,
    const QString& outputPath, int format, ImageSaveProgress progress, void* context, ImageSaveResult& result)
{
    result = {};
    quint64 maximum = 0;
    int error = ImageSaveMaximumBytes(size, sector, format, maximum);
    if (error != 0) return error;
    struct stat inputInfo, outputInfo;
    if (fstat(source, &inputInfo) != 0 || fstat(output, &outputInfo) != 0) return errno;
    const int inputFlags = fcntl(source, F_GETFL), outputFlags = fcntl(output, F_GETFL);
    if (inputFlags < 0 || outputFlags < 0) return errno;
    if ((inputFlags & O_ACCMODE) != O_RDONLY || (inputFlags & (O_DIRECT | O_PATH)) != 0) return EINVAL;
    if ((outputFlags & O_ACCMODE) != O_RDWR || (outputFlags & (O_APPEND | O_DIRECT | O_PATH)) != 0) return EINVAL;
    if ((!S_ISREG(inputInfo.st_mode) && !S_ISBLK(inputInfo.st_mode)) || !S_ISREG(outputInfo.st_mode)
        || outputInfo.st_size != 0 || outputInfo.st_nlink != 1) return EINVAL;
    if (inputInfo.st_dev == outputInfo.st_dev && inputInfo.st_ino == outputInfo.st_ino) return EINVAL;
    const bool rawIso = format == ImageSaveIso;
    if (rawIso ? !outputPath.endsWith(QStringLiteral(".iso"), Qt::CaseInsensitive)
        : GetImageCompressionType(QFile::encodeName(outputPath).constData()) != format) return EINVAL;
    uint64_t sequence = 0;
    if (S_ISBLK(inputInfo.st_mode) && ioctl(source, BLKGETDISKSEQ, &sequence) != 0) return errno;
    error = checkSource(source, inputInfo, size, sector, sequence);
    if (error == 0) error = InitializeNativeHashes();
    SaveState state{source, output, size, 0, sector, progress, context, {}};
    hash_init[HASH_SHA256](&state.hash);
    if (error == 0 && progress) progress(context, ImageSavePhase::Copy, 0, size);
    if (error == 0) {
        if (format == IMG_COMPRESSION_VHD) error = saveVhd(state);
        else if (format == IMG_COMPRESSION_VHDX) error = saveVhdx(state);
        else if (rawIso) {
            QByteArray buffer(transferBytes, Qt::Uninitialized);
            for (quint64 offset = 0; error == 0 && offset < size; offset += quint64(buffer.size())) {
                const size_t bytes = size_t(qMin(quint64(buffer.size()), size - offset));
                error = state.read(buffer, offset, bytes);
                if (error == 0) error = positioned(output, buffer.data(), bytes, offset, true);
            }
        } else error = saveFfu(state);
    }
    if (error == 0) error = checkSource(source, inputInfo, size, sector, sequence);
    if (error == 0 && fsync(output) != 0) error = errno;
    hash_final[HASH_SHA256](&state.hash);
    const QByteArray expected(reinterpret_cast<const char*>(state.hash.buf), 32);
    IMAGE_FILE_SOURCE saved = {-1, {}, false, false, nullptr, 0};
    const QByteArray path = QFile::encodeName(outputPath);
    if (error == 0) error = OpenImageFileSource(path.constData(), &saved);
    if (error == 0 && (saved.info.st_dev != outputInfo.st_dev || saved.info.st_ino != outputInfo.st_ino)) error = ESTALE;
    VerifyState verified{{}, 0, size, progress, context};
    hash_init[HASH_SHA256](&verified.hash);
    COMPRESSED_IMAGE_RESULT decoded = {};
    if (error == 0 && progress) progress(context, ImageSavePhase::VerifyImage, 0, size);
    if (error == 0 && rawIso) {
        if (saved.info.st_size != off_t(size)) error = ESTALE;
        QByteArray buffer(transferBytes, Qt::Uninitialized);
        for (quint64 offset = 0; error == 0 && offset < size; offset += quint64(buffer.size())) {
            const size_t bytes = size_t(qMin(quint64(buffer.size()), size - offset));
            error = positioned(saved.fd, buffer.data(), bytes, offset, false);
            if (error == 0) error = verifyDecoded(&verified, reinterpret_cast<const uint8_t*>(buffer.constData()), bytes, offset);
        }
    } else if (error == 0) error = ReadCompressedImageSource(&saved, path.constData(), size, verifyDecoded, &verified, &decoded);
    if (saved.fd >= 0) {
        const int descriptor = saved.fd;
        saved.fd = -1;
        if (close(descriptor) != 0 && error == 0) error = errno;
    }
    hash_final[HASH_SHA256](&verified.hash);
    if (error == 0 && (verified.bytes != size || QByteArray(reinterpret_cast<const char*>(verified.hash.buf), 32) != expected)) error = EBADMSG;
    HASH_CONTEXT sourceHash;
    hash_init[HASH_SHA256](&sourceHash);
    QByteArray buffer(transferBytes, '\0');
    if (error == 0 && progress) progress(context, ImageSavePhase::VerifySource, 0, size);
    for (quint64 offset = 0; error == 0 && offset < size; offset += quint64(buffer.size())) {
        const size_t bytes = size_t(qMin(quint64(buffer.size()), size - offset));
        error = positioned(source, buffer.data(), bytes, offset, false);
        if (error == 0) {
            hash_write[HASH_SHA256](&sourceHash, reinterpret_cast<const uint8_t*>(buffer.constData()), bytes);
            if (progress) progress(context, ImageSavePhase::VerifySource, offset + bytes, size);
        }
    }
    hash_final[HASH_SHA256](&sourceHash);
    if (error == 0 && QByteArray(reinterpret_cast<const char*>(sourceHash.buf), 32) != expected) error = ESTALE;
    if (error == 0) error = checkSource(source, inputInfo, size, sector, sequence);
    if (error == 0 && IsRufusJobCancelled()) error = ECANCELED;
    if (error == 0) {
        result.bytes = size;
        result.sha256 = expected;
    }
    result.error = error;
    return error;
}
}
