// SPDX-License-Identifier: GPL-3.0-or-later
// Independent test serializer using the published FFU structures. No production
// parser is used to encode or calculate the expected raw-disk hash.
void appendFfu32(QByteArray& data, quint32 value)
{
    for (unsigned i = 0; i < 4; i++)
        data.append(char(value >> (8 * i)));
}

QByteArray ffuLiteralXpress(const QByteArray& data)
{
    QByteArray packed;
    for (qsizetype i = 0; i < data.size(); i += 32) {
        packed.append(QByteArray(4, '\0'));
        packed.append(data.constData() + i, qMin(qsizetype(32), data.size() - i));
    }
    return packed;
}

QByteArray ffuLiteralLznt1(const QByteArray& data)
{
    QByteArray packed;
    for (qsizetype i = 0; i < data.size(); i += 4096) {
        const unsigned count = unsigned(qMin(qsizetype(4096), data.size() - i));
        const unsigned header = 0x3000 | (count - 1);
        packed.append(char(header)); packed.append(char(header >> 8));
        packed.append(data.constData() + i, count);
    }
    return packed;
}

QByteArray ffuLiteralHuffman(const QByteArray& data)
{
    QByteArray packed;
    packed.reserve(data.size() * 9 / 8 + (data.size() / 65536 + 1) * 260);
    for (qsizetype start = 0; start < data.size(); start += 65536) {
        const qsizetype end = qMin(start + 65536, data.size());
        const char repeated = data[start]; bool uniform = true;
        for (qsizetype i = start + 1; i < end; i++) if (data[i] != repeated) { uniform = false; break; }
        if (uniform && end - start == 65536) {
            // Two canonical one-bit codes: one literal and one length-65535
            // distance-one match. This keeps full restore fixtures compact.
            QByteArray table(256, '\0');
            const unsigned symbol = quint8(repeated);
            table[symbol / 2] = char(1u << (4 * (symbol % 2))); table[135] = char(0x10);
            packed.append(table); packed.append(QByteArray::fromHex("00400000fffcff"));
            continue;
        }
        packed.append(QByteArray(256, char(0x99))); // Complete canonical 9-bit tree.
        quint32 bits = 0; unsigned count = 0;
        for (qsizetype i = start; i < end; i++) {
            bits = (bits << 9) | quint8(data[i]); count += 9;
            if (count >= 16) {
                count -= 16; const unsigned word = bits >> count;
                packed.append(char(word)); packed.append(char(word >> 8)); bits &= (1u << count) - 1;
            }
        }
        if (count) {
            const unsigned word = bits << (16 - count); packed.append(char(word)); packed.append(char(word >> 8));
        }
        packed.append(QByteArray(2, '\0')); // MS-XCA lookahead padding; end symbol optional.
    }
    return packed;
}

