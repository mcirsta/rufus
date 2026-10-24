// SPDX-License-Identifier: GPL-3.0-or-later
#include "qt/prepare_image_process.h"
#include "platform/format_options.h"
#include "dos_locale_policy.h"
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
bool nativeHelper = false;
int launches = 0;
constexpr quint64 mib = 1024 * 1024;

rufus::PrepareImageRequest dosRequest()
{
    rufus::PrepareImageRequest request;
    request.freeDos = request.mbr = request.isDevice = true;
    request.path = QStringLiteral("/dev/fixture");
    request.device = 123;
    request.size = 64 * mib;
    request.volumeId = 0x12345678;
    request.label = QStringLiteral("FREEDOS");
    request.diskSequence = 7;
    request.objectPath = QStringLiteral("/org/freedesktop/UDisks2/block_devices/fixture");
    request.serviceOwner = QStringLiteral(":1.2");
    request.identity = {QStringLiteral("fixture")};
    request.dosKeyboard = QStringLiteral("us");
    request.dosCodepage = 437;
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
        qFatal("Unexpected FreeDOS helper launch");
    ++launches;
    const QStringList childArguments = nativeHelper ? QStringList{QStringLiteral("--device")} : QStringList{};
    __real__ZN8QProcess5startERK7QStringRK5QListIS0_E6QFlagsIN13QIODeviceBase12OpenModeFlagEE(
        process, fixturePath, childArguments, mode);
}

class TestFreeDosRequest : public QObject
{
    Q_OBJECT
    QTemporaryDir directory_;
    const QByteArray originalCaller_ = qgetenv("PKEXEC_UID");

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
        for (const char* phase : {"checking", "partitioning", "formatting", "bootloader", "mounting", "extracting"})
            frames.append({{"event", "phase"}, {"phase", phase}});
        // Independent Windows FreeDOS expectation: one preceding slot plus
        // five milestones from ExtractFreeDOS. Do not derive this test oracle.
        for (int step = 0; step < 6; ++step)
            frames.append({{"event", "step"}, {"operation", OP_FILE_COPY}});
        frames.append({{"event", "phase"}, {"phase", "finalizing"}});
        rufus::PrepareImageOutcome outcome;
        outcome.started = true;
        outcome.offset = mib;
        outcome.size = outcome.filesystemBytes = 63 * mib;
        outcome.copiedBytes = 401000;
        frames.append(rufus::EncodePrepareOutcome(outcome));
        return frames;
    }

