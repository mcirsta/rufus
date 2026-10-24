// SPDX-License-Identifier: GPL-3.0-or-later

static QByteArray readOptimizedNtfsFile(const QString& volume, const QString& path)
{
    QProcess reader;
    reader.start(QStringLiteral("/usr/bin/setpriv"), {QStringLiteral("--reuid=1000"), QStringLiteral("--regid=1000"),
        QStringLiteral("--clear-groups"), QStringLiteral("/usr/bin/ntfs_file_fixture"), QStringLiteral("get"), volume, path});
    require(reader.waitForStarted() && reader.waitForFinished(30000), "independent NTFS readback child joined");
    const QByteArray result = reader.readAllStandardOutput();
    if (reader.exitCode() != 0) fprintf(stderr, "%s", reader.readAllStandardError().constData());
    require(reader.exitStatus() == QProcess::NormalExit && reader.exitCode() == 0, "independent NTFS file readback");
    return result;
}

static QByteArray readOptimizedPartition(const QJsonObject& record, bool recovery)
{
    const QString prefix = recovery ? QStringLiteral("recovery") : QStringLiteral("os");
    const quint64 offset = record.value(prefix + QStringLiteral("Offset")).toString().toULongLong();
    const quint64 size = record.value(prefix + QStringLiteral("Bytes")).toString().toULongLong();
    QFile disk(QStringLiteral("/dev/sda"));
    require(disk.open(QIODevice::ReadOnly) && disk.seek(qint64(offset)), "optimized partition independent extent");
    const QString volume = QStringLiteral("/run/caller-home/readback.ntfs");
    QFile copy(volume);
    require(copy.open(QIODevice::WriteOnly | QIODevice::Truncate), "NTFS readback ordinary file");
    quint64 position = 0;
    QByteArray boot;
    while (position < size)
    {
        const QByteArray bytes = disk.read(qint64(qMin(quint64(65536), size - position)));
        require(!bytes.isEmpty() && copy.write(bytes) == bytes.size(), "NTFS readback copy");
        if (position == 0) boot = bytes.first(512);
        position += quint64(bytes.size());
    }
    copy.close(); disk.close();
    require(chown(QFile::encodeName(volume).constData(), 1000, 1000) == 0, "NTFS readback owned by caller");
    const QByteArray payload = readOptimizedNtfsFile(volume, QStringLiteral("payload.bin"));
    require(QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex() == record.value("payloadSha256").toString().toLatin1(),
        "NTFS payload preserved through movement and growth");
    const QByteArray xml = readOptimizedNtfsFile(volume, recovery ? QStringLiteral("Recovery/WindowsRE/ReAgent.xml")
        : QStringLiteral("Windows/System32/Recovery/ReAgent.xml"));
    QXmlStreamReader reader(xml);
    unsigned locations = 0;
    while (!reader.atEnd())
    {
        reader.readNext();
        if (reader.isStartElement() && (reader.name() == QLatin1String("WinreLocation") || reader.name() == QLatin1String("ImageLocation")))
        {
            require(reader.attributes().value(QLatin1String("offset")) == record.value("recoveryOffset").toString(), "new recovery offset in ReAgent XML");
            locations++;
        }
    }
    require(!reader.hasError() && locations == 2, "both recovery locations independently read back");
    if (!recovery)
    {
        const auto* fields = reinterpret_cast<const uchar*>(boot.constData());
        require(qFromLittleEndian<quint64>(fields + 40) == (size - 512) / 4096 * 8, "grown NTFS reaches partition capacity");
        QFile volumeFile(volume); require(volumeFile.open(QIODevice::ReadOnly) && volumeFile.seek(qint64(size - 512)), "NTFS backup boot offset");
        require(volumeFile.read(512) == boot, "grown NTFS backup boot matches");
    }
    require(QFile::remove(volume), "NTFS readback file removed");
    return xml;
}

