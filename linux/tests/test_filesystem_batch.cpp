// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/udf_format.h"
#include "platform/ext_format.h"
#include "platform/format_options.h"
#include "qt/prepare_image_protocol.h"
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>
#include <cerrno>
#include <unistd.h>

namespace
{
constexpr quint64 MiB = 1024 * 1024;
bool cancelled = false;

void retagUdfDescriptor(QByteArray& descriptor)
{
    const quint16 length = qFromLittleEndian<quint16>(descriptor.constData() + 10);
    quint16 crc = 0;
    for (quint16 index = 0; index < length; ++index) {
        crc ^= quint16(quint8(descriptor[16 + index])) << 8;
        for (unsigned bit = 0; bit < 8; ++bit) crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
    }
    qToLittleEndian<quint16>(crc, descriptor.data() + 8);
    quint8 checksum = 0;
    for (unsigned index = 0; index < 16; ++index)
        if (index != 4) checksum += quint8(descriptor[index]);
    descriptor[4] = char(checksum);
}
}
extern "C" int __wrap_IsRufusJobCancelled() { return cancelled; }

class TestFilesystemBatch : public QObject
{
    Q_OBJECT
    QTemporaryDir directory_;
private slots:
    void nativeLayoutsAndProtocol_data()
    {
        QTest::addColumn<int>("filesystem"); QTest::addColumn<int>("layout");
        for (int filesystem : {FS_UDF, FS_EXT2, FS_EXT3})
        for (int layout = 0; layout < 3; ++layout)
            QTest::newRow(qPrintable(QStringLiteral("fs%1-layout%2").arg(filesystem).arg(layout))) << filesystem << layout;
    }
    void nativeLayoutsAndProtocol()
    {
        QFETCH(int, filesystem); QFETCH(int, layout);
        const auto scheme = layout == 0 ? rufus::PartitionScheme::Mbr : layout == 1 ? rufus::PartitionScheme::Gpt : rufus::PartitionScheme::Sfd;
        const auto clusters = rufus::NativeFileSystemClusterSizes(512 * MiB, filesystem, false, BT_NON_BOOTABLE, scheme);
        QCOMPARE(clusters.Allowed, quint32(256)); QCOMPARE(clusters.Default, quint32(256));
        rufus::PrepareImageRequest request;
        request.path = QStringLiteral("/test/native-disk"); request.size = 512 * MiB;
        request.device = 2064; request.isDevice = true; request.diskSequence = 10;
        request.objectPath = QStringLiteral("/org/freedesktop/UDisks2/block_devices/test");
        request.serviceOwner = QStringLiteral(":1.23"); request.identity = QStringList{QStringLiteral("test")};
        request.nonBootable = true; request.fileSystem = filesystem; request.clusterSize = 256;
        request.mbr = layout == 0; request.sfd = layout == 2;
        request.label = QStringLiteral("Native data");
        GPT_IMAGE_RESULT partition = {};
        QCOMPARE(rufus::PlanFormatPartition(request, partition), 0);
        QCOMPARE(partition.offset, layout == 2 ? quint64(0) : MiB);
        QCOMPARE(rufus::CheckFormatSettings(request, partition), 0);
        if (filesystem != FS_UDF)
            QCOMPARE(rufus::NativeFileSystemClusterSizes(64 * MiB, filesystem, false, BT_NON_BOOTABLE, scheme).Allowed, quint32(0));
        const QJsonObject encoded = rufus::EncodePrepareRequest(request);
        QVERIFY(!encoded.isEmpty());
        QCOMPARE(encoded.value("operation").toString(), QStringLiteral("format-native-device-v1"));
        rufus::PrepareImageRequest decoded;
        QVERIFY(rufus::DecodePrepareRequest(encoded, decoded));
        QCOMPARE(decoded.fileSystem, filesystem); QCOMPARE(decoded.mbr, request.mbr); QCOMPARE(decoded.sfd, request.sfd);
        QCOMPARE(rufus::EncodePrepareRequest(decoded), encoded);
        QJsonObject invalid = encoded;
        invalid.insert("cluster", QStringLiteral("4096"));
        QVERIFY(!rufus::DecodePrepareRequest(invalid, decoded));
        invalid = encoded; QJsonObject format = encoded.value("nativeFormat").toObject();
        format.insert("layout", QStringLiteral("auto")); invalid.insert("nativeFormat", format);
        QVERIFY(!rufus::DecodePrepareRequest(invalid, decoded));
        request.volumeId = 1; QVERIFY(rufus::EncodePrepareRequest(request).isEmpty()); request.volumeId = 0;
        request.imagePath = QStringLiteral("/unwanted/source.iso");
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
    }
    void udfNativeFormatting_data()
    {
        QTest::addColumn<int>("revision"); QTest::addColumn<QString>("label"); QTest::addColumn<bool>("quick");
        QTest::addColumn<quint32>("sector"); QTest::addColumn<quint32>("physical");
        QTest::addColumn<quint64>("capacity");
        QTest::newRow("1.02") << 0x0102 << QStringLiteral("Data") << true << quint32(512) << quint32(512) << 8 * MiB;
        QTest::newRow("1.50") << 0x0150 << QString() << true << quint32(512) << quint32(512) << 8 * MiB;
        QTest::newRow("2.00") << 0x0200 << QString::fromUtf8("Rufus Ω") << true << quint32(512) << quint32(512) << 8 * MiB;
        QTest::newRow("2.01-utf16") << 0x0201 << QString(32, QChar(0x4e2d)) << true << quint32(512) << quint32(512) << 8 * MiB;
        QTest::newRow("2.01-full") << 0x0201 << QStringLiteral("Full format") << false << quint32(512) << quint32(512) << 8 * MiB;
        QTest::newRow("512e") << 0x0201 << QStringLiteral("Physical 4K") << true << quint32(512) << quint32(4096) << 8 * MiB;
        QTest::newRow("1024") << 0x0201 << QStringLiteral("Logical 1K") << true << quint32(1024) << quint32(4096) << 8 * MiB;
        QTest::newRow("2048") << 0x0201 << QStringLiteral("Logical 2K") << true << quint32(2048) << quint32(4096) << 8 * MiB;
        QTest::newRow("4Kn-minimum") << 0x0201 << QStringLiteral("Minimum 4K") << true << quint32(4096) << quint32(4096) << quint64(258 * 4096);
        QTest::newRow("4Kn-two-anchors") << 0x0201 << QStringLiteral("Logical 4K") << true << quint32(4096) << quint32(4096) << quint64(3071 * 4096);
        QTest::newRow("4Kn-three-anchors") << 0x0201 << QStringLiteral("Logical 4K") << true << quint32(4096) << quint32(4096) << quint64(3072 * 4096);
        QTest::newRow("4Kn-full") << 0x0201 << QStringLiteral("Full 4K") << false << quint32(4096) << quint32(4096) << 8 * MiB;
        // A real native-provider image at its maximum. Its bitmap is bounded
        // to 512 MiB; the almost-16-TiB data extent remains a sparse hole.
        QTest::newRow("4Kn-provider-maximum") << 0x0201 << QStringLiteral("Maximum 4K") << true
            << quint32(4096) << quint32(4096) << quint64(UINT32_MAX) * 4096;
    }
    void udfNativeFormatting()
    {
        QFETCH(int, revision); QFETCH(QString, label); QFETCH(bool, quick);
        QFETCH(quint32, sector); QFETCH(quint32, physical); QFETCH(quint64, capacity);
        QVERIFY(directory_.isValid());
        QFile image(directory_.filePath(QStringLiteral("volume.img")));
        QVERIFY(image.open(QIODevice::ReadWrite | QIODevice::Truncate));
        QVERIFY(image.resize(capacity));
        QVERIFY(image.seek(capacity / 2)); QCOMPARE(image.write(QByteArray(4096, char(0xa5))), qint64(4096));
        QVERIFY(image.flush()); QVERIFY(image.seek(19));
        rufus::UdfFormatOptions options = {capacity, label, quint16(revision), quick};
        options.sectorSize = sector; options.physicalSectorSize = physical;
        FORMAT_RESULT result;
        const int error = rufus::FormatUdfImage(image.handle(), QStringLiteral(RUFUS_MKUDFFS_PATH), options, result);
        QVERIFY2(error == 0, qPrintable(QStringLiteral("errno %1: %2").arg(error).arg(QString::fromUtf8(result.diagnostics))));
        QVERIFY(result.started); QCOMPARE(result.exit_code, 0); QCOMPARE(result.sync_error, 0);
        QCOMPARE(result.filesystem_bytes, capacity); QCOMPARE(lseek(image.handle(), 0, SEEK_CUR), off_t(19));
        QCOMPARE(rufus::CheckUdfVolume(image.handle(), options), 0);
        QVERIFY(image.seek(capacity / 2));
        QCOMPARE(image.read(4096), QByteArray(4096, quick ? char(0xa5) : char(0)));
        rufus::UdfFormatOptions wrongLabel = options; wrongLabel.label += QLatin1Char('x');
        if (wrongLabel.label.size() <= 32) QCOMPARE(rufus::CheckUdfVolume(image.handle(), wrongLabel), EBADMSG);
        // Flip one byte in an independently located required backup anchor;
        // successful formatter exit alone must never qualify the result.
        const qint64 anchorByte = qint64(capacity) - sector + 20;
        QVERIFY(image.seek(anchorByte)); const QByteArray original = image.read(1); QCOMPARE(original.size(), 1);
        QVERIFY(image.seek(anchorByte)); QCOMPARE(image.write(QByteArray(1, original[0] ^ 1)), qint64(1)); QVERIFY(image.flush());
        QCOMPARE(rufus::CheckUdfVolume(image.handle(), options), EBADMSG);
        // Restore that anchor, then inject valid-CRC out-of-bounds sequence
        // lengths in all provider anchors. This must refuse before a read
        // sized by disk metadata, independently of the descriptor CRC gate.
        QVERIFY(image.seek(anchorByte)); QCOMPARE(image.write(original), qint64(1));
        const quint64 blocks = capacity / sector;
        for (quint64 location : {quint64(256), blocks - 1, blocks - 257}) {
            if (location == blocks - 257 && blocks < 3072) continue;
            QVERIFY(image.seek(location * sector)); QByteArray anchor = image.read(sector);
            QCOMPARE(anchor.size(), int(sector));
            qToLittleEndian<quint32>(65536 + sector, anchor.data() + 16);
            retagUdfDescriptor(anchor);
            QVERIFY(image.seek(location * sector)); QCOMPARE(image.write(anchor), qint64(sector));
        }
        QVERIFY(image.flush());
        QCOMPARE(rufus::CheckUdfVolume(image.handle(), options), EBADMSG);
    }
    void udfGeometryAndProviderBoundaries_data()
    {
        QTest::addColumn<quint32>("sector");
        for (quint32 sector : {512, 1024, 2048, 4096})
            QTest::newRow(qPrintable(QString::number(sector))) << sector;
    }
    void udfGeometryAndProviderBoundaries()
    {
        QFETCH(quint32, sector);
        rufus::UdfFormatOptions options = {8 * MiB, QStringLiteral("Boundary")};
        options.sectorSize = sector; options.physicalSectorSize = 4096;
        QCOMPARE(rufus::CheckUdfFormatOptions(options), 0);
        options.blockSize = sector;
        QCOMPARE(rufus::CheckUdfFormatOptions(options), 0);
        options.blockSize = sector == 512 ? 4096 : 512;
        QCOMPARE(rufus::CheckUdfFormatOptions(options), ENOTSUP);
        options.blockSize = 0;
        options.size = quint64(UINT32_MAX) * sector;
        QCOMPARE(rufus::CheckUdfFormatOptions(options), 0);
        QFile image(directory_.filePath(QStringLiteral("boundary.img")));
        QVERIFY(image.open(QIODevice::ReadWrite | QIODevice::Truncate)); QVERIFY(image.resize(options.size));
        // Exercise the actual provider's limit with a sparse no-write file;
        // the following extra block would otherwise be silently discarded.
        QProcess provider;
        provider.start(QStringLiteral(RUFUS_MKUDFFS_PATH), {QStringLiteral("--utf8"),
            QStringLiteral("--blocksize=%1").arg(sector), QStringLiteral("--media-type=hd"),
            QStringLiteral("--udfrev=0x0201"), QStringLiteral("--no-write"), image.fileName()});
        QVERIFY(provider.waitForFinished(15000));
        const QByteArray diagnostic = provider.readAllStandardError();
        QVERIFY2(provider.exitStatus() == QProcess::NormalExit && provider.exitCode() == 0, diagnostic.constData());
        const QByteArray output = provider.readAllStandardOutput();
        QVERIFY(output.contains("blocks=4294967295\n"));
        QVERIFY(!diagnostic.contains("Disk is too big"));
        options.size += sector;
        QCOMPARE(rufus::CheckUdfFormatOptions(options), EINVAL);
        FORMAT_RESULT result;
        QCOMPARE(rufus::FormatUdfImage(image.handle(), QStringLiteral(RUFUS_MKUDFFS_PATH), options, result), EINVAL);
        QVERIFY(!result.started);
        options.size = qMax(MiB, quint64(258) * sector);
        QCOMPARE(rufus::CheckUdfFormatOptions(options), 0);
        if (sector == 4096) {
            options.size -= sector;
            QCOMPARE(rufus::CheckUdfFormatOptions(options), EINVAL);
        }
        options.size = 8 * MiB;
        options.physicalSectorSize = sector / 2;
        QCOMPARE(rufus::CheckUdfFormatOptions(options), EINVAL);
    }
    void udfPrewriteRefusals()
    {
        QFile image(directory_.filePath(QStringLiteral("refusal.img")));
        QVERIFY(image.open(QIODevice::ReadWrite | QIODevice::Truncate)); QVERIFY(image.resize(8 * MiB));
        QCOMPARE(image.write("untouched source"), qint64(16)); QVERIFY(image.flush());
        FORMAT_RESULT result;
        rufus::UdfFormatOptions options = {8 * MiB, QStringLiteral("Data"), 0x0250, true};
        QCOMPARE(rufus::FormatUdfImage(image.handle(), QStringLiteral(RUFUS_MKUDFFS_PATH), options, result), ENOTSUP);
        QVERIFY(!result.started);
        options.revision = 0x0201;
        QCOMPARE(rufus::FormatUdfImage(image.handle(), QStringLiteral("/no/udf/provider"), options, result), ENOENT);
        QVERIFY(!result.started);
        cancelled = true;
        QCOMPARE(rufus::FormatUdfImage(image.handle(), QStringLiteral(RUFUS_MKUDFFS_PATH), options, result), ECANCELED);
        cancelled = false; QVERIFY(!result.started);
        QVERIFY(image.seek(0)); QCOMPARE(image.read(16), QByteArray("untouched source"));
        QCOMPARE(rufus::CheckUdfFormatOptions({8 * MiB + 1, QStringLiteral("Data")}), EINVAL);
        QCOMPARE(rufus::CheckUdfFormatOptions({8 * MiB, QString(33, QLatin1Char('x'))}), EINVAL);
        QCOMPARE(rufus::CheckUdfFormatOptions({quint64(UINT32_MAX) * 512 + 512, QStringLiteral("Data")}), EINVAL);
    }
};
QTEST_GUILESS_MAIN(TestFilesystemBatch)
#include "test_filesystem_batch.moc"
