// SPDX-License-Identifier: GPL-3.0-or-later
// Independent VTSI fixture construction and readback on the guest's USB disk.
struct VtsiGuestExtent
{
    quint64 diskOffset;
    quint64 bytes;
};

void appendVtsi64(QByteArray& output, quint64 value)
{
    for (unsigned byte = 0; byte < 8; byte++) output.append(char(value >> (8 * byte)));
}

void putVtsiValue(QByteArray& output, qsizetype offset, quint64 value, unsigned bytes)
{
    for (unsigned byte = 0; byte < bytes; byte++) output[offset + byte] = char(value >> (8 * byte));
}

quint32 vtsiGuestChecksum(const QByteArray& bytes)
{
    quint32 sum = 0;
    for (char byte : bytes) sum += quint8(byte);
    return ~sum;
}

QByteArray vtsiGuestLayoutHash(quint64 diskBytes, const QList<VtsiGuestExtent>& extents)
{
    QByteArray values;
    quint64 sourceOffset = 0;
    for (const VtsiGuestExtent& extent : extents)
    {
        appendVtsi64(values, extent.diskOffset);
        appendVtsi64(values, sourceOffset);
        appendVtsi64(values, extent.bytes);
        sourceOffset += extent.bytes;
    }
    appendVtsi64(values, diskBytes);
    appendVtsi64(values, sourceOffset);
    return QCryptographicHash::hash(values, QCryptographicHash::Sha256);
}

QByteArray vtsiGuestTail(quint64 diskBytes, quint64 dataBytes, const QList<VtsiGuestExtent>& extents)
{
    constexpr qsizetype sectorBytes = 512;
    require(dataBytes % sectorBytes == 0 && diskBytes % sectorBytes == 0, "VTSI guest sector geometry");
    QByteArray table;
    for (const VtsiGuestExtent& extent : extents)
    {
        require(extent.diskOffset % sectorBytes == 0 && extent.bytes % sectorBytes == 0,
            "VTSI guest extent alignment");
        appendVtsi64(table, extent.diskOffset / sectorBytes);
        appendVtsi64(table, extent.bytes / sectorBytes);
        appendVtsi64(table, 0xdeadbeef); // The original decoder consumes packed data consecutively.
    }
    QByteArray footer(sectorBytes, '\0');
    footer.replace(0, 8, QByteArray("VENTOY\0\0", 8));
    putVtsiValue(footer, 8, 1, 2);
    putVtsiValue(footer, 12, diskBytes, 8);
    putVtsiValue(footer, 20, 0x12345678, 4);
    putVtsiValue(footer, 28, quint64(extents.size()), 4);
    putVtsiValue(footer, 32, vtsiGuestChecksum(table), 4);
    putVtsiValue(footer, 36, dataBytes, 8);
    putVtsiValue(footer, 24, vtsiGuestChecksum(footer), 4);
    table.append(QByteArray((sectorBytes - table.size() % sectorBytes) % sectorBytes, '\0'));
    return table + footer;
}