void makeGuestFfu(const QString& rawPath, const QString& packedPath, bool compressed, unsigned algorithm = 3, bool grouped = false)
{
    constexpr qsizetype blockBytes = 131072;
    QFile raw(rawPath);
    require(raw.open(QIODevice::ReadOnly) && raw.size() % blockBytes == 0, "FFU captured disk");
    const quint32 blocks = quint32(raw.size() / blockBytes);
    const QByteArray manifest = "[FullFlash]\r\nOSVersion=10.0.22621.0\r\n[Store]\r\nMinSectorCount="
        + QByteArray::number(raw.size() / 512) + "\r\nSectorSize=512\r\n";
    QByteArray image;
    appendFfu32(image, 24);
    image.append("ImageFlash  ", 12);
    appendFfu32(image, quint32(manifest.size()));
    appendFfu32(image, 128);
    image.append(manifest);
    image.resize(blockBytes, '\0');
    QByteArray descriptors, payload, hashes;
    quint32 descriptorCount = 0;
    // A temporary first-block image is superseded by the final data later.
    for (quint32 i = 0; i <= blocks;) {
        const quint32 count = i == 0 ? 1 : grouped ? qMin(quint32(2), blocks + 1 - i) : 1;
        const QByteArray decoded = i == 0 ? QByteArray(blockBytes, '\0') : raw.read(count * blockBytes);
        require(decoded.size() == count * blockBytes, "FFU encoder chunk group");
        const QByteArray packed = !compressed ? decoded : algorithm == 2 ? ffuLiteralLznt1(decoded)
            : algorithm == 4 ? ffuLiteralHuffman(decoded) : ffuLiteralXpress(decoded);
        appendFfu32(descriptors, 1);
        appendFfu32(descriptors, count);
        if (compressed)
            appendFfu32(descriptors, quint32(packed.size()));
        const bool fromEnd = i != 0 && i + count == blocks + 1;
        appendFfu32(descriptors, fromEnd ? 2 : 0);
        appendFfu32(descriptors, fromEnd ? count - 1 : i == 0 ? 0 : i - 1);
        for (quint32 j = 0; j < count; j++)
            hashes.append(QCryptographicHash::hash(QByteArrayView(decoded.constData() + j * blockBytes, blockBytes), QCryptographicHash::Sha256));
        payload.append(packed); i += count; descriptorCount++;
    }
    raw.close();
    QByteArray store;
    appendFfu32(store, 0);
    store.append(compressed ? QByteArray::fromHex("0100000003000000") : QByteArray::fromHex("0100000002000000"));
    store.append(QByteArray(192, '\0'));
    appendFfu32(store, blockBytes);
    appendFfu32(store, descriptorCount);
    appendFfu32(store, quint32(descriptors.size()));
    for (unsigned i = 0; i < 7; i++)
        appendFfu32(store, 0);
    appendFfu32(store, descriptorCount);
    if (compressed)
        appendFfu32(store, algorithm);
    store.append(descriptors);
    store.resize((store.size() + blockBytes - 1) / blockBytes * blockBytes, '\0');
    QByteArray metadata = image + store;
    QByteArray allHashes;
    for (qsizetype offset = 0; offset < metadata.size(); offset += blockBytes)
        allHashes.append(QCryptographicHash::hash(QByteArrayView(metadata.constData() + offset, blockBytes), QCryptographicHash::Sha256));
    allHashes.append(hashes);
    QByteArray security;
    appendFfu32(security, 32);
    security.append("SignedImage ", 12);
    appendFfu32(security, 128);
    appendFfu32(security, 0x800c);
    appendFfu32(security, 0);
    appendFfu32(security, quint32(allHashes.size()));
    security.append(allHashes);
    security.resize((security.size() + blockBytes - 1) / blockBytes * blockBytes, '\0');
    QFile output(packedPath);
    require(output.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && output.write(security) == security.size() && output.write(metadata) == metadata.size()
        && output.write(payload) == payload.size() && output.flush(), "independent FFU fixture encoded");
    output.close();
    require(chown(QFile::encodeName(packedPath).constData(), 1000, 1000) == 0, "FFU caller ownership");
}

void runFfuQtCases(rufus::PrepareImageRequest base, const QString& rawPath, const QByteArray& expectedDisk)
{
    constexpr qsizetype logicalBytes = 1024 * 1024;
    const QString smallPath = QStringLiteral("/run/caller-home/small.raw");
    const QString packedPath = QStringLiteral("/run/caller-home/source.ffu");
    QFile raw(rawPath);
    require(raw.open(QIODevice::ReadOnly), "FFU boot prefix source");
    const QByteArray first = raw.read(65536);
    require(first.size() == 65536, "FFU boot prefix size");
    raw.close();
    QByteArray logical(logicalBytes, '\0');
    logical.replace(0, first.size(), first);
    logical.replace(logicalBytes - 65536, 65536, QByteArray(65536, char(0x3c)));
    writeGuestFile(smallPath, logical);
    makeGuestFfu(smallPath, packedPath, true);
    require(QFile::remove(smallPath), "small FFU raw source removed");
    auto request = compressedRequest(base, packedPath, logicalBytes,
        QCryptographicHash::hash(logical, QCryptographicHash::Sha256), IMG_COMPRESSION_FFU);
    publishTargetMount(rawObjects, request, {}, rawPartition);
    runCompressedQtCase(request);
    QFile disk(QStringLiteral("/dev/sda"));
    require(disk.open(QIODevice::ReadOnly) && disk.read(logicalBytes - 131072) == logical.left(logicalBytes - 131072), "XPRESS FFU Qt readback");
    require(disk.seek(qint64(base.size - 512)) && disk.read(512) == logical.right(512), "FFU target end-relative tail");
    disk.close();
    require(QFile::remove(packedPath), "small FFU source removed");
    makeGuestFfu(rawPath, packedPath, false);
    require(QFile::remove(rawPath), "full FFU raw source removed");
    request = compressedRequest(base, packedPath, base.size, expectedDisk, IMG_COMPRESSION_FFU);
    publishTargetMount(rawObjects, request, {}, rawPartition);
    runCompressedQtCase(request);
    require(diskDigest() == expectedDisk, "full FFU Qt restore readback");
    require(QFile::remove(packedPath), "full FFU source removed");
    printf("RUFUS_FFU_QT_HELPER_DONE SHA256=%s\n", expectedDisk.toHex().constData());
}

