// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/prepare_device.h"
#include "platform/prepare_source.h"
#include "platform/iso_write_check.h"
#include "platform/udisks_client.h"
#include "qt/prepare_image_process.h"
#include "qt/main_window.h"
#include "qt/selection_controller.h"
#include "qt/windows_user_dialog.h"
#include "qt/image_mode_dialog.h"
#include "unattend.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QScreen>
#include <QSignalSpy>
#include <QTest>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QProgressBar>
#include <QRadioButton>
#include <cerrno>
#include <csignal>
#include <unistd.h>

namespace
{
constexpr int promptTimeoutMs = 30000;
constexpr int promptPollMs = 50;
constexpr int jobTimeoutMs = 30000;
constexpr int distributionTimeoutMs = 120000;
constexpr int largeFileTimeoutMs = 3600000;
constexpr int screenshotPaintMs = 300;
constexpr quint64 fixtureMiB = 1024 * 1024;
constexpr quint64 partialBootBytes = 4096;
constexpr int preparationJobs = 9;
constexpr int extractionJobs = 9;
constexpr int bootWriteJobs = 5;
const QString helperPath = QStringLiteral("/opt/rufus/rufus-prepare-helper");
const QString probePath = QStringLiteral("/opt/rufus/prepare_helper_probe");
const QString bootProbePath = QStringLiteral("/opt/rufus/prepare_boot_probe");
const QString extractProbePath = QStringLiteral("/opt/rufus/prepare_extract_probe");

QByteArray readFile(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool fixtureGuest()
{
    const bool user = getuid() == 1000 && geteuid() == 1000;
    const bool marker = readFile(QStringLiteral("/etc/rufus-gpt-fixture")) == "RUFUS-GPT-TEST\n";
    const bool machine = readFile(QStringLiteral("/sys/class/dmi/id/product_name")).trimmed() == "RufusUSBFixture";
    return user && marker && machine;
}

QByteArray xdo(const QStringList& arguments)
{
    QProcess process;
    process.start(QStringLiteral("/usr/bin/xdotool"), arguments);
    if (!process.waitForFinished(3000))
    {
        process.kill();
        process.waitForFinished(3000);
        return {};
    }
    return process.readAllStandardOutput().trimmed();
}

QString authenticationWindow()
{
    return QString::fromLatin1(xdo({QStringLiteral("search"), QStringLiteral("--onlyvisible"),
        QStringLiteral("--name"), QStringLiteral("^Authentication Required$")}).split('\n').value(0));
}

int authorizationCount()
{
    return readFile(QStringLiteral("/home/rufus/authorization.log")).count("string \"org.rufus.Prepare");
}

int callerMode(QApplication& application, bool boot)
{
    rufus::PrepareImageRequest request;
    const QJsonDocument document = QJsonDocument::fromJson(readFile(QStringLiteral("/home/rufus/device-request.json")));
    if (!rufus::DecodePrepareRequest(document.object(), request) || !request.isDevice)
        return 77;
    const QString helper = boot ? bootProbePath : (request.imagePath.isEmpty() ? probePath : extractProbePath);
    rufus::PrepareImageProcess process(helper);
    QObject::connect(&process, &rufus::PrepareImageProcess::finished, &application, &QApplication::quit);
    if (!process.start(request))
        return 77;
    return application.exec();
}
}

class TestPrepareDeviceGuest : public QObject
{
    Q_OBJECT
    QProcess agent_;
    rufus::PrepareImageRequest request_;
    rufus::Device device_;
    rufus::MainWindow* cancelWindow_ = nullptr;
    rufus::SelectionController* cancelController_ = nullptr;
    bool cancelledCopy_ = false;

    void cancelInstalledCopy()
    {
        if (!cancelController_ || cancelledCopy_)
            return;
        if (!cancelController_->isoWriter().statusText().startsWith(QStringLiteral("Copying ISO files:")))
            return;
        cancelledCopy_ = true;
        auto* cancel = cancelWindow_->findChild<QPushButton*>(QStringLiteral("closeButton"));
        QVERIFY(cancel && cancel->isEnabled());
        QTest::mouseClick(cancel, Qt::LeftButton);
        auto* confirmation = cancelWindow_->findChild<QMessageBox*>(QStringLiteral("cancelConfirmation"));
        QVERIFY(confirmation && confirmation->isVisible());
        QTest::mouseClick(confirmation->button(QMessageBox::Yes), Qt::LeftButton);
    }

    void discovered(const QList<rufus::Device>& devices, const QString&)
    {
        for (const rufus::Device& device : devices)
        {
            if (device.path != QStringLiteral("/dev/sda"))
                continue;
            device_ = device;
            request_.path = device.path;
            request_.device = device.number;
            request_.size = device.size;
            request_.objectPath = device.objectPath;
            request_.serviceOwner = device.serviceOwner;
            request_.identity = device.identity;
            request_.diskSequence = rufus::ReadDiskSequence(device.number);
            request_.isDevice = true;
            request_.volumeId = 0x1234abcd;
            request_.label = QStringLiteral("RUFUS TEST");
            if (readFile(QStringLiteral("/etc/rufus-fat16-fixture")) == "RUFUS-FAT16-TEST\n")
            {
                request_.fileSystem = FS_FAT16;
                request_.clusterSize = 2048;
            }
            if (readFile(QStringLiteral("/etc/rufus-ntfs-fixture")) == "RUFUS-NTFS-TEST\n")
            {
                request_.fileSystem = FS_NTFS;
                request_.clusterSize = 4096;
                request_.volumeId = 0;
                request_.label = QStringLiteral("Rufus NTFS ș😀");
            }
        }
    }

