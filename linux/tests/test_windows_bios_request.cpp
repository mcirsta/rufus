// SPDX-License-Identifier: GPL-3.0-or-later
#include "qt/prepare_image_process.h"
#include "platform/format_options.h"
#include "platform/prepare_source.h"
#include "unattend.h"
#include <QFile>
#include <QJsonDocument>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QTest>
#include <QSignalSpy>
#include <cerrno>
#include <unistd.h>

namespace
{
constexpr quint64 mib = 1024 * 1024;
QString processMode;
int authorizationLaunches = 0;

rufus::PrepareImageRequest windowsRequest()
{
    rufus::PrepareImageRequest request;
    request.windowsBios = request.mbr = request.isDevice = true;
    request.path = QStringLiteral("/dev/fixture");
    request.device = 123;
    request.size = 64 * mib;
    request.fileSystem = FS_NTFS;
    request.clusterSize = 4096;
    request.label = QStringLiteral("WINDOWS");
    request.diskSequence = 7;
    request.objectPath = QStringLiteral("/org/freedesktop/UDisks2/block_devices/fixture");
    request.serviceOwner = QStringLiteral(":1.2");
    request.identity = {QStringLiteral("fixture")};
    request.imagePath = QStringLiteral("/tmp/selected.iso");
    request.imageDevice = quint64(1) << 54;
    request.imageInode = (quint64(1) << 54) + 1;
    request.imageSize = mib;
    request.imageModified = QStringLiteral("100.000000001");
    request.imageChanged = QStringLiteral("101.000000002");
    return request;
}
}

// Only the process-lifecycle cases redirect authorization to the existing
// protocol fixture. Direct probe launches and production pkexec stay unchanged.
extern "C" void __real__ZN8QProcess5startERK7QStringRK5QListIS0_E6QFlagsIN13QIODeviceBase12OpenModeFlagEE(
    QProcess*, const QString&, const QStringList&, QIODevice::OpenMode);
extern "C" void __wrap__ZN8QProcess5startERK7QStringRK5QListIS0_E6QFlagsIN13QIODeviceBase12OpenModeFlagEE(
    QProcess* process, const QString& program, const QStringList& arguments, QIODevice::OpenMode mode)
{
    if (program != QStringLiteral("/usr/bin/pkexec"))
    {
        __real__ZN8QProcess5startERK7QStringRK5QListIS0_E6QFlagsIN13QIODeviceBase12OpenModeFlagEE(process, program, arguments, mode);
        return;
    }
    const QString fixture = QStringLiteral(RUFUS_ISO_HELPER_FIXTURE);
    const QStringList expected = {QStringLiteral("--disable-internal-agent"), fixture, QStringLiteral("--device")};
    if (arguments != expected || processMode.isEmpty()) qFatal("Unexpected Windows fixture authorization launch");
    ++authorizationLaunches;
    const QString copied = processMode == QStringLiteral("zero-copy") ? QStringLiteral("0") : QStringLiteral("4096");
    __real__ZN8QProcess5startERK7QStringRK5QListIS0_E6QFlagsIN13QIODeviceBase12OpenModeFlagEE(
        process, fixture, {processMode, copied}, mode);
}

class TestWindowsBiosRequest : public QObject
{
    Q_OBJECT
private slots:
    void roundTrip_data()
    {
        QTest::addColumn<bool>("dual");
        QTest::addColumn<bool>("enhanced");
        QTest::addColumn<bool>("rufusMbr");
        QTest::addColumn<bool>("customized");
        QTest::addColumn<int>("fileSystem");
        for (int fs : {FS_FAT16, FS_FAT32, FS_NTFS})
        for (bool rufusMbr : {false, true})
        for (bool customized : {false, true})
        for (int mode : {0, 1, 2})
        {
            if (fs != FS_NTFS && mode == 2) continue; // EFI uses the same FAT volume.
            const QByteArray name = QByteArray(fs == FS_FAT16 ? "fat16-" : fs == FS_FAT32 ? "fat32-" : "ntfs-")
                + (rufusMbr ? "rufus-" : "win7-") + QByteArray::number(mode) + (customized ? "-settings" : "-ordinary");
            QTest::newRow(name.constData()) << (mode == 2) << (mode != 0) << rufusMbr << customized << fs;
        }
    }