void checkFfuGptTarget(const QByteArray& table, quint64 capacity)
{
    constexpr qsizetype sector = 512;
    const qsizetype tableBytes = table.size();
    const quint64 tableSectors = quint64(tableBytes + sector - 1) / sector;
    const quint64 targetSectors = capacity / sector;
    QFile target(QStringLiteral("/dev/sda")); require(target.open(QIODevice::ReadOnly), "GPT larger target readback");
    require(target.seek(2 * sector) && target.read(tableBytes) == table, "GPT primary partition table unchanged");
    require(target.seek(qint64((targetSectors - 1 - tableSectors) * sector)) && target.read(tableBytes) == table, "GPT backup table relocated");
    for (bool backup : {false, true}) {
        require(target.seek(qint64(backup ? capacity - sector : sector)), "GPT relocated header position");
        QByteArray header = target.read(sector); require(header.size() == sector, "GPT relocated header size");
        quint64 current = 0, alternate = 0, last = 0; quint32 crc = 0;
        for (unsigned i = 0; i < 8; i++) {
            current |= quint64(quint8(header[24 + i])) << (8 * i);
            alternate |= quint64(quint8(header[32 + i])) << (8 * i);
            last |= quint64(quint8(header[48 + i])) << (8 * i);
        }
        for (unsigned i = 0; i < 4; i++) { crc |= quint32(quint8(header[16 + i])) << (8 * i); header[16 + i] = 0; }
        require(current == (backup ? targetSectors - 1 : 1) && alternate == (backup ? 1 : targetSectors - 1)
            && last == targetSectors - 2 - tableSectors && crc == quint32(crc32(0, reinterpret_cast<const Bytef*>(header.constData()), 92)),
            "GPT relocated header geometry and CRC");
    }
    target.close();
}

void runFfuGptQtCase(rufus::PrepareImageRequest base)
{
    constexpr qsizetype bytes = 8 * 1024 * 1024, sector = 512, tableBytes = 16384;
    const quint64 sectors = bytes / sector;
    QByteArray disk(bytes, '\0'), table(tableBytes, '\0');
    // Deterministic GPT fixture with one unchanged partition and Unicode name.
    table.replace(0, 32, QByteArray::fromHex("28732ac11ff8d211ba4b00a0c93ec93b7856341234127856901234567890abcd"));
    for (unsigned i = 0; i < 8; i++) {
        table[32 + i] = char(quint64(2048) >> (8 * i));
        table[40 + i] = char(quint64(4095) >> (8 * i));
    }
    table[56] = 'E'; table[58] = char(0xa9); table[59] = 3;
    const quint32 tableCrc = quint32(crc32(0, reinterpret_cast<const Bytef*>(table.constData()), table.size()));
    disk[510] = char(0x55); disk[511] = char(0xaa); disk[450] = char(0xee); disk[454] = 1;
    const quint32 protective = quint32(sectors - 1);
    for (unsigned i = 0; i < 4; i++) disk[458 + i] = char(protective >> (8 * i));
    for (bool backup : {false, true}) {
        QByteArray header; header.append("EFI PART", 8);
        appendFfu32(header, 0x10000); appendFfu32(header, 92); appendFfu32(header, 0); appendFfu32(header, 0);
        for (quint64 value : {backup ? sectors - 1 : quint64(1), backup ? quint64(1) : sectors - 1,
                quint64(34), sectors - 34})
            for (unsigned i = 0; i < 8; i++) header.append(char(value >> (8 * i)));
        header.append(QByteArray(16, char(0x6c)));
        const quint64 tableLba = backup ? sectors - 33 : 2;
        for (unsigned i = 0; i < 8; i++) header.append(char(tableLba >> (8 * i)));
        appendFfu32(header, 128); appendFfu32(header, 128); appendFfu32(header, tableCrc);
        const quint32 crc = quint32(crc32(0, reinterpret_cast<const Bytef*>(header.constData()), header.size()));
        for (unsigned i = 0; i < 4; i++) header[16 + i] = char(crc >> (8 * i));
        header.resize(sector, '\0'); disk.replace(backup ? bytes - sector : sector, sector, header);
        disk.replace(qsizetype(tableLba * sector), tableBytes, table);
    }
    const QString raw = QStringLiteral("/run/caller-home/gpt.raw"), packed = QStringLiteral("/run/caller-home/gpt.ffu");
    writeGuestFile(raw, disk); makeGuestFfu(raw, packed, true, 2, true);
    auto request = compressedRequest(base, packed, bytes, QCryptographicHash::hash(disk, QCryptographicHash::Sha256), IMG_COMPRESSION_FFU);
    publishTargetMount(rawObjects, request, {}, rawPartition); runCompressedQtCase(request);
    checkFfuGptTarget(table, base.size); require(QFile::remove(raw) && QFile::remove(packed), "GPT fixture cleanup");
    puts("RUFUS_FFU_GPT_ENLARGED PARTITION_TABLE_UNCHANGED=1 HEADERS_VALID=1");
}

