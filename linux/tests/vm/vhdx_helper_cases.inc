// SPDX-License-Identifier: GPL-3.0-or-later
// qemu-img is an independent test encoder, never a production dependency.
void makeGuestVhdx(const QString& rawPath, const QString& packedPath, bool fixed)
{
    QProcess encoder;
    encoder.setProcessChannelMode(QProcess::ForwardedChannels);
    encoder.start(QStringLiteral("/usr/bin/qemu-img"), {QStringLiteral("convert"),
        QStringLiteral("-f"), QStringLiteral("raw"), QStringLiteral("-O"), QStringLiteral("vhdx"),
        QStringLiteral("-o"), fixed ? QStringLiteral("subformat=fixed,block_size=1048576")
            : QStringLiteral("subformat=dynamic,block_size=1048576"), rawPath, packedPath});
    require(encoder.waitForStarted(processTimeoutMs) && encoder.waitForFinished(60000)
        && encoder.exitStatus() == QProcess::NormalExit && encoder.exitCode() == 0, "independent VHDX encoder");
    require(chown(QFile::encodeName(packedPath).constData(), 1000, 1000) == 0, "VHDX caller ownership");
}

void runVhdxQtCases(rufus::PrepareImageRequest base, const QString& rawPath, const QByteArray& expectedDisk)
{
    constexpr qsizetype logicalBytes = 3 * 1024 * 1024;
    const QString smallPath = QStringLiteral("/run/caller-home/small.raw");
    const QString packedPath = QStringLiteral("/run/caller-home/source.vhdx");
    QFile raw(rawPath);
    require(raw.open(QIODevice::ReadOnly), "VHDX original source");
    const QByteArray first = raw.read(65536);
    require(first.size() == 65536, "VHDX boot prefix");
    raw.close();
    QByteArray logical(logicalBytes, '\0');
    logical.replace(0, first.size(), first);
    logical.replace(logicalBytes - 65536, 65536, QByteArray(65536, char(0x3c)));
    writeGuestFile(smallPath, logical);
    makeGuestVhdx(smallPath, packedPath, false);
    require(QFile::remove(smallPath), "VHDX small raw source removed");
    auto request = compressedRequest(base, packedPath, logicalBytes,
        QCryptographicHash::hash(logical, QCryptographicHash::Sha256), IMG_COMPRESSION_VHDX);
    publishTargetMount(rawObjects, request, {}, rawPartition);
    runCompressedQtCase(request);
    QFile disk(QStringLiteral("/dev/sda"));
    require(disk.open(QIODevice::ReadOnly) && disk.read(logicalBytes) == logical, "VHDX dynamic Qt logical readback");
    require(disk.seek(qint64(base.size - 512)) && disk.read(512) == QByteArray(512, char(0xa5)),
        "VHDX dynamic Qt target tail preserved");
    disk.close();
    require(QFile::remove(packedPath), "dynamic VHDX source removed");
    makeGuestVhdx(rawPath, packedPath, true);
    require(QFile::remove(rawPath), "VHDX full raw source removed");
    request = compressedRequest(base, packedPath, base.size, expectedDisk, IMG_COMPRESSION_VHDX);
    publishTargetMount(rawObjects, request, {}, rawPartition);
    runCompressedQtCase(request);
    require(diskDigest() == expectedDisk, "VHDX full Qt restore readback");
    require(QFile::remove(packedPath), "fixed VHDX source removed");
    printf("RUFUS_VHDX_QT_HELPER_DONE SHA256=%s\n", expectedDisk.toHex().constData());
}
