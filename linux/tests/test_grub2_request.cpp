// SPDX-License-Identifier: GPL-3.0-or-later
#include "qt/prepare_image_process.h"
#include "platform/format_options.h"
#include "unattend.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <cerrno>

namespace
{
QString fixturePath;
int launches = 0;
int fatBits = 32;
constexpr quint64 mib = 1024 * 1024;
constexpr quint32 fat16ClusterBytes = 2048, fat32ClusterBytes = 512;

rufus::PrepareImageRequest grub2Request()
{
    rufus::PrepareImageRequest request;
    request.grub2 = request.mbr = request.isDevice = true;
    request.path = QStringLiteral("/dev/fixture");
    request.device = 123;
    request.size = 64 * mib;
    request.volumeId = fatBits == 0 ? 0 : 0x12345678;
    request.fileSystem = fatBits == 0 ? FS_NTFS : fatBits == 16 ? FS_FAT16 : FS_FAT32;
    request.clusterSize = fatBits == 0 ? 4096 : fatBits == 16 ? 2048 : 512;
    request.label = QStringLiteral("GRUB2");
    request.diskSequence = 7;
    request.objectPath = QStringLiteral("/org/freedesktop/UDisks2/block_devices/fixture");
    request.serviceOwner = QStringLiteral(":1.2");
    request.identity = {QStringLiteral("fixture")};
    return request;
}

rufus::PrepareImageRequest grub2ImageRequest()
{
    auto request = grub2Request();
    request.imagePath = QStringLiteral("/tmp/selected.iso");
    request.imageDevice = 123;
    request.imageInode = 456;
    request.imageSize = mib;
    request.imageModified = QStringLiteral("100.000000001");
    request.imageChanged = QStringLiteral("101.000000002");
    return request;
}
}

// Redirect only this test's authorization launch; production still uses pkexec.
extern "C" void __real__ZN8QProcess5startERK7QStringRK5QListIS0_E6QFlagsIN13QIODeviceBase12OpenModeFlagEE(
    QProcess*, const QString&, const QStringList&, QIODevice::OpenMode);
extern "C" void __wrap__ZN8QProcess5startERK7QStringRK5QListIS0_E6QFlagsIN13QIODeviceBase12OpenModeFlagEE(
    QProcess* process, const QString& program, const QStringList& arguments, QIODevice::OpenMode mode)
{
    const QStringList expected = {QStringLiteral("--disable-internal-agent"), fixturePath, QStringLiteral("--device")};
    if (program != QStringLiteral("/usr/bin/pkexec") || arguments != expected)
        qFatal("Unexpected GRUB2 helper launch");
    ++launches;
    __real__ZN8QProcess5startERK7QStringRK5QListIS0_E6QFlagsIN13QIODeviceBase12OpenModeFlagEE(
        process, fixturePath, {}, mode);
}

class TestGrub2Request : public QObject
{
    Q_OBJECT
    QTemporaryDir directory_;

