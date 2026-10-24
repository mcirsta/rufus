// SPDX-License-Identifier: GPL-3.0-or-later
// Independent VHD containers around guest data; real helper restores /dev/sda.
void putVhdBe(QByteArray& bytes, qsizetype offset, quint64 value, unsigned width)
{
    for (unsigned index = 0; index < width; index++)
        bytes[offset + index] = char(value >> (8 * (width - 1 - index)));
}

quint32 vhdGuestChecksum(const QByteArray& bytes, qsizetype checksumOffset)
{
    quint32 sum = 0;
    for (qsizetype index = 0; index < bytes.size(); index++)
        if (index < checksumOffset || index >= checksumOffset + 4)
            sum += quint8(bytes[index]);
    return ~sum;
}

QByteArray vhdGuestFooter(quint64 diskBytes, bool dynamic)
{
    QByteArray footer(512, '\0');
    footer.replace(0, 8, "conectix");
    putVhdBe(footer, 8, 2, 4);
    putVhdBe(footer, 12, 0x10000, 4);
    putVhdBe(footer, 16, dynamic ? 512 : UINT64_MAX, 8);
    putVhdBe(footer, 40, diskBytes, 8);
    putVhdBe(footer, 48, diskBytes, 8);
    putVhdBe(footer, 60, dynamic ? 3 : 2, 4);
    putVhdBe(footer, 64, vhdGuestChecksum(footer, 64), 4);
    return footer;
}

QByteArray vhdGuestDynamic(const QByteArray& first, const QByteArray& last)
{
    constexpr quint64 blockBytes = 64 * 1024;
    constexpr quint64 diskBytes = 4 * blockBytes;
    require(first.size() == qint64(blockBytes) && last.size() == qint64(blockBytes), "VHD guest block sizes");
    const QByteArray footer = vhdGuestFooter(diskBytes, true);
    QByteArray header(1024, '\0');
    header.replace(0, 8, "cxsparse");
    putVhdBe(header, 8, UINT64_MAX, 8);
    putVhdBe(header, 16, 1536, 8);
    putVhdBe(header, 24, 0x10000, 4);
    putVhdBe(header, 28, 4, 4);
    putVhdBe(header, 32, blockBytes, 4);
    putVhdBe(header, 36, vhdGuestChecksum(header, 36), 4);
    QByteArray bat(512, char(0xff));
    putVhdBe(bat, 0, 4, 4);
    putVhdBe(bat, 12, 133, 4);
    QByteArray bitmap(512, '\0');
    bitmap.replace(0, 16, QByteArray(16, char(0xff)));
    return footer + header + bat + bitmap + first + bitmap + last + footer;
}

void runVhdHelperCases(rufus::PrepareImageRequest base, const QString& rawPath,
    const QByteArray& expectedDisk)
{
    constexpr qsizetype blockBytes = 64 * 1024;
    constexpr qsizetype logicalBytes = 4 * blockBytes;
    const QString packedPath = QStringLiteral("/run/caller-home/source.vhd");
    QFile raw(rawPath);
    require(raw.open(QIODevice::ReadOnly), "VHD guest original source");
    const QByteArray first = raw.read(blockBytes);
    require(first.size() == blockBytes, "VHD guest boot block");
    raw.close();
    const QByteArray last(blockBytes, char(0x3c));
    const QByteArray packed = vhdGuestDynamic(first, last);
    writeGuestFile(packedPath, packed);
    require(chown(QFile::encodeName(packedPath).constData(), 1000, 1000) == 0, "VHD guest caller ownership");
    QByteArray logical(logicalBytes, '\0');
    logical.replace(0, blockBytes, first);
    logical.replace(3 * blockBytes, blockBytes, last);
    auto request = compressedRequest(base, packedPath, logicalBytes,
        QCryptographicHash::hash(logical, QCryptographicHash::Sha256), IMG_COMPRESSION_VHD);
    auto mismatch = request;
    mismatch.decodedImageSha256[0] ^= 1;
    runCompressedHelperCase({"sha", ESTALE, 0, 0}, mismatch);
    const bool qtFixture = QFile::exists(QStringLiteral("/etc/rufus-compressed-qt-fixture"));
    if (qtFixture)
    {
        const int target = open("/dev/sda", O_RDWR | O_EXCL | O_CLOEXEC);
        const QByteArray poison(logicalBytes, char(0xa5));
        require(target >= 0 && pwrite(target, poison.constData(), size_t(poison.size()), 0) == poison.size()
            && pwrite(target, poison.constData(), 512, off_t(base.size - 512)) == 512
            && fsync(target) == 0 && close(target) == 0, "poison VHD Qt logical range and tail");
        publishTargetMount(rawObjects, request, {}, rawPartition);
        runCompressedQtCase(request);
        QFile disk(QStringLiteral("/dev/sda"));
        require(disk.open(QIODevice::ReadOnly) && disk.read(logicalBytes) == logical,
            "ordinary dynamic VHD Qt logical disk readback");
        require(disk.seek(qint64(base.size - 512))
            && disk.read(512) == QByteArray(512, char(0xa5)), "ordinary VHD Qt preserves target tail");
        disk.close();
    }
    else
        runCompressedHelperCase({"observed-dynamic", 0, logicalBytes, logicalBytes, "observe", true}, request, logical);
    require(QFile::remove(packedPath), "small VHD guest source removed");

    // Reuse the 64-MiB raw image and append only the fixed VHD footer.
    require(QFile::rename(rawPath, packedPath), "rename bootable raw image as fixed VHD data");
    QFile full(packedPath);
    require(full.open(QIODevice::Append), "append bootable VHD footer");
    const QByteArray footer = vhdGuestFooter(base.size, false);
    require(full.write(footer) == footer.size() && full.flush(), "complete VHD guest footer");
    full.close();
    request = compressedRequest(base, packedPath, base.size, expectedDisk, IMG_COMPRESSION_VHD);
    if (qtFixture)
    {
        publishTargetMount(rawObjects, request, {}, rawPartition);
        runCompressedQtCase(request);
        printf("RUFUS_VHD_QT_HELPER_DONE SHA256=%s\n", expectedDisk.toHex().constData());
    }
    else
        runCompressedHelperCase({"bootable-restore", 0, base.size, base.size, nullptr, true}, request);
    require(diskDigest() == expectedDisk, "complete VHD boot image equality");
    require(QFile::remove(packedPath), "bootable VHD guest source removed");
    printf("RUFUS_VHD_HELPER_DONE SHA256=%s\n", expectedDisk.toHex().constData());
}
