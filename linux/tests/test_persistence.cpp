// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/format_options.h"
#include "qt/prepare_image_protocol.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <cerrno>
#include <unistd.h>

namespace
{
constexpr quint64 MiB = 1024 * 1024;
constexpr quint64 diskBytes = 1024 * MiB;
constexpr quint64 persistenceBytes = 256 * MiB;
constexpr quint64 payloadBytes = 2 * MiB;

rufus::PrepareImageRequest requestFixture()
{
    rufus::PrepareImageRequest request;
    request.path = QStringLiteral("/dev/fixture");
    request.device = 123;
    request.isDevice = true;
    request.size = diskBytes;
    request.clusterSize = 4096;
    request.fileSystem = FS_NTFS;
    request.label = QStringLiteral("LIVE");
    request.diskSequence = 7;
    request.objectPath = QStringLiteral("/org/freedesktop/UDisks2/block_devices/fixture");
    request.serviceOwner = QStringLiteral(":1.2");
    request.identity = {QStringLiteral("fixture")};
    request.imagePath = QStringLiteral("/unused/live.iso");
    request.imageDevice = 1;
    request.imageInode = 2;
    request.imageSize = 16 * MiB;
    request.imageModified = request.imageChanged = QStringLiteral("123:456");
    request.persistenceSize = persistenceBytes;
    return request;
}
}

class TestPersistence : public QObject
{
    Q_OBJECT
private slots:
    void invalidGeometry()
    {
        GPT_IMAGE_OPTIONS gpt = {diskBytes, 512, 63, 4096, 0, 0, persistenceBytes};
        MBR_IMAGE_OPTIONS mbr = {diskBytes, 512, 63, 4096, 0, 0,
            FS_FAT32, MBR_BOOT_SYSLINUX, 0, persistenceBytes};
        for (quint64 size : {persistenceBytes - 1, diskBytes, quint64(UINT64_MAX)})
        {
            gpt.persistence_size = mbr.persistence_size = size;
            GPT_IMAGE_RESULT plannedGpt = {};
            MBR_IMAGE_RESULT plannedMbr = {};
            const int expected = size < persistenceBytes ? EINVAL : ENOSPC;
            QCOMPARE(PlanGptImageLayout(&gpt, &plannedGpt), expected);
            QCOMPARE(PlanMbrImageLayout(&mbr, &plannedMbr), expected);
        }
    }

    void partitionTables_data()
    {
        QTest::addColumn<bool>("gpt");
        QTest::addColumn<bool>("payload");
        QTest::newRow("mbr") << false << false;
        QTest::newRow("mbr-three") << false << true;
        QTest::newRow("gpt") << true << false;
        QTest::newRow("gpt-three") << true << true;
    }