    void roundTrip()
    {
        QFETCH(bool, dual); QFETCH(bool, enhanced); QFETCH(bool, rufusMbr); QFETCH(bool, customized);
        QFETCH(int, fileSystem);
        auto request = windowsRequest();
        request.fileSystem = fileSystem;
        request.clusterSize = fileSystem == FS_FAT16 ? 2048 : fileSystem == FS_FAT32 ? 512 : 4096;
        request.volumeId = fileSystem != FS_NTFS ? 0x1234abcd : 0;
        request.uefiNtfs = dual;
        request.enhancedInstall = enhanced;
        request.windowsRufusMbr = rufusMbr;
        request.allowFixedUsb = true;
        request.rockRidge = request.joliet = false;
        request.windowsFlags = customized ? UNATTEND_NO_DATA_COLLECTION : 0;
        const QJsonObject encoded = rufus::EncodePrepareRequest(request);
        QVERIFY(!encoded.isEmpty());
        const char* operation = fileSystem == FS_FAT16 ? "prepare-windows-fat16-bios-device-v1" :
            fileSystem == FS_FAT32 ? "prepare-windows-fat32-bios-device-v1" :
            dual ? "prepare-windows-ntfs-dual-device-v1" : "prepare-windows-ntfs-bios-device-v1";
        QCOMPARE(encoded.value("operation"), QJsonValue(operation));
        QCOMPARE(encoded.value("rufusMbr"), QJsonValue(rufusMbr));
        rufus::PrepareImageRequest decoded;
        QVERIFY(rufus::DecodePrepareRequest(encoded, decoded));
        QVERIFY(decoded.windowsBios && decoded.mbr && decoded.isDevice && decoded.allowFixedUsb);
        QVERIFY(!decoded.isSourceFree() && !decoded.isStandaloneUefiNtfs());
        QVERIFY(!decoded.syslinux && !decoded.grub2 && !decoded.freeDos && !decoded.nonBootable);
        QCOMPARE(decoded.usesUefiNtfs(), dual);
        QCOMPARE(decoded.enhancedInstall, enhanced);
        QCOMPARE(decoded.windowsRufusMbr, rufusMbr);
        QCOMPARE(decoded.imagePath, request.imagePath);
        QCOMPARE(decoded.imageDevice, request.imageDevice);
        QCOMPARE(decoded.imageInode, request.imageInode);
        QCOMPARE(decoded.imageSize, request.imageSize);
        QCOMPARE(decoded.imageModified, request.imageModified);
        QCOMPARE(decoded.imageChanged, request.imageChanged);
        QCOMPARE(decoded.windowsFlags, request.windowsFlags);
        QCOMPARE(decoded.fileSystem, fileSystem);
        QCOMPARE(decoded.volumeId, request.volumeId);
        QCOMPARE(decoded.clusterSize, request.clusterSize);
        QVERIFY(!decoded.rockRidge && !decoded.joliet);
        QCOMPARE(rufus::EncodePrepareRequest(decoded), encoded);
        QCOMPARE(rufus::FormatMbrBootCode(decoded), rufusMbr ? MBR_BOOT_RUFUS : MBR_BOOT_WIN7);
        GPT_IMAGE_RESULT layout = {};
        QCOMPARE(rufus::PlanFormatPartition(decoded, layout), 0);
        QCOMPARE(layout.offset, mib);
        QCOMPARE(layout.size, quint64(dual ? 64962560 : 66060288));
        QCOMPARE(layout.uefi_ntfs_offset, quint64(dual ? 66028032 : 0));
        QCOMPARE(layout.uefi_ntfs_size, quint64(dual ? 1064448 : 0));
        QCOMPARE(rufus::CheckFormatSettings(decoded, layout), 0);
    }