    void writeHelper(const rufus::PrepareImageRequest& request, const QList<QJsonObject>& frames,
        const QByteArray& ending = {}, const QByteArray& core = {})
    {
        fixturePath = directory_.filePath(QStringLiteral("helper"));
        auto metadataRequest = request;
        if (request.grub4dos && !core.isEmpty())
        {
            metadataRequest.grub2CorePath = QStringLiteral("/tmp/placeholder");
            metadataRequest.grub2CoreSize = quint64(core.size());
            metadataRequest.grub2CoreSha256 = QCryptographicHash::hash(core, QCryptographicHash::Sha256);
        }
        QJsonObject expectedObject = rufus::EncodePrepareRequest(metadataRequest);
        if (!core.isEmpty()) expectedObject.remove("grubCore");
        const QByteArray expected = QJsonDocument(expectedObject).toJson(QJsonDocument::Compact);
        QByteArray code = "#!/usr/bin/python3\nimport json, sys, time, select, hashlib, stat\nfrom pathlib import Path\n"
            "print('{\"event\":\"ready\"}', flush=True)\n"
            "request = json.loads(sys.stdin.readline())\n";
        if (!core.isEmpty())
        {
            const QByteArray marker = QFile::encodeName(directory_.filePath(QStringLiteral("staged-path")));
            code += "metadata = request.pop('grubCore')\n"
                "assert set(metadata) == {'path', 'size', 'sha256'}\n"
                "path = Path(metadata['path'])\n"
                "data = path.read_bytes()\n"
                "assert data == bytes.fromhex('" + core.toHex() + "')\n"
                "assert metadata['size'] == str(len(data))\n"
                "assert metadata['sha256'] == hashlib.sha256(data).hexdigest()\n"
                "assert stat.S_IMODE(path.stat().st_mode) == 0o400\n"
                "assert stat.S_IMODE(path.parent.stat().st_mode) == 0o700\n"
                "Path(bytes.fromhex('" + marker.toHex() + "').decode()).write_text(str(path))\n";
        }
        code += "assert request == json.loads(bytes.fromhex('" + expected.toHex() + "'))\n";
        for (const QJsonObject& frame : frames)
            code += "print(bytes.fromhex('" + QJsonDocument(frame).toJson(QJsonDocument::Compact).toHex() + "').decode(), flush=True)\n";
        code += ending;
        QFile helper(fixturePath);
        QVERIFY(helper.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(helper.write(code), code.size());
        QVERIFY(helper.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    }

    QList<QJsonObject> completeFrames(bool image = false) const
    {
        QList<QJsonObject> frames;
        for (const char* phase : {"checking", "partitioning", "formatting", "bootloader"})
            frames.append({{"event", "phase"}, {"phase", phase}});
        if (image)
            for (const char* phase : {"mounting", "extracting"})
                frames.append({{"event", "phase"}, {"phase", phase}});
        frames.append({{"event", "phase"}, {"phase", "finalizing"}});
        rufus::PrepareImageOutcome outcome;
        outcome.started = true;
        outcome.offset = mib;
        outcome.size = outcome.filesystemBytes = 63 * mib;
        outcome.copiedBytes = image ? 4096 : 0;
        frames.append(rufus::EncodePrepareOutcome(outcome));
        return frames;
    }

private slots:
    void initTestCase_data()
    {
        QTest::addColumn<int>("bits");
        QTest::newRow("fat16") << 16;
        QTest::newRow("fat32") << 32;
        QTest::newRow("ntfs") << 0;
    }

    void init()
    {
        QVERIFY(directory_.isValid());
        launches = 0;
        QFETCH_GLOBAL(int, bits);
        fatBits = bits;
    }

    void roundTrip()
    {
        auto request = grub2Request();
        request.allowFixedUsb = true;
        const QJsonObject encoded = rufus::EncodePrepareRequest(request);
        QCOMPARE(encoded.size(), 12);
        const QString operation = fatBits == 0 ? QStringLiteral("prepare-grub2-ntfs-device-v1")
            : QStringLiteral("prepare-grub2-fat%1-device-v1").arg(fatBits);
        QCOMPARE(encoded.value("operation"), QJsonValue(operation));
        QVERIFY(!encoded.contains("source"));
        rufus::PrepareImageRequest decoded;
        QVERIFY(rufus::DecodePrepareRequest(encoded, decoded));
        QVERIFY(decoded.grub2 && decoded.mbr && decoded.isDevice && decoded.allowFixedUsb);
        QVERIFY(!decoded.freeDos && !decoded.nonBootable && !decoded.sfd && !decoded.syslinux && !decoded.usesUefiNtfs());
        QCOMPARE(decoded.syslinuxSelection, int(BT_IMAGE));
        QCOMPARE(decoded.fileSystem, request.fileSystem);
        QCOMPARE(rufus::EncodePrepareRequest(decoded), encoded);
        GPT_IMAGE_RESULT partition;
        QCOMPARE(rufus::PlanFormatPartition(decoded, partition), 0);
        QCOMPARE(partition.offset, mib);
        QCOMPARE(partition.uefi_ntfs_size, quint64(0));
        QCOMPARE(rufus::FormatMbrBootCode(decoded), MBR_BOOT_GRUB2);
        QCOMPARE(rufus::CheckFormatSettings(decoded, partition), 0);
        QVERIFY(QJsonDocument(encoded).toJson(QJsonDocument::Compact).size() < rufus::PrepareMessageBytes);
        RUFUS_PROGRESS_OPTIONS options = {};
        options.bootable = true;
        options.selection = BT_GRUB2;
        RUFUS_PROGRESS progress;
        InitProgressState(&progress, &options);
        QCOMPARE(progress.nb_slots[OP_FILE_COPY], 3);
    }

    void grub4dosRoundTrip_data()
    {
        QTest::addColumn<int>("filesystem");
        QTest::newRow("fat16") << int(FS_FAT16);
        QTest::newRow("fat32") << int(FS_FAT32);
        QTest::newRow("ntfs") << int(FS_NTFS);
    }

    void grub4dosRoundTrip()
    {
        QFETCH(int, filesystem);
        auto request = grub2Request();
        request.grub2 = false; request.grub4dos = true;
        const bool ntfs = filesystem == FS_NTFS;
        request.fileSystem = filesystem;
        request.clusterSize = ntfs ? 4096 : filesystem == FS_FAT16 ? fat16ClusterBytes : fat32ClusterBytes;
        if (ntfs) request.volumeId = 0;
        request.grub2CorePath = QStringLiteral("/tmp/private/grldr");
        request.grub2CoreSize = 322965;
        request.grub2CoreSha256 = QByteArray::fromHex("19ba57329be745ceae46354c5d411b7d8efec6802cfd7f6fee986df4b763f6fe");
        const QJsonObject good = rufus::EncodePrepareRequest(request);
        QVERIFY(!good.isEmpty());
        const QString operation = filesystem == FS_NTFS ? QStringLiteral("prepare-grub4dos-ntfs-device-v1") : filesystem == FS_FAT16 ?
            QStringLiteral("prepare-grub4dos-fat16-device-v1") : QStringLiteral("prepare-grub4dos-fat32-device-v1");
        QCOMPARE(good.value("operation"), QJsonValue(operation));
        rufus::PrepareImageRequest decoded;
        QVERIFY(rufus::DecodePrepareRequest(good, decoded));
        QVERIFY(decoded.grub4dos && decoded.isSourceFree() && !decoded.grub2 && !decoded.usesUefiNtfs());
        QCOMPARE(decoded.fileSystem, filesystem);
        QCOMPARE(rufus::EncodePrepareRequest(decoded), good);
        QCOMPARE(rufus::FormatMbrBootCode(decoded), MBR_BOOT_GRUB4DOS);
        GPT_IMAGE_RESULT partition = {};
        QCOMPARE(rufus::PlanFormatPartition(decoded, partition), 0);
        QCOMPARE(partition.offset, mib);
        QCOMPARE(rufus::CheckFormatSettings(decoded, partition), 0);
        for (const QString& key : good.keys())
        {
            auto changed = good; changed.remove(key);
            QVERIFY2(!rufus::DecodePrepareRequest(changed, decoded), qPrintable(key));
        }
        for (int filesystem : {FS_EXFAT, FS_EXT2, FS_EXT3})
        {
            auto changed = request; changed.fileSystem = filesystem;
            QVERIFY(rufus::EncodePrepareRequest(changed).isEmpty());
        }
        for (bool rufus::PrepareImageRequest::* flag : {&rufus::PrepareImageRequest::rawWrite,
                &rufus::PrepareImageRequest::nonBootable, &rufus::PrepareImageRequest::sfd,
                &rufus::PrepareImageRequest::syslinux, &rufus::PrepareImageRequest::freeDos,
                &rufus::PrepareImageRequest::grub2, &rufus::PrepareImageRequest::windowsBios,
                &rufus::PrepareImageRequest::uefiNtfs, &rufus::PrepareImageRequest::enhancedInstall})
        {
            auto changed = request; changed.*flag = true;
            QVERIFY(rufus::EncodePrepareRequest(changed).isEmpty());
        }
        auto unknown = good;
        unknown.insert("operation", "prepare-grub4dos-exfat-device-v1");
        QVERIFY(!rufus::DecodePrepareRequest(unknown, decoded));
        if (ntfs)
        {
            auto serial = request; serial.volumeId = 1;
            QVERIFY(rufus::EncodePrepareRequest(serial).isEmpty());
        }
        request.grub2CoreSha256.clear();
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        QCOMPARE(launches, 0);
    }

    void grub4dosImageRoundTrip()
    {
        auto request = grub2ImageRequest();
        request.grub2 = false;
        request.grub4dos = true;
        const QJsonObject encoded = rufus::EncodePrepareRequest(request);
        QVERIFY(!encoded.isEmpty());
        QVERIFY(encoded.contains("source") && !encoded.contains("grubCore"));
        const QString operation = request.fileSystem == FS_NTFS
            ? QStringLiteral("prepare-grub4dos-ntfs-device-v1")
            : QStringLiteral("prepare-grub4dos-fat%1-device-v1").arg(fatBits);
        QCOMPARE(encoded.value("operation"), QJsonValue(operation));
        rufus::PrepareImageRequest decoded;
        QVERIFY(rufus::DecodePrepareRequest(encoded, decoded));
        QVERIFY(decoded.grub4dos && !decoded.grub2 && !decoded.isSourceFree());
        QVERIFY(!decoded.usesUefiNtfs());
        QCOMPARE(decoded.imagePath, request.imagePath);
        QCOMPARE(decoded.imageModified, request.imageModified);
        QCOMPARE(decoded.fileSystem, request.fileSystem);
        QCOMPARE(decoded.clusterSize, request.clusterSize);
        QCOMPARE(decoded.volumeId, request.volumeId);
        QCOMPARE(rufus::EncodePrepareRequest(decoded), encoded);
        QCOMPARE(rufus::FormatMbrBootCode(decoded), MBR_BOOT_GRUB4DOS);
        GPT_IMAGE_RESULT partition = {};
        QCOMPARE(rufus::PlanFormatPartition(decoded, partition), 0);
        QCOMPARE(rufus::CheckFormatSettings(decoded, partition), 0);
        QCOMPARE(partition.offset, mib);
        QCOMPARE(partition.uefi_ntfs_size, quint64(0));
        auto incomplete = request;
        incomplete.imageChanged.clear();
        QVERIFY(rufus::EncodePrepareRequest(incomplete).isEmpty());
        auto suppliedLoader = request;
        suppliedLoader.grub2CorePath = QStringLiteral("/tmp/private/grldr");
        suppliedLoader.grub2CoreSize = 512;
        suppliedLoader.grub2CoreSha256 = QByteArray(32, 'x');
        QVERIFY(rufus::EncodePrepareRequest(suppliedLoader).isEmpty());
        auto enhanced = request;
        enhanced.enhancedInstall = true;
        QVERIFY(rufus::EncodePrepareRequest(enhanced).isEmpty());
        auto dualMode = request;
        dualMode.uefiNtfs = true;
        QCOMPARE(rufus::EncodePrepareRequest(dualMode).isEmpty(), request.fileSystem != FS_NTFS);
        auto customized = request;
        customized.windowsFlags = UNATTEND_NO_DATA_COLLECTION;
        QVERIFY(rufus::EncodePrepareRequest(customized).isEmpty());
        if (request.fileSystem == FS_NTFS)
        {
            auto serial = request;
            serial.volumeId = 1;
            QVERIFY(rufus::EncodePrepareRequest(serial).isEmpty());
            auto suppliedSerial = encoded;
            suppliedSerial.insert("volume", QStringLiteral("1"));
            QVERIFY(!rufus::DecodePrepareRequest(suppliedSerial, decoded));
        }
        for (int filesystem : {FS_EXFAT, FS_EXT2})
        {
            auto unsupported = request;
            unsupported.fileSystem = filesystem;
            QVERIFY(rufus::EncodePrepareRequest(unsupported).isEmpty());
        }
        auto noSource = encoded;
        noSource.remove("source");
        QVERIFY(!rufus::DecodePrepareRequest(noSource, decoded));
        auto unsupported = encoded;
        unsupported.insert("operation", QStringLiteral("prepare-grub4dos-exfat-device-v1"));
        QVERIFY(!rufus::DecodePrepareRequest(unsupported, decoded));
        auto injectedLoader = encoded;
        injectedLoader.insert("grubCore", QJsonObject{{"path", QStringLiteral("/tmp/grldr")},
            {"size", QStringLiteral("512")}, {"sha256", QString(64, QLatin1Char('a'))}});
        QVERIFY(!rufus::DecodePrepareRequest(injectedLoader, decoded));
        writeHelper(request, completeFrames(true));
        rufus::PrepareImageProcess process(fixturePath);
        QSignalSpy done(&process, &rufus::PrepareImageProcess::finished);
        QVERIFY(process.start(request));
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 5000);
        QVERIFY2(process.state() == rufus::PrepareImageProcess::State::Complete, qPrintable(process.problem()));
        QCOMPARE(process.outcome().copiedBytes, quint64(4096));
        QCOMPARE(launches, 1);
        QVERIFY(!process.start(request, QByteArray(512, 'x')));
        QCOMPARE(launches, 1);
        auto emptyFrames = completeFrames(true);
        emptyFrames.last().insert("copiedBytes", QStringLiteral("0"));
        writeHelper(request, emptyFrames);
        rufus::PrepareImageProcess emptyProcess(fixturePath);
        QSignalSpy emptyDone(&emptyProcess, &rufus::PrepareImageProcess::finished);
        QVERIFY(emptyProcess.start(request));
        QTRY_COMPARE_WITH_TIMEOUT(emptyDone.size(), 1, 5000);
        QCOMPARE(emptyProcess.state(), rufus::PrepareImageProcess::State::Failed);
        QCOMPARE(launches, 2);
    }

    void grub4dosResponse_data()
    {
        QTest::addColumn<QString>("mode");
        QTest::addColumn<int>("filesystem");
        for (int filesystem : {FS_FAT16, FS_FAT32, FS_NTFS})
        {
            for (const char* mode : {"success", "zero-copy", "short-copy", "extra-copy", "sync-error",
                    "cleanup-error", "skip-boot", "skip-mount", "skip-extract", "skip-finalize",
                    "wrong-exit", "missing-result", "held-exit", "cancel"})
                QTest::newRow(qPrintable(QStringLiteral("%1-%2").arg(filesystem == FS_NTFS ? "ntfs" : filesystem == FS_FAT16 ? "fat16" : "fat32", mode)))
                    << QString::fromLatin1(mode) << filesystem;
        }
    }

    void grub4dosResponse()
    {
        QFETCH(QString, mode);
        QFETCH(int, filesystem);
        auto request = grub2Request();
        request.grub2 = false; request.grub4dos = true;
        const bool ntfs = filesystem == FS_NTFS;
        request.fileSystem = filesystem;
        request.clusterSize = ntfs ? 4096 : filesystem == FS_FAT16 ? fat16ClusterBytes : fat32ClusterBytes;
        if (ntfs) request.volumeId = 0;
        const QByteArray loader(700, 'l');
        auto frames = completeFrames(true);
        frames.last().insert("copiedBytes", QString::number(loader.size()));
        if (mode == QStringLiteral("zero-copy")) frames.last().insert("copiedBytes", "0");
        if (mode == QStringLiteral("short-copy")) frames.last().insert("copiedBytes", "699");
        if (mode == QStringLiteral("extra-copy")) frames.last().insert("copiedBytes", "701");
        if (mode == QStringLiteral("sync-error")) frames.last().insert("syncError", EIO);
        if (mode == QStringLiteral("cleanup-error")) frames.last().insert("cleanupError", EBUSY);
        if (mode == QStringLiteral("skip-boot")) frames.removeAt(3);
        if (mode == QStringLiteral("skip-mount")) frames.removeAt(4);
        if (mode == QStringLiteral("skip-extract")) frames.removeAt(5);
        if (mode == QStringLiteral("skip-finalize")) frames.removeAt(6);
        if (mode == QStringLiteral("missing-result")) frames.removeLast();
        const bool held = mode == QStringLiteral("held-exit") || mode == QStringLiteral("cancel");
        const QString reported = directory_.filePath(QStringLiteral("loader-reported"));
        QFile::remove(reported);
        QByteArray ending;
        if (held)
            ending = "Path(bytes.fromhex('" + QFile::encodeName(reported).toHex() + "').decode()).touch()\nsys.stdin.read()\n";
        if (mode == QStringLiteral("wrong-exit")) ending = "sys.exit(1)\n";
        writeHelper(request, frames, ending, loader);
        rufus::PrepareImageProcess process(fixturePath);
        QSignalSpy done(&process, &rufus::PrepareImageProcess::finished);
        QVERIFY(!process.start(request)); // Required loader missing: no authorization.
        QCOMPARE(launches, 0);
        QVERIFY(process.start(request, loader));
        if (held)
        {
            QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(reported), 5000);
            QVERIFY(process.isActive()); QCOMPARE(done.size(), 0);
            QFile marker(directory_.filePath(QStringLiteral("staged-path")));
            QVERIFY(marker.open(QIODevice::ReadOnly));
            const QString staged = QString::fromUtf8(marker.readAll());
            QVERIFY(QFileInfo::exists(staged));
            QCOMPARE(QFileInfo(staged).fileName(), QStringLiteral("grldr"));
            // Releasing stdin is the normal cancellation path. Both variants
            // keep staging until the held child actually exits.
            process.cancel();
        }
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 5000);
        const auto expected = held ? rufus::PrepareImageProcess::State::Cancelled
            : mode == QStringLiteral("success") ? rufus::PrepareImageProcess::State::Complete
            : rufus::PrepareImageProcess::State::Failed;
        QCOMPARE(process.state(), expected);
        QCOMPARE(launches, 1);
        QFile marker(directory_.filePath(QStringLiteral("staged-path")));
        QVERIFY(marker.open(QIODevice::ReadOnly));
        const QString staged = QString::fromUtf8(marker.readAll());
        QVERIFY(!QFileInfo::exists(staged));
        QVERIFY(!QFileInfo::exists(QFileInfo(staged).path()));
    }

    void extRoundTrip()
    {
        for (int fileSystem : {FS_EXT2, FS_EXT3})
        {
            auto request = grub2Request();
            request.size = 320 * mib;
            request.fileSystem = fileSystem;
            request.clusterSize = RUFUS_EXT_DEFAULT_CLUSTER;
            request.volumeId = 0;
            const QJsonObject encoded = rufus::EncodePrepareRequest(request);
            const QString operation = fileSystem == FS_EXT2 ?
                QStringLiteral("prepare-grub2-ext2-device-v1") : QStringLiteral("prepare-grub2-ext3-device-v1");
            QCOMPARE(encoded.value("operation"), QJsonValue(operation));
            rufus::PrepareImageRequest decoded;
            QVERIFY(rufus::DecodePrepareRequest(encoded, decoded));
            QCOMPARE(decoded.fileSystem, fileSystem);
            QVERIFY(decoded.grub2 && decoded.mbr && !decoded.usesUefiNtfs());
            QCOMPARE(rufus::EncodePrepareRequest(decoded), encoded);
            GPT_IMAGE_RESULT layout = {};
            QCOMPARE(rufus::PlanFormatPartition(decoded, layout), 0);
            QCOMPARE(rufus::CheckFormatSettings(decoded, layout), 0);
            QCOMPARE(layout.uefi_ntfs_size, quint64(0));
            QJsonObject changed = encoded;
            changed.insert("cluster", QStringLiteral("512"));
            QVERIFY(!rufus::DecodePrepareRequest(changed, decoded));
            changed = encoded;
            changed.insert("volume", QStringLiteral("1"));
            QVERIFY(!rufus::DecodePrepareRequest(changed, decoded));
            request.clusterSize = 512;
            QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
            request.clusterSize = RUFUS_EXT_DEFAULT_CLUSTER;
            request.volumeId = 1;
            QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        }
        auto request = grub2Request();
        request.fileSystem = FS_EXT4;
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
    }

    void imageRoundTrip()
    {
        auto request = grub2ImageRequest();
        request.rockRidge = request.joliet = false;
        for (bool withCore : {false, true})
        {
            if (withCore)
            {
                request.grub2CorePath = QStringLiteral("/tmp/private/core.img");
                request.grub2CoreSize = 512;
                request.grub2CoreSha256 = QCryptographicHash::hash(QByteArray("core"), QCryptographicHash::Sha256);
            }
            const QJsonObject encoded = rufus::EncodePrepareRequest(request);
            QVERIFY(!encoded.isEmpty());
            QVERIFY(encoded.contains("source"));
            QCOMPARE(encoded.contains("grubCore"), withCore);
            rufus::PrepareImageRequest decoded;
            QVERIFY(rufus::DecodePrepareRequest(encoded, decoded));
            QVERIFY(decoded.grub2 && decoded.mbr && !decoded.isSourceFree());
            QVERIFY(!decoded.usesUefiNtfs());
            QCOMPARE(decoded.imagePath, request.imagePath);
            QCOMPARE(decoded.imageInode, request.imageInode);
            QCOMPARE(decoded.imageSize, request.imageSize);
            QCOMPARE(decoded.imageModified, request.imageModified);
            QVERIFY(!decoded.rockRidge && !decoded.joliet);
            QCOMPARE(rufus::EncodePrepareRequest(decoded), encoded);
            QVERIFY(QJsonDocument(encoded).toJson(QJsonDocument::Compact).size() < rufus::PrepareMessageBytes);
            for (const char* key : {"windows", "enhancedInstall", "dos", "unexpected"})
            {
                QJsonObject altered = encoded;
                altered.insert(QLatin1String(key), QJsonObject());
                QVERIFY(!rufus::DecodePrepareRequest(altered, decoded));
            }
            const QJsonObject source = encoded.value("source").toObject();
            for (const QString& key : source.keys())
            {
                QJsonObject altered = encoded, incomplete = source;
                incomplete.remove(key);
                altered.insert("source", incomplete);
                QVERIFY2(!rufus::DecodePrepareRequest(altered, decoded), qPrintable(key));
            }
            for (const char* key : {"inode", "size"})
            {
                QJsonObject altered = encoded, empty = source;
                empty.insert(QLatin1String(key), QStringLiteral("0"));
                altered.insert("source", empty);
                QVERIFY(!rufus::DecodePrepareRequest(altered, decoded));
            }
        }
        for (int filesystem : {FS_EXT2, FS_EXT3})
        {
            request.fileSystem = filesystem;
            request.clusterSize = RUFUS_EXT_DEFAULT_CLUSTER;
            request.volumeId = 0;
            QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        }
        QCOMPARE(launches, 0);
    }

    void imageUefiRoundTrip_data()
    {
        QTest::addColumn<bool>("grub4dos");
        QTest::newRow("GRUB2") << false;
        QTest::newRow("GRUB4DOS") << true;
    }

    void imageUefiRoundTrip()
    {
        QFETCH(bool, grub4dos);
        auto request = grub2ImageRequest();
        request.grub2 = !grub4dos;
        request.grub4dos = grub4dos;
        request.uefiNtfs = true;
        QJsonObject encoded = rufus::EncodePrepareRequest(request);
        if (fatBits != 0)
        {
            QVERIFY(encoded.isEmpty());
            return;
        }
        const QString operation = grub4dos ? QStringLiteral("prepare-grub4dos-ntfs-uefi-device-v1")
            : QStringLiteral("prepare-grub2-ntfs-uefi-device-v1");
        QCOMPARE(encoded.value("operation"), QJsonValue(operation));
        QVERIFY(encoded.contains("source") && !encoded.contains("grubCore"));
        rufus::PrepareImageRequest decoded;
        QVERIFY(rufus::DecodePrepareRequest(encoded, decoded));
        QCOMPARE(decoded.grub2, !grub4dos);
        QCOMPARE(decoded.grub4dos, grub4dos);
        QVERIFY(decoded.mbr && decoded.uefiNtfs && decoded.usesUefiNtfs());
        QVERIFY(!decoded.isSourceFree() && !decoded.isStandaloneUefiNtfs());
        QCOMPARE(decoded.imagePath, request.imagePath);
        QCOMPARE(rufus::EncodePrepareRequest(decoded), encoded);
        GPT_IMAGE_RESULT partition = {}, biosPartition = {};
        QCOMPARE(rufus::PlanFormatPartition(decoded, partition), 0);
        QCOMPARE(rufus::CheckFormatSettings(decoded, partition), 0);
        QCOMPARE(partition.offset, mib);
        QVERIFY(partition.uefi_ntfs_size > 0 && partition.uefi_ntfs_offset >= partition.offset + partition.size);
        request.uefiNtfs = false;
        QCOMPARE(rufus::PlanFormatPartition(request, biosPartition), 0);
        QVERIFY(partition.size < biosPartition.size);
        QCOMPARE(rufus::FormatMbrBootCode(decoded), grub4dos ? MBR_BOOT_GRUB4DOS : MBR_BOOT_GRUB2);
        QJsonObject changed = encoded;
        changed.remove("source");
        QVERIFY(!rufus::DecodePrepareRequest(changed, decoded));
        changed = encoded;
        changed.insert("operation", "prepare-grub2-ext2-device-v1");
        QVERIFY(!rufus::DecodePrepareRequest(changed, decoded));
        for (const char* key : {"windows", "enhancedInstall", "uefiNtfs"})
        {
            changed = encoded;
            changed.insert(QLatin1String(key), true);
            QVERIFY(!rufus::DecodePrepareRequest(changed, decoded));
        }
        if (grub4dos)
        {
            auto suppliedLoader = request;
            suppliedLoader.uefiNtfs = true;
            suppliedLoader.grub2CorePath = QStringLiteral("/tmp/private/grldr");
            suppliedLoader.grub2CoreSize = 512;
            suppliedLoader.grub2CoreSha256 = QByteArray(32, 'x');
            QVERIFY(rufus::EncodePrepareRequest(suppliedLoader).isEmpty());
            changed = encoded;
            changed.remove("source");
            changed.insert("grubCore", QJsonObject{{"path", QStringLiteral("/tmp/private/grldr")},
                {"size", QStringLiteral("512")}, {"sha256", QString(64, QLatin1Char('a'))}});
            QVERIFY(!rufus::DecodePrepareRequest(changed, decoded));
            auto frames = completeFrames(true);
            rufus::PrepareImageOutcome outcome;
            outcome.started = true;
            outcome.offset = partition.offset;
            outcome.size = outcome.filesystemBytes = partition.size;
            outcome.copiedBytes = 4096;
            frames.last() = rufus::EncodePrepareOutcome(outcome);
            request.uefiNtfs = true;
            writeHelper(request, frames);
            rufus::PrepareImageProcess process(fixturePath);
            QSignalSpy done(&process, &rufus::PrepareImageProcess::finished);
            QVERIFY(process.start(request));
            QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 5000);
            QVERIFY2(process.state() == rufus::PrepareImageProcess::State::Complete, qPrintable(process.problem()));
            QCOMPARE(process.outcome().copiedBytes, outcome.copiedBytes);
            QCOMPARE(launches, 1);
            QVERIFY(!process.start(request, QByteArray(512, 'x')));
            QCOMPARE(launches, 1);
        }
        request.uefiNtfs = true;
        request.imageInode = 0;
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        request = grub2ImageRequest(); request.uefiNtfs = true; request.mbr = false;
        request.grub2 = !grub4dos;
        request.grub4dos = grub4dos;
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        QCOMPARE(launches, grub4dos ? 1 : 0);
    }

    void incompatibleOptions()
    {
        const auto good = grub2Request();
        for (bool rufus::PrepareImageRequest::* flag : {&rufus::PrepareImageRequest::rawWrite,
                &rufus::PrepareImageRequest::nonBootable, &rufus::PrepareImageRequest::freeDos,
                &rufus::PrepareImageRequest::syslinux, &rufus::PrepareImageRequest::sfd, &rufus::PrepareImageRequest::uefiNtfs,
                &rufus::PrepareImageRequest::enhancedInstall, &rufus::PrepareImageRequest::windowsSilentConfirmed})
        {
            auto request = good;
            request.*flag = true;
            QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        }
        auto request = good; request.isDevice = false;
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        request = good; request.grub2 = false; request.nonBootable = false;
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        request = good; request.imagePath = QStringLiteral("/tmp/source.iso");
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        request = good; request.windowsFlags = 1;
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        request = good; request.windowsUsername = QStringLiteral("user");
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        request = good; request.windowsEdition = 1;
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        request = good; request.dosCodepage = 437;
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        request = good; request.dosKeyboard = QStringLiteral("us");
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        request = good; request.dosRufusMbr = false;
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        for (int fileSystem : {FS_EXFAT, FS_UNKNOWN})
        {
            request = good; request.fileSystem = fileSystem;
            QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        }
        request = good; request.fileSystem = FS_NTFS; request.volumeId = 1;
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        request = good; request.mbr = false;
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        const int selections[] = {BT_SYSLINUX_V4, BT_SYSLINUX_V6, BT_GRUB2, BT_MAX, -1};
        for (int selection : selections)
        {
            request = good; request.syslinuxSelection = selection;
            QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
            rufus::PrepareImageProcess process(QStringLiteral("/unused-helper"));
            QVERIFY(!process.start(request));
        }
        QCOMPARE(launches, 0);
    }

    void malformedMessages()
    {
        const QJsonObject good = rufus::EncodePrepareRequest(grub2Request());
        rufus::PrepareImageRequest decoded;
        decoded.label = QStringLiteral("unchanged");
        for (const QString& field : good.keys())
        {
            for (const QJsonValue& value : {QJsonValue(), QJsonValue(1), QJsonValue(QJsonValue::Undefined)})
            {
                QJsonObject changed = good;
                changed.insert(field, value);
                QVERIFY2(!rufus::DecodePrepareRequest(changed, decoded), qPrintable(field));
                QCOMPARE(decoded.label, QStringLiteral("unchanged"));
            }
            QJsonObject changed = good;
            changed.remove(field); changed.insert("unexpected", true);
            QVERIFY(!rufus::DecodePrepareRequest(changed, decoded));
        }
        for (const char* field : {"source", "windows", "enhancedInstall", "dos", "command", "formatter", "inode", "version"})
        {
            QJsonObject changed = good;
            changed.insert(QLatin1String(field), QJsonObject());
            QVERIFY(!rufus::DecodePrepareRequest(changed, decoded));
        }
        for (const char* operation : {"prepare-grub2-device-v1", "prepare-grub2-exfat-device-v1",
                "prepare-grub2-fat16-image-v1", "prepare-grub2-fat32-device-v2"})
        {
            QJsonObject changed = good;
            changed.insert("operation", operation);
            QVERIFY(!rufus::DecodePrepareRequest(changed, decoded));
        }
    }

    void coreMetadata()
    {
        auto request = grub2Request();
        request.grub2CorePath = QStringLiteral("/tmp/private/core.img");
        request.grub2CoreSize = 35918;
        request.grub2CoreSha256 = QCryptographicHash::hash(QByteArray("identity"), QCryptographicHash::Sha256);
        const QJsonObject good = rufus::EncodePrepareRequest(request);
        QVERIFY(!good.isEmpty());
        rufus::PrepareImageRequest decoded;
        QVERIFY(rufus::DecodePrepareRequest(good, decoded));
        QCOMPARE(decoded.grub2CorePath, request.grub2CorePath);
        QCOMPARE(decoded.grub2CoreSize, request.grub2CoreSize);
        QCOMPARE(decoded.grub2CoreSha256, request.grub2CoreSha256);
        QCOMPARE(rufus::EncodePrepareRequest(decoded), good);
        QVERIFY(QJsonDocument(good).toJson(QJsonDocument::Compact).size() < rufus::PrepareMessageBytes);
        const QJsonObject metadata = good.value("grubCore").toObject();
        for (const QString& key : metadata.keys())
        {
            for (const QJsonValue& value : {QJsonValue(), QJsonValue(1), QJsonValue(QJsonValue::Undefined)})
            {
                QJsonObject changed = metadata;
                changed.insert(key, value);
                QJsonObject message = good; message.insert("grubCore", changed);
                QVERIFY(!rufus::DecodePrepareRequest(message, decoded));
            }
        }
        for (const QString& hash : {QStringLiteral(""), QString(62, 'a'), QString(66, 'a'),
                QString(64, 'A'), QString(64, 'x'), QString(64, 'a') + QStringLiteral(" ")})
        {
            QJsonObject changed = metadata; changed.insert("sha256", hash);
            QJsonObject message = good; message.insert("grubCore", changed);
            QVERIFY(!rufus::DecodePrepareRequest(message, decoded));
        }
        for (const QString& size : {QStringLiteral("0"), QStringLiteral("511"), QStringLiteral("1048065"),
                QStringLiteral("035918"), QStringLiteral("+35918"), QStringLiteral("35918 ")})
        {
            QJsonObject changed = metadata; changed.insert("size", size);
            QJsonObject message = good; message.insert("grubCore", changed);
            QVERIFY(!rufus::DecodePrepareRequest(message, decoded));
        }
        for (const QString& path : {QStringLiteral("relative.img"), QStringLiteral(""),
                QStringLiteral("/tmp/core") + QChar::Null})
        {
            QJsonObject changed = metadata; changed.insert("path", path);
            QJsonObject message = good; message.insert("grubCore", changed);
            QVERIFY(!rufus::DecodePrepareRequest(message, decoded));
        }
        QJsonObject changed = metadata; changed.insert("version", "2.12");
        QJsonObject message = good; message.insert("grubCore", changed);
        QVERIFY(!rufus::DecodePrepareRequest(message, decoded));
        message = good; message.insert("grubCore", QJsonArray());
        QVERIFY(!rufus::DecodePrepareRequest(message, decoded));
        auto nonGrub = grub2Request(); nonGrub.grub2 = false; nonGrub.nonBootable = true;
        message = rufus::EncodePrepareRequest(nonGrub); message.insert("grubCore", metadata);
        QVERIFY(!rufus::DecodePrepareRequest(message, decoded));
        request.grub2 = false;
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        request = grub2Request(); request.grub2CoreSize = 512;
        QVERIFY(request.hasGrub2Core());
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
    }

    void stagedCore_data()
    {
        QTest::addColumn<QString>("mode");
        for (const char* mode : {"success", "cancel", "error", "destroy"})
            QTest::newRow(mode) << QString::fromLatin1(mode);
    }

    void stagedCore()
    {
        QFETCH(QString, mode);
        const auto request = grub2Request();
        QByteArray core(35918, 'c');
        const QByteArray expected = core;
        auto frames = completeFrames();
        if (mode == QStringLiteral("error")) frames.last().insert("error", EIO);
        const QString release = directory_.filePath(QStringLiteral("release"));
        QFile::remove(release);
        QByteArray ending = "sys.stdin.read()\n" + QByteArray(mode == QStringLiteral("error") ? "sys.exit(1)\n" : "");
        if (mode == QStringLiteral("success"))
            ending = "while not Path(bytes.fromhex('" + QFile::encodeName(release).toHex() + "').decode()).exists():\n time.sleep(0.01)\n";
        const QString marker = directory_.filePath(QStringLiteral("staged-path"));
        QFile::remove(marker);
        writeHelper(request, frames, ending, core);
        auto process = std::make_unique<rufus::PrepareImageProcess>(fixturePath);
        QSignalSpy done(process.get(), &rufus::PrepareImageProcess::finished);
        QSignalSpy phases(process.get(), &rufus::PrepareImageProcess::phaseChanged);
        QVERIFY(process->start(request, core));
        core.fill('x'); // Mutating the caller's array cannot change the frozen file.
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(marker), 5000);
        QFile record(marker);
        QVERIFY(record.open(QIODevice::ReadOnly));
        const QString path = QString::fromLocal8Bit(record.readAll());
        QFile staged(path);
        QVERIFY(staged.open(QIODevice::ReadOnly));
        QCOMPARE(staged.readAll(), expected);
        staged.close();
        QTRY_COMPARE_WITH_TIMEOUT(phases.size(), 5, 5000);
        QVERIFY(process->isActive());
        QCOMPARE(done.size(), 0);
        if (mode == QStringLiteral("destroy"))
            process.reset();
        else
        {
            if (mode == QStringLiteral("success"))
            {
                QFile file(release);
                QVERIFY(file.open(QIODevice::WriteOnly));
                file.close();
            }
            else
                process->cancel(); // Close stdin and let the helper exit normally.
            QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 5000);
            const auto state = mode == QStringLiteral("success") ? rufus::PrepareImageProcess::State::Complete
                : mode == QStringLiteral("error") ? rufus::PrepareImageProcess::State::Failed
                : rufus::PrepareImageProcess::State::Cancelled;
            QCOMPARE(process->state(), state);
        }
        QVERIFY(!QFileInfo::exists(path));
        QVERIFY(!QFileInfo::exists(QFileInfo(path).absolutePath()));
        QCOMPARE(launches, 1);
    }

    void stagingRejection()
    {
        const QDir temporary(QDir::tempPath());
        const QStringList before = temporary.entryList({QStringLiteral("rufus-grub2-*")}, QDir::Dirs | QDir::NoDotAndDotDot);
        rufus::PrepareImageProcess process(QStringLiteral("/unused-helper"));
        const auto request = grub2Request();
        QVERIFY(!process.start(request, QByteArray(511, 'c')));
        QVERIFY(!process.start(request, QByteArray(1048065, 'c')));
        auto ambiguous = request; ambiguous.grub2CorePath = QStringLiteral("/existing/core.img");
        QVERIFY(!process.start(ambiguous, QByteArray(512, 'c')));
        auto nonGrub = request; nonGrub.grub2 = false; nonGrub.nonBootable = true;
        QVERIFY(!process.start(nonGrub, QByteArray(512, 'c')));
        auto incompatible = request; incompatible.imagePath = QStringLiteral("/tmp/source.iso");
        QVERIFY(!process.start(incompatible, QByteArray(512, 'c')));
        QCOMPARE(launches, 0);
        QCOMPARE(temporary.entryList({QStringLiteral("rufus-grub2-*")}, QDir::Dirs | QDir::NoDotAndDotDot), before);
    }

    void stagingLaunchFailure_data()
    {
        QTest::addColumn<int>("exitCode");
        QTest::newRow("failed-start") << -1;
        QTest::newRow("dismissed-authorization") << 126;
        QTest::newRow("denied-authorization") << 127;
    }

    void stagingLaunchFailure()
    {
        QFETCH(int, exitCode);
        const QDir temporary(QDir::tempPath());
        const QStringList before = temporary.entryList({QStringLiteral("rufus-grub2-*")}, QDir::Dirs | QDir::NoDotAndDotDot);
        fixturePath = directory_.filePath(QStringLiteral("launch-failure"));
        QFile::remove(fixturePath);
        if (exitCode >= 0)
        {
            QFile helper(fixturePath);
            QVERIFY(helper.open(QIODevice::WriteOnly));
            const QByteArray code = "#!/usr/bin/python3\nimport sys\nsys.exit(" + QByteArray::number(exitCode) + ")\n";
            QCOMPARE(helper.write(code), code.size());
            QVERIFY(helper.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        }
        rufus::PrepareImageProcess process(fixturePath);
        QSignalSpy done(&process, &rufus::PrepareImageProcess::finished);
        QVERIFY(process.start(grub2Request(), QByteArray(512, 'c')));
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 5000);
        const auto expected = exitCode == 126 ? rufus::PrepareImageProcess::State::Cancelled
            : rufus::PrepareImageProcess::State::Failed;
        QCOMPARE(process.state(), expected);
        QVERIFY(!process.outcome().started);
        QCOMPARE(launches, 1);
        QCOMPARE(temporary.entryList({QStringLiteral("rufus-grub2-*")}, QDir::Dirs | QDir::NoDotAndDotDot), before);
    }

    void response_data()
    {
        QTest::addColumn<QString>("mode");
        for (const char* mode : {"success", "skip-boot", "skip-finalize", "copy-step", "counts", "mount", "copied-bytes",
                "sync-error", "cleanup-error", "wrong-exit", "missing-result"})
            QTest::newRow(mode) << QString::fromLatin1(mode);
    }

    void response()
    {
        QFETCH(QString, mode);
        const auto request = grub2Request();
        auto frames = completeFrames();
        if (mode == QStringLiteral("skip-boot")) frames.removeAt(3);
        if (mode == QStringLiteral("skip-finalize")) frames.removeAt(4);
        if (mode == QStringLiteral("copy-step")) frames.insert(4, {{"event", "step"}, {"operation", OP_FILE_COPY}});
        if (mode == QStringLiteral("counts")) frames.insert(4, rufus::EncodePrepareProgress(OP_FILE_COPY, 1, 2));
        if (mode == QStringLiteral("mount")) frames.insert(3, {{"event", "phase"}, {"phase", "mounting"}});
        if (mode == QStringLiteral("copied-bytes")) frames.last().insert("copiedBytes", QStringLiteral("1"));
        if (mode == QStringLiteral("sync-error")) frames.last().insert("syncError", EIO);
        if (mode == QStringLiteral("cleanup-error")) frames.last().insert("cleanupError", EIO);
        if (mode == QStringLiteral("missing-result")) frames.removeLast();
        writeHelper(request, frames, mode == QStringLiteral("wrong-exit") ? "sys.exit(1)\n" : QByteArray());
        rufus::PrepareImageProcess process(fixturePath);
        QSignalSpy done(&process, &rufus::PrepareImageProcess::finished);
        QSignalSpy steps(&process, &rufus::PrepareImageProcess::stepChanged);
        QVERIFY(process.start(request));
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 5000);
        const auto expected = mode == QStringLiteral("success") ? rufus::PrepareImageProcess::State::Complete
            : rufus::PrepareImageProcess::State::Failed;
        QCOMPARE(process.state(), expected);
        QCOMPARE(launches, 1);
        QCOMPARE(steps.size(), 0);
    }

    void imageResponse_data()
    {
        QTest::addColumn<QString>("mode");
        QTest::addColumn<bool>("uefi");
        for (bool uefi : {false, true})
        for (const char* mode : {"success", "zero-copy", "skip-boot", "skip-mount", "skip-extract", "skip-finalize",
                "sync-error", "cleanup-error", "wrong-exit", "missing-result", "duplicate-boot"})
        {
            const QByteArray name = QByteArray(uefi ? "uefi-" : "bios-") + mode;
            QTest::newRow(name.constData()) << QString::fromLatin1(mode) << uefi;
        }
    }

    void imageResponse()
    {
        QFETCH(QString, mode);
        QFETCH(bool, uefi);
        auto request = grub2ImageRequest();
        if (uefi)
        {
            request.uefiNtfs = true;
            request.fileSystem = FS_NTFS;
            request.clusterSize = 4096;
            request.volumeId = 0;
        }
        auto frames = completeFrames(true);
        GPT_IMAGE_RESULT partition = {};
        QCOMPARE(rufus::PlanFormatPartition(request, partition), 0);
        frames.last().insert("size", QString::number(partition.size));
        frames.last().insert("filesystemBytes", QString::number(partition.size));
        if (mode == QStringLiteral("zero-copy")) frames.last().insert("copiedBytes", QStringLiteral("0"));
        if (mode == QStringLiteral("skip-boot")) frames.removeAt(3);
        if (mode == QStringLiteral("skip-mount")) frames.removeAt(4);
        if (mode == QStringLiteral("skip-extract")) frames.removeAt(5);
        if (mode == QStringLiteral("skip-finalize")) frames.removeAt(6);
        if (mode == QStringLiteral("sync-error")) frames.last().insert("syncError", EIO);
        if (mode == QStringLiteral("cleanup-error")) frames.last().insert("cleanupError", EBUSY);
        if (mode == QStringLiteral("missing-result")) frames.removeLast();
        if (mode == QStringLiteral("duplicate-boot")) frames.insert(4, frames[3]);
        const QByteArray core(700, 'c');
        writeHelper(request, frames, mode == QStringLiteral("wrong-exit") ? "sys.exit(1)\n" : QByteArray(), core);
        rufus::PrepareImageProcess process(fixturePath);
        QSignalSpy done(&process, &rufus::PrepareImageProcess::finished);
        QVERIFY(process.start(request, core));
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 5000);
        const auto expected = mode == QStringLiteral("success") ? rufus::PrepareImageProcess::State::Complete
            : rufus::PrepareImageProcess::State::Failed;
        QCOMPARE(process.state(), expected);
        QCOMPARE(launches, 1);
        QFile marker(directory_.filePath(QStringLiteral("staged-path")));
        QVERIFY(marker.open(QIODevice::ReadOnly));
        const QString staged = QString::fromUtf8(marker.readAll());
        QVERIFY(!staged.isEmpty());
        QVERIFY(!QFileInfo::exists(staged));
        QVERIFY(!QFileInfo::exists(QFileInfo(staged).path()));
    }

    void waitsForExit_data()
    {
        QTest::addColumn<bool>("image");
        QTest::addColumn<bool>("uefi");
        QTest::newRow("standalone") << false << false;
        QTest::newRow("iso") << true << false;
        QTest::newRow("iso-uefi") << true << true;
    }

    void waitsForExit()
    {
        QFETCH(bool, image);
        QFETCH(bool, uefi);
        auto request = image ? grub2ImageRequest() : grub2Request();
        if (uefi)
        {
            request.uefiNtfs = true; request.fileSystem = FS_NTFS;
            request.clusterSize = 4096; request.volumeId = 0;
        }
        const QString marker = directory_.filePath(QStringLiteral("reported-%1").arg(fatBits));
        QFile::remove(marker);
        const QByteArray ending = "Path(bytes.fromhex('" + QFile::encodeName(marker).toHex() + "').decode()).touch()\n"
            "sys.stdin.read()\n";
        writeHelper(request, completeFrames(image), ending);
        rufus::PrepareImageProcess process(fixturePath);
        QSignalSpy done(&process, &rufus::PrepareImageProcess::finished);
        QVERIFY(process.start(request));
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(marker), 5000);
        QVERIFY(process.isActive());
        QCOMPARE(done.size(), 0);
        // Even cancellation after a success frame still waits for child exit.
        process.cancel();
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 5000);
        QCOMPARE(process.state(), rufus::PrepareImageProcess::State::Cancelled);
        QCOMPARE(launches, 1);
    }

    void cancellation_data()
    {
        waitsForExit_data();
    }

    void cancellation()
    {
        QFETCH(bool, image);
        QFETCH(bool, uefi);
        auto request = image ? grub2ImageRequest() : grub2Request();
        if (uefi)
        {
            request.uefiNtfs = true; request.fileSystem = FS_NTFS;
            request.clusterSize = 4096; request.volumeId = 0;
        }
        rufus::PrepareImageOutcome outcome;
        outcome.error = ECANCELED;
        outcome.started = true;
        const auto result = QJsonDocument(rufus::EncodePrepareOutcome(outcome)).toJson(QJsonDocument::Compact);
        writeHelper(request, completeFrames().mid(0, 4),
            "sys.stdin.read()\nprint(bytes.fromhex('" + result.toHex() + "').decode(), flush=True)\nsys.exit(1)\n");
        rufus::PrepareImageProcess process(fixturePath);
        QSignalSpy done(&process, &rufus::PrepareImageProcess::finished);
        QSignalSpy phases(&process, &rufus::PrepareImageProcess::phaseChanged);
        QVERIFY(process.start(request));
        QTRY_COMPARE_WITH_TIMEOUT(phases.size(), 4, 5000);
        process.cancel();
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 5000);
        QCOMPARE(process.state(), rufus::PrepareImageProcess::State::Cancelled);
        QCOMPARE(launches, 1);
    }
};

QTEST_GUILESS_MAIN(TestGrub2Request)
#include "test_grub2_request.moc"