static void checkOptimizedGpt(const QJsonObject& record, quint64 capacity)
{
    QFile disk(QStringLiteral("/dev/sda"));
    require(disk.open(QIODevice::ReadOnly) && disk.seek(512), "optimized GPT primary");
    QByteArray primary = disk.read(512);
    require(disk.seek(qint64(capacity - 512)), "optimized GPT backup");
    QByteArray backup = disk.read(512);
    for (QByteArray* header : {&primary, &backup})
    {
        const quint32 expected = qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(header->constData()) + 16);
        qToLittleEndian<quint32>(0, reinterpret_cast<uchar*>(header->data()) + 16);
        require(crc32(0, reinterpret_cast<const Bytef*>(header->constData()), 92) == expected, "optimized GPT header CRC");
    }
    require(disk.seek(1024), "optimized GPT partition array");
    const QByteArray table = disk.read(16384);
    require(disk.seek(qint64(capacity - 512 - 16384)) && disk.read(16384) == table, "optimized GPT table copies match");
    const auto* fields = reinterpret_cast<const uchar*>(table.constData());
    require(crc32(0, reinterpret_cast<const Bytef*>(fields), 16384)
        == qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(primary.constData()) + 88), "optimized GPT table CRC");
    const quint64 osOffset = record.value("osOffset").toString().toULongLong();
    const quint64 osBytes = record.value("osBytes").toString().toULongLong();
    require(qFromLittleEndian<quint64>(fields + 3 * 128 + 32) * 512 == osOffset
        && (qFromLittleEndian<quint64>(fields + 3 * 128 + 40) + 1) * 512 == osOffset + osBytes, "selected GPT slot four resized");
    require(qFromLittleEndian<quint64>(fields + 7 * 128 + 32) * 512 == record.value("recoveryOffset").toString().toULongLong(),
        "recovery GPT slot eight relocated");
}

void runFfuOptimizedQtCases(rufus::PrepareImageRequest base, const QString& rawPath, const QByteArray& expectedDisk)
{
    // This initramfs has devtmpfs but no udev daemon. Supply the same links it
    // normally publishes. Production still validates the actual kernel nodes.
    struct stat info;
    require(stat("/dev/sda", &info) == 0 && QDir().mkpath(QStringLiteral("/dev/block")), "optimized guest partition links");
    for (unsigned number : {4u, 8u})
    {
        const QByteArray link = QByteArray("/dev/block/") + QByteArray::number(major(info.st_rdev)) + ':'
            + QByteArray::number(minor(info.st_rdev) + number);
        const QByteArray node = QByteArray("../sda") + QByteArray::number(number);
        require(symlink(node.constData(), link.constData()) == 0 || errno == EEXIST, "optimized guest udev partition link");
    }
    const QJsonArray cases = QJsonDocument::fromJson(readFile(QStringLiteral("/ffu-optimized/cases.json"))).array();
    require(cases.size() == 2, "two independent optimized FFU fixtures");
    for (const QJsonValue& value : cases)
    {
        const QJsonObject record = value.toObject();
        const QString name = record.value("name").toString();
        const QString path = QStringLiteral("/run/caller-home/") + name + QStringLiteral(".ffu");
        require(QFile::copy(QStringLiteral("/ffu-optimized/") + name + QStringLiteral(".ffu"), path)
            && chown(QFile::encodeName(path).constData(), 1000, 1000) == 0, "optimized caller source");
        auto request = compressedRequest(base, path, record.value("captured").toString().toULongLong(),
            QByteArray::fromHex(record.value("sha256").toString().toLatin1()), IMG_COMPRESSION_FFU);
        request.ffuMinimumSize = record.value("minimum").toString().toULongLong();
        publishTargetMount(rawObjects, request, {}, rawPartition);
        runCompressedQtCase(request);
        checkOptimizedGpt(record, base.size);
        const QByteArray os = readOptimizedPartition(record, false);
        require(readOptimizedPartition(record, true) == os, "recovery copy equals updated OS XML");
        require(QCryptographicHash::hash(readFile(path), QCryptographicHash::Sha256).toHex()
            == record.value("sourceSha256").toString().toLatin1(), "optimized source unchanged");
        require(QFile::remove(path), "optimized source removed after readback");
        printf("RUFUS_FFU_OPTIMIZED_CASE NAME=%s NTFS_GROWN=1 RECOVERY_MOVED=1 XML_READBACK=1 GPT_VALID=1 SOURCE_UNCHANGED=1\n", qPrintable(name));
    }
    // One representative full restore and BIOS boot for the shared sector writer.
    const QString path = QStringLiteral("/run/caller-home/boot.ffu");
    makeGuestFfu(rawPath, path, false);
    require(chown(QFile::encodeName(path).constData(), 1000, 1000) == 0, "representative FFU source owner");
    const auto request = compressedRequest(base, path, base.size, expectedDisk, IMG_COMPRESSION_FFU);
    publishTargetMount(rawObjects, request, {}, rawPartition);
    runCompressedQtCase(request);
    require(diskDigest() == expectedDisk && QFile::remove(path), "representative full restore readback and cleanup");
    printf("RUFUS_FFU_OPTIMIZED_DONE SHA256=%s\n", expectedDisk.toHex().constData());
}