    void grub2SbrRequest_data()
    {
        QTest::addColumn<int>("filesystem");
        QTest::newRow("FAT16") << int(FS_FAT16);
        QTest::newRow("NTFS") << int(FS_NTFS);
        QTest::newRow("FAT32") << int(FS_FAT32);
    }

    void grub2SbrRequest()
    {
        QFETCH(int, filesystem);
        auto request = windowsRequest();
        request.enhancedInstall = true;
        request.fileSystem = filesystem;
        request.clusterSize = filesystem == FS_FAT16 ? 2048 : filesystem == FS_FAT32 ? 512 : 4096;
        request.volumeId = filesystem != FS_NTFS ? 0x1234abcd : 0;
        request.grub2CorePath = QStringLiteral("/tmp/selected-core.img");
        request.grub2CoreSize = 700;
        request.grub2CoreSha256 = QByteArray(32, 'x');
        for (bool rufusMbr : {false, true})
        {
            request.windowsRufusMbr = rufusMbr;
            const QJsonObject encoded = rufus::EncodePrepareRequest(request);
            QVERIFY(!encoded.isEmpty());
            const char* operation = filesystem == FS_FAT16 ? "prepare-windows-fat16-bios-device-v1"
                : filesystem == FS_FAT32 ? "prepare-windows-fat32-bios-device-v1"
                : "prepare-windows-ntfs-bios-device-v1";
            QCOMPARE(encoded.value("operation"), QJsonValue(operation));
            rufus::PrepareImageRequest decoded;
            QVERIFY(rufus::DecodePrepareRequest(encoded, decoded));
            QVERIFY(decoded.windowsBios && !decoded.grub2 && decoded.hasGrub2Core());
            QCOMPARE(decoded.grub2CorePath, request.grub2CorePath);
            QCOMPARE(decoded.grub2CoreSize, request.grub2CoreSize);
            QCOMPARE(decoded.grub2CoreSha256, request.grub2CoreSha256);
            QCOMPARE(rufus::EncodePrepareRequest(decoded), encoded);
            QCOMPARE(rufus::FormatMbrBootCode(decoded), rufusMbr ? MBR_BOOT_RUFUS : MBR_BOOT_WIN7);
        }
        for (const char* change : {"alt-e-off", "uefi", "hash", "relative", "oversize", "customized"})
        {
            auto invalid = request;
            const QString mode = QString::fromLatin1(change);
            if (mode == QStringLiteral("alt-e-off")) invalid.enhancedInstall = false;
            if (mode == QStringLiteral("uefi")) invalid.uefiNtfs = true;
            if (mode == QStringLiteral("hash")) invalid.grub2CoreSha256.clear();
            if (mode == QStringLiteral("relative")) invalid.grub2CorePath = QStringLiteral("relative.img");
            if (mode == QStringLiteral("oversize")) invalid.grub2CoreSize = rufus::PrepareCoreMaximumBytes + 1;
            if (mode == QStringLiteral("customized")) invalid.windowsFlags = UNATTEND_NO_DATA_COLLECTION;
            QVERIFY(rufus::EncodePrepareRequest(invalid).isEmpty());
        }
        QJsonObject forged = rufus::EncodePrepareRequest(request);
        forged.remove("enhancedInstall");
        rufus::PrepareImageRequest decoded;
        QVERIFY(!rufus::DecodePrepareRequest(forged, decoded));
        forged = rufus::EncodePrepareRequest(request);
        forged.insert("windows", QJsonObject{{"flags", QString::number(UNATTEND_NO_DATA_COLLECTION)}, {"username", ""}});
        QVERIFY(!rufus::DecodePrepareRequest(forged, decoded));
    }