    void approve()
    {
        // Window discovery starts a process. Under TCG, that work can dominate
        // QTRY's polling delays, so enforce the deadline with elapsed time.
        QElapsedTimer timer;
        timer.start();
        QString window;
        while (window.isEmpty() && timer.elapsed() < promptTimeoutMs)
        {
            window = authenticationWindow();
            if (window.isEmpty())
                QTest::qWait(promptPollMs);
        }
        QVERIFY2(!window.isEmpty(), "The desktop authorization window did not appear.");
        xdo({QStringLiteral("windowfocus"), window});
        xdo({QStringLiteral("windowraise"), window});
        xdo({QStringLiteral("type"), QStringLiteral("--clearmodifiers"), QStringLiteral("rufus-fixture")});
        // A mapped X11 window may not have painted yet, especially under TCG.
        QTest::qWait(screenshotPaintMs);
        QVERIFY(QApplication::primaryScreen()->grabWindow(0)
            .save(QStringLiteral("/home/rufus/helper-authentication.png")));
        xdo({QStringLiteral("key"), QStringLiteral("Return")});
    }

    void checkFinished(rufus::PrepareImageProcess& process, int before, int error, bool started)
    {
        QTRY_VERIFY_WITH_TIMEOUT(!process.isActive(), jobTimeoutMs);
        if (!process.problem().isEmpty())
            qInfo().noquote() << process.problem();
        QCOMPARE(process.outcome().error, error);
        QCOMPARE(process.outcome().started, started);
        QCOMPARE(authorizationCount(), before + 1);
        QCOMPARE(getuid(), uid_t(1000));
        QCOMPARE(geteuid(), uid_t(1000));
    }

    void log(const QString& message) { qInfo().noquote() << message; }

    void extractionFault(const char* mode)
    {
        QFile file(QStringLiteral("/home/rufus/extract-fault"));
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(file.write(mode), qint64(strlen(mode)));
    }

    void bootFault(const char* mode)
    {
        QFile file(QStringLiteral("/home/rufus/boot-fault"));
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(file.write(mode), qint64(strlen(mode)));
    }

    void checkBootReport(const QByteArray& pid, int error, quint64 written, quint64 verified)
    {
        const QList<QByteArray> fields = readFile(QStringLiteral("/run/rufus-boot-report")).split(' ');
        QCOMPARE(fields.size(), 4);
        if (!pid.isEmpty())
            QCOMPARE(fields[0], pid);
        QCOMPARE(fields[1].toInt(), error);
        QCOMPARE(fields[2].toULongLong(), written);
        QCOMPARE(fields[3].toULongLong(), verified);
        qInfo("RUFUS_BOOT_BOUNDARIES_PASSED ERROR=%d WRITTEN=%llu VERIFIED=%llu", error, written, verified);
    }

    void acceptSource(const char* name = "TREE")
    {
        const QString path = QStringLiteral("/home/rufus/%1.iso").arg(QLatin1String(name));
        QCOMPARE(rufus::CapturePrepareSource(path, request_), 0);
    }

    QByteArray waitForExtraction(const QByteArray& previous)
    {
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < jobTimeoutMs)
        {
            const QByteArray pid = readFile(QStringLiteral("/run/rufus-extractor.pid")).trimmed();
            const QByteArray checked = readFile(QStringLiteral("/home/rufus/private-mount-ok")).trimmed();
            if (!pid.isEmpty() && pid != previous && checked == pid)
                return pid;
            QTest::qWait(25);
        }
        return {};
    }

private slots:
    void initTestCase()
    {
        QVERIFY(fixtureGuest());
        rufus::UDisksClient discovery(QDBusConnection::systemBus());
        connect(&discovery, &rufus::UDisksClient::devicesChanged, this, &TestPrepareDeviceGuest::discovered);
        discovery.start();
        QTRY_VERIFY_WITH_TIMEOUT(request_.diskSequence != 0, jobTimeoutMs);
        const bool alpine = readFile(QStringLiteral("/etc/rufus-alpine-fixture")) == "RUFUS-ALPINE-TEST\n";
        const bool largeFile = QFileInfo::exists(QStringLiteral("/etc/rufus-large-file-path"));
        const bool windows = QFileInfo::exists(QStringLiteral("/etc/rufus-windows-fixture"));
        const bool wimSplit = QFileInfo::exists(QStringLiteral("/etc/rufus-wim-split-fixture"));
        quint64 expectedMiB = 64;
        if (windows || wimSplit)
            expectedMiB = 16 * 1024;
        else if (largeFile)
            expectedMiB = 6 * 1024;
        else if (alpine)
            expectedMiB = 512;
        QCOMPARE(request_.size, expectedMiB * fixtureMiB);
        agent_.setProcessChannelMode(QProcess::ForwardedChannels);
        agent_.start(QStringLiteral("/usr/lib/polkit-kde-authentication-agent-1"), {});
        QVERIFY(agent_.waitForStarted(3000));
        QTest::qWait(1000);
        QCOMPARE(authorizationCount(), 0);
    }

    void cancelAuthorization()
    {
        rufus::PrepareImageProcess process(helperPath);
        const int before = authorizationCount();
        QVERIFY(process.start(request_));
        QTRY_VERIFY_WITH_TIMEOUT(!authenticationWindow().isEmpty(), promptTimeoutMs);
        process.cancel();
        checkFinished(process, before, ECANCELED, false);
        QTRY_VERIFY_WITH_TIMEOUT(authenticationWindow().isEmpty(), promptTimeoutMs);
    }

    void changedSequence()
    {
        rufus::PrepareImageRequest changed = request_;
        ++changed.diskSequence;
        rufus::PrepareImageProcess process(helperPath);
        const int before = authorizationCount();
        QVERIFY(process.start(changed));
        approve();
        checkFinished(process, before, ESTALE, false);
    }

    void changedIdentity()
    {
        rufus::PrepareImageRequest changed = request_;
        changed.identity.last() += QLatin1Char('x');
        rufus::PrepareImageProcess process(helperPath);
        const int before = authorizationCount();
        QVERIFY(process.start(changed));
        approve();
        checkFinished(process, before, ESTALE, false);
    }

