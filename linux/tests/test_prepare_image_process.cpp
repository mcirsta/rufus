// SPDX-License-Identifier: GPL-3.0-or-later
#include "qt/prepare_image_process.h"
#include "unattend.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <cerrno>
#include <memory>
#include <sys/stat.h>

class TestPrepareImageProcess : public QObject
{
    Q_OBJECT
    QTemporaryDir directory_;
    rufus::PrepareImageRequest request_;

    rufus::PrepareImageRequest silentRequest() const
    {
        rufus::PrepareImageRequest request;
        request.isDevice = true;
        request.path = QStringLiteral("/dev/test");
        request.device = 123;
        request.size = 64 * 1024 * 1024;
        request.diskSequence = 7;
        request.objectPath = QStringLiteral("/org/freedesktop/UDisks2/block_devices/test");
        request.serviceOwner = QStringLiteral(":1.2");
        request.identity = {QStringLiteral("fixture")};
        request.imagePath = QStringLiteral("/tmp/source.iso");
        request.imageDevice = 1;
        request.imageInode = 2;
        request.imageSize = 4096;
        request.imageModified = request.imageChanged = QStringLiteral("1:0");
        request.windowsFlags = UNATTEND_SILENT_INSTALL | UNATTEND_SET_USER |
            UNATTEND_NO_DATA_COLLECTION | UNATTEND_DUPLICATE_LOCALE;
        request.windowsUsername = QStringLiteral("Măriuș");
        request.windowsInstallPath = QStringLiteral("/sources/install.wim");
        request.windowsEdition = 4;
        request.windowsSilentConfirmed = true;
        request.windowsRegion = {QStringLiteral("0409:00020409;ro-RO"), QStringLiteral("ro-RO"),
            QStringLiteral("en-GB"), QStringLiteral("zh-Hans-CN"), {}, QStringLiteral("GTB Standard Time")};
        return request;
    }

private slots:
    void supplementaryArchiveProtocol()
    {
        rufus::PrepareImageRequest request;
        request.isDevice = true; request.nonBootable = true;
        request.path = QStringLiteral("/dev/test"); request.device = 123; request.size = 64 * 1024 * 1024;
        request.diskSequence = 7; request.clusterSize = 4096; request.label = QStringLiteral("EXTRA");
        request.objectPath = QStringLiteral("/org/freedesktop/UDisks2/block_devices/test");
        request.serviceOwner = QStringLiteral(":1.2"); request.identity = {QStringLiteral("fixture")};
        request.archive = {QStringLiteral("/tmp/drivers.zip"), 1, 2, 4096, QStringLiteral("1:0"), QStringLiteral("1:0")};
        rufus::PrepareImageRequest decoded;
        for (int filesystem : {FS_FAT16, FS_FAT32, FS_NTFS, FS_EXFAT}) {
            request.fileSystem = filesystem;
            const QJsonObject encoded = rufus::EncodePrepareRequest(request);
            QVERIFY(!encoded.isEmpty()); QVERIFY(rufus::DecodePrepareRequest(encoded, decoded));
            QCOMPARE(decoded.archive.path, request.archive.path);
            QCOMPARE(decoded.archive.size, request.archive.size);
            auto invalid = encoded;
            auto archive = invalid.value("archive").toObject();
            archive.insert("extra", true); invalid.insert("archive", archive);
            QVERIFY(!rufus::DecodePrepareRequest(invalid, decoded));
        }
        request.fileSystem = FS_FAT32;
        const QJsonObject good = rufus::EncodePrepareRequest(request);
        for (const QJsonValue& invalid : {QJsonValue(4096), QJsonValue("-1"), QJsonValue("0"),
                QJsonValue("01"), QJsonValue("9223372036854775808")}) {
            auto changed = good; auto archive = changed.value("archive").toObject();
            archive.insert("size", invalid); changed.insert("archive", archive);
            QVERIFY(!rufus::DecodePrepareRequest(changed, decoded));
        }
        request.archive.path = QStringLiteral("relative.zip"); QVERIFY(!rufus::ValidatePrepareArchive(request));
        request.archive.path = QStringLiteral("/tmp/extra.zip");
        request.rawWrite = true; QVERIFY(!rufus::ValidatePrepareArchive(request)); request.rawWrite = false;
        request.fileSystem = FS_EXT2; QVERIFY(!rufus::ValidatePrepareArchive(request));
        request.fileSystem = FS_FAT32; request.isDevice = false; QVERIFY(!rufus::ValidatePrepareArchive(request));
        rufus::PrepareImageOutcome outcome, parsed;
        outcome.archiveDone = true; outcome.archiveBytes = 123;
        const auto result = rufus::EncodePrepareOutcome(outcome);
        QVERIFY(rufus::DecodePrepareOutcome(result, parsed));
        QVERIFY(parsed.archiveDone); QCOMPARE(parsed.archiveBytes, quint64(123));
        auto changed = result; auto archive = changed.value("archive").toObject();
        archive.insert("bytes", 123); changed.insert("archive", archive);
        QVERIFY(!rufus::DecodePrepareOutcome(changed, parsed));
        int operation = OP_NOOP; quint64 processed = 0, total = 0;
        QVERIFY(rufus::DecodePrepareProgress(rufus::EncodePrepareProgress(OP_EXTRACT_ZIP, 45, 45), operation, processed, total));
        QCOMPARE(operation, int(OP_EXTRACT_ZIP)); QCOMPARE(processed, quint64(45)); QCOMPARE(total, quint64(45));
    }

    void optimizedFfuProtocol()
    {
        auto request = silentRequest();
        request.windowsFlags = 0; request.windowsUsername.clear();
        request.windowsInstallPath.clear(); request.windowsEdition = 0;
        request.windowsRegion = {}; request.windowsSilentConfirmed = false;
        request.rawWrite = true;
        request.imagePath = QStringLiteral("/tmp/optimized.ffu");
        request.imageCompression = IMG_COMPRESSION_FFU;
        request.decodedImageSize = 128 * 1024 * 1024;
        request.ffuMinimumSize = 32 * 1024 * 1024;
        request.imageSectorSize = 512;
        request.decodedImageSha256 = QByteArray(32, 'h');
        request.imageLayoutSha256 = QByteArray(32, 'l');
        QCOMPARE(rufus::PrepareRawWriteBytes(request), request.ffuMinimumSize);
        const auto good = rufus::EncodePrepareRequest(request);
        QVERIFY(!good.isEmpty());
        rufus::PrepareImageRequest decoded;
        QVERIFY(rufus::DecodePrepareRequest(good, decoded));
        QCOMPARE(decoded.ffuMinimumSize, request.ffuMinimumSize);
        QCOMPARE(decoded.decodedImageSize, request.decodedImageSize);
        for (const QJsonValue& value : {QJsonValue("0"), QJsonValue("134217729"),
                QJsonValue(33554432), QJsonValue(true), QJsonValue("-1")})
        {
            auto bad = good;
            auto source = bad.value("source").toObject();
            auto compression = source.value("compression").toObject();
            compression.insert("minimum", value); source.insert("compression", compression); bad.insert("source", source);
            QVERIFY(!rufus::DecodePrepareRequest(bad, decoded));
        }
        request.ffuMinimumSize = 0;
        QVERIFY(!rufus::ValidatePrepareCompressedImage(request)); // Captured geometry exceeds this target.
        request.size = request.decodedImageSize;
        QVERIFY(rufus::ValidatePrepareCompressedImage(request));
        request.imageCompression = IMG_COMPRESSION_VHDX;
        request.ffuMinimumSize = 32 * 1024 * 1024;
        QVERIFY(!rufus::ValidatePrepareCompressedImage(request));
    }

    void isoEspProtocol()
    {
        auto request = silentRequest();
        request.fileSystem = FS_FAT32; request.uefiNtfs = false; request.isoEsp = true;
        request.windowsFlags = 0; request.windowsUsername.clear();
        request.windowsInstallPath.clear(); request.windowsEdition = 0;
        request.windowsRegion = {}; request.windowsSilentConfirmed = false;
        const auto good = rufus::EncodePrepareRequest(request);
        QVERIFY(!good.isEmpty());
        rufus::PrepareImageRequest decoded;
        QVERIFY(rufus::DecodePrepareRequest(good, decoded)); QVERIFY(decoded.isoEsp);
        for (const QJsonValue& value : {QJsonValue(false), QJsonValue(1), QJsonValue("true"), QJsonValue(QJsonValue::Null)})
        {
            auto bad = good; bad.insert("isoEsp", value);
            QVERIFY(!rufus::DecodePrepareRequest(bad, decoded));
        }
        auto legacy = good; legacy.remove("isoEsp");
        QVERIFY(rufus::DecodePrepareRequest(legacy, decoded)); QVERIFY(!decoded.isoEsp);
        request.quickFormat = false; QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        request.quickFormat = true; request.fileSystem = FS_NTFS;
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        request.fileSystem = FS_FAT32; request.mbr = true;
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        request.mbr = false; request.rawWrite = true;
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
    }