void runFfuVariantQtCases(rufus::PrepareImageRequest base, const QString& rawPath, const QByteArray& expectedDisk)
{
    constexpr qsizetype logicalBytes = 1048576;
    const QString smallPath = QStringLiteral("/run/caller-home/small.raw");
    const QString packedPath = QStringLiteral("/run/caller-home/source.ffu");
    QFile raw(rawPath); require(raw.open(QIODevice::ReadOnly), "FFU variant boot source");
    const QByteArray first = raw.read(65536); raw.close();
    require(first.size() == 65536, "FFU variant boot prefix");
    QByteArray logical(logicalBytes, '\0'); logical.replace(0, first.size(), first);
    logical.replace(logicalBytes - 65536, 65536, QByteArray(65536, char(0x4a)));
    writeGuestFile(smallPath, logical);
    for (unsigned algorithm : {2u, 4u}) {
        makeGuestFfu(smallPath, packedPath, true, algorithm, true);
        auto request = compressedRequest(base, packedPath, logicalBytes,
            QCryptographicHash::hash(logical, QCryptographicHash::Sha256), IMG_COMPRESSION_FFU);
        publishTargetMount(rawObjects, request, {}, rawPartition); runCompressedQtCase(request);
        QFile disk(QStringLiteral("/dev/sda"));
        require(disk.open(QIODevice::ReadOnly) && disk.read(logicalBytes - 262144) == logical.left(logicalBytes - 262144), "FFU variant real logical readback");
        require(disk.seek(qint64(base.size - 512)) && disk.read(512) == logical.right(512), "FFU variant end-relative tail");
        disk.close(); require(QFile::remove(packedPath), "small FFU variant released");
    }
    require(QFile::remove(smallPath), "FFU variants raw prefix released");
    runFfuGptQtCase(base);
    makeGuestFfu(rawPath, packedPath, true, 2, true);
    require(QFile::remove(rawPath), "full FFU variant raw released");
    auto request = compressedRequest(base, packedPath, base.size, expectedDisk, IMG_COMPRESSION_FFU);
    publishTargetMount(rawObjects, request, {}, rawPartition); runCompressedQtCase(request);
    require(diskDigest() == expectedDisk && QFile::remove(packedPath), "LZNT1 grouped FFU complete restore");
    printf("RUFUS_FFU_VARIANTS_DONE SHA256=%s\n", expectedDisk.toHex().constData());
}

