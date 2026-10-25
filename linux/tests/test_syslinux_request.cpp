// SPDX-License-Identifier: GPL-3.0-or-later
#include "qt/prepare_image_process.h"
#include "platform/format_options.h"
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <cerrno>

namespace
{
QString fixturePath;
int launches = 0;
int selection = BT_SYSLINUX_V6;
bool ntfs = false;
constexpr quint64 mib = 1024 * 1024;

rufus::PrepareImageRequest syslinuxRequest()
{
    rufus::PrepareImageRequest request;
    request.syslinux = request.isDevice = true;
    request.syslinuxSelection = selection;
    request.path = QStringLiteral("/dev/fixture");
    request.device = 123;
    request.size = 64 * mib;
    request.volumeId = ntfs ? 0 : 0x12345678;
    request.fileSystem = ntfs ? FS_NTFS : FS_FAT32;
    request.clusterSize = ntfs ? 4096 : 512;
    request.label = QStringLiteral("SYSLINUX");
    request.diskSequence = 7;
    request.objectPath = QStringLiteral("/org/freedesktop/UDisks2/block_devices/fixture");
    request.serviceOwner = QStringLiteral(":1.2");
    request.identity = {QStringLiteral("fixture")};
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
        qFatal("Unexpected Syslinux helper launch");
    ++launches;
    __real__ZN8QProcess5startERK7QStringRK5QListIS0_E6QFlagsIN13QIODeviceBase12OpenModeFlagEE(
        process, fixturePath, {}, mode);
}

class TestSyslinuxRequest : public QObject
{
    Q_OBJECT
    QTemporaryDir directory_;