    void oldBiosProtocol()
    {
        auto request = silentRequest();
        request.windowsFlags = 0; request.windowsUsername.clear();
        request.windowsInstallPath.clear(); request.windowsEdition = 0;
        request.windowsRegion = {}; request.windowsSilentConfirmed = false;
        request.windowsBios = request.mbr = request.oldBiosFixes = true;
        const auto good = rufus::EncodePrepareRequest(request);
        QVERIFY(!good.isEmpty());
        rufus::PrepareImageRequest decoded;
        QVERIFY(rufus::DecodePrepareRequest(good, decoded));
        QVERIFY(decoded.oldBiosFixes && decoded.windowsBios && decoded.mbr);
        for (const QJsonValue& value : {QJsonValue(false), QJsonValue(1), QJsonValue("true"), QJsonValue(QJsonValue::Null)})
        {
            auto bad = good; bad.insert("oldBiosFixes", value);
            QVERIFY(!rufus::DecodePrepareRequest(bad, decoded));
        }
        auto legacy = good; legacy.remove("oldBiosFixes");
        QVERIFY(rufus::DecodePrepareRequest(legacy, decoded)); QVERIFY(!decoded.oldBiosFixes);
        request.windowsBios = false; request.nonBootable = true; request.imagePath.clear();
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        auto bad = rufus::EncodePrepareRequest(silentRequest()); bad.insert("oldBiosFixes", true);
        QVERIFY(!rufus::DecodePrepareRequest(bad, decoded));
    }