    void partitionTables()
    {
        QFETCH(bool, gpt);
        QFETCH(bool, payload);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QFile disk(directory.filePath(QStringLiteral("target.img")));
        QVERIFY(disk.open(QIODevice::ReadWrite));
        QVERIFY(disk.resize(diskBytes));
        QCOMPARE(lseek(disk.handle(), 17, SEEK_SET), off_t(17));
        const GPT_IMAGE_OPTIONS gptOptions = {diskBytes, 512, 63, 4096, 0,
            payload ? payloadBytes : 0, persistenceBytes};
        const MBR_IMAGE_OPTIONS mbrOptions = {diskBytes, 512, 63, 4096, 0x12345678, 0,
            FS_NTFS, MBR_BOOT_SYSLINUX, payload ? payloadBytes : 0, persistenceBytes};
        GPT_IMAGE_RESULT layout = {};
        MBR_IMAGE_RESULT mbr = {};
        if (gpt)
        {
            QCOMPARE(CreateGptImagePartition(disk.handle(), &gptOptions, &layout), 0);
            QCOMPARE(CheckGptImagePartition(disk.handle(), &gptOptions, &layout), 0);
        }
        else
        {
            QCOMPARE(PrepareMbrTarget(disk.handle(), &mbrOptions, &mbr), 0);
            QCOMPARE(CheckMbrImage(disk.handle(), &mbrOptions, &mbr), 0);
            layout.offset = mbr.offset;
            layout.size = mbr.size;
            layout.persistence_offset = mbr.persistence_offset;
            layout.persistence_size = mbr.persistence_size;
            layout.uefi_ntfs_offset = mbr.uefi_ntfs_offset;
        }
        QCOMPARE(lseek(disk.handle(), 0, SEEK_CUR), off_t(17));
        QVERIFY(layout.persistence_size >= persistenceBytes);
        QVERIFY(layout.persistence_size < persistenceBytes + 63 * 512);
        QCOMPARE(layout.persistence_size % (63 * 512), quint64(0));
        QVERIFY(layout.offset + layout.size <= layout.persistence_offset);
        if (payload)
            QVERIFY(layout.persistence_offset + layout.persistence_size <= layout.uefi_ntfs_offset);
        QProcess inspector;
        const QString sfdisk = QStandardPaths::findExecutable(QStringLiteral("sfdisk"));
        QVERIFY2(!sfdisk.isEmpty(), "sfdisk from util-linux/fdisk is required");
        inspector.start(sfdisk, {QStringLiteral("--json"), disk.fileName()});
        QVERIFY(inspector.waitForFinished());
        QCOMPARE(inspector.exitCode(), 0);
        const QJsonObject table = QJsonDocument::fromJson(inspector.readAllStandardOutput())
            .object().value(QStringLiteral("partitiontable")).toObject();
        const QJsonArray partitions = table.value(QStringLiteral("partitions")).toArray();
        QCOMPARE(partitions.size(), payload ? 3 : 2);
        const QJsonObject persistent = partitions[1].toObject();
        QCOMPARE(persistent.value(QStringLiteral("start")).toDouble() * 512, double(layout.persistence_offset));
        QCOMPARE(persistent.value(QStringLiteral("size")).toDouble() * 512, double(layout.persistence_size));
        QCOMPARE(persistent.value(QStringLiteral("type")).toString().toUpper(), gpt
            ? QStringLiteral("0FC63DAF-8483-4772-8E79-3D69D8477DE4") : QStringLiteral("83"));
        if (gpt)
        {
            QCOMPARE(persistent.value(QStringLiteral("name")).toString(), QStringLiteral("Linux Persistence"));
            QVERIFY(persistent.value(QStringLiteral("attrs")).toString().isEmpty());
        }
        else
        {
            QVERIFY(!persistent.value(QStringLiteral("bootable")).toBool());
            QVERIFY(partitions[0].toObject().value(QStringLiteral("bootable")).toBool());
            // A change in the extra partition's active flag must fail readback.
            const unsigned char active = 0x80;
            QCOMPARE(pwrite(disk.handle(), &active, 1, 462), ssize_t(1));
            QCOMPARE(CheckMbrImage(disk.handle(), &mbrOptions, &mbr), EBADMSG);
        }
        if (payload)
        {
            const QJsonObject boot = partitions[2].toObject();
            QCOMPARE(boot.value(QStringLiteral("start")).toDouble() * 512, double(layout.uefi_ntfs_offset));
            QCOMPARE(boot.value(QStringLiteral("type")).toString().toUpper(), gpt
                ? QStringLiteral("EBD0A0A2-B9E5-4433-87C0-68B6B72699C7") : QStringLiteral("EF"));
            if (gpt)
                QVERIFY(!boot.value(QStringLiteral("attrs")).toString().isEmpty());
        }
    }

    void protocol()
    {
        for (int ext : {2, 3, 4})
        {
            auto request = requestFixture();
            request.persistenceExtVersion = ext;
            const QJsonObject message = rufus::EncodePrepareRequest(request);
            QVERIFY(!message.isEmpty());
            rufus::PrepareImageRequest decoded;
            QVERIFY(rufus::DecodePrepareRequest(message, decoded));
            QCOMPARE(decoded.persistenceSize, persistenceBytes);
            QCOMPARE(decoded.persistenceExtVersion, ext);
        }
        const QJsonObject message = rufus::EncodePrepareRequest(requestFixture());
        for (const QJsonObject& option : {
            QJsonObject{{"size", QStringLiteral("0")}, {"ext", 3}},
            QJsonObject{{"size", QString::number(persistenceBytes)}, {"ext", 3.5}},
            QJsonObject{{"size", QString::number(persistenceBytes)}, {"ext", 5}},
            QJsonObject{{"size", QString::number(persistenceBytes)}, {"ext", 3}, {"casper", true}},
            QJsonObject{{"size", double(persistenceBytes)}, {"ext", 3}}})
        {
            QJsonObject invalid = message;
            invalid.insert(QStringLiteral("persistence"), option);
            rufus::PrepareImageRequest decoded;
            QVERIFY(!rufus::DecodePrepareRequest(invalid, decoded));
        }
        for (quint64 size : {quint64(1), persistenceBytes - MiB, persistenceBytes + 1, diskBytes})
        {
            auto request = requestFixture();
            request.persistenceSize = size;
            QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        }
        auto request = requestFixture();
        request.rawWrite = true;
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        request = requestFixture(); request.sfd = true;
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        request = requestFixture(); request.nonBootable = true;
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        request = requestFixture(); request.windowsFlags = 1;
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        request = requestFixture(); request.isDevice = false;
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
    }
};
QTEST_GUILESS_MAIN(TestPersistence)
#include "test_persistence.moc"