    void writeHelper(const rufus::PrepareImageRequest& request, const QList<QJsonObject>& frames,
        const QByteArray& ending = {})
    {
        fixturePath = directory_.filePath(QStringLiteral("helper"));
        const QByteArray expected = QJsonDocument(rufus::EncodePrepareRequest(request)).toJson(QJsonDocument::Compact);
        QByteArray code = "#!/usr/bin/python3\nimport json, sys, time, select\nfrom pathlib import Path\n"
            "print('{\"event\":\"ready\"}', flush=True)\n"
            "request = json.loads(sys.stdin.readline())\nassert request == json.loads(bytes.fromhex('" + expected.toHex() + "'))\n";
        for (const QJsonObject& frame : frames)
            code += "print(bytes.fromhex('" + QJsonDocument(frame).toJson(QJsonDocument::Compact).toHex() + "').decode(), flush=True)\n";
        code += ending;
        QFile helper(fixturePath);
        QVERIFY(helper.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(helper.write(code), code.size());
        QVERIFY(helper.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    }

    QList<QJsonObject> completeFrames() const
    {
        QList<QJsonObject> frames;
        for (const char* phase : {"checking", "partitioning", "formatting", "bootloader"})
            frames.append({{"event", "phase"}, {"phase", phase}});
        if (ntfs) frames.insert(3, {{"event", "phase"}, {"phase", "mounting"}});
        // Independent Windows oracle: three reserved slots, two emitted steps.
        for (int step = 0; step < 2; ++step)
            frames.append({{"event", "step"}, {"operation", OP_FILE_COPY}});
        frames.append({{"event", "phase"}, {"phase", "finalizing"}});
        rufus::PrepareImageOutcome outcome;
        outcome.started = true;
        outcome.offset = mib;
        outcome.size = outcome.filesystemBytes = 63 * mib;
        frames.append(rufus::EncodePrepareOutcome(outcome));
        return frames;
    }

private slots:
    void initTestCase_data()
    {
        QTest::addColumn<int>("bootType");
        QTest::addColumn<bool>("ntfsMode");
        QTest::newRow("v4") << int(BT_SYSLINUX_V4) << false;
        QTest::newRow("v6") << int(BT_SYSLINUX_V6) << false;
        QTest::newRow("v6-ntfs") << int(BT_SYSLINUX_V6) << true;
    }

    void init()
    {
        QVERIFY(directory_.isValid());
        launches = 0;
        QFETCH_GLOBAL(int, bootType);
        selection = bootType;
        QFETCH_GLOBAL(bool, ntfsMode);
        ntfs = ntfsMode;
    }

    void roundTrip_data()
    {
        QTest::addColumn<int>("fileSystem");
        for (int bits : {16, 32})
            QTest::newRow(qPrintable(QStringLiteral("FAT%1").arg(bits))) << int(bits == 16 ? FS_FAT16 : FS_FAT32);
    }

    void roundTrip()
    {
        QFETCH(int, fileSystem);
        auto request = syslinuxRequest();
        if (ntfs) fileSystem = FS_NTFS;
        request.fileSystem = fileSystem;
        request.clusterSize = fileSystem == FS_NTFS ? 4096 : fileSystem == FS_FAT16 ? 2048 : 512;
        request.allowFixedUsb = true;
        const QJsonObject encoded = rufus::EncodePrepareRequest(request);
        QCOMPARE(encoded.size(), 12);
        const QString operation = ntfs ? QStringLiteral("prepare-syslinux6-ntfs-device-v1") : QStringLiteral("prepare-syslinux%1-fat%2-device-v1")
            .arg(selection == BT_SYSLINUX_V4 ? 4 : 6).arg(fileSystem == FS_FAT16 ? 16 : 32);
        QCOMPARE(encoded.value("operation"), QJsonValue(operation));
        QVERIFY(!encoded.contains("source"));
        rufus::PrepareImageRequest decoded;
        QVERIFY(rufus::DecodePrepareRequest(encoded, decoded));
        QVERIFY(decoded.syslinux && decoded.isDevice && decoded.allowFixedUsb);
        QVERIFY(!decoded.freeDos && !decoded.nonBootable && !decoded.sfd && !decoded.mbr && !decoded.usesUefiNtfs());
        QCOMPARE(decoded.syslinuxSelection, selection);
        QCOMPARE(decoded.fileSystem, fileSystem);
        QCOMPARE(rufus::EncodePrepareRequest(decoded), encoded);
        GPT_IMAGE_RESULT partition;
        QCOMPARE(rufus::PlanFormatPartition(decoded, partition), 0);
        QCOMPARE(partition.offset, mib);
        QCOMPARE(partition.uefi_ntfs_size, quint64(0));
        QCOMPARE(rufus::FormatMbrBootCode(decoded), MBR_BOOT_SYSLINUX);
        QCOMPARE(rufus::CheckFormatSettings(decoded, partition), 0);
        QVERIFY(QJsonDocument(encoded).toJson(QJsonDocument::Compact).size() < rufus::PrepareMessageBytes);
        RUFUS_PROGRESS_OPTIONS options = {};
        options.bootable = true;
        options.selection = selection;
        RUFUS_PROGRESS progress;
        InitProgressState(&progress, &options);
        QCOMPARE(progress.nb_slots[OP_FILE_COPY], 3);
    }

    void incompatibleOptions()
    {
        const auto good = syslinuxRequest();
        for (bool rufus::PrepareImageRequest::* flag : {&rufus::PrepareImageRequest::rawWrite,
                &rufus::PrepareImageRequest::nonBootable, &rufus::PrepareImageRequest::freeDos,
                &rufus::PrepareImageRequest::mbr, &rufus::PrepareImageRequest::sfd, &rufus::PrepareImageRequest::uefiNtfs,
                &rufus::PrepareImageRequest::enhancedInstall, &rufus::PrepareImageRequest::windowsSilentConfirmed})
        {
            auto request = good;
            request.*flag = true;
            QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        }
        auto request = good; request.isDevice = false;
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        request = good; request.syslinux = false;
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
        request = good; request.fileSystem = FS_NTFS; request.volumeId = 0;
        request.syslinuxSelection = BT_SYSLINUX_V4;
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        request.syslinuxSelection = BT_IMAGE;
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        const int selections[] = {BT_IMAGE, BT_GRUB2, BT_NON_BOOTABLE, BT_MAX, -1};
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
        const QJsonObject good = rufus::EncodePrepareRequest(syslinuxRequest());
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
        for (const char* operation : {"prepare-syslinux-device-v1", "prepare-fat16-syslinux-device-v1",
                "prepare-syslinux5-fat16-device-v1", "prepare-syslinux4-ntfs-device-v1", "prepare-syslinux5-ntfs-device-v1", "prepare-freedos-fat32-device-v1"})
        {
            QJsonObject changed = good;
            changed.insert("operation", operation);
            QVERIFY(!rufus::DecodePrepareRequest(changed, decoded));
        }
    }

    void response_data()
    {
        QTest::addColumn<QString>("mode");
        for (const char* mode : {"success", "skip-boot", "skip-finalize", "short-copy", "extra-step", "early-step",
                "late-step", "wrong-operation", "fractional-operation", "step-fields", "counts", "mount", "copied-bytes",
                "sync-error", "cleanup-error", "wrong-exit", "missing-result"})
            QTest::newRow(mode) << QString::fromLatin1(mode);
    }

    void response()
    {
        QFETCH(QString, mode);
        const auto request = syslinuxRequest();
        auto frames = completeFrames();
        const int shift = ntfs ? 1 : 0;
        if (mode == QStringLiteral("skip-boot")) frames.removeAt(3 + shift);
        if (mode == QStringLiteral("skip-finalize")) frames.removeAt(6 + shift);
        if (mode == QStringLiteral("short-copy")) frames.removeAt(5 + shift);
        if (mode == QStringLiteral("extra-step")) frames.insert(5 + shift, frames[4 + shift]);
        if (mode == QStringLiteral("early-step")) frames.insert(0, frames[4 + shift]);
        if (mode == QStringLiteral("late-step")) frames.move(5 + shift, 6 + shift);
        if (mode == QStringLiteral("wrong-operation")) frames[4 + shift].insert("operation", OP_PATCH);
        if (mode == QStringLiteral("fractional-operation")) frames[4 + shift].insert("operation", double(OP_FILE_COPY) + .5);
        if (mode == QStringLiteral("step-fields")) frames[4 + shift].insert("unexpected", true);
        if (mode == QStringLiteral("counts")) frames[4 + shift] = rufus::EncodePrepareProgress(OP_FILE_COPY, 1, 2);
        if (mode == QStringLiteral("mount"))
        {
            if (ntfs) frames.removeAt(3);
            else frames.insert(3, {{"event", "phase"}, {"phase", "mounting"}});
        }
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
        if (mode == QStringLiteral("success"))
        {
            QCOMPARE(steps.size(), 2);
            for (const auto& step : steps) QCOMPARE(step[0].toInt(), int(OP_FILE_COPY));
        }
    }

    void waitsForExit()
    {
        const auto request = syslinuxRequest();
        const QString marker = directory_.filePath(QStringLiteral("reported-%1-%2").arg(selection).arg(ntfs));
        const QByteArray ending = "Path(bytes.fromhex('" + QFile::encodeName(marker).toHex() + "').decode()).touch()\n"
            "sys.stdin.read()\n";
        writeHelper(request, completeFrames(), ending);
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

    void cancellation()
    {
        const auto request = syslinuxRequest();
        rufus::PrepareImageOutcome outcome;
        outcome.error = ECANCELED;
        outcome.started = true;
        const auto result = QJsonDocument(rufus::EncodePrepareOutcome(outcome)).toJson(QJsonDocument::Compact);
        writeHelper(request, completeFrames().mid(0, ntfs ? 5 : 4),
            "sys.stdin.read()\nprint(bytes.fromhex('" + result.toHex() + "').decode(), flush=True)\nsys.exit(1)\n");
        rufus::PrepareImageProcess process(fixturePath);
        QSignalSpy done(&process, &rufus::PrepareImageProcess::finished);
        QSignalSpy phases(&process, &rufus::PrepareImageProcess::phaseChanged);
        QVERIFY(process.start(request));
        QTRY_COMPARE_WITH_TIMEOUT(phases.size(), ntfs ? 5 : 4, 5000);
        process.cancel();
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 5000);
        QCOMPARE(process.state(), rufus::PrepareImageProcess::State::Cancelled);
        QCOMPARE(launches, 1);
    }
};

QTEST_GUILESS_MAIN(TestSyslinuxRequest)
#include "test_syslinux_request.moc"
