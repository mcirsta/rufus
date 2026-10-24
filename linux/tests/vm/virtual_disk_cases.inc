// SPDX-License-Identifier: GPL-3.0-or-later
// Independent guest containers; the production reader never calls qemu-img.
void writeChainFile(const QString& path, const QByteArray& bytes)
{
    writeGuestFile(path, bytes);
    require(chown(QFile::encodeName(path).constData(), 1000, 1000) == 0, "chain caller ownership");
}
quint64 chainLe(const QByteArray& bytes, qsizetype offset, unsigned width)
{
    quint64 value = 0;
    for (unsigned i = 0; i < width; i++) value |= quint64(quint8(bytes[offset + i])) << (8 * i);
    return value;
}
void chainPut(QByteArray& bytes, qsizetype offset, quint64 value, unsigned width)
{
    for (unsigned i = 0; i < width; i++) bytes[offset + i] = char(value >> (8 * i));
}
QByteArray chainUtf16(const QString& text)
{
    QByteArray result(text.size() * 2, '\0');
    for (qsizetype i = 0; i < text.size(); i++) chainPut(result, i * 2, text[i].unicode(), 2);
    return result;
}
void chainCrc(QByteArray& bytes, qsizetype offset, qsizetype size)
{
    chainPut(bytes, offset + 4, 0, 4);
    quint32 crc = UINT32_MAX;
    for (qsizetype i = 0; i < size; i++) {
        crc ^= quint8(bytes[offset + i]);
        for (unsigned j = 0; j < 8; j++) crc = (crc >> 1) ^ (0x82f63b78u & (0u - (crc & 1)));
    }
    chainPut(bytes, offset + 4, ~crc, 4);
}
QByteArray chainVhd(const QString& parentPath, const QString& locator, const QByteArray& raw)
{
    constexpr qsizetype block = 65536;
    const QByteArray text = chainUtf16(locator);
    require(text.size() <= 512 && raw.size() % block == 0, "VHD parent fixture bounds");
    struct stat info;
    require(stat(QFile::encodeName(parentPath).constData(), &info) == 0, "VHD parent timestamp");
    QByteArray mark = vhdGuestFooter(quint64(raw.size()), true);
    putVhdBe(mark, 60, 4, 4); putVhdBe(mark, 64, vhdGuestChecksum(mark, 64), 4);
    QByteArray header(1024, '\0'); header.replace(0, 8, "cxsparse");
    putVhdBe(header, 8, UINT64_MAX, 8); putVhdBe(header, 16, 2048, 8);
    putVhdBe(header, 24, 0x10000, 4); putVhdBe(header, 28, raw.size() / block, 4);
    putVhdBe(header, 32, block, 4); putVhdBe(header, 56, quint64(info.st_mtim.tv_sec - 946684800), 4);
    header.replace(576, 4, "W2ru"); putVhdBe(header, 580, 1, 4); putVhdBe(header, 584, text.size(), 4); putVhdBe(header, 592, 1536, 8);
    putVhdBe(header, 36, vhdGuestChecksum(header, 36), 4);
    QByteArray locatorBytes(512, '\0'); locatorBytes.replace(0, text.size(), text);
    QByteArray bat(512, char(0xff)); putVhdBe(bat, 0, 5, 4);
    QByteArray bitmap(512, '\0'); bitmap[0] = char(0x40); // Sector1 child, all others parent.
    QByteArray payload(block, '\0'); payload.replace(512, 512, raw.mid(512, 512));
    return mark + header + locatorBytes + bat + bitmap + payload + mark;
}
QByteArray chainVhdx(const QByteArray& parent, const QString& locator, const QByteArray& first, bool dirty)
{
    constexpr qsizetype mib = 1048576, table = 65536;
    QByteArray child = parent.left(4 * mib);
    qsizetype bat = 0, metadata = 0;
    const QByteArray batId = QByteArray::fromHex("66 77 c2 2d 23 f6 00 42 9d 64 11 5e 9b fd 4a 08");
    for (qsizetype entry = 3 * table + 16; entry < 3 * table + 80; entry += 32) {
        const qsizetype start = qsizetype(chainLe(child, entry + 16, 8));
        if (child.mid(entry, 16) == batId) bat = start; else metadata = start;
    }
    require(bat && metadata && bat + mib <= child.size() && metadata + mib <= child.size(), "QEMU parent regions");
    child.replace(bat, mib, QByteArray(mib, '\0'));
    const unsigned count = unsigned(chainLe(child, metadata + 10, 2));
    qsizetype parameters = 0;
    const QByteArray parametersId = QByteArray::fromHex("37 67 a1 ca 36 fa 43 4d b3 b6 33 f0 aa 44 e7 6b");
    for (unsigned i = 0; i < count; i++) {
        const qsizetype entry = metadata + 32 + i * 32;
        if (child.mid(entry, 16) == parametersId) parameters = metadata + qsizetype(chainLe(child, entry + 16, 4));
    }
    require(parameters && chainLe(child, parameters, 4) == mib, "QEMU parent block size");
    chainPut(child, parameters + 4, 2, 4);
    QByteArray id = parent.mid(2 * table + 32, 16);
    const QByteArray order = QByteArray::fromHex("03 02 01 00 05 04 07 06 08 09 0a 0b 0c 0d 0e 0f");
    QByteArray guid;
    for (unsigned i = 0; i < 16; i++) { if (i == 4 || i == 6 || i == 8 || i == 10) guid += '-'; guid += QByteArray(1, id[quint8(order[i])]).toHex(); }
    const QStringList keys = {QStringLiteral("parent_linkage"), QStringLiteral("relative_path")};
    const QStringList values = {QString::fromLatin1('{' + guid + '}'), locator};
    QByteArray location(44, '\0'); location.replace(0, 16, QByteArray::fromHex("b7 ef 4a b0 9e d1 81 4a b7 89 25 b8 e9 44 59 13")); chainPut(location, 18, 2, 2);
    for (unsigned i = 0; i < 2; i++) {
        const QByteArray key = chainUtf16(keys[i]), value = chainUtf16(values[i]);
        chainPut(location, 20 + 12 * i, location.size(), 4); location += key;
        chainPut(location, 24 + 12 * i, location.size(), 4); location += value;
        chainPut(location, 28 + 12 * i, key.size(), 2); chainPut(location, 30 + 12 * i, value.size(), 2);
    }
    const qsizetype entry = metadata + 32 + count * 32;
    child.replace(entry, 16, QByteArray::fromHex("2d 5f d3 a8 0b b3 4d 45 ab f7 d3 d8 48 34 ab 0c"));
    chainPut(child, entry + 16, table + 8192, 4); chainPut(child, entry + 20, location.size(), 4); chainPut(child, entry + 24, 4, 4); chainPut(child, entry + 28, 0, 4);
    chainPut(child, metadata + 10, count + 1, 2); child.replace(metadata + table + 8192, location.size(), location);
    chainPut(child, bat, 4 * mib | 7, 8); chainPut(child, bat + 4096 * 8, 5 * mib | 6, 8);
    QByteArray payload(mib, '\0'); if (!dirty) payload.replace(0, 4096, first);
    QByteArray bitmap(mib, '\0'); bitmap[0] = char(0xff); child += payload + bitmap;
    if (dirty) {
        const QByteArray logGuid(16, char(0x53));
        for (qsizetype offset : {table, 2 * table}) { child.replace(offset + 48, 16, logGuid); chainCrc(child, offset, 4096); }
        QByteArray log(8192, '\0'); log.replace(0, 4, "loge"); chainPut(log, 8, 8192, 4); chainPut(log, 16, 2, 8); chainPut(log, 24, 1, 4);
        log.replace(32, 16, logGuid); chainPut(log, 48, child.size(), 8); chainPut(log, 56, child.size(), 8);
        log.replace(64, 4, "desc"); log.replace(68, 4, first.right(4)); log.replace(72, 8, first.left(8)); chainPut(log, 80, 4 * mib, 8); chainPut(log, 88, 2, 8);
        log.replace(4096, 4, "data"); log.replace(4104, 4084, first.mid(8, 4084)); chainPut(log, 8188, 2, 4); chainCrc(log, 0, log.size());
        child.replace(mib, log.size(), log);
    }
    return child;
}
void runVirtualDiskQtCases(rufus::PrepareImageRequest base, const QString& rawPath, const QByteArray& expectedDisk)
{
    constexpr qsizetype smallBytes = 262144;
    const QString parentPath = QStringLiteral("/run/caller-home/parent.vhd"), childPath = QStringLiteral("/run/caller-home/child.vhd");
    QFile raw(rawPath); require(raw.open(QIODevice::ReadOnly), "chain raw source"); const QByteArray first = raw.read(65536); raw.close();
    QByteArray logical(smallBytes, '\0'); logical.replace(0, first.size(), first);
    const QByteArray parent = logical + vhdGuestFooter(smallBytes, false); writeChainFile(parentPath, parent);
    QByteArray child = chainVhd(parentPath, QStringLiteral("parent.vhd"), logical); writeChainFile(childPath, child);
    auto request = compressedRequest(base, childPath, smallBytes, QCryptographicHash::hash(logical, QCryptographicHash::Sha256), IMG_COMPRESSION_VHD);
    publishTargetMount(rawObjects, request, {}, rawPartition);
    require(chown(QFile::encodeName(parentPath).constData(), 0, 0) == 0, "wrong parent owner fixture");
    runCompressedHelperCase({"parent-owner", EACCES, 0, 0}, request);
    require(chown(QFile::encodeName(parentPath).constData(), 1000, 1000) == 0, "parent owner restored");
    require(mount("/dev/sda1", "/mnt/target", "vfat", 0, "uid=1000,gid=1000") == 0, "parent target mount");
    const QString targetParent = QStringLiteral("/mnt/target/parent.vhd"); writeChainFile(targetParent, parent);
    writeChainFile(childPath, chainVhd(targetParent, QStringLiteral("../../mnt/target/parent.vhd"), logical));
    request = compressedRequest(base, childPath, smallBytes, QCryptographicHash::hash(logical, QCryptographicHash::Sha256), IMG_COMPRESSION_VHD);
    publishTargetMount(rawObjects, request, QByteArray("/mnt/target"), rawPartition);
    runCompressedHelperCase({"parent-target", EBUSY, 0, 0}, request);
    require(!targetMounted(rawPartition), "target parent released");
    writeChainFile(childPath, chainVhd(parentPath, QStringLiteral("parent.vhd"), logical));
    request = compressedRequest(base, childPath, smallBytes, QCryptographicHash::hash(logical, QCryptographicHash::Sha256), IMG_COMPRESSION_VHD);
    publishTargetMount(rawObjects, request, {}, rawPartition); runCompressedQtCase(request);
    QFile disk(QStringLiteral("/dev/sda")); require(disk.open(QIODevice::ReadOnly) && disk.read(smallBytes) == logical, "VHD chain logical readback"); disk.close();
    require(readFile(parentPath) == parent && QFile::remove(parentPath) && QFile::remove(childPath), "VHD parent immutable released");
    const QString packed = QStringLiteral("/run/caller-home/parent.vhdx"), childX = QStringLiteral("/run/caller-home/child.vhdx"), smallRaw = QStringLiteral("/run/caller-home/small.raw");
    logical = QByteArray(3 * 1048576, '\0'); logical.replace(0, first.size(), first); writeChainFile(smallRaw, logical);
    makeGuestVhdx(smallRaw, packed, false); const QByteArray smallParent = readFile(packed);
    writeChainFile(childX, chainVhdx(smallParent, QStringLiteral("parent.vhdx"), logical.left(4096), false));
    request = compressedRequest(base, childX, logical.size(), QCryptographicHash::hash(logical, QCryptographicHash::Sha256), IMG_COMPRESSION_VHDX);
    publishTargetMount(rawObjects, request, {}, rawPartition); runCompressedQtCase(request);
    require(disk.open(QIODevice::ReadOnly) && disk.read(logical.size()) == logical, "VHDX chain logical readback"); disk.close();
    require(readFile(packed) == smallParent && QFile::remove(smallRaw) && QFile::remove(packed) && QFile::remove(childX), "VHDX small immutable released");
    makeGuestVhdx(rawPath, packed, false); const QByteArray fullParent = readFile(packed);
    const QByteArray dirty = chainVhdx(fullParent, QStringLiteral("parent.vhdx"), first.left(4096), true); writeChainFile(childX, dirty);
    require(QFile::remove(rawPath), "chain original raw released");
    request = compressedRequest(base, childX, base.size, expectedDisk, IMG_COMPRESSION_VHDX);
    publishTargetMount(rawObjects, request, {}, rawPartition); runCompressedQtCase(request);
    require(diskDigest() == expectedDisk && readFile(packed) == fullParent && readFile(childX) == dirty, "parent plus log whole disk and source immutability");
    require(QFile::remove(packed) && QFile::remove(childX), "chain sources released");
    printf("RUFUS_VIRTUAL_DISK_DONE SHA256=%s\n", expectedDisk.toHex().constData());
}