    void invalidSettings()
    {
        const auto good = windowsRequest();
        for (bool rufusMbr : {false, true})
        {
            auto request = good;
            request.windowsRufusMbr = rufusMbr;
            for (bool rufus::PrepareImageRequest::* field : {&rufus::PrepareImageRequest::rawWrite,
                    &rufus::PrepareImageRequest::nonBootable, &rufus::PrepareImageRequest::sfd,
                    &rufus::PrepareImageRequest::syslinux, &rufus::PrepareImageRequest::grub2,
                    &rufus::PrepareImageRequest::freeDos})
            {
                auto changed = request;
                changed.*field = true;
                QVERIFY(rufus::EncodePrepareRequest(changed).isEmpty());
            }
            for (int fs : {FS_EXFAT, FS_EXT2, FS_EXT3})
            {
                auto changed = request;
                changed.fileSystem = fs;
                QVERIFY(rufus::EncodePrepareRequest(changed).isEmpty());
            }
            request = good; request.isDevice = false;
            QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
            request = good; request.mbr = false;
            QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
            request = good; request.uefiNtfs = true;
            QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
            request = good; request.imagePath.clear();
            QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
            request = good; request.imagePath = QStringLiteral("relative.iso");
            QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
            request = good; request.imageInode = 0;
            QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
            request = good; request.imageSize = 0;
            QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
            request = good; request.imageChanged.clear();
            QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
            request = good; request.dosKeyboard = QStringLiteral("us");
            QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
            request = good; request.volumeId = 1;
            QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
            request = good; request.fileSystem = FS_FAT32; request.uefiNtfs = true; request.enhancedInstall = true;
            QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        }
        auto request = good; request.windowsBios = false; request.mbr = false; request.windowsRufusMbr = false;
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
    }

    void invalidPackets()
    {
        for (int fs : {FS_NTFS, FS_FAT32})
        for (bool dual : {false, true})
        {
            auto request = windowsRequest();
            request.enhancedInstall = request.uefiNtfs = dual;
            if (fs == FS_FAT32)
            {
                request.fileSystem = fs;
                request.clusterSize = 512;
                request.uefiNtfs = false;
            }
            const auto good = rufus::EncodePrepareRequest(request);
            rufus::PrepareImageRequest decoded;
            decoded.path = QStringLiteral("unchanged");
            for (const char* key : {"source", "rufusMbr", "device", "sequence", "identity"})
            {
                auto changed = good; changed.remove(key);
                QVERIFY(!rufus::DecodePrepareRequest(changed, decoded));
                QCOMPARE(decoded.path, QStringLiteral("unchanged"));
            }
            for (const QJsonValue& value : {QJsonValue("false"), QJsonValue(0), QJsonValue()})
            {
                auto changed = good; changed.insert("rufusMbr", value);
                QVERIFY(!rufus::DecodePrepareRequest(changed, decoded));
            }
            for (const char* operation : {"prepare-ntfs-device-v1", "prepare-grub2-ntfs-device-v1",
                    "prepare-uefi-ntfs-mbr-device-v1", "format-ntfs-mbr-device-v1", "prepare-windows-ntfs-bios-device-v2"})
            {
                auto changed = good; changed.insert("operation", operation);
                QVERIFY(!rufus::DecodePrepareRequest(changed, decoded));
            }
            for (const char* key : {"dos", "grubCore", "formatter", "uefiNtfs", "unexpected"})
            {
                auto changed = good; changed.insert(key, QJsonObject());
                QVERIFY(!rufus::DecodePrepareRequest(changed, decoded));
            }
            for (const char* key : {"inode", "size"})
            {
                auto changed = good;
                auto source = changed.value("source").toObject(); source.insert(key, "0");
                changed.insert("source", source);
                QVERIFY(!rufus::DecodePrepareRequest(changed, decoded));
            }
            if (dual && fs == FS_NTFS)
            {
                auto changed = good; changed.remove("enhancedInstall");
                QVERIFY(!rufus::DecodePrepareRequest(changed, decoded));
                changed = good; changed.insert("enhancedInstall", false);
                QVERIFY(!rufus::DecodePrepareRequest(changed, decoded));
            }
        }
    }