void runVtsiHelperCases(rufus::PrepareImageRequest base, const QString& rawPath,
    const QByteArray& expectedDisk)
{
    constexpr quint64 sectorBytes = 512;
    constexpr quint64 extentBytes = 64 * 1024;
    constexpr quint64 secondOffset = 384 * sectorBytes;
    constexpr quint64 logicalBytes = 512 * sectorBytes;
    const QList<VtsiGuestExtent> sparseExtents = {{0, extentBytes}, {secondOffset, extentBytes}};
    const QString packedPath = QStringLiteral("/run/caller-home/source.vtsi");
    QFile raw(rawPath);
    require(raw.open(QIODevice::ReadOnly), "VTSI guest original source");
    const QByteArray first = raw.read(qint64(extentBytes));
    require(first.size() == qint64(extentBytes), "VTSI guest first extent");
    raw.close();
    const QByteArray second(qsizetype(extentBytes), char(0x3c));
    const QByteArray packed = first + second + vtsiGuestTail(logicalBytes, 2 * extentBytes, sparseExtents);
    writeGuestFile(packedPath, packed);
    require(chown(QFile::encodeName(packedPath).constData(), 1000, 1000) == 0, "VTSI guest caller ownership");
    QByteArray logical(qsizetype(logicalBytes), '\0');
    logical.replace(0, qsizetype(extentBytes), first);
    logical.replace(qsizetype(secondOffset), qsizetype(extentBytes), second);
    auto request = compressedRequest(base, packedPath, logicalBytes,
        QCryptographicHash::hash(logical, QCryptographicHash::Sha256), BLED_COMPRESSION_VTSI);
    request.imageLayoutSha256 = vtsiGuestLayoutHash(logicalBytes, sparseExtents);
    const bool qtFixture = QFile::exists(QStringLiteral("/etc/rufus-compressed-qt-fixture"));
    auto mismatch = request;
    mismatch.imageLayoutSha256[0] ^= 1;
    runCompressedHelperCase({"layout", ESTALE, 0, 0}, mismatch);
    QByteArray expected(qsizetype(logicalBytes), char(0xa5));
    expected.replace(0, qsizetype(extentBytes), first);
    expected.replace(qsizetype(secondOffset), qsizetype(extentBytes), second);
    if (qtFixture)
    {
        const int target = open("/dev/sda", O_RDWR | O_EXCL | O_CLOEXEC);
        const QByteArray poison(qsizetype(logicalBytes), char(0xa5));
        require(target >= 0 && pwrite(target, poison.constData(), size_t(poison.size()), 0) == poison.size()
            && fsync(target) == 0 && close(target) == 0, "poison VTSI Qt sparse gap");
        publishTargetMount(rawObjects, request, {}, rawPartition);
        runCompressedQtCase(request);
        QFile disk(QStringLiteral("/dev/sda"));
        require(disk.open(QIODevice::ReadOnly) && disk.seek(qint64(extentBytes))
            && disk.read(qint64(secondOffset - extentBytes)) == QByteArray(qsizetype(secondOffset - extentBytes), char(0xa5)),
            "ordinary VTSI Qt preserves sparse gap");
        require(disk.seek(qint64(base.size - sectorBytes))
            && disk.read(qint64(sectorBytes)) == QByteArray(qsizetype(sectorBytes), char(0xa5)),
            "ordinary VTSI Qt preserves target tail");
        disk.close();
    }
    else
        runCompressedHelperCase({"observed-sparse", 0, 2 * extentBytes, 2 * extentBytes, "observe", true},
            request, {}, expected);
    require(QFile::remove(packedPath), "small VTSI guest source removed");

    // Reuse the original 64-MiB file's data area, avoiding a second 64-MiB
    // allocation inside the independent caller's 128-MiB tmpfs.
    require(QFile::rename(rawPath, packedPath), "rename bootable raw image as VTSI data area");
    QFile full(packedPath);
    require(full.open(QIODevice::Append), "append bootable VTSI metadata");
    const QList<VtsiGuestExtent> completeExtents = {{0, base.size}};
    const QByteArray tail = vtsiGuestTail(base.size, base.size, completeExtents);
    require(full.write(tail) == tail.size() && full.flush(), "complete VTSI guest footer");
    full.close();
    request = compressedRequest(base, packedPath, base.size, expectedDisk, BLED_COMPRESSION_VTSI);
    request.imageLayoutSha256 = vtsiGuestLayoutHash(base.size, completeExtents);
    if (qtFixture)
    {
        publishTargetMount(rawObjects, request, {}, rawPartition);
        runCompressedQtCase(request);
        printf("RUFUS_VTSI_QT_HELPER_DONE SHA256=%s\n", expectedDisk.toHex().constData());
    }
    else
        runCompressedHelperCase({"bootable-restore", 0, base.size, base.size, nullptr, true}, request);
    require(diskDigest() == expectedDisk, "complete VTSI boot image equality");
    require(QFile::remove(packedPath), "bootable VTSI guest source removed");
    printf("RUFUS_VTSI_HELPER_DONE SHA256=%s\n", expectedDisk.toHex().constData());
}