private slots:
    void init()
    {
        QVERIFY(directory_.isValid());
        nativeHelper = false;
        launches = 0;
    }

    void roundTrip_data()
    {
        QTest::addColumn<int>("fileSystem");
        QTest::addColumn<bool>("rufusMbr");
        for (int bits : {16, 32})
            for (bool rufusMbr : {true, false})
                QTest::newRow(qPrintable(QStringLiteral("FAT%1-%2").arg(bits).arg(rufusMbr ? "Rufus" : "Win7")))
                    << int(bits == 16 ? FS_FAT16 : FS_FAT32) << rufusMbr;
    }

    void roundTrip()
    {
        QFETCH(int, fileSystem);
        QFETCH(bool, rufusMbr);
        auto request = dosRequest();
        request.fileSystem = fileSystem;
        request.clusterSize = fileSystem == FS_FAT16 ? 2048 : 512;
        request.dosRufusMbr = rufusMbr;
        request.dosKeyboard = QStringLiteral("ro");
        request.dosCodepage = 852;
        request.allowFixedUsb = true;
        const QJsonObject encoded = rufus::EncodePrepareRequest(request);
        QCOMPARE(encoded.size(), 13);
        const QString operation = fileSystem == FS_FAT16 ? QStringLiteral("prepare-freedos-fat16-device-v1")
            : QStringLiteral("prepare-freedos-fat32-device-v1");
        QCOMPARE(encoded.value("operation"), QJsonValue(operation));
        QVERIFY(!encoded.contains("source"));
        rufus::PrepareImageRequest decoded;
        QVERIFY(rufus::DecodePrepareRequest(encoded, decoded));
        QVERIFY(decoded.freeDos && decoded.isDevice && decoded.mbr && decoded.allowFixedUsb);
        QVERIFY(!decoded.nonBootable && !decoded.syslinux && !decoded.sfd && !decoded.usesUefiNtfs());
        QCOMPARE(decoded.fileSystem, fileSystem);
        QCOMPARE(decoded.dosRufusMbr, rufusMbr);
        QCOMPARE(decoded.dosKeyboard, request.dosKeyboard);
        QCOMPARE(decoded.dosCodepage, request.dosCodepage);
        QCOMPARE(rufus::EncodePrepareRequest(decoded), encoded);
        GPT_IMAGE_RESULT partition;
        QCOMPARE(rufus::PlanFormatPartition(decoded, partition), 0);
        QCOMPARE(partition.offset, mib);
        QCOMPARE(partition.uefi_ntfs_size, quint64(0));
        QCOMPARE(rufus::CheckFormatSettings(decoded, partition), 0);
        QVERIFY(QJsonDocument(encoded).toJson(QJsonDocument::Compact).size() < rufus::PrepareMessageBytes);
        auto other = dosRequest();
        other.freeDos = false;
        other.nonBootable = true;
        other.dosKeyboard.clear();
        other.dosCodepage = 0;
        QVERIFY(rufus::DecodePrepareRequest(rufus::EncodePrepareRequest(other), decoded));
        QVERIFY(!decoded.freeDos && decoded.dosKeyboard.isEmpty());
        QCOMPARE(decoded.dosCodepage, quint32(0));
        QVERIFY(decoded.dosRufusMbr);
    }

    void catalogs()
    {
        auto request = dosRequest();
        rufus::PrepareImageRequest decoded;
        for (size_t i = 0; const char* keyboard = GetFreeDOSKeyboard(i); ++i)
        {
            request.dosKeyboard = QString::fromLatin1(keyboard);
            QVERIFY(rufus::DecodePrepareRequest(rufus::EncodePrepareRequest(request), decoded));
            QCOMPARE(decoded.dosKeyboard, request.dosKeyboard);
        }
        request.dosKeyboard = QStringLiteral("us");
        for (size_t i = 0; const uint32_t codepage = GetFreeDOSCodepage(i); ++i)
        {
            request.dosCodepage = codepage;
            QVERIFY(rufus::DecodePrepareRequest(rufus::EncodePrepareRequest(request), decoded));
            QCOMPARE(decoded.dosCodepage, codepage);
        }
    }

    void invalidChoices_data()
    {
        QTest::addColumn<QString>("keyboard");
        QTest::addColumn<quint32>("codepage");
        for (const QString& keyboard : {QString(), QStringLiteral("unknown"), QStringLiteral("US"),
                QStringLiteral("us "), QStringLiteral("ｕｓ"), QStringLiteral("us") + QChar::Null,
                QStringLiteral("../us"), QString(5000, QLatin1Char('a'))})
            QTest::newRow(qPrintable(QStringLiteral("keyboard-%1").arg(keyboard.left(20)))) << keyboard << quint32(437);
        for (quint32 codepage : {0u, 850u, 65001u, UINT32_MAX})
            QTest::newRow(qPrintable(QStringLiteral("codepage-%1").arg(codepage))) << QStringLiteral("us") << codepage;
    }

    void invalidChoices()
    {
        QFETCH(QString, keyboard);
        QFETCH(quint32, codepage);
        auto request = dosRequest();
        QJsonObject encoded = rufus::EncodePrepareRequest(request);
        request.dosKeyboard = keyboard;
        request.dosCodepage = codepage;
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        QJsonObject dos = encoded.value("dos").toObject();
        dos.insert("keyboard", keyboard);
        dos.insert("codepage", QString::number(codepage));
        encoded.insert("dos", dos);
        rufus::PrepareImageRequest decoded = dosRequest();
        QVERIFY(!rufus::DecodePrepareRequest(encoded, decoded));
        QCOMPARE(decoded.dosKeyboard, QStringLiteral("us"));
        QCOMPARE(decoded.dosCodepage, quint32(437));
        rufus::PrepareImageProcess process(QStringLiteral("/unused-helper"));
        QVERIFY(!process.start(request));
        QCOMPARE(launches, 0);
    }

    void incompatibleOptions()
    {
        const auto good = dosRequest();
        for (bool rufus::PrepareImageRequest::* flag : {&rufus::PrepareImageRequest::rawWrite,
                &rufus::PrepareImageRequest::nonBootable, &rufus::PrepareImageRequest::syslinux,
                &rufus::PrepareImageRequest::sfd, &rufus::PrepareImageRequest::uefiNtfs,
                &rufus::PrepareImageRequest::enhancedInstall, &rufus::PrepareImageRequest::windowsSilentConfirmed})
        {
            auto request = good;
            request.*flag = true;
            QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        }
        auto request = good; request.isDevice = false;
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        request = good; request.mbr = false;
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        request = good; request.imagePath = QStringLiteral("/tmp/source.iso");
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        request = good; request.windowsFlags = 1;
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        request = good; request.windowsUsername = QStringLiteral("user");
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        request = good; request.windowsEdition = 1;
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        for (int fileSystem : {FS_NTFS, FS_EXFAT, FS_UNKNOWN})
        {
            request = good; request.fileSystem = fileSystem;
            QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        }
        request = good; request.freeDos = false; request.nonBootable = true;
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        request.dosKeyboard.clear();
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        request.dosCodepage = 0; request.dosRufusMbr = false;
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        QCOMPARE(launches, 0);
    }

    void malformedMessages()
    {
        const QJsonObject good = rufus::EncodePrepareRequest(dosRequest());
        const QJsonObject settings = good.value("dos").toObject();
        rufus::PrepareImageRequest decoded;
        for (const char* field : {"keyboard", "codepage", "rufusMbr"})
        {
            for (const QJsonValue& value : {QJsonValue(), QJsonValue(1), QJsonValue(QJsonValue::Undefined)})
            {
                QJsonObject changed = good, dos = settings;
                dos.insert(QLatin1String(field), value);
                changed.insert("dos", dos);
                QVERIFY(!rufus::DecodePrepareRequest(changed, decoded));
            }
        }
        for (const char* codepage : {"0437", "437.0", "+437", " 437", "4294967733", "18446744073709551616"})
        {
            QJsonObject changed = good, dos = settings;
            dos.insert("codepage", QLatin1String(codepage));
            changed.insert("dos", dos);
            QVERIFY(!rufus::DecodePrepareRequest(changed, decoded));
        }
        for (const char* field : {"source", "windows", "enhancedInstall", "command", "formatter", "inode"})
        {
            QJsonObject changed = good;
            changed.insert(QLatin1String(field), QJsonObject());
            QVERIFY(!rufus::DecodePrepareRequest(changed, decoded));
        }
        for (const QString& field : settings.keys())
        {
            QJsonObject changed = good, dos = settings;
            dos.remove(field); dos.insert("unexpected", true); // Keep the field count.
            changed.insert("dos", dos);
            QVERIFY(!rufus::DecodePrepareRequest(changed, decoded));
        }
        for (const char* operation : {"prepare-device-v1", "prepare-syslinux-device-v1", "format-fat32-mbr-device-v1", "raw-write-device-v1"})
        {
            QJsonObject changed = good;
            changed.insert("operation", operation);
            QVERIFY(!rufus::DecodePrepareRequest(changed, decoded));
        }
        QJsonObject changed = good;
        changed.remove("dos");
        QVERIFY(!rufus::DecodePrepareRequest(changed, decoded));
    }

    void response_data()
    {
        QTest::addColumn<QString>("mode");
        for (const char* mode : {"success", "skip-boot", "skip-mount", "skip-copy", "skip-finalize", "short-copy",
                "extra-step", "early-step", "wrong-operation", "fractional-operation", "step-fields", "counts",
                "no-files", "oversize-files", "sync-error", "cleanup-error", "wrong-exit", "missing-result"})
            QTest::newRow(mode) << QString::fromLatin1(mode);
    }

    void response()
    {
        QFETCH(QString, mode);
        auto request = dosRequest();
        request.dosKeyboard = QStringLiteral("ro");
        request.dosCodepage = 852;
        request.dosRufusMbr = false;
        QList<QJsonObject> frames = completeFrames();
        if (mode == QStringLiteral("skip-boot")) frames.removeAt(3);
        if (mode == QStringLiteral("skip-mount")) frames.removeAt(4);
        if (mode == QStringLiteral("skip-copy")) frames.remove(5, 7);
        if (mode == QStringLiteral("skip-finalize")) frames.removeAt(12);
        if (mode == QStringLiteral("short-copy")) frames.removeAt(6);
        if (mode == QStringLiteral("extra-step")) frames.insert(6, frames[6]);
        if (mode == QStringLiteral("early-step")) frames.insert(0, frames[6]);
        if (mode == QStringLiteral("wrong-operation")) frames[6].insert("operation", OP_PATCH);
        if (mode == QStringLiteral("fractional-operation")) frames[6].insert("operation", double(OP_FILE_COPY) + 0.5);
        if (mode == QStringLiteral("step-fields")) frames[6].insert("unexpected", true);
        if (mode == QStringLiteral("counts")) frames[6] = rufus::EncodePrepareProgress(OP_FILE_COPY, 1, 6);
        if (mode == QStringLiteral("no-files")) frames.last().insert("copiedBytes", "0");
        if (mode == QStringLiteral("oversize-files")) frames.last().insert("copiedBytes", QString::number(request.size));
        if (mode == QStringLiteral("sync-error")) frames.last().insert("syncError", EIO);
        if (mode == QStringLiteral("cleanup-error")) frames.last().insert("cleanupError", EBUSY);
        if (mode == QStringLiteral("missing-result")) frames.removeLast();
        writeHelper(request, frames, mode == QStringLiteral("wrong-exit") ? "sys.exit(1)\n" : QByteArray());
        rufus::PrepareImageProcess process(fixturePath);
        QSignalSpy done(&process, &rufus::PrepareImageProcess::finished);
        QSignalSpy steps(&process, &rufus::PrepareImageProcess::stepChanged);
        QVERIFY(process.start(request));
        request.dosKeyboard = QStringLiteral("us"); // The accepted request is frozen before ready.
        request.dosCodepage = 437;
        request.dosRufusMbr = true;
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 5000);
        QCOMPARE(launches, 1);
        const bool success = mode == QStringLiteral("success");
        QCOMPARE(process.state(), success ? rufus::PrepareImageProcess::State::Complete : rufus::PrepareImageProcess::State::Failed);
        if (success)
        {
            QCOMPARE(steps.size(), 6);
            for (const auto& step : steps) QCOMPARE(step[0].toInt(), int(OP_FILE_COPY));
        }
    }

    void waitsForExit()
    {
        const auto request = dosRequest();
        const QString gate = directory_.filePath(QStringLiteral("gate"));
        const QString marker = directory_.filePath(QStringLiteral("reported"));
        QFile gateFile(gate);
        QVERIFY(gateFile.open(QIODevice::WriteOnly));
        gateFile.close();
        const QByteArray ending = "gate = Path(bytes.fromhex('" + QFile::encodeName(gate).toHex() + "').decode())\n"
            "Path(bytes.fromhex('" + QFile::encodeName(marker).toHex() + "').decode()).touch()\n"
            "while gate.exists():\n"
            " if select.select([sys.stdin], [], [], 0.01)[0]: break\n";
        writeHelper(request, completeFrames(), ending);
        rufus::PrepareImageProcess process(fixturePath);
        QSignalSpy done(&process, &rufus::PrepareImageProcess::finished);
        QVERIFY(process.start(request));
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(marker), 5000);
        QVERIFY(process.isActive());
        QCOMPARE(done.size(), 0);
        QVERIFY(QFile::remove(gate));
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 5000);
        QCOMPARE(process.state(), rufus::PrepareImageProcess::State::Complete);
        QCOMPARE(launches, 1);
    }

    void cancellation()
    {
        const auto request = dosRequest();
        const auto frames = completeFrames();
        rufus::PrepareImageOutcome outcome;
        outcome.error = ECANCELED;
        outcome.started = true;
        const QByteArray result = QJsonDocument(rufus::EncodePrepareOutcome(outcome)).toJson(QJsonDocument::Compact);
        const QByteArray ending = "sys.stdin.read()\nprint('{\"event\":\"phase\",\"phase\":\"finalizing\"}', flush=True)\n"
            "print(bytes.fromhex('" + result.toHex() + "').decode(), flush=True)\nsys.exit(1)\n";
        writeHelper(request, frames.mid(0, 7), ending);
        rufus::PrepareImageProcess process(fixturePath);
        QSignalSpy done(&process, &rufus::PrepareImageProcess::finished);
        QSignalSpy steps(&process, &rufus::PrepareImageProcess::stepChanged);
        QVERIFY(process.start(request));
        QTRY_COMPARE_WITH_TIMEOUT(steps.size(), 1, 5000);
        process.cancel();
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 5000);
        QCOMPARE(process.state(), rufus::PrepareImageProcess::State::Cancelled);
        QCOMPARE(launches, 1);
    }

    void invalidNativeTarget_data() { roundTrip_data(); }

    void invalidNativeTarget()
    {
        QFETCH(int, fileSystem);
        QFETCH(bool, rufusMbr);
        auto request = dosRequest();
        request.fileSystem = fileSystem;
        request.dosRufusMbr = rufusMbr;
        request.path = directory_.filePath(QStringLiteral("untouched"));
        QFile target(request.path);
        QVERIFY(target.open(QIODevice::ReadWrite | QIODevice::Truncate));
        QCOMPARE(target.write("unchanged"), qint64(9));
        QVERIFY(target.flush());
        fixturePath = QString::fromLocal8Bit(RUFUS_PREPARE_PROBE);
        nativeHelper = true;
        qputenv("RUFUS_TEST_FREEDOS_PREFLIGHT", "1");
        qputenv("PKEXEC_UID", "1000");
        rufus::PrepareImageProcess process(fixturePath);
        QSignalSpy done(&process, &rufus::PrepareImageProcess::finished);
        QSignalSpy phases(&process, &rufus::PrepareImageProcess::phaseChanged);
        QVERIFY(process.start(request));
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 5000);
        QCOMPARE(process.state(), rufus::PrepareImageProcess::State::Failed);
        QVERIFY(process.outcome().error != 0);
        QVERIFY(!process.outcome().started);
        QCOMPARE(process.outcome().copiedBytes, quint64(0));
        QCOMPARE(phases.size(), 1);
        QCOMPARE(phases[0][0].toString(), QStringLiteral("checking"));
        QVERIFY(target.seek(0));
        QCOMPARE(target.readAll(), QByteArray("unchanged"));
        QCOMPARE(launches, 1);
    }

    void cleanup()
    {
        qunsetenv("RUFUS_TEST_FREEDOS_PREFLIGHT");
        if (originalCaller_.isNull()) qunsetenv("PKEXEC_UID");
        else qputenv("PKEXEC_UID", originalCaller_);
    }
};

QTEST_GUILESS_MAIN(TestFreeDosRequest)
#include "test_freedos_request.moc"