void runFfuGeometryQtCases(rufus::PrepareImageRequest base, const QString& rawPath, const QByteArray& expectedDisk)
{
    constexpr quint64 sourceBytes = 8 * 1024 * 1024 + 1536;
    const QString capturedPath = QStringLiteral("/run/caller-home/exact.ffu");
    QFile source(QStringLiteral("/dev/vda")), output(capturedPath);
    require(source.open(QIODevice::ReadOnly) && output.open(QIODevice::ReadWrite | QIODevice::Truncate), "FFU geometry source/output acquisition");
    uint64_t capacity = 0; int readOnly = 0;
    require(ioctl(source.handle(), BLKGETSIZE64, &capacity) == 0 && capacity == sourceBytes
        && ioctl(source.handle(), BLKROGET, &readOnly) == 0 && readOnly == 1, "non-aligned kernel read-only FFU source");
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (quint64 done = 0; done < sourceBytes;) {
        const QByteArray bytes = source.read(qint64(qMin(quint64(65536), sourceBytes - done)));
        require(!bytes.isEmpty(), "FFU source independent bytes"); hash.addData(bytes); done += quint64(bytes.size());
    }
    const QByteArray expected = hash.result();
    const bool extendedLayout = QFile::exists(QStringLiteral("/etc/rufus-ffu-layouts-fixture"));
    const qsizetype tableBytes = extendedLayout ? 2 * 1024 * 1024 : 16384;
    require(source.seek(1024), "FFU source partition table offset");
    const QByteArray table = source.read(tableBytes); require(table.size() == tableBytes, "FFU source partition table");
    rufus::ImageSaveResult saved;
    require(rufus::SaveDeviceImage(source.handle(), sourceBytes, 512, output.handle(), capturedPath,
        IMG_COMPRESSION_FFU, nullptr, nullptr, saved) == 0 && saved.bytes == sourceBytes && saved.sha256 == expected,
        "FFU exact source capture, verification and hash");
    const qint64 storedBytes = output.size();
    require(output.seek(24), "FFU capture security geometry");
    const QByteArray sizes = output.read(8); require(sizes.size() == 8, "FFU security lengths");
    const quint64 imageOffset = (32 + quint64(qFromLittleEndian<quint32>(sizes.constData()))
        + qFromLittleEndian<quint32>(sizes.constData() + 4) + 131071) / 131072 * 131072;
    require(output.seek(qint64(imageOffset + 16)), "FFU capture manifest geometry");
    const QByteArray length = output.read(4); require(length.size() == 4, "FFU manifest length");
    const quint32 manifestBytes = qFromLittleEndian<quint32>(length.constData());
    const quint64 storeOffset = (imageOffset + 24 + manifestBytes + 131071) / 131072 * 131072;
    require(storedBytes > 0 && quint64(storedBytes) < sourceBytes / 8
        && output.seek(qint64(storeOffset + 8)) && output.read(2) == QByteArray::fromHex("0300"),
        "production FFU capture uses compact compressed store");
    require(output.seek(qint64(storeOffset + 248)) && output.read(4) == QByteArray::fromHex("03000000"),
        "production FFU capture codec accepted by desktop DISM");
    require(!extendedLayout || manifestBytes > 131072, "FFU extended manifest spans chunks");
    require(ioctl(source.handle(), BLKROGET, &readOnly) == 0 && readOnly == 1, "FFU source remains read-only");
    source.close(); output.close();
    require(chown(QFile::encodeName(capturedPath).constData(), 1000, 1000) == 0, "FFU capture caller ownership");
    auto request = compressedRequest(base, capturedPath, sourceBytes, expected, IMG_COMPRESSION_FFU);
    QFile dirty(QStringLiteral("/dev/sda"));
    require(dirty.open(QIODevice::WriteOnly), "FFU dirty restore target");
    const QByteArray poison(65536, char(0xa5));
    for (quint64 offset = 0; offset < base.size; offset += quint64(poison.size()))
        require(dirty.write(poison) == poison.size(), "FFU target poison write");
    require(dirty.flush() && fsync(dirty.handle()) == 0, "FFU dirty target sync");
    dirty.close();
    publishTargetMount(rawObjects, request, {}, rawPartition); runCompressedQtCase(request);
    checkFfuGptTarget(table, base.size);
    QFile original(QStringLiteral("/dev/vda")), target(QStringLiteral("/dev/sda"));
    require(original.open(QIODevice::ReadOnly) && target.open(QIODevice::ReadOnly)
        && original.seek(1024) && target.seek(1024), "FFU source/target independent full readback");
    constexpr quint64 forwardBytes = 8 * 1024 * 1024, chunkBytes = 131072;
    for (quint64 offset = 1024; offset < forwardBytes;) {
        const qint64 count = qint64(qMin(quint64(65536), forwardBytes - offset));
        const QByteArray bytes = original.read(count);
        require(bytes.size() == count && target.read(count) == bytes, "compressed FFU forward bytes include restored zeros");
        offset += quint64(count);
    }
    const quint64 gapEnd = base.size - qMax(chunkBytes, quint64(tableBytes) + 512);
    for (quint64 offset = forwardBytes; offset < gapEnd;) {
        const qint64 count = qint64(qMin(quint64(poison.size()), gapEnd - offset));
        require(target.read(count) == poison.left(count), "FFU undeclared larger-target gap preserved");
        offset += quint64(count);
    }
    require(original.seek(qint64(sourceBytes - chunkBytes)), "FFU original end-relative tail");
    require(target.seek(qint64(base.size - chunkBytes)), "FFU relocated end-relative tail");
    const QByteArray tail = original.read(chunkBytes - 512);
    require(tail.size() == chunkBytes - 512 && target.read(chunkBytes - 512) == tail,
        "compressed FFU end-relative bytes independent readback");
    original.close(); target.close();
    require(QFile::remove(capturedPath), "FFU capture fixture released");
    printf("RUFUS_FFU_CAPTURE_EXACT SOURCE_BYTES=%llu BLOCK_BYTES=131072 READ_ONLY=1 SHA256=%s\n",
        static_cast<unsigned long long>(sourceBytes), expected.toHex().constData());
    printf("RUFUS_FFU_CAPTURE_COMPRESSED STORED_BYTES=%lld CODEC=3 DIRTY_READBACK=1 GAP_PRESERVED=1\n",
        static_cast<long long>(storedBytes));
    printf("RUFUS_FFU_CAPTURE_LAYOUT MANIFEST_BYTES=%u TABLE_BYTES=%lld MULTI_CHUNK=%d\n",
        manifestBytes, static_cast<long long>(tableBytes), int(extendedLayout));
    makeGuestFfu(rawPath, capturedPath, true, 3, true);
    require(QFile::remove(rawPath), "representative FFU raw source released");
    request = compressedRequest(base, capturedPath, base.size, expectedDisk, IMG_COMPRESSION_FFU);
    publishTargetMount(rawObjects, request, {}, rawPartition); runCompressedQtCase(request);
    require(diskDigest() == expectedDisk && QFile::remove(capturedPath), "representative FFU full restore");
    printf("RUFUS_FFU_GEOMETRY_DONE SHA256=%s\n", expectedDisk.toHex().constData());
}