    void extendedLabelProtocol()
    {
        auto request = silentRequest();
        request.extendedLabel = true;
        request.displayLabel = QString::fromUtf8("Măriuș 安装 💾 / long original label");
        const auto good = rufus::EncodePrepareRequest(request);
        QVERIFY(!good.isEmpty());
        rufus::PrepareImageRequest decoded;
        QVERIFY(rufus::DecodePrepareRequest(good, decoded));
        QVERIFY(decoded.extendedLabel); QCOMPARE(decoded.displayLabel, request.displayLabel);
        for (const QJsonValue& label : {QJsonValue(1), QJsonValue(QJsonValue::Null), QJsonValue("bad\nsection"),
            QJsonValue(QString(128, QLatin1Char('x'))), QJsonValue(QString(QChar(0xd800))), QJsonValue(QString(QChar(0)))})
        {
            auto object = good; object.insert("autorun", QJsonObject{{"label", label}});
            QVERIFY(!rufus::DecodePrepareRequest(object, decoded));
        }
        auto extra = good; extra.insert("autorun", QJsonObject{{"label", "ok"}, {"path", "/tmp/escape"}});
        QVERIFY(!rufus::DecodePrepareRequest(extra, decoded));
        extra = rufus::EncodePrepareRequest(request_); extra.insert("autorun", QJsonObject{{"label", "ok"}});
        QVERIFY(!rufus::DecodePrepareRequest(extra, decoded));
        request.extendedLabel = false;
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty()); // Disabled jobs cannot carry hidden metadata.
        request.displayLabel.clear();
        const auto plain = rufus::EncodePrepareRequest(request);
        QVERIFY(rufus::DecodePrepareRequest(plain, decoded));
        QVERIFY(!decoded.extendedLabel && decoded.displayLabel.isEmpty());
    }

    void fullFormatProtocol()
    {
        auto request = silentRequest();
        request.imagePath.clear(); request.windowsFlags = 0; request.windowsUsername.clear();
        request.windowsInstallPath.clear(); request.windowsEdition = 0;
        request.windowsRegion = {}; request.windowsSilentConfirmed = false;
        request.nonBootable = request.mbr = true;
        for (int filesystem : {FS_FAT16, FS_FAT32, FS_NTFS, FS_EXFAT})
        {
            request.fileSystem = filesystem;
            request.clusterSize = filesystem == FS_FAT16 ? 2048 : filesystem == FS_FAT32 ? 512 : 4096;
            request.quickFormat = false;
            const QJsonObject full = rufus::EncodePrepareRequest(request);
            QVERIFY(!full.isEmpty() && full.value(QStringLiteral("fullFormat")).toBool());
            rufus::PrepareImageRequest decoded;
            QVERIFY(rufus::DecodePrepareRequest(full, decoded));
            QVERIFY(!decoded.quickFormat);
            QCOMPARE(decoded.fileSystem, filesystem);
            QJsonObject quick = full; quick.remove(QStringLiteral("fullFormat"));
            QVERIFY(rufus::DecodePrepareRequest(quick, decoded));
            QVERIFY(decoded.quickFormat);
            for (const QJsonValue& value : {QJsonValue(false), QJsonValue(1), QJsonValue(QStringLiteral("true")), QJsonValue(QJsonValue::Null)})
            {
                QJsonObject bad = full; bad.insert(QStringLiteral("fullFormat"), value);
                QVERIFY(!rufus::DecodePrepareRequest(bad, decoded));
            }
        }
        request.fileSystem = FS_FAT32; request.size = quint64(64) << 30;
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        request.size = quint64(64) << 20; request.nonBootable = false;
        request.imageDevice = request.imageInode = request.imageSize = 0;
        request.imageModified.clear(); request.imageChanged.clear();
        request.mbr = false; request.clusterSize = 512;
        request.rawWrite = true; request.zeroMode = ZERO_FULL;
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        request.quickFormat = true;
        QJsonObject zero = rufus::EncodePrepareRequest(request);
        QVERIFY(!zero.isEmpty()); zero.insert(QStringLiteral("fullFormat"), true);
        rufus::PrepareImageRequest decoded;
        QVERIFY(!rufus::DecodePrepareRequest(zero, decoded));
    }

    void fixedUsbRequest_data()
    {
        QTest::addColumn<QString>("route");
        for (const char* route : {"iso", "syslinux", "format", "sfd", "file"})
            QTest::newRow(route) << QString::fromLatin1(route);
    }

    void fixedUsbRequest()
    {
        QFETCH(QString, route);
        auto request = silentRequest();
        request.windowsFlags = 0;
        request.windowsUsername.clear();
        request.windowsInstallPath.clear();
        request.windowsEdition = 0;
        request.windowsRegion = {};
        request.windowsSilentConfirmed = false;
        request.syslinux = route == QStringLiteral("syslinux");
        request.nonBootable = route == QStringLiteral("format") || route == QStringLiteral("sfd");
        request.sfd = route == QStringLiteral("sfd");
        const bool file = route == QStringLiteral("file");
        if (request.nonBootable || file) request.imagePath.clear();
        if (file) { request.isDevice = false; request.inode = 456; }
        const QJsonObject original = rufus::EncodePrepareRequest(request);
        request.allowFixedUsb = true;
        const QJsonObject encoded = rufus::EncodePrepareRequest(request);
        rufus::PrepareImageRequest decoded;
        if (file)
        {
            QVERIFY(encoded.isEmpty());
            QJsonObject invalid = original;
            invalid.insert("allowFixedUsb", true);
            QVERIFY(!rufus::DecodePrepareRequest(invalid, decoded));
            return;
        }
        QCOMPARE(encoded.size(), original.size() + 1);
        QVERIFY(rufus::DecodePrepareRequest(encoded, decoded));
        QVERIFY(decoded.allowFixedUsb);
        for (const QJsonValue& value : {QJsonValue(false), QJsonValue(1), QJsonValue("true"), QJsonValue()})
        {
            QJsonObject invalid = encoded;
            invalid.insert("allowFixedUsb", value);
            QVERIFY(!rufus::DecodePrepareRequest(invalid, decoded));
        }
        QVERIFY(rufus::DecodePrepareRequest(original, decoded));
        QVERIFY(!decoded.allowFixedUsb);
    }

    void nonBootableRequest_data()
    {
        QTest::addColumn<int>("fileSystem");
        QTest::addColumn<QString>("operation");
        QTest::newRow("FAT16") << int(FS_FAT16) << QStringLiteral("format-fat16-device-v1");
        QTest::newRow("FAT32") << int(FS_FAT32) << QStringLiteral("format-fat32-device-v1");
        QTest::newRow("MBR-FAT16") << int(FS_FAT16) << QStringLiteral("format-fat16-mbr-device-v1");
        QTest::newRow("SFD-FAT16") << int(FS_FAT16) << QStringLiteral("format-fat16-sfd-device-v1");
        QTest::newRow("SFD-FAT32") << int(FS_FAT32) << QStringLiteral("format-fat32-sfd-device-v1");
        QTest::newRow("MBR-FAT32") << int(FS_FAT32) << QStringLiteral("format-fat32-mbr-device-v1");
        QTest::newRow("SFD-exFAT") << int(FS_EXFAT) << QStringLiteral("format-exfat-sfd-device-v1");
        QTest::newRow("SFD-NTFS") << int(FS_NTFS) << QStringLiteral("format-ntfs-sfd-device-v1");
        QTest::newRow("NTFS") << int(FS_NTFS) << QStringLiteral("format-ntfs-device-v1");
        QTest::newRow("MBR-NTFS") << int(FS_NTFS) << QStringLiteral("format-ntfs-mbr-device-v1");
    }

    void nonBootableRequest()
    {
        QFETCH(int, fileSystem);
        QFETCH(QString, operation);
        rufus::PrepareImageRequest request = request_;
        request.fileSystem = fileSystem;
        if (fileSystem == FS_NTFS || fileSystem == FS_EXFAT)
            request.volumeId = 0;
        request.nonBootable = true;
        request.sfd = operation.contains(QStringLiteral("-sfd-"));
        request.mbr = operation.contains(QStringLiteral("-mbr-"));
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty()); // Never a file job.
        request.isDevice = true;
        request.path = QStringLiteral("/dev/test");
        request.device = 123;
        request.diskSequence = 7;
        request.objectPath = QStringLiteral("/org/freedesktop/UDisks2/block_devices/test");
        request.serviceOwner = QStringLiteral(":1.2");
        request.identity = {QStringLiteral("fixture")};
        const QJsonObject encoded = rufus::EncodePrepareRequest(request);
        QCOMPARE(encoded.value("operation").toString(), operation);
        rufus::PrepareImageRequest decoded;
        QVERIFY(rufus::DecodePrepareRequest(encoded, decoded));
        QVERIFY(decoded.nonBootable && decoded.isDevice && !decoded.syslinux);
        QVERIFY(decoded.imagePath.isEmpty());
        QCOMPARE(decoded.fileSystem, fileSystem);
        QCOMPARE(decoded.sfd, request.sfd);
        QCOMPARE(decoded.mbr, request.mbr);
        QCOMPARE(decoded.volumeId, request.volumeId);
        for (const QString& field : {QStringLiteral("source"), QStringLiteral("windows"),
            QStringLiteral("enhancedInstall"), QStringLiteral("inode"), QStringLiteral("formatter"), QStringLiteral("mbr"), QStringLiteral("sfd")})
        {
            QJsonObject changed = encoded;
            changed.insert(field, QJsonObject());
            QVERIFY(!rufus::DecodePrepareRequest(changed, decoded));
            QVERIFY(decoded.nonBootable); // Rejection preserves the destination.
        }
        request.imagePath = QStringLiteral("/tmp/source.iso");
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        request.imagePath.clear();
        request.syslinux = true;
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        request.syslinux = false;
        request.enhancedInstall = true;
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        request.enhancedInstall = false;
        request.windowsFlags = UNATTEND_NO_DATA_COLLECTION;
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        request.windowsFlags = 0;
        request.windowsEdition = 1;
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        request.windowsEdition = 0;
        if (fileSystem == FS_NTFS || fileSystem == FS_EXFAT)
        {
            request.volumeId = 1;
            QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
            QJsonObject changed = encoded;
            changed.insert("volume", QStringLiteral("1"));
            QVERIFY(!rufus::DecodePrepareRequest(changed, decoded));
            request.volumeId = 0;
        }
        request.nonBootable = false;
        if (request.mbr || request.sfd)
            QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        request.mbr = request.sfd = false;
        QVERIFY(rufus::DecodePrepareRequest(rufus::EncodePrepareRequest(request), decoded));
        QCOMPARE(decoded.nonBootable, fileSystem == FS_EXFAT); // exFAT retains its format-only contract.
        QVERIFY(!decoded.mbr && !decoded.sfd);
        QCOMPARE(decoded.fileSystem, fileSystem);
    }

    void unsupportedSfdRequests()
    {
        rufus::PrepareImageRequest request = silentRequest();
        request.imagePath.clear();
        request.windowsFlags = 0;
        request.windowsUsername.clear();
        request.windowsInstallPath.clear();
        request.windowsEdition = 0;
        request.windowsSilentConfirmed = false;
        request.windowsRegion = {};
        request.nonBootable = true;
        request.sfd = true;
        for (int fileSystem : {FS_MAX, FS_UNKNOWN})
        {
            request.fileSystem = fileSystem;
            QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        }
        request.fileSystem = FS_FAT32;
        const QJsonObject encoded = rufus::EncodePrepareRequest(request);
        QVERIFY(!encoded.isEmpty());
        request.mbr = true;
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        rufus::PrepareImageRequest decoded;
        for (const char* name : {"format-exfat-sfd-image-v1", "format-ntfs-sfd-image-v1", "format-fat32-sfd-image-v1"})
        {
            QJsonObject changed = encoded;
            changed.insert("operation", QString::fromLatin1(name));
            QVERIFY(!rufus::DecodePrepareRequest(changed, decoded));
        }
    }

    void unsupportedMbrFormats()
    {
        rufus::PrepareImageRequest request;
        request.isDevice = true;
        request.path = QStringLiteral("/dev/test");
        request.device = 123;
        request.size = 64 * 1024 * 1024;
        request.diskSequence = 7;
        request.objectPath = QStringLiteral("/org/freedesktop/UDisks2/block_devices/test");
        request.serviceOwner = QStringLiteral(":1.2");
        request.identity = {QStringLiteral("fixture")};
        request.nonBootable = true;
        request.mbr = true;
        request.fileSystem = -1;
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        request.fileSystem = FS_FAT32;
        QJsonObject encoded = rufus::EncodePrepareRequest(request);
        QVERIFY(!encoded.isEmpty());
        rufus::PrepareImageRequest valid;
        QVERIFY(rufus::DecodePrepareRequest(encoded, valid));
        encoded.insert("operation", QStringLiteral("format-unknown-mbr-device-v1"));
        rufus::PrepareImageRequest decoded;
        QVERIFY(!rufus::DecodePrepareRequest(encoded, decoded));
    }

    void exfatRequestBoundary_data()
    {
        QTest::addColumn<bool>("mbr");
        QTest::newRow("GPT") << false;
        QTest::newRow("MBR") << true;
    }

    void exfatRequestBoundary()
    {
        QFETCH(bool, mbr);
        rufus::PrepareImageRequest request;
        request.mbr = mbr;
        request.nonBootable = mbr; // Existing GPT exFAT requests imply format-only.
        request.isDevice = true;
        request.fileSystem = FS_EXFAT;
        request.path = QStringLiteral("/dev/test");
        request.device = 123;
        request.size = 64 * 1024 * 1024;
        request.clusterSize = 128 * 1024;
        request.diskSequence = 7;
        request.objectPath = QStringLiteral("/org/freedesktop/UDisks2/block_devices/test");
        request.serviceOwner = QStringLiteral(":1.2");
        request.identity = {QStringLiteral("fixture")};
        request.label = QStringLiteral("Rufus ș😀");
        const QJsonObject encoded = rufus::EncodePrepareRequest(request);
        QCOMPARE(encoded.value("operation").toString(), mbr ? QStringLiteral("format-exfat-mbr-device-v1")
            : QStringLiteral("format-exfat-device-v1"));
        rufus::PrepareImageRequest decoded;
        QVERIFY(rufus::DecodePrepareRequest(encoded, decoded));
        QCOMPARE(decoded.fileSystem, int(FS_EXFAT));
        QVERIFY(decoded.nonBootable);
        QCOMPARE(decoded.mbr, mbr);
        QCOMPARE(decoded.clusterSize, request.clusterSize);
        if (mbr)
        {
            request.nonBootable = false;
            QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
            request.nonBootable = true;
        }
        QCOMPARE(decoded.label, request.label);
        QVERIFY(decoded.isDevice && !decoded.syslinux && decoded.imagePath.isEmpty());
        for (const QString& field : {QStringLiteral("source"), QStringLiteral("windows"),
            QStringLiteral("enhancedInstall"), QStringLiteral("inode"), QStringLiteral("formatter"), QStringLiteral("mbr")})
        {
            QJsonObject changed = encoded;
            changed.insert(field, QJsonObject());
            QVERIFY(!rufus::DecodePrepareRequest(changed, decoded));
            QCOMPARE(decoded.fileSystem, int(FS_EXFAT));
        }
        QJsonObject serial = encoded;
        serial.insert("volume", QStringLiteral("1"));
        QVERIFY(!rufus::DecodePrepareRequest(serial, decoded));
        request.volumeId = 1;
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        request.volumeId = 0;
        request.isDevice = false;
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        request.isDevice = true;
        request.imagePath = QStringLiteral("/tmp/source.iso");
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        request.imagePath.clear();
        request.syslinux = true;
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        request.syslinux = false;
        request.enhancedInstall = true;
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        request.enhancedInstall = false;
        request.windowsFlags = UNATTEND_NO_DATA_COLLECTION;
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        request.windowsFlags = 0;
        request.windowsEdition = 1;
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        QJsonObject legacy = encoded;
        legacy.insert("operation", QStringLiteral("prepare-device-v1"));
        QVERIFY(rufus::DecodePrepareRequest(legacy, decoded));
        QCOMPARE(decoded.fileSystem, int(FS_FAT32));
        QVERIFY(!decoded.mbr && !decoded.nonBootable);
    }

    void standaloneUefiRequest_data()
    {
        QTest::addColumn<int>("fileSystem");
        QTest::addColumn<bool>("mbr");
        QTest::newRow("GPT-NTFS") << int(FS_NTFS) << false;
        QTest::newRow("GPT-exFAT") << int(FS_EXFAT) << false;
        QTest::newRow("MBR-NTFS") << int(FS_NTFS) << true;
        QTest::newRow("MBR-exFAT") << int(FS_EXFAT) << true;
    }

    void standaloneUefiRequest()
    {
        QFETCH(int, fileSystem);
        QFETCH(bool, mbr);
        rufus::PrepareImageRequest request;
        request.isDevice = request.uefiNtfs = true;
        request.mbr = mbr;
        request.fileSystem = fileSystem;
        request.path = QStringLiteral("/dev/test");
        request.device = 123;
        request.size = 64 * 1024 * 1024;
        request.clusterSize = 4096;
        request.diskSequence = 7;
        request.objectPath = QStringLiteral("/org/freedesktop/UDisks2/block_devices/test");
        request.serviceOwner = QStringLiteral(":1.2");
        request.identity = {QStringLiteral("fixture")};
        request.label = QStringLiteral("Rufus ș😀");
        const QJsonObject encoded = rufus::EncodePrepareRequest(request);
        const QString expected = mbr ? (fileSystem == FS_NTFS
            ? QStringLiteral("prepare-uefi-ntfs-mbr-device-v1") : QStringLiteral("prepare-uefi-exfat-mbr-device-v1"))
            : (fileSystem == FS_NTFS ? QStringLiteral("prepare-ntfs-device-v1") : QStringLiteral("prepare-uefi-exfat-device-v1"));
        QCOMPARE(encoded.value("operation").toString(), expected);
        rufus::PrepareImageRequest decoded;
        QVERIFY(rufus::DecodePrepareRequest(encoded, decoded));
        QVERIFY(decoded.uefiNtfs && decoded.usesUefiNtfs());
        QVERIFY(!decoded.nonBootable && decoded.imagePath.isEmpty());
        QCOMPARE(decoded.fileSystem, fileSystem);
        QCOMPARE(decoded.mbr, mbr);
        QCOMPARE(decoded.label, request.label);
        QCOMPARE(rufus::EncodePrepareRequest(decoded), encoded);
        for (bool rufus::PrepareImageRequest::* flag : {&rufus::PrepareImageRequest::nonBootable,
            &rufus::PrepareImageRequest::sfd,
            &rufus::PrepareImageRequest::syslinux, &rufus::PrepareImageRequest::rawWrite,
            &rufus::PrepareImageRequest::enhancedInstall})
        {
            auto changed = request;
            changed.*flag = true;
            QVERIFY(rufus::EncodePrepareRequest(changed).isEmpty());
        }
        for (int unsupported : {FS_FAT16, FS_FAT32, FS_UNKNOWN})
        {
            auto changed = request;
            changed.fileSystem = unsupported;
            QVERIFY(rufus::EncodePrepareRequest(changed).isEmpty());
        }
        request.isDevice = false;
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        request.isDevice = true;
        request.imagePath = QStringLiteral("/tmp/source.iso");
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        request.imagePath.clear();
        request.windowsFlags = UNATTEND_NO_DATA_COLLECTION;
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        request.windowsFlags = 0;
        request.windowsEdition = 1;
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        if (fileSystem == FS_EXFAT || mbr)
        {
            for (const char* field : {"source", "windows", "enhancedInstall", "formatter", "uefiNtfs", "mbr"})
            {
                QJsonObject changed = encoded;
                changed.insert(QLatin1String(field), QJsonObject());
                QVERIFY(!rufus::DecodePrepareRequest(changed, decoded));
                QVERIFY(decoded.uefiNtfs); // A refusal leaves the destination unchanged.
            }
            QJsonObject serial = encoded;
            serial.insert("volume", QStringLiteral("1"));
            QVERIFY(!rufus::DecodePrepareRequest(serial, decoded));
            QJsonObject plain = encoded;
            plain.insert("operation", mbr ? (fileSystem == FS_NTFS
                ? QStringLiteral("format-ntfs-mbr-device-v1") : QStringLiteral("format-exfat-mbr-device-v1"))
                : QStringLiteral("format-exfat-device-v1"));
            QVERIFY(rufus::DecodePrepareRequest(plain, decoded));
            QVERIFY(decoded.nonBootable && !decoded.uefiNtfs && !decoded.usesUefiNtfs());
            QCOMPARE(decoded.mbr, mbr);
        }
    }

    void fat16RequestBoundary()
    {
        request_.fileSystem = FS_FAT16;
        request_.clusterSize = 2048;
        const QJsonObject encoded = rufus::EncodePrepareRequest(request_);
        QCOMPARE(encoded.value("operation").toString(), QStringLiteral("prepare-fat16-image-v1"));
        rufus::PrepareImageRequest decoded;
        QVERIFY(rufus::DecodePrepareRequest(encoded, decoded));
        QCOMPARE(decoded.fileSystem, int(FS_FAT16));
        QCOMPARE(decoded.clusterSize, request_.clusterSize);
        QCOMPARE(decoded.volumeId, request_.volumeId);
        QVERIFY(!decoded.isDevice && !decoded.syslinux);
        for (int fs : {FS_UNKNOWN, FS_EXFAT, FS_MAX, FS_NTFS})
        {
            request_.fileSystem = fs;
            QVERIFY(rufus::EncodePrepareRequest(request_).isEmpty());
        }
        request_.fileSystem = FS_FAT16;
        request_.isDevice = true;
        QCOMPARE(rufus::EncodePrepareRequest(request_).value("operation").toString(), QStringLiteral("prepare-fat16-device-v1"));
        request_.isDevice = false;
        request_.syslinux = true;
        QVERIFY(rufus::EncodePrepareRequest(request_).isEmpty());
        request_.syslinux = false;
        request_.imagePath = QStringLiteral("/tmp/source.iso");
        QVERIFY(rufus::EncodePrepareRequest(request_).isEmpty());
        request_.imagePath.clear();
        request_.windowsFlags = UNATTEND_NO_DATA_COLLECTION;
        QVERIFY(rufus::EncodePrepareRequest(request_).isEmpty());
        request_.windowsFlags = 0;
        request_.enhancedInstall = true;
        QVERIFY(rufus::EncodePrepareRequest(request_).isEmpty());
        for (const QString& field : {QStringLiteral("source"), QStringLiteral("windows"), QStringLiteral("filesystem")})
        {
            QJsonObject changed = encoded;
            changed.insert(field, QJsonObject());
            QVERIFY(!rufus::DecodePrepareRequest(changed, decoded));
            QCOMPARE(decoded.fileSystem, int(FS_FAT16)); // Rejection preserves the destination.
        }
        QJsonObject device = rufus::EncodePrepareRequest(silentRequest());
        device.insert("operation", QStringLiteral("prepare-fat16-image-v1"));
        QVERIFY(!rufus::DecodePrepareRequest(device, decoded));
        device.insert("operation", QStringLiteral("prepare-fat16-device-v1"));
        QVERIFY(rufus::DecodePrepareRequest(device, decoded));
        QCOMPARE(decoded.fileSystem, int(FS_FAT16));
        QJsonObject fat32 = encoded;
        fat32.insert("operation", QStringLiteral("prepare-image-v1"));
        QVERIFY(rufus::DecodePrepareRequest(fat32, decoded));
        QCOMPARE(decoded.fileSystem, int(FS_FAT32)); // Reuse cannot retain FAT16.
    }

    void fat16DeviceRequestBoundary()
    {
        request_.fileSystem = FS_FAT16;
        request_.isDevice = true;
        request_.path = QStringLiteral("/dev/test");
        request_.device = 123;
        request_.diskSequence = 7;
        request_.clusterSize = 2048;
        request_.objectPath = QStringLiteral("/org/freedesktop/UDisks2/block_devices/test");
        request_.serviceOwner = QStringLiteral(":1.2");
        request_.identity = {QStringLiteral("fixture")};
        const QJsonObject encoded = rufus::EncodePrepareRequest(request_);
        rufus::PrepareImageRequest decoded;
        QVERIFY(rufus::DecodePrepareRequest(encoded, decoded));
        QCOMPARE(decoded.fileSystem, int(FS_FAT16));
        QVERIFY(decoded.isDevice && !decoded.syslinux && decoded.imagePath.isEmpty());
        QCOMPARE(decoded.clusterSize, request_.clusterSize);
        QCOMPARE(decoded.volumeId, request_.volumeId);
        for (const QString& field : {QStringLiteral("source"), QStringLiteral("windows"), QStringLiteral("enhancedInstall")})
        {
            QJsonObject changed = encoded;
            changed.insert(field, QJsonObject());
            QVERIFY(!rufus::DecodePrepareRequest(changed, decoded));
        }
        request_.imagePath = QStringLiteral("/tmp/source.iso");
        QVERIFY(!rufus::DecodePrepareRequest(rufus::EncodePrepareRequest(request_), decoded));
        request_.imageDevice = 1;
        request_.imageInode = 2;
        request_.imageSize = 4096;
        request_.imageModified = request_.imageChanged = QStringLiteral("1:0");
        QVERIFY(rufus::DecodePrepareRequest(rufus::EncodePrepareRequest(request_), decoded));
        QCOMPARE(decoded.fileSystem, int(FS_FAT16));
        QCOMPARE(decoded.imagePath, request_.imagePath);
        QVERIFY(decoded.isDevice && !decoded.syslinux);
        request_.imagePath.clear();
        request_.syslinux = true;
        QVERIFY(rufus::EncodePrepareRequest(request_).isEmpty());
        request_.syslinux = false;
        request_.windowsFlags = UNATTEND_NO_DATA_COLLECTION;
        QVERIFY(rufus::EncodePrepareRequest(request_).isEmpty());
        request_.windowsFlags = 0;
        request_.enhancedInstall = true;
        QVERIFY(rufus::EncodePrepareRequest(request_).isEmpty());
    }

    void syslinuxProtocol_data()
    {
        QTest::addColumn<int>("fileSystem");
        QTest::addColumn<QString>("operation");
        QTest::addColumn<bool>("uefi");
        QTest::newRow("FAT32") << int(FS_FAT32) << QStringLiteral("prepare-syslinux-device-v1") << false;
        QTest::newRow("FAT16") << int(FS_FAT16) << QStringLiteral("prepare-fat16-syslinux-device-v1") << false;
        QTest::newRow("NTFS") << int(FS_NTFS) << QStringLiteral("prepare-ntfs-syslinux-device-v1") << false;
        QTest::newRow("NTFS-UEFI") << int(FS_NTFS) << QStringLiteral("prepare-ntfs-syslinux-uefi-device-v1") << true;
    }

    void syslinuxProtocol()
    {
        rufus::PrepareImageRequest request = silentRequest();
        request.windowsFlags = 0;
        request.windowsUsername.clear();
        request.windowsInstallPath.clear();
        request.windowsEdition = 0;
        request.windowsRegion = {};
        request.windowsSilentConfirmed = false;
        request.syslinux = true;
        QFETCH(int, fileSystem);
        QFETCH(QString, operation);
        QFETCH(bool, uefi);
        request.uefiNtfs = uefi;
        request.fileSystem = fileSystem;
        if (fileSystem == FS_NTFS) request.clusterSize = 4096;
        const QJsonObject encoded = rufus::EncodePrepareRequest(request);
        QCOMPARE(encoded.value("operation").toString(), operation);
        rufus::PrepareImageRequest decoded;
        QVERIFY(rufus::DecodePrepareRequest(encoded, decoded));
        QVERIFY(decoded.syslinux && decoded.isDevice);
        QCOMPARE(decoded.usesUefiNtfs(), uefi);
        QVERIFY(!decoded.isStandaloneSyslinux());
        QCOMPARE(decoded.syslinuxSelection, int(BT_IMAGE));
        QCOMPARE(decoded.fileSystem, fileSystem);
        QCOMPARE(decoded.imagePath, request.imagePath);
        QJsonObject changed = encoded;
        changed.remove("source");
        QVERIFY(!rufus::DecodePrepareRequest(changed, decoded));
        changed = encoded;
        changed.insert("enhancedInstall", false);
        QVERIFY(!rufus::DecodePrepareRequest(changed, decoded));
        changed = encoded;
        changed.insert("windows", QJsonObject{{"flags", "0"}, {"username", ""}});
        QVERIFY(!rufus::DecodePrepareRequest(changed, decoded));
        changed = encoded;
        changed.insert("installer", "/tmp/untrusted");
        QVERIFY(!rufus::DecodePrepareRequest(changed, decoded));
        request.fileSystem = FS_EXFAT;
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        request.fileSystem = fileSystem;
        request.enhancedInstall = true;
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        request.enhancedInstall = false;
        request.windowsFlags = UNATTEND_NO_DATA_COLLECTION;
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        request.windowsFlags = 0;
        request.isDevice = false;
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        request.isDevice = true;
        request.imagePath.clear();
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        request.syslinux = false;
        QVERIFY(rufus::DecodePrepareRequest(rufus::EncodePrepareRequest(request), decoded));
        QVERIFY(!decoded.syslinux); // Decoder reuse must not carry the route forward.
        QJsonObject legacy = encoded;
        legacy.insert("operation", QStringLiteral("prepare-syslinux-device-v1"));
        QVERIFY(rufus::DecodePrepareRequest(legacy, decoded));
        QCOMPARE(decoded.fileSystem, int(FS_FAT32));
        legacy.insert("filesystem", FS_FAT16);
        QVERIFY(!rufus::DecodePrepareRequest(legacy, decoded));
    }

    void progressFramesPreserveCounters()
    {
        const quint64 total = UINT64_MAX;
        const quint64 processed = total / 2;
        QJsonObject frame = rufus::EncodePrepareProgress(OP_FILE_COPY, processed, total);
        int operation = OP_MAX;
        quint64 decoded = 0, decodedTotal = 0;
        QVERIFY(rufus::DecodePrepareProgress(frame, operation, decoded, decodedTotal));
        QCOMPARE(operation, int(OP_FILE_COPY));
        QCOMPARE(decoded, processed);
        QCOMPARE(decodedTotal, total);
        frame["processed"] = QStringLiteral("18446744073709551616");
        QVERIFY(!rufus::DecodePrepareProgress(frame, operation, decoded, decodedTotal));
        frame["processed"] = 5;
        QVERIFY(!rufus::DecodePrepareProgress(frame, operation, decoded, decodedTotal));
        frame = rufus::EncodePrepareProgress(OP_FILE_COPY, 11, 10);
        QVERIFY(!rufus::DecodePrepareProgress(frame, operation, decoded, decodedTotal));
        frame = rufus::EncodePrepareProgress(OP_FILE_COPY, 1, 10);
        frame["operation"] = 7.5;
        QVERIFY(!rufus::DecodePrepareProgress(frame, operation, decoded, decodedTotal));
        for (int nativeOperation : {OP_BADBLOCKS, OP_FORMAT})
        {
            frame = rufus::EncodePrepareProgress(nativeOperation, 1, 100);
            QVERIFY(rufus::DecodePrepareProgress(frame, operation, decoded, decodedTotal));
            QCOMPARE(operation, nativeOperation);
        }
        frame = rufus::EncodePrepareProgress(OP_MAX, 1, 10);
        QVERIFY(!rufus::DecodePrepareProgress(frame, operation, decoded, decodedTotal));
        frame = rufus::EncodePrepareProgress(OP_FILE_COPY, 0, 0);
        QVERIFY(rufus::DecodePrepareProgress(frame, operation, decoded, decodedTotal));
        frame["unexpected"] = true;
        QVERIFY(!rufus::DecodePrepareProgress(frame, operation, decoded, decodedTotal));
    }

    void enhancedInstallBoundary()
    {
        rufus::PrepareImageRequest source;
        source.isDevice = true;
        source.enhancedInstall = true;
        source.path = QStringLiteral("/dev/test");
        source.device = 9;
        source.diskSequence = 1;
        source.objectPath = QStringLiteral("/org/freedesktop/UDisks2/block_devices/test");
        source.serviceOwner = QStringLiteral(":1.2");
        source.identity = {QStringLiteral("fixture")};
        QVERIFY(rufus::EncodePrepareRequest(source).isEmpty());
        source.imagePath = QStringLiteral("/tmp/source.iso");
        source.imageDevice = 1;
        source.imageInode = 2;
        source.imageSize = 4096;
        source.imageModified = source.imageChanged = QStringLiteral("1:0");
        const QJsonObject encoded = rufus::EncodePrepareRequest(source);
        rufus::PrepareImageRequest decoded;
        QVERIFY(rufus::DecodePrepareRequest(encoded, decoded));
        QVERIFY(decoded.enhancedInstall);
        QCOMPARE(decoded.windowsFlags, 0); // Separate from customization/expert choices.
        QJsonObject changed = encoded;
        changed.insert("enhancedInstall", QStringLiteral("true"));
        QVERIFY(!rufus::DecodePrepareRequest(changed, decoded));
        changed = encoded;
        changed.remove("source");
        QVERIFY(!rufus::DecodePrepareRequest(changed, decoded));
        changed = encoded;
        changed.remove("enhancedInstall");
        QVERIFY(rufus::DecodePrepareRequest(changed, decoded));
        QVERIFY(!decoded.enhancedInstall);
        source.isDevice = false;
        QVERIFY(rufus::EncodePrepareRequest(source).isEmpty());
    }

    void ntfsRequestBoundary()
    {
        rufus::PrepareImageRequest source;
        source.isDevice = true;
        source.fileSystem = FS_NTFS;
        source.path = QStringLiteral("/dev/test");
        source.device = 123;
        source.size = 64 * 1024 * 1024;
        source.diskSequence = 7;
        source.clusterSize = 4096;
        source.label = QStringLiteral("Rufus ș😀");
        source.objectPath = QStringLiteral("/org/freedesktop/UDisks2/block_devices/test");
        source.serviceOwner = QStringLiteral(":1.2");
        source.identity = {QStringLiteral("fixture")};
        rufus::PrepareImageRequest decoded;
        QVERIFY(rufus::DecodePrepareRequest(rufus::EncodePrepareRequest(source), decoded));
        QVERIFY(decoded.fileSystem == FS_NTFS && decoded.isDevice);
        QCOMPARE(decoded.label, source.label);
        source.imagePath = QStringLiteral("/tmp/source.iso");
        QVERIFY(!rufus::DecodePrepareRequest(rufus::EncodePrepareRequest(source), decoded));
        source.imageDevice = 1;
        source.imageInode = 2;
        source.imageSize = 4096;
        source.imageModified = source.imageChanged = QStringLiteral("1:0");
        QVERIFY(rufus::DecodePrepareRequest(rufus::EncodePrepareRequest(source), decoded));
        QCOMPARE(decoded.imagePath, source.imagePath);
        QCOMPARE(decoded.imageSize, source.imageSize);
        source.windowsFlags = UNATTEND_NO_DATA_COLLECTION;
        QVERIFY(rufus::DecodePrepareRequest(rufus::EncodePrepareRequest(source), decoded));
        QCOMPARE(decoded.windowsFlags, UNATTEND_NO_DATA_COLLECTION);
        QJsonObject customized = rufus::EncodePrepareRequest(source);
        QJsonObject windows = customized.value("windows").toObject();
        windows.insert("path", QStringLiteral("/not-a-request-field"));
        customized.insert("windows", windows);
        QVERIFY(!rufus::DecodePrepareRequest(customized, decoded));
        windows.remove("path");
        windows.insert("flags", QStringLiteral("-1"));
        customized.insert("windows", windows);
        QVERIFY(!rufus::DecodePrepareRequest(customized, decoded));
        source.windowsUsername = QStringLiteral("Măriuș Test");
        source.windowsFlags = UNATTEND_SET_USER;
        QVERIFY(rufus::DecodePrepareRequest(rufus::EncodePrepareRequest(source), decoded));
        QCOMPARE(decoded.windowsUsername, source.windowsUsername);
        source.imagePath.clear();
        QVERIFY(rufus::EncodePrepareRequest(source).isEmpty());
        source.windowsFlags = 0;
        source.windowsUsername.clear();
        source.volumeId = 1;
        QVERIFY(!rufus::DecodePrepareRequest(rufus::EncodePrepareRequest(source), decoded));
        source.volumeId = 0;
        source.isDevice = false;
        rufus::PrepareImageProcess process(QStringLiteral(RUFUS_PREPARE_HELPER));
        QVERIFY(!process.start(source));
    }

    void silentRequestRoundTrip()
    {
        const auto request = silentRequest();
        const QJsonObject encoded = rufus::EncodePrepareRequest(request);
        const QByteArray bytes = QJsonDocument(encoded).toJson(QJsonDocument::Compact);
        QVERIFY(bytes.size() + 1 < rufus::PrepareMessageBytes);
        rufus::PrepareImageRequest decoded;
        QVERIFY(rufus::DecodePrepareRequest(QJsonDocument::fromJson(bytes).object(), decoded));
        QCOMPARE(decoded.windowsFlags, request.windowsFlags);
        QCOMPARE(decoded.windowsUsername, request.windowsUsername);
        QCOMPARE(decoded.windowsInstallPath, request.windowsInstallPath);
        QCOMPARE(decoded.windowsEdition, 4);
        QVERIFY(decoded.windowsSilentConfirmed);
        QCOMPARE(decoded.windowsRegion.inputLocale, request.windowsRegion.inputLocale);
        QCOMPARE(decoded.windowsRegion.systemLocale, request.windowsRegion.systemLocale);
        QCOMPARE(decoded.windowsRegion.userLocale, request.windowsRegion.userLocale);
        QCOMPARE(decoded.windowsRegion.uiLanguage, request.windowsRegion.uiLanguage);
        QCOMPARE(decoded.windowsRegion.uiLanguageFallback, request.windowsRegion.uiLanguageFallback);
        QCOMPARE(decoded.windowsRegion.timezone, request.windowsRegion.timezone);
        // Reusing the decoder cannot carry silent settings into an ordinary job.
        auto ordinary = request;
        ordinary.windowsFlags = UNATTEND_NO_DATA_COLLECTION;
        QVERIFY(rufus::EncodePrepareRequest(ordinary).isEmpty());
        ordinary.windowsInstallPath.clear();
        ordinary.windowsEdition = 0;
        ordinary.windowsRegion = {};
        ordinary.windowsSilentConfirmed = false;
        ordinary.windowsUsername.clear();
        QVERIFY(rufus::DecodePrepareRequest(rufus::EncodePrepareRequest(ordinary), decoded));
        QVERIFY(!decoded.hasWindowsInstallSettings());
    }

    void malformedSilentRequests_data()
    {
        QTest::addColumn<QString>("field");
        QTest::addColumn<QJsonValue>("value");
        QTest::newRow("missing-edition") << QStringLiteral("edition") << QJsonValue(QJsonValue::Undefined);
        QTest::newRow("numeric-edition") << QStringLiteral("edition") << QJsonValue(4);
        QTest::newRow("zero-edition") << QStringLiteral("edition") << QJsonValue("0");
        QTest::newRow("overflow-edition") << QStringLiteral("edition") << QJsonValue("2147483648");
        QTest::newRow("padded-edition") << QStringLiteral("edition") << QJsonValue("04");
        QTest::newRow("missing-confirmation") << QStringLiteral("confirmed") << QJsonValue(QJsonValue::Undefined);
        QTest::newRow("text-confirmation") << QStringLiteral("confirmed") << QJsonValue("true");
        QTest::newRow("missing-archive") << QStringLiteral("archive") << QJsonValue(QJsonValue::Undefined);
        QTest::newRow("null-archive") << QStringLiteral("archive") << QJsonValue(QStringLiteral("/sources/install.wim") + QChar::Null);
        QTest::newRow("missing-region") << QStringLiteral("region") << QJsonValue(QJsonValue::Undefined);
        QTest::newRow("injected-setup-language") << QStringLiteral("setupLanguage") << QJsonValue("en-US");
    }

    void malformedSilentRequests()
    {
        QFETCH(QString, field);
        QFETCH(QJsonValue, value);
        QJsonObject frame = rufus::EncodePrepareRequest(silentRequest());
        QJsonObject windows = frame["windows"].toObject();
        QJsonObject silent = windows["silent"].toObject();
        silent.insert(field, value);
        windows.insert("silent", silent);
        frame.insert("windows", windows);
        rufus::PrepareImageRequest decoded;
        decoded.path = QStringLiteral("unchanged");
        QVERIFY(!rufus::DecodePrepareRequest(frame, decoded));
        QCOMPARE(decoded.path, QStringLiteral("unchanged"));
    }

    void silentRegionAndFlagBoundaries()
    {
        const auto request = silentRequest();
        const QJsonObject good = rufus::EncodePrepareRequest(request);
        const QJsonObject goodWindows = good["windows"].toObject();
        const QJsonObject goodSilent = goodWindows["silent"].toObject();
        const QJsonObject goodRegion = goodSilent["region"].toObject();
        rufus::PrepareImageRequest decoded;
        for (const QString& field : goodRegion.keys())
        {
            QJsonObject frame = good, windows = goodWindows, silent = goodSilent, region = goodRegion;
            region.remove(field); // Even an empty fallback must be explicit.
            silent.insert("region", region);
            windows.insert("silent", silent);
            frame.insert("windows", windows);
            QVERIFY(!rufus::DecodePrepareRequest(frame, decoded));
            region.insert(field, 42);
            silent.insert("region", region);
            windows.insert("silent", silent);
            frame.insert("windows", windows);
            QVERIFY(!rufus::DecodePrepareRequest(frame, decoded));
        }
        QJsonObject frame = good, windows = goodWindows;
        windows.remove("silent");
        frame.insert("windows", windows);
        QVERIFY(!rufus::DecodePrepareRequest(frame, decoded));
        windows = goodWindows;
        windows.insert("flags", QString::number(UNATTEND_NO_DATA_COLLECTION));
        frame.insert("windows", windows);
        QVERIFY(!rufus::DecodePrepareRequest(frame, decoded));
    }

    void init()
    {
        QVERIFY(directory_.isValid());
        request_ = {};
        request_.path = directory_.filePath(QStringLiteral("target.img"));
        request_.size = 64 * 1024 * 1024;
        request_.volumeId = 0x1234abcd;
        request_.label = QStringLiteral("RUFUS TEST");
        QFile file(request_.path);
        QVERIFY(file.open(QIODevice::ReadWrite | QIODevice::Truncate));
        QVERIFY(file.resize(qint64(request_.size)));
        struct stat info;
        QCOMPARE(fstat(file.handle(), &info), 0);
        request_.device = quint64(info.st_dev);
        request_.inode = quint64(info.st_ino);
    }

    void nativeCompletionAndReuse_data()
    {
        QTest::addColumn<int>("fileSystem");
        QTest::newRow("FAT32") << int(FS_FAT32);
        QTest::newRow("FAT16") << int(FS_FAT16);
    }

    void nativeCompletionAndReuse()
    {
        QFETCH(int, fileSystem);
        request_.fileSystem = fileSystem;
        request_.clusterSize = fileSystem == FS_FAT16 ? 2048 : 512;
        rufus::PrepareImageProcess process(QString::fromLocal8Bit(RUFUS_PREPARE_HELPER));
        QSignalSpy completed(&process, &rufus::PrepareImageProcess::finished);
        QSignalSpy phases(&process, &rufus::PrepareImageProcess::phaseChanged);
        QVERIFY(process.start(request_));
        QVERIFY(!process.start(request_));
        QTRY_COMPARE_WITH_TIMEOUT(completed.size(), 1, 10000);
        QCOMPARE(process.state(), rufus::PrepareImageProcess::State::Complete);
        QVERIFY(!process.isActive());
        QCOMPARE(process.outcome().error, 0);
        QVERIFY(process.outcome().started);
        QVERIFY(process.outcome().filesystemBytes > 0);
        QCOMPARE(phases.size(), 3);
        QCOMPARE(phases[0][0].toString(), QStringLiteral("checking"));
        QCOMPARE(phases[1][0].toString(), QStringLiteral("partitioning"));
        QCOMPARE(phases[2][0].toString(), QStringLiteral("formatting"));
        // Reuse the same process object with the other FAT type; no old request
        // or phase state may survive. Actual bytes must match the chosen type.
        QFile image(request_.path);
        QVERIFY(image.open(QIODevice::ReadOnly));
        QVERIFY(image.seek(qint64(process.outcome().offset) + (fileSystem == FS_FAT16 ? 54 : 82)));
        QCOMPARE(image.read(8), fileSystem == FS_FAT16 ? QByteArray("FAT16   ") : QByteArray("FAT32   "));
        image.close();
        request_.fileSystem = fileSystem == FS_FAT16 ? FS_FAT32 : FS_FAT16;
        request_.clusterSize = request_.fileSystem == FS_FAT16 ? 2048 : 512;
        QVERIFY(process.start(request_));
        QTRY_COMPARE_WITH_TIMEOUT(completed.size(), 2, 10000);
        QCOMPARE(process.state(), rufus::PrepareImageProcess::State::Complete);
        QVERIFY(image.open(QIODevice::ReadOnly));
        QVERIFY(image.seek(qint64(process.outcome().offset) + (request_.fileSystem == FS_FAT16 ? 54 : 82)));
        QCOMPARE(image.read(8), request_.fileSystem == FS_FAT16 ? QByteArray("FAT16   ") : QByteArray("FAT32   "));
    }

    void cancelBeforeRequest()
    {
        rufus::PrepareImageProcess process(QString::fromLocal8Bit(RUFUS_PREPARE_HELPER));
        QSignalSpy completed(&process, &rufus::PrepareImageProcess::finished);
        QVERIFY(process.start(request_));
        process.cancel();
        process.cancel();
        QTRY_COMPARE_WITH_TIMEOUT(completed.size(), 1, 5000);
        QCOMPARE(process.state(), rufus::PrepareImageProcess::State::Cancelled);
        QVERIFY(!process.outcome().started);
        QFile file(request_.path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray(qsizetype(request_.size), '\0'));
    }

    void changedTargetFailsBeforeWrites()
    {
        rufus::PrepareImageProcess process(QString::fromLocal8Bit(RUFUS_PREPARE_HELPER));
        QSignalSpy completed(&process, &rufus::PrepareImageProcess::finished);
        ++request_.inode;
        QVERIFY(process.start(request_));
        QTRY_COMPARE_WITH_TIMEOUT(completed.size(), 1, 5000);
        QCOMPARE(process.state(), rufus::PrepareImageProcess::State::Failed);
        QCOMPARE(process.outcome().error, ESTALE);
        QVERIFY(!process.outcome().started);
        QVERIFY(!process.problem().isEmpty());
    }

    void failedLaunchDoesNotRetry()
    {
        rufus::PrepareImageProcess process(directory_.filePath(QStringLiteral("missing")));
        QSignalSpy completed(&process, &rufus::PrepareImageProcess::finished);
        QVERIFY(process.start(request_));
        QTRY_COMPARE_WITH_TIMEOUT(completed.size(), 1, 5000);
        QCOMPARE(process.state(), rufus::PrepareImageProcess::State::Failed);
        QVERIFY(!process.isActive());
        QVERIFY(!process.outcome().started);
    }

    void fileRequestCannotStartExtraction()
    {
        rufus::PrepareImageProcess process(QString::fromLocal8Bit(RUFUS_PREPARE_HELPER));
        request_.imagePath = directory_.filePath(QStringLiteral("source.iso"));
        QVERIFY(!process.start(request_));
        QCOMPARE(process.state(), rufus::PrepareImageProcess::State::Idle);
    }

    void cancelledJobWithCleanupFailureIsFailed()
    {
        rufus::PrepareImageOutcome outcome;
        outcome.error = ECANCELED;
        outcome.cleanupError = EBUSY;
        outcome.started = true;
        const QByteArray response = QJsonDocument(rufus::EncodePrepareOutcome(outcome)).toJson(QJsonDocument::Compact);
        const QString name = directory_.filePath(QStringLiteral("cleanup-helper"));
        QFile script(name);
        QVERIFY(script.open(QIODevice::WriteOnly | QIODevice::Truncate));
        const QByteArray code = "#!/usr/bin/python3\nimport sys\n"
            "print('{\"event\":\"ready\"}', flush=True)\nsys.stdin.readline()\n"
            "print(bytes.fromhex('" + response.toHex() + "').decode(), flush=True)\nsys.exit(1)\n";
        QCOMPARE(script.write(code), code.size());
        QVERIFY(script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        script.close();
        rufus::PrepareImageProcess process(name);
        QSignalSpy completed(&process, &rufus::PrepareImageProcess::finished);
        QVERIFY(process.start(request_));
        QTRY_COMPARE_WITH_TIMEOUT(completed.size(), 1, 5000);
        QCOMPARE(process.state(), rufus::PrepareImageProcess::State::Failed);
        QCOMPARE(process.outcome().error, ECANCELED);
        QCOMPARE(process.outcome().cleanupError, EBUSY);
        QVERIFY(process.problem().contains(QStringLiteral("cleanup")));
    }

    void invalidHelperResponse_data()
    {
        QTest::addColumn<QByteArray>("reply");
        QTest::addColumn<int>("exitCode");
        QTest::newRow("invalid-json") << QByteArray("not json\n") << 0;
        QTest::newRow("second-ready") << QByteArray("{\"event\":\"ready\"}\n") << 0;
        QTest::newRow("progress-before-copy-phase") <<
            QJsonDocument(rufus::EncodePrepareProgress(OP_FILE_COPY, 1, 10)).toJson(QJsonDocument::Compact) + '\n' << 1;
        QTest::newRow("missing-result") << QByteArray() << 0;
        QTest::newRow("oversized") << QByteArray(8192, 'x') << 0;
        QTest::newRow("incomplete") << QByteArray("{\"event\":") << 0;
        rufus::PrepareImageOutcome error;
        error.error = EIO;
        const QByteArray result = QJsonDocument(rufus::EncodePrepareOutcome(error)).toJson(QJsonDocument::Compact);
        QTest::newRow("finalizing-without-mount-stage")
            << QByteArray("{\"event\":\"phase\",\"phase\":\"finalizing\"}\n") + result + '\n' << 1;
    }

    void flushFailureReportsPartialImage()
    {
        qputenv("RUFUS_TEST_SYNC_ERROR", "1");
        rufus::PrepareImageProcess process(QString::fromLocal8Bit(RUFUS_PREPARE_PROBE));
        QSignalSpy completed(&process, &rufus::PrepareImageProcess::finished);
        QVERIFY(process.start(request_));
        QTRY_COMPARE_WITH_TIMEOUT(completed.size(), 1, 5000);
        QCOMPARE(process.state(), rufus::PrepareImageProcess::State::Failed);
        QVERIFY(process.outcome().started);
        QCOMPARE(process.outcome().syncError, EIO);
        QVERIFY(process.problem().contains(QStringLiteral("flush")));
    }

    void invalidHelperResponse()
    {
        QFETCH(QByteArray, reply);
        QFETCH(int, exitCode);
        const QString name = directory_.filePath(QStringLiteral("invalid-helper"));
        QFile script(name);
        QVERIFY(script.open(QIODevice::WriteOnly | QIODevice::Truncate));
        const QByteArray code = "#!/usr/bin/python3\nimport sys\n"
            "print('{\"event\":\"ready\"}', flush=True)\nsys.stdin.readline()\n"
            "sys.stdout.buffer.write(bytes.fromhex('" + reply.toHex() + "'))\n"
            "sys.stdout.flush()\nsys.exit(" + QByteArray::number(exitCode) + ")\n";
        QCOMPARE(script.write(code), code.size());
        QVERIFY(script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        script.close();
        rufus::PrepareImageProcess process(name);
        QSignalSpy completed(&process, &rufus::PrepareImageProcess::finished);
        QVERIFY(process.start(request_));
        QTRY_COMPARE_WITH_TIMEOUT(completed.size(), 1, 5000);
        QCOMPARE(process.state(), rufus::PrepareImageProcess::State::Failed);
        QVERIFY(process.outcome().started); // A lost result cannot promise unchanged media.
        QVERIFY(!process.problem().isEmpty());
    }

    void activeCancellationAndDestruction_data()
    {
        QTest::addColumn<bool>("destroy");
        QTest::addColumn<int>("fileSystem");
        QTest::newRow("FAT32-cancel") << false << int(FS_FAT32);
        QTest::newRow("FAT32-destroy-controller") << true << int(FS_FAT32);
        QTest::newRow("FAT16-cancel") << false << int(FS_FAT16);
        QTest::newRow("FAT16-destroy-controller") << true << int(FS_FAT16);
    }

    void activeCancellationAndDestruction()
    {
        QFETCH(bool, destroy);
        QFETCH(int, fileSystem);
        request_.fileSystem = fileSystem;
        request_.clusterSize = fileSystem == FS_FAT16 ? 2048 : 512;
        const QString marker = directory_.filePath(QStringLiteral("child.pid"));
        const QString name = directory_.filePath(QStringLiteral("formatter"));
        QFile::remove(marker);
        QFile script(name);
        QVERIFY(script.open(QIODevice::WriteOnly | QIODevice::Truncate));
        const QByteArray code = "#!/usr/bin/python3\nimport os, signal, time\n"
            "from pathlib import Path\nsignal.signal(signal.SIGTERM, signal.SIG_IGN)\n"
            "Path(os.environ['RUFUS_TEST_PID']).write_text(str(os.getpid()))\ntime.sleep(60)\n";
        QCOMPARE(script.write(code), code.size());
        QVERIFY(script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        script.close();
        qputenv("RUFUS_TEST_FORMATTER", QFile::encodeName(name));
        qputenv("RUFUS_TEST_PID", QFile::encodeName(marker));
        std::unique_ptr<rufus::PrepareImageProcess> process = std::make_unique<rufus::PrepareImageProcess>(
            QString::fromLocal8Bit(RUFUS_PREPARE_PROBE));
        QSignalSpy completed(process.get(), &rufus::PrepareImageProcess::finished);
        QVERIFY(process->start(request_));
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo(marker).size() > 0, 5000);
        QFile pidFile(marker);
        QVERIFY(pidFile.open(QIODevice::ReadOnly));
        const QByteArray pid = pidFile.readAll();
        QVERIFY(pid.toInt() > 0);
        if (destroy)
            process.reset();
        else
        {
            process->cancel();
            QTRY_COMPARE_WITH_TIMEOUT(completed.size(), 1, 5000);
            QCOMPARE(process->state(), rufus::PrepareImageProcess::State::Cancelled);
            QVERIFY(process->outcome().started);
            QCOMPARE(process->outcome().syncError, 0);
        }
        QVERIFY(!QFileInfo::exists(QStringLiteral("/proc/") + QString::fromLatin1(pid)));
    }

    void cleanup()
    {
        qunsetenv("RUFUS_TEST_FORMATTER");
        qunsetenv("RUFUS_TEST_PID");
        qunsetenv("RUFUS_TEST_SYNC_ERROR");
    }
};

QTEST_GUILESS_MAIN(TestPrepareImageProcess)
#include "test_prepare_image_process.moc"