    void processLifecycle_data()
    {
        QTest::addColumn<bool>("dual");
        QTest::addColumn<bool>("rufusMbr");
        QTest::addColumn<QString>("mode");
        QTest::addColumn<int>("expected");
        QTest::addColumn<bool>("core");
        for (bool dual : {false, true})
        for (bool rufusMbr : {false, true})
        {
            const QByteArray prefix = QByteArray(rufusMbr ? "rufus-" : "win7-") + (dual ? "dual-" : "bios-");
            for (const char* mode : {"ok", "hold-after-result"})
                QTest::newRow((prefix + mode).constData()) << dual << rufusMbr << QString::fromLatin1(mode) << 0 << false;
            QTest::newRow((prefix + "sync-error").constData()) << dual << rufusMbr << QStringLiteral("sync-error") << EIO << false;
            QTest::newRow((prefix + "cleanup-error").constData()) << dual << rufusMbr << QStringLiteral("cleanup-error") << EBUSY << false;
            for (const char* mode : {"zero-copy", "skip-finalizing", "progress-wrong-phase"})
                QTest::newRow((prefix + mode).constData()) << dual << rufusMbr << QString::fromLatin1(mode) << EIO << false;
        }
        for (bool rufusMbr : {false, true})
        for (const char* mode : {"ok", "hold-after-result"})
            QTest::newRow(qPrintable(QStringLiteral("sbr-%1-%2").arg(rufusMbr).arg(QString::fromLatin1(mode))))
                << false << rufusMbr << QString::fromLatin1(mode) << 0 << true;
    }

    void processLifecycle()
    {
        QFETCH(bool, dual); QFETCH(bool, rufusMbr); QFETCH(QString, mode); QFETCH(int, expected);
        QFETCH(bool, core);
        auto request = windowsRequest();
        request.uefiNtfs = dual;
        request.enhancedInstall = dual || core;
        request.windowsRufusMbr = rufusMbr;
        processMode = mode;
        authorizationLaunches = 0;
        rufus::PrepareImageProcess process(QStringLiteral(RUFUS_ISO_HELPER_FIXTURE));
        QSignalSpy finished(&process, &rufus::PrepareImageProcess::finished);
        QSignalSpy phases(&process, &rufus::PrepareImageProcess::phaseChanged);
        QSignalSpy progress(&process, &rufus::PrepareImageProcess::progressChanged);
        QVERIFY(process.start(request, core ? QByteArray(700, '\x17') : QByteArray()));
        if (mode == QStringLiteral("hold-after-result"))
        {
            QTRY_COMPARE_WITH_TIMEOUT(phases.size(), 7, 5000);
            QTest::qWait(50); // Give the successful result time to be read.
            QVERIFY(process.isActive());
            QCOMPARE(finished.size(), 0);
            QCOMPARE(process.state(), rufus::PrepareImageProcess::State::Running);
            process.cancel(); // Release EOF; success must not precede child exit.
        }
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
        QCOMPARE(authorizationLaunches, 1);
        QVERIFY(!process.isActive());
        if (mode == QStringLiteral("hold-after-result"))
            QCOMPARE(process.state(), rufus::PrepareImageProcess::State::Cancelled);
        else
        {
            QCOMPARE(process.outcome().error, expected);
            QCOMPARE(process.state(), expected == 0 ? rufus::PrepareImageProcess::State::Complete
                : rufus::PrepareImageProcess::State::Failed);
        }
        if (expected == 0)
        {
            QStringList names;
            for (const QList<QVariant>& phase : phases) names.append(phase[0].toString());
            QCOMPARE(names, QStringList({"checking", "partitioning", "formatting", "bootloader", "mounting", "extracting", "finalizing"}));
            QCOMPARE(progress.size(), 3);
            QCOMPARE(progress.last()[1].toULongLong(), quint64(10));
            QCOMPARE(progress.last()[2].toULongLong(), quint64(10));
        }
    }