QByteArray makeGuestPartialFfu(const QString& rawPath, const QString& packedPath, bool wrongBase)
{
    constexpr qsizetype blockBytes = 131072;
    QFile raw(rawPath);
    require(raw.open(QIODevice::ReadOnly), "partial FFU base fixture");
    const qint64 diskBytes = raw.size();
    QByteArray prefix = raw.read(512);
    require(prefix.size() == 512 && raw.seek(2 * blockBytes), "partial FFU validation prefix");
    const QByteArray old = raw.read(blockBytes);
    require(old == QByteArray(blockBytes, '\0'), "partial FFU old alignment gap");
    raw.close();
    if (wrongBase) prefix[0] = char(quint8(prefix[0]) ^ 1);
    const QByteArray payload(blockBytes, char(0x5c));
    const QByteArray manifest = "[FullFlash]\r\nOSVersion=10.0.22621.0\r\n[Store]\r\nMinSectorCount="
        + QByteArray::number(diskBytes / 512) + "\r\nSectorSize=512\r\n";
    QByteArray image;
    appendFfu32(image, 24); image.append("ImageFlash  ", 12);
    appendFfu32(image, quint32(manifest.size())); appendFfu32(image, 128);
    image.append(manifest); image.resize(blockBytes, '\0');
    QByteArray validation;
    appendFfu32(validation, 0); appendFfu32(validation, 0); appendFfu32(validation, quint32(prefix.size()));
    validation.append(prefix);
    appendFfu32(validation, 2 * blockBytes / 512); appendFfu32(validation, 0);
    appendFfu32(validation, quint32(old.size())); validation.append(old);
    QByteArray store;
    appendFfu32(store, 1); // Partial update, same V1 format as the published spec.
    store.append(QByteArray::fromHex("0100000002000000")); store.append(QByteArray(192, '\0'));
    appendFfu32(store, blockBytes); appendFfu32(store, 1); appendFfu32(store, 16);
    appendFfu32(store, 2); appendFfu32(store, quint32(validation.size()));
    for (unsigned i = 0; i < 5; ++i) appendFfu32(store, 0);
    appendFfu32(store, 1);
    require(store.size() == 248, "partial FFU fixed store header");
    store.append(validation);
    for (quint32 value : {1u, 1u, 0u, 2u}) appendFfu32(store, value);
    store.resize((store.size() + blockBytes - 1) / blockBytes * blockBytes, '\0');
    const QByteArray metadata = image + store;
    QByteArray hashes;
    for (qsizetype offset = 0; offset < metadata.size(); offset += blockBytes)
        hashes.append(QCryptographicHash::hash(QByteArrayView(metadata.constData() + offset, blockBytes), QCryptographicHash::Sha256));
    hashes.append(QCryptographicHash::hash(payload, QCryptographicHash::Sha256));
    QByteArray security;
    appendFfu32(security, 32); security.append("SignedImage ", 12); appendFfu32(security, 128);
    appendFfu32(security, 0x800c); appendFfu32(security, 0); appendFfu32(security, quint32(hashes.size()));
    security.append(hashes); security.resize(blockBytes, '\0');
    writeGuestFile(packedPath, security + metadata + payload);
    require(chown(QFile::encodeName(packedPath).constData(), 1000, 1000) == 0, "partial FFU caller ownership");
    QCryptographicHash logical(QCryptographicHash::Sha256);
    const QByteArray zeros(blockBytes, '\0');
    for (qint64 offset = 0; offset < diskBytes; offset += blockBytes)
        logical.addData(offset == 2 * blockBytes ? payload : zeros);
    return logical.result();
}