    void fileRequestIsNotElevated()
    {
        QProcess process;
        const int before = authorizationCount();
        process.start(QStringLiteral("/usr/bin/pkexec"),
            {QStringLiteral("--disable-internal-agent"), helperPath, QStringLiteral("--device")});
        approve();
        QVERIFY(process.waitForReadyRead(jobTimeoutMs));
        const QByteArray ready = process.readAllStandardOutput();
        QVERIFY(ready.contains("ready"));
        rufus::PrepareImageRequest file = request_;
        file.isDevice = false;
        file.fileSystem = FS_FAT32;
        file.imagePath.clear(); // Send a valid file-only request to the elevated endpoint.
        process.write(QJsonDocument(rufus::EncodePrepareRequest(file)).toJson(QJsonDocument::Compact) + '\n');
        QVERIFY(process.waitForFinished(jobTimeoutMs));
        rufus::PrepareImageOutcome outcome;
        QVERIFY(rufus::DecodePrepareOutcome(QJsonDocument::fromJson(process.readAllStandardOutput()).object(), outcome));
        QCOMPARE(outcome.error, EPERM);
        QVERIFY(!outcome.started);
        QCOMPARE(authorizationCount(), before + 1);
        // The root observer checks the whole disk against its pre-test hash.
        QFile marker(QStringLiteral("/home/rufus/check-unchanged"));
        QVERIFY(marker.open(QIODevice::WriteOnly));
        marker.close();
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(QStringLiteral("/home/rufus/unchanged-ok")), jobTimeoutMs);
    }

    void cancelActiveFormatter()
    {
        rufus::PrepareImageProcess process(probePath);
        connect(&process, &rufus::PrepareImageProcess::logMessage, this, &TestPrepareDeviceGuest::log);
        const int before = authorizationCount();
        QVERIFY(process.start(request_));
        approve();
        QTRY_VERIFY_WITH_TIMEOUT(!readFile(QStringLiteral("/run/rufus-formatter.pid")).isEmpty(), jobTimeoutMs);
        const QByteArray pid = readFile(QStringLiteral("/run/rufus-formatter.pid")).trimmed().split(' ')[0];
        QCOMPARE(kill(pid.toInt(), 0), -1);
        QCOMPARE(errno, EPERM); // The GUI cannot signal this root child.
        process.cancel();
        checkFinished(process, before, ECANCELED, true);
        QVERIFY(!QFileInfo::exists(QStringLiteral("/proc/") + QString::fromLatin1(pid)));
    }

    void callerDeathCleansUp()
    {
        const QByteArray previous = readFile(QStringLiteral("/run/rufus-formatter.pid"));
        QFile request(QStringLiteral("/home/rufus/device-request.json"));
        QVERIFY(request.open(QIODevice::WriteOnly));
        request.write(QJsonDocument(rufus::EncodePrepareRequest(request_)).toJson(QJsonDocument::Compact));
        request.close();
        QProcess caller;
        const int before = authorizationCount();
        caller.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--caller")});
        approve();
        QByteArray current;
        QTRY_VERIFY_WITH_TIMEOUT(!(current = readFile(QStringLiteral("/run/rufus-formatter.pid"))).isEmpty()
            && current != previous, jobTimeoutMs);
        const QList<QByteArray> pids = current.trimmed().split(' ');
        QCOMPARE(pids.size(), 2);
        caller.kill(); // Ordinary GUI caller dies; its root helper must notice EOF.
        QVERIFY(caller.waitForFinished(3000));
        for (const QByteArray& pid : pids)
            QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(QStringLiteral("/proc/") + QString::fromLatin1(pid)), jobTimeoutMs);
        QCOMPARE(authorizationCount(), before + 1);
    }

    void cancelBootWrite()
    {
        QVERIFY(request_.fileSystem == FS_NTFS);
        bootFault("pause");
        rufus::PrepareImageProcess process(bootProbePath);
        const int before = authorizationCount();
        QVERIFY(process.start(request_));
        approve();
        QTRY_VERIFY_WITH_TIMEOUT(!readFile(QStringLiteral("/run/rufus-boot-writer.pid")).isEmpty(), jobTimeoutMs);
        const QByteArray pid = readFile(QStringLiteral("/run/rufus-boot-writer.pid"));
        process.cancel();
        checkFinished(process, before, ECANCELED, true);
        QCOMPARE(process.outcome().syncError, 0);
        checkBootReport(pid, ECANCELED, partialBootBytes, 0);
    }

    void callerDeathDuringBootWrite()
    {
        bootFault("pause");
        const QByteArray previous = readFile(QStringLiteral("/run/rufus-boot-writer.pid"));
        QFile request(QStringLiteral("/home/rufus/device-request.json"));
        QVERIFY(request.open(QIODevice::WriteOnly));
        request.write(QJsonDocument(rufus::EncodePrepareRequest(request_)).toJson(QJsonDocument::Compact));
        request.close();
        QProcess caller;
        const int before = authorizationCount();
        caller.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--boot-caller")});
        approve();
        QByteArray pid;
        QTRY_VERIFY_WITH_TIMEOUT(!(pid = readFile(QStringLiteral("/run/rufus-boot-writer.pid"))).isEmpty()
            && pid != previous, jobTimeoutMs);
        caller.kill();
        QVERIFY(caller.waitForFinished(3000));
        QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(QStringLiteral("/proc/") + QString::fromLatin1(pid)), jobTimeoutMs);
        QCOMPARE(authorizationCount(), before + 1);
        checkBootReport(pid, ECANCELED, partialBootBytes, 0);
    }

    void bootWriteChecks()
    {
        const struct { const char* mode; int error; } cases[] = {{"sync", EIO}, {"corrupt", EBADMSG}, {"none", 0}};
        for (const auto& item : cases)
        {
            bootFault(item.mode);
            rufus::PrepareImageProcess process(bootProbePath);
            const int before = authorizationCount();
            QVERIFY(process.start(request_));
            approve();
            checkFinished(process, before, item.error, true);
            QCOMPARE(process.outcome().syncError, item.error == EIO ? EIO : 0);
            checkBootReport({}, item.error, fixtureMiB, item.error == 0 ? fixtureMiB : 0);
        }
    }

    void nativeSuccess()
    {
        rufus::PrepareImageProcess process(helperPath);
        QSignalSpy phases(&process, &rufus::PrepareImageProcess::phaseChanged);
        connect(&process, &rufus::PrepareImageProcess::logMessage, this, &TestPrepareDeviceGuest::log);
        const int before = authorizationCount();
        QVERIFY(process.start(request_));
        approve();
        checkFinished(process, before, 0, true);
        QCOMPARE(process.state(), rufus::PrepareImageProcess::State::Complete);
        QVERIFY(process.outcome().filesystemBytes > 0);
        if (request_.fileSystem == FS_NTFS)
        {
            QCOMPARE(phases.size(), 5);
            QCOMPARE(phases[3][0].toString(), QStringLiteral("bootloader"));
            QCOMPARE(phases[4][0].toString(), QStringLiteral("finalizing"));
            QCOMPARE(process.outcome().filesystemBytes, process.outcome().size);
            QVERIFY(QFileInfo::exists(QStringLiteral("/sys/block/sda/sda2/size")));
        }
        else
            QCOMPARE(phases.size(), 3);
        qInfo("RUFUS_NATIVE_PREPARED OFFSET=%llu SIZE=%llu", process.outcome().offset, process.outcome().size);
    }

    void mountedTargetIsRefused()
    {
        QFile mountRequest(QStringLiteral("/home/rufus/mount-target"));
        QVERIFY(mountRequest.open(QIODevice::WriteOnly));
        mountRequest.close();
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(QStringLiteral("/home/rufus/mounted-ready")), jobTimeoutMs);
        rufus::PrepareImageProcess process(helperPath);
        const int before = authorizationCount();
        QVERIFY(process.start(request_));
        approve();
        checkFinished(process, before, EBUSY, false);
        QFile unmountRequest(QStringLiteral("/home/rufus/unmount-target"));
        QVERIFY(unmountRequest.open(QIODevice::WriteOnly));
        unmountRequest.close();
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(QStringLiteral("/home/rufus/mounted-refusal-ok")), jobTimeoutMs);
    }

    void noAgentDoesNotRetry()
    {
        agent_.terminate();
        QVERIFY(agent_.waitForFinished(3000));
        rufus::PrepareImageProcess process(helperPath);
        const int before = authorizationCount();
        QVERIFY(process.start(request_));
        checkFinished(process, before, EACCES, false);
        const int expected = preparationJobs + (request_.fileSystem == FS_NTFS ? bootWriteJobs : 0);
        QCOMPARE(authorizationCount(), expected);
        qInfo("RUFUS_NATIVE_AUTHORIZATION_PASSED REQUESTS=%d", expected);
    }

    void changedSourceIsRefused()
    {
        acceptSource();
        request_.imageModified = QStringLiteral("0:0");
        rufus::PrepareImageProcess process(helperPath);
        const int before = authorizationCount();
        QVERIFY(process.start(request_));
        approve();
        checkFinished(process, before, ESTALE, false);
    }

    void unsupportedSourceIsRefused()
    {
        acceptSource("LINK");
        rufus::PrepareImageProcess process(helperPath);
        const int before = authorizationCount();
        QVERIFY(process.start(request_));
        approve();
        checkFinished(process, before, ENOTSUP, false);
    }

    void rootOwnedSourceIsRefused()
    {
        QCOMPARE(rufus::CapturePrepareSource(QStringLiteral("/opt/rufus/root-owned.iso"), request_), 0);
        rufus::PrepareImageProcess process(helperPath);
        const int before = authorizationCount();
        QVERIFY(process.start(request_));
        approve();
        checkFinished(process, before, EACCES, false);
        QFile marker(QStringLiteral("/home/rufus/check-unchanged"));
        QVERIFY(marker.open(QIODevice::WriteOnly));
        marker.close();
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(QStringLiteral("/home/rufus/unchanged-ok")), jobTimeoutMs);
    }

    void cancelMountedExtraction()
    {
        acceptSource();
        extractionFault("pause");
        const QByteArray previous = readFile(QStringLiteral("/run/rufus-extractor.pid")).trimmed();
        rufus::PrepareImageProcess process(extractProbePath);
        const int before = authorizationCount();
        QVERIFY(process.start(request_));
        approve();
        const QByteArray pid = waitForExtraction(previous);
        QVERIFY(!pid.isEmpty());
        process.cancel();
        checkFinished(process, before, ECANCELED, true);
        QCOMPARE(process.outcome().cleanupError, 0);
        QCOMPARE(process.outcome().syncError, 0);
        QVERIFY(process.outcome().copiedBytes > 0);
    }

    void callerDeathDuringExtraction()
    {
        acceptSource();
        extractionFault("pause");
        QFile request(QStringLiteral("/home/rufus/device-request.json"));
        QVERIFY(request.open(QIODevice::WriteOnly));
        request.write(QJsonDocument(rufus::EncodePrepareRequest(request_)).toJson(QJsonDocument::Compact));
        request.close();
        const QByteArray previous = readFile(QStringLiteral("/run/rufus-extractor.pid")).trimmed();
        QProcess caller;
        const int before = authorizationCount();
        caller.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--caller")});
        approve();
        const QByteArray pid = waitForExtraction(previous);
        QVERIFY(!pid.isEmpty());
        caller.kill();
        QVERIFY(caller.waitForFinished(3000));
        QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(QStringLiteral("/proc/") + QString::fromLatin1(pid)), jobTimeoutMs);
        QCOMPARE(authorizationCount(), before + 1);
    }

    void mountAndCleanupFailures_data()
    {
        QTest::addColumn<QByteArray>("fault");
        QTest::newRow("mount") << QByteArray("mount");
        QTest::newRow("flush") << QByteArray("flush");
        QTest::newRow("unmount") << QByteArray("unmount");
    }

    void mountAndCleanupFailures()
    {
        QFETCH(QByteArray, fault);
        acceptSource();
        extractionFault(fault.constData());
        rufus::PrepareImageProcess process(extractProbePath);
        const int before = authorizationCount();
        QVERIFY(process.start(request_));
        approve();
        checkFinished(process, before, fault == "unmount" ? EBUSY : EIO, true);
        QCOMPARE(process.state(), rufus::PrepareImageProcess::State::Failed);
        QCOMPARE(process.outcome().syncError, fault == "flush" ? EIO : 0);
        QCOMPARE(process.outcome().cleanupError, fault == "unmount" ? EBUSY : 0);
    }

    void nativeExtraction()
    {
        acceptSource();
        extractionFault("none");
        rufus::PrepareImageProcess process(helperPath);
        QSignalSpy phases(&process, &rufus::PrepareImageProcess::phaseChanged);
        connect(&process, &rufus::PrepareImageProcess::logMessage, this, &TestPrepareDeviceGuest::log);
        const int before = authorizationCount();
        QVERIFY(process.start(request_));
        approve();
        checkFinished(process, before, 0, true);
        QCOMPARE(process.outcome().cleanupError, 0);
        QCOMPARE(process.outcome().syncError, 0);
        QCOMPARE(process.state(), rufus::PrepareImageProcess::State::Complete);
        QCOMPARE(phases.size(), request_.fileSystem == FS_NTFS ? 7 : 6);
        QCOMPARE(phases.last()[0].toString(), QStringLiteral("finalizing"));
        QVERIFY(process.outcome().copiedBytes > 0);
        QCOMPARE(authorizationCount(), 9);
        qInfo("RUFUS_NATIVE_PREPARED OFFSET=%llu SIZE=%llu", process.outcome().offset, process.outcome().size);
        qInfo("RUFUS_NATIVE_EXTRACTION_PASSED REQUESTS=9");
    }

    void nativeUefiExtraction()
    {
        acceptSource("UEFI");
        extractionFault("none");
        rufus::IsoWriteCheck checked;
        QCOMPARE(rufus::CheckIsoWriteImage(request_, getuid(), checked), 0);
        QVERIFY(!checked.partition.started && checked.extraPartitions == 0);
        rufus::PrepareImageProcess process(helperPath);
        QSignalSpy phases(&process, &rufus::PrepareImageProcess::phaseChanged);
        connect(&process, &rufus::PrepareImageProcess::logMessage, this, &TestPrepareDeviceGuest::log);
        const int before = authorizationCount();
        QVERIFY(process.start(request_));
        approve();
        checkFinished(process, before, 0, true);
        QCOMPARE(process.state(), rufus::PrepareImageProcess::State::Complete);
        QCOMPARE(process.outcome().syncError, 0);
        QCOMPARE(process.outcome().cleanupError, 0);
        QVERIFY(process.outcome().copiedBytes > 0);
        QCOMPARE(phases.size(), 6);
        QCOMPARE(phases.last()[0].toString(), QStringLiteral("finalizing"));
        const int expectedRequests = request_.fileSystem == FS_FAT16 ? extractionJobs : 1;
        QCOMPARE(authorizationCount(), expectedRequests);
        qInfo("RUFUS_NATIVE_PREPARED OFFSET=%llu SIZE=%llu", process.outcome().offset, process.outcome().size);
        qInfo("RUFUS_NATIVE_UEFI_EXTRACTION_PASSED REQUESTS=%d", expectedRequests);
    }

    void nativeWimSplitExtraction()
    {
        const QString source = QString::fromUtf8(readFile(QStringLiteral("/etc/rufus-wim-split-source")).trimmed());
        QVERIFY(source.startsWith(QStringLiteral("/source/")));
        QCOMPARE(rufus::CapturePrepareSource(source, request_), 0);
        request_.enhancedInstall = true;
        request_.clusterSize = 4096;
        request_.label = QStringLiteral("WINDOWS 11");
        QVERIFY(request_.fileSystem != FS_NTFS);
        QCOMPARE(request_.windowsFlags, 0);
        rufus::PrepareImageProcess process(helperPath);
        QSignalSpy phases(&process, &rufus::PrepareImageProcess::phaseChanged);
        connect(&process, &rufus::PrepareImageProcess::logMessage, this, &TestPrepareDeviceGuest::log);
        const int before = authorizationCount();
        QVERIFY(process.start(request_));
        approve();
        QTRY_VERIFY_WITH_TIMEOUT(!process.isActive(), largeFileTimeoutMs);
        checkFinished(process, before, 0, true);
        QCOMPARE(process.state(), rufus::PrepareImageProcess::State::Complete);
        QCOMPARE(process.outcome().syncError, 0);
        QCOMPARE(process.outcome().cleanupError, 0);
        QVERIFY(process.outcome().copiedBytes > 4 * 1024 * fixtureMiB);
        QCOMPARE(phases.last()[0].toString(), QStringLiteral("finalizing"));
        QCOMPARE(authorizationCount(), 1);
        qInfo("RUFUS_NATIVE_PREPARED OFFSET=%llu SIZE=%llu VOLUME_ID=%u", process.outcome().offset,
            process.outcome().size, request_.volumeId);
        qInfo("RUFUS_WIM_SPLIT_PASSED REQUESTS=1 BYTES=%llu", process.outcome().copiedBytes);
    }

    void largeFileFatIsRefused()
    {
        acceptSource("LARGE");
        rufus::PrepareImageRequest fat = request_;
        fat.fileSystem = FS_FAT32;
        fat.label = QStringLiteral("LARGE");
        rufus::PrepareImageProcess process(helperPath);
        const int before = authorizationCount();
        QVERIFY(process.start(fat));
        approve();
        checkFinished(process, before, EFBIG, false);
        QFile marker(QStringLiteral("/home/rufus/check-unchanged"));
        QVERIFY(marker.open(QIODevice::WriteOnly));
        marker.close();
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(QStringLiteral("/home/rufus/unchanged-ok")), largeFileTimeoutMs);
    }

    void nativeLargeExtraction()
    {
        QVERIFY(request_.fileSystem == FS_NTFS);
        acceptSource("LARGE");
        const quint64 expected = readFile(QStringLiteral("/etc/rufus-large-file-bytes")).trimmed().toULongLong();
        QVERIFY(expected > 4 * 1024 * fixtureMiB);
        rufus::PrepareImageProcess process(helperPath);
        QSignalSpy phases(&process, &rufus::PrepareImageProcess::phaseChanged);
        connect(&process, &rufus::PrepareImageProcess::logMessage, this, &TestPrepareDeviceGuest::log);
        const int before = authorizationCount();
        QVERIFY(process.start(request_));
        approve();
        QTRY_VERIFY_WITH_TIMEOUT(!process.isActive(), largeFileTimeoutMs);
        checkFinished(process, before, 0, true);
        QCOMPARE(process.state(), rufus::PrepareImageProcess::State::Complete);
        QCOMPARE(process.outcome().copiedBytes, expected);
        QCOMPARE(process.outcome().syncError, 0);
        QCOMPARE(process.outcome().cleanupError, 0);
        QCOMPARE(phases.size(), 7);
        QCOMPARE(phases.last()[0].toString(), QStringLiteral("finalizing"));
        QCOMPARE(authorizationCount(), 2);
        qInfo("RUFUS_NATIVE_PREPARED OFFSET=%llu SIZE=%llu", process.outcome().offset, process.outcome().size);
        qInfo("RUFUS_NATIVE_LARGE_EXTRACTION_PASSED REQUESTS=2 BYTES=%llu", process.outcome().copiedBytes);
    }

    void nativeUefiWidgets()
    {
        const bool windows = QFileInfo::exists(QStringLiteral("/etc/rufus-windows-fixture"));
        const bool privacy = QFileInfo::exists(QStringLiteral("/etc/rufus-windows-privacy"));
        const QString effectiveLabel = windows ? QStringLiteral("Windows 11") : QStringLiteral("RUFUS TEST");
        rufus::SelectionController controller;
        controller.writer().setConnection(QDBusConnection::systemBus());
        controller.writer().setHelper(helperPath);
        controller.isoWriter().configure(QDBusConnection::systemBus(), helperPath);
        rufus::UDisksClient discovery(QDBusConnection::systemBus());
        connect(&discovery, &rufus::UDisksClient::devicesChanged, &controller, &rufus::SelectionController::setDiscovery);
        rufus::MainWindow window(controller);
        window.show();
        connect(&controller, &rufus::SelectionController::logMessage, this, &TestPrepareDeviceGuest::log);
        discovery.start();
        QTRY_VERIFY_WITH_TIMEOUT(!controller.selection().devices.isEmpty(), jobTimeoutMs);
        for (const rufus::Device& device : controller.selection().devices)
        {
            if (device.path == QStringLiteral("/dev/sda"))
                controller.selectDevice(device.id);
        }
        QVERIFY(!controller.selection().deviceId.isEmpty());
        controller.selectImage(windows ? QStringLiteral("/home/rufus/WINDOWS.iso") : QStringLiteral("/home/rufus/UEFI.iso"));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.isAnalyzing(), jobTimeoutMs);
        QCOMPARE(controller.selection().imageState, rufus::ImageState::Partial);
        QVERIFY(controller.selection().imageAnalysis.pending & ISO_PENDING_SECURE_BOOT);
        if (windows)
        {
            QCOMPARE(controller.selection().windowsAnalysis.state, WINDOWS_IMAGE_KNOWN);
            QCOMPARE(controller.selection().imageAnalysis.report.win_version.major, quint16(11));
            QCOMPARE(controller.selection().fileSystem, int(FS_NTFS));
            QCOMPARE(controller.selection().clusterSize, quint32(4096));
        }
        auto* label = window.findChild<QLineEdit*>(QStringLiteral("volumeEdit"));
        auto* start = window.findChild<QPushButton*>(QStringLiteral("startButton"));
        auto* progress = window.findChild<QProgressBar*>(QStringLiteral("progressBar"));
        QVERIFY(label && start && progress);
        QVERIFY(label->isEnabled());
        label->selectAll();
        QTest::keyClicks(label, windows ? effectiveLabel : QStringLiteral("rufus test"));
        QVERIFY(start->isEnabled());
        QTest::mouseClick(start, Qt::LeftButton);
        if (windows)
        {
            QTRY_COMPARE_WITH_TIMEOUT(controller.isoWriter().state(), rufus::IsoWriteState::ChoosingWindowsOptions, jobTimeoutMs);
            auto* options = window.findChild<rufus::WindowsUserDialog*>(QStringLiteral("windowsUserDialog"));
            QVERIFY(options && options->isVisible());
            const QList<QCheckBox*> choices = options->findChildren<QCheckBox*>();
            for (QCheckBox* choice : choices)
            {
                const int flag = choice->property("unattendFlag").toInt();
                choice->setChecked(privacy && flag == UNATTEND_NO_DATA_COLLECTION);
            }
            QCOMPARE(options->selectedFlags(), privacy ? UNATTEND_NO_DATA_COLLECTION : 0);
            auto* regional = options->findChild<QCheckBox*>(QStringLiteral("windowsOption%1").arg(UNATTEND_DUPLICATE_LOCALE));
            QVERIFY(regional && !regional->isEnabled());
            QVERIFY(regional->toolTip().contains(QStringLiteral("Windows host")));
            QCOMPARE(authorizationCount(), 0);
            QTest::qWait(screenshotPaintMs);
            QVERIFY(QApplication::primaryScreen()->grabWindow(0)
                .save(QStringLiteral("/home/rufus/windows-options.png")));
            auto* actions = options->findChild<QDialogButtonBox*>();
            QVERIFY(actions);
            QTest::mouseClick(actions->button(QDialogButtonBox::Ok), Qt::LeftButton);
        }
        QTRY_COMPARE_WITH_TIMEOUT(controller.isoWriter().state(), rufus::IsoWriteState::AwaitingConfirmation, jobTimeoutMs);
        auto* confirmation = window.findChild<QMessageBox*>(QStringLiteral("isoWriteConfirmation"));
        QVERIFY(confirmation && confirmation->isVisible());
        QCOMPARE(controller.isoWriter().request().label, effectiveLabel);
        QCOMPARE(controller.isoWriter().request().fileSystem == FS_NTFS, windows);
        QCOMPARE(controller.isoWriter().request().windowsFlags, privacy ? UNATTEND_NO_DATA_COLLECTION : 0);
        QVERIFY(confirmation->informativeText().contains(QStringLiteral("Volume label: ") + effectiveLabel));
        QCOMPARE(authorizationCount(), 0);
        QTest::qWait(screenshotPaintMs);
        QVERIFY(QApplication::primaryScreen()->grabWindow(0)
            .save(QStringLiteral("/home/rufus/iso-confirmation.png")));
        const quint32 volumeId = controller.isoWriter().request().volumeId;
        QTest::mouseClick(confirmation->button(QMessageBox::Ok), Qt::LeftButton);
        approve();
        QTRY_VERIFY_WITH_TIMEOUT(!controller.isoWriter().isActive(), windows ? largeFileTimeoutMs : distributionTimeoutMs);
        QVERIFY2(controller.isoWriter().state() == rufus::IsoWriteState::Complete,
            qPrintable(controller.isoWriter().statusText()));
        const rufus::PrepareImageOutcome outcome = controller.isoWriter().outcome();
        QCOMPARE(outcome.error, 0);
        QCOMPARE(outcome.syncError, 0);
        QCOMPARE(outcome.cleanupError, 0);
        QCOMPARE(outcome.copiedBytes, controller.isoWriter().check().tree.bytes_written);
        QCOMPARE(authorizationCount(), 1);
        QCOMPARE(progress->value(), MAX_PROGRESS);
        QCOMPARE(progress->format(), QStringLiteral("READY"));
        QCOMPARE(getuid(), uid_t(1000));
        QCOMPARE(geteuid(), uid_t(1000));
        QVERIFY(window.grab().save(QStringLiteral("/home/rufus/iso-complete.png")));
        qInfo("RUFUS_NATIVE_PREPARED OFFSET=%llu SIZE=%llu VOLUME_ID=%u", outcome.offset, outcome.size, volumeId);
        qInfo("RUFUS_NATIVE_UEFI_EXTRACTION_PASSED REQUESTS=1");
        if (windows)
            qInfo("RUFUS_WINDOWS_CREATION_PASSED REQUESTS=1 BYTES=%llu", outcome.copiedBytes);
        if (privacy)
            qInfo("RUFUS_WINDOWS_CUSTOMIZATION_PASSED FLAGS=8 REQUESTS=1");
        qInfo("RUFUS_ISO_WIDGETS_PASSED REQUESTS=1");
    }

    void installedGuiStarts()
    {
        const QString executable = QString::fromUtf8(readFile(QStringLiteral("/etc/rufus-installed-gui"))).trimmed();
        QVERIFY(QFileInfo(executable).isExecutable());
        QProcess application;
        application.setProcessChannelMode(QProcess::ForwardedChannels);
        application.start(executable, {});
        QVERIFY(application.waitForStarted(jobTimeoutMs));
        QString window;
        QElapsedTimer timer;
        timer.start();
        while (window.isEmpty() && timer.elapsed() < jobTimeoutMs)
        {
            window = QString::fromLatin1(xdo({QStringLiteral("search"), QStringLiteral("--onlyvisible"),
                QStringLiteral("--pid"), QString::number(application.processId()),
                QStringLiteral("--name"), QStringLiteral("^Rufus")}).split('\n').value(0));
            QTest::qWait(promptPollMs);
        }
        QVERIFY2(!window.isEmpty(), "The installed application did not show its main window.");
        QTest::qWait(screenshotPaintMs);
        QVERIFY(QApplication::primaryScreen()->grabWindow(0).save(QStringLiteral("/home/rufus/installed-gui.png")));
        QCOMPARE(authorizationCount(), 0);
        xdo({QStringLiteral("windowfocus"), window});
        xdo({QStringLiteral("key"), QStringLiteral("Escape")});
        QTRY_COMPARE_WITH_TIMEOUT(application.state(), QProcess::NotRunning, jobTimeoutMs);
        QCOMPARE(application.exitStatus(), QProcess::NormalExit);
        QCOMPARE(application.exitCode(), 0);
        qInfo("RUFUS_INSTALLED_GUI_STARTED UID=1000 REQUESTS=0");
    }

    void installedSyslinuxWidgets()
    {
        const QString installedHelper = QString::fromUtf8(readFile(QStringLiteral("/etc/rufus-installed-helper"))).trimmed();
        QVERIFY(QFileInfo(installedHelper).isExecutable());
        rufus::SelectionController controller;
        controller.writer().setConnection(QDBusConnection::systemBus());
        controller.writer().setHelper(helperPath);
        controller.isoWriter().configure(QDBusConnection::systemBus(), installedHelper);
        rufus::UDisksClient discovery(QDBusConnection::systemBus());
        connect(&discovery, &rufus::UDisksClient::devicesChanged, &controller, &rufus::SelectionController::setDiscovery);
        connect(&controller, &rufus::SelectionController::logMessage, this, &TestPrepareDeviceGuest::log);
        rufus::MainWindow window(controller);
        window.show();
        discovery.start();
        QTRY_VERIFY_WITH_TIMEOUT(!controller.selection().devices.isEmpty(), jobTimeoutMs);
        for (const rufus::Device& device : controller.selection().devices)
        {
            if (device.path == QStringLiteral("/dev/sda"))
                controller.selectDevice(device.id);
        }
        QVERIFY(!controller.selection().deviceId.isEmpty());
        controller.selectImage(QStringLiteral("/home/rufus/ALPINE.iso"));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.isAnalyzing(), jobTimeoutMs);
        QVERIFY(controller.selection().syslinux);
        QCOMPARE(controller.selection().fileSystem, int(FS_FAT32));
        auto* clusters = window.findChild<QComboBox*>(QStringLiteral("clusterCombo"));
        QVERIFY(clusters && clusters->findData(4096) >= 0);
        clusters->setCurrentIndex(clusters->findData(4096));
        QVERIFY(QMetaObject::invokeMethod(clusters, "activated", Q_ARG(int, clusters->currentIndex())));
        auto* start = window.findChild<QPushButton*>(QStringLiteral("startButton"));
        auto* progress = window.findChild<QProgressBar*>(QStringLiteral("progressBar"));
        QVERIFY(start && progress && start->isEnabled());
        cancelWindow_ = &window;
        cancelController_ = &controller;
        connect(&controller.isoWriter(), &rufus::IsoWriteController::changed,
            this, &TestPrepareDeviceGuest::cancelInstalledCopy);
        constexpr int jobs = 2; // One cancelled creation, followed by one complete creation.
        for (int job = 0; job < jobs; ++job)
        {
            QTest::mouseClick(start, Qt::LeftButton);
            QTRY_COMPARE_WITH_TIMEOUT(controller.isoWriter().state(), rufus::IsoWriteState::ChoosingMode, jobTimeoutMs);
            auto* mode = window.findChild<rufus::ImageModeDialog*>();
            QVERIFY(mode);
            const auto choices = mode->findChildren<QRadioButton*>();
            QVERIFY(!choices.isEmpty() && choices.first()->isChecked());
            QTest::mouseClick(mode->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok), Qt::LeftButton);
            QTRY_COMPARE_WITH_TIMEOUT(controller.isoWriter().state(), rufus::IsoWriteState::AwaitingConfirmation, jobTimeoutMs);
            auto* confirmation = window.findChild<QMessageBox*>(QStringLiteral("isoWriteConfirmation"));
            QVERIFY(confirmation && confirmation->isVisible());
            QVERIFY(confirmation->informativeText().contains(QStringLiteral("MBR, FAT32, BIOS")));
            QVERIFY(controller.isoWriter().request().syslinux);
            QCOMPARE(authorizationCount(), job);
            QTest::mouseClick(confirmation->button(QMessageBox::Ok), Qt::LeftButton);
            approve();
            QTRY_VERIFY_WITH_TIMEOUT(!controller.isoWriter().isActive(), distributionTimeoutMs);
            const rufus::PrepareImageOutcome outcome = controller.isoWriter().outcome();
            QCOMPARE(authorizationCount(), job + 1);
            QCOMPARE(outcome.syncError, 0);
            QCOMPARE(outcome.cleanupError, 0);
            QVERIFY(outcome.started);
            if (job == 0)
            {
                QVERIFY(cancelledCopy_);
                QCOMPARE(controller.isoWriter().state(), rufus::IsoWriteState::Cancelled);
                QCOMPARE(outcome.error, ECANCELED);
                QCOMPARE(progress->format(), QStringLiteral("CANCELLED"));
                QVERIFY(window.grab().save(QStringLiteral("/home/rufus/installed-cancelled.png")));
                QFile check(QStringLiteral("/home/rufus/check-cleanup"));
                QVERIFY(check.open(QIODevice::WriteOnly));
                check.close();
                QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(QStringLiteral("/home/rufus/cleanup-ok")), jobTimeoutMs);
                qInfo("RUFUS_INSTALLED_CANCELLED REQUESTS=1");
            }
            else
            {
                QVERIFY2(controller.isoWriter().state() == rufus::IsoWriteState::Complete,
                    qPrintable(controller.isoWriter().statusText()));
                QCOMPARE(outcome.error, 0);
                QCOMPARE(outcome.copiedBytes, controller.isoWriter().check().tree.bytes_written);
                QCOMPARE(progress->value(), MAX_PROGRESS);
                QCOMPARE(progress->format(), QStringLiteral("READY"));
                QCOMPARE(geteuid(), uid_t(1000));
                QVERIFY(window.grab().save(QStringLiteral("/home/rufus/installed-complete.png")));
                qInfo("RUFUS_INSTALLED_CREATED REQUESTS=1 BYTES=%llu", outcome.copiedBytes);
            }
        }
        cancelController_ = nullptr;
        cancelWindow_ = nullptr;
    }

    void nativeAlpineExtraction()
    {
        acceptSource("ALPINE");
        rufus::PrepareImageProcess process(helperPath);
        connect(&process, &rufus::PrepareImageProcess::logMessage, this, &TestPrepareDeviceGuest::log);
        const int before = authorizationCount();
        QVERIFY(process.start(request_));
        approve();
        QTRY_VERIFY_WITH_TIMEOUT(!process.isActive(), distributionTimeoutMs);
        checkFinished(process, before, 0, true);
        QCOMPARE(process.state(), rufus::PrepareImageProcess::State::Complete);
        QCOMPARE(process.outcome().syncError, 0);
        QCOMPARE(process.outcome().cleanupError, 0);
        QVERIFY(process.outcome().copiedBytes > 0);
        QCOMPARE(authorizationCount(), 1);
        qInfo("RUFUS_NATIVE_PREPARED OFFSET=%llu SIZE=%llu", process.outcome().offset, process.outcome().size);
        qInfo("RUFUS_NATIVE_ALPINE_EXTRACTION_PASSED REQUESTS=1");
    }

    void cleanupTestCase()
    {
        if (agent_.state() != QProcess::NotRunning)
        {
            agent_.terminate();
            agent_.waitForFinished(3000);
        }
    }
};

int main(int argc, char** argv)
{
    if (!fixtureGuest())
        return 77;
    QApplication application(argc, argv);
    if (argc == 2 && strcmp(argv[1], "--caller") == 0)
        return callerMode(application, false);
    if (argc == 2 && strcmp(argv[1], "--boot-caller") == 0)
        return callerMode(application, true);
    TestPrepareDeviceGuest test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_prepare_device_guest.moc"
