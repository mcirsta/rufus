// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/prepare_device.h"
#include "platform/udisks_client.h"
#include "qt/prepare_image_process.h"

#include <QApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QScreen>
#include <QTest>
#include <cerrno>
#include <csignal>
#include <unistd.h>

namespace
{
constexpr int promptTimeoutMs = 30000;
constexpr int jobTimeoutMs = 30000;
const QString helperPath = QStringLiteral("/opt/rufus/rufus-prepare-helper");
const QString probePath = QStringLiteral("/opt/rufus/prepare_helper_probe");

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

int callerMode(QApplication& application)
{
    rufus::PrepareImageRequest request;
    const QJsonDocument document = QJsonDocument::fromJson(readFile(QStringLiteral("/home/rufus/device-request.json")));
    if (!rufus::DecodePrepareRequest(document.object(), request) || !request.isDevice)
        return 77;
    rufus::PrepareImageProcess process(probePath);
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

    void discovered(const QList<rufus::Device>& devices, const QString&)
    {
        for (const rufus::Device& device : devices)
        {
            if (device.path != QStringLiteral("/dev/sda"))
                continue;
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
        }
    }

    void approve()
    {
        QTRY_VERIFY_WITH_TIMEOUT(!authenticationWindow().isEmpty(), promptTimeoutMs);
        xdo({QStringLiteral("windowfocus"), authenticationWindow()});
        QVERIFY(QApplication::primaryScreen()->grabWindow(0)
            .save(QStringLiteral("/home/rufus/helper-authentication.png")));
        xdo({QStringLiteral("type"), QStringLiteral("--clearmodifiers"), QStringLiteral("rufus-fixture")});
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

private slots:
    void initTestCase()
    {
        QVERIFY(fixtureGuest());
        rufus::UDisksClient discovery(QDBusConnection::systemBus());
        connect(&discovery, &rufus::UDisksClient::devicesChanged, this, &TestPrepareDeviceGuest::discovered);
        discovery.start();
        QTRY_VERIFY_WITH_TIMEOUT(request_.diskSequence != 0, jobTimeoutMs);
        QCOMPARE(request_.size, quint64(64 * 1024 * 1024));
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

    void nativeSuccess()
    {
        rufus::PrepareImageProcess process(helperPath);
        connect(&process, &rufus::PrepareImageProcess::logMessage, this, &TestPrepareDeviceGuest::log);
        const int before = authorizationCount();
        QVERIFY(process.start(request_));
        approve();
        checkFinished(process, before, 0, true);
        QCOMPARE(process.state(), rufus::PrepareImageProcess::State::Complete);
        QVERIFY(process.outcome().filesystemBytes > 0);
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
        QCOMPARE(authorizationCount(), 9);
        qInfo("RUFUS_NATIVE_AUTHORIZATION_PASSED REQUESTS=9");
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
        return callerMode(application);
    TestPrepareDeviceGuest test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_prepare_device_guest.moc"