    void helperRefusesBeforeWrites_data()
    {
        QTest::addColumn<bool>("dual");
        QTest::addColumn<bool>("rufusMbr");
        QTest::addColumn<bool>("invalidSource");
        QTest::addColumn<bool>("fat32");
        for (bool fat32 : {false, true})
        for (bool invalidSource : {false, true})
        for (bool dual : {false, true})
        for (bool rufusMbr : {false, true})
        {
            const QByteArray name = QByteArray(fat32 ? "fat32-" : "ntfs-") + (rufusMbr ? "rufus-" : "win7-")
                + (dual ? "dual-" : "bios-") + (invalidSource ? "invalid-source" : "missing-source");
            QTest::newRow(name.constData()) << dual << rufusMbr << invalidSource << fat32;
        }
    }

    void helperRefusesBeforeWrites()
    {
        QFETCH(bool, dual); QFETCH(bool, rufusMbr); QFETCH(bool, invalidSource);
        QFETCH(bool, fat32);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        auto request = windowsRequest();
        request.enhancedInstall = request.uefiNtfs = dual;
        if (fat32)
        {
            request.fileSystem = FS_FAT32;
            request.clusterSize = 512;
            request.uefiNtfs = false;
        }
        request.windowsRufusMbr = rufusMbr;
        request.path = directory.filePath(QStringLiteral("target"));
        QFile target(request.path);
        QVERIFY(target.open(QIODevice::ReadWrite));
        QCOMPARE(target.write("untouched"), qint64(9));
        QVERIFY(target.flush());
        request.imagePath = directory.filePath(QStringLiteral("selected.iso"));
        if (invalidSource)
        {
            QFile source(request.imagePath);
            QVERIFY(source.open(QIODevice::WriteOnly));
            // Deliberately invalid ISO: fresh source preflight must reject it
            // before the test-only target gate, not just retain its file identity.
            QCOMPARE(source.write("source"), qint64(6));
            source.close();
            QCOMPARE(rufus::CapturePrepareSource(request.imagePath, request), 0);
        }
        QProcess helper;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("RUFUS_TEST_WINDOWS_BIOS_PREFLIGHT"), QStringLiteral("1"));
        environment.insert(QStringLiteral("PKEXEC_UID"), QString::number(getuid()));
        helper.setProcessEnvironment(environment);
        helper.start(QStringLiteral(RUFUS_PREPARE_PROBE), {QStringLiteral("--device")});
        QVERIFY(helper.waitForStarted());
        QByteArray output;
        while (!output.contains('\n'))
        {
            QVERIFY(helper.waitForReadyRead(5000));
            output += helper.readAllStandardOutput();
        }
        QCOMPARE(output, QByteArray("{\"event\":\"ready\"}\n"));
        const QByteArray encoded = QJsonDocument(rufus::EncodePrepareRequest(request)).toJson(QJsonDocument::Compact) + '\n';
        QCOMPARE(helper.write(encoded), encoded.size());
        QVERIFY(helper.waitForFinished(5000));
        QCOMPARE(helper.exitCode(), 1);
        rufus::PrepareImageOutcome outcome;
        bool result = false;
        for (const QByteArray& line : helper.readAllStandardOutput().split('\n'))
        {
            const auto frame = QJsonDocument::fromJson(line).object();
            if (frame.value("event") == "result") result = rufus::DecodePrepareOutcome(frame, outcome);
        }
        QVERIFY(result);
        QCOMPARE(outcome.error, invalidSource ? EIO : ENOENT);
        QVERIFY(!outcome.started);
        QCOMPARE(outcome.copiedBytes, quint64(0));
        QCOMPARE(helper.readAllStandardError().contains("RUFUS_TEST_TARGET_GATE"), false);
        QVERIFY(target.seek(0));
        QCOMPARE(target.readAll(), QByteArray("untouched"));
    }
};

QTEST_GUILESS_MAIN(TestWindowsBiosRequest)
#include "test_windows_bios_request.moc"