QByteArray runFfuPartialQtCases(rufus::PrepareImageRequest base, const QString& rawPath, const QByteArray& expectedDisk)
{
    constexpr qsizetype blockBytes = 131072;
    const QString packedPath = QStringLiteral("/run/caller-home/source.ffu");
    makeGuestFfu(rawPath, packedPath, false);
    auto request = compressedRequest(base, packedPath, base.size, expectedDisk, IMG_COMPRESSION_FFU);
    publishTargetMount(rawObjects, request, {}, rawPartition);
    runCompressedQtCase(request);
    require(diskDigest() == expectedDisk, "partial FFU base installed");
    QByteArray logical = makeGuestPartialFfu(rawPath, packedPath, true);
    request = compressedRequest(base, packedPath, base.size, logical, IMG_COMPRESSION_FFU);
    runCompressedHelperCase({"ffu-base-mismatch", ESTALE, 0, 0}, request);
    require(diskDigest() == expectedDisk, "wrong base refused unchanged");
    logical = makeGuestPartialFfu(rawPath, packedPath, false);
    request = compressedRequest(base, packedPath, base.size, logical, IMG_COMPRESSION_FFU);
    publishTargetMount(rawObjects, request, {}, rawPartition);
    runCompressedQtCase(request, true);
    QFile expected(rawPath);
    require(expected.open(QIODevice::ReadWrite) && expected.seek(2 * blockBytes)
        && expected.write(QByteArray(blockBytes, char(0x5c))) == blockBytes && expected.flush()
        && expected.seek(0), "independent expected partial image");
    QCryptographicHash hash(QCryptographicHash::Sha256);
    require(hash.addData(&expected), "independent partial whole-device hash");
    const QByteArray finalHash = hash.result();
    expected.close();
    require(diskDigest() == finalHash, "partial FFU changed only its declared block");
    require(QFile::remove(rawPath) && QFile::remove(packedPath), "partial FFU sources released");
    printf("RUFUS_FFU_PARTIAL_DONE SHA256=%s\n", finalHash.toHex().constData());
    return finalHash;
}
