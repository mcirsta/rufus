// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/udisks_client.h"
#include "qt/main_window.h"
#include "qt/selection_controller.h"

#include <QComboBox>
#include <QDBusArgument>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QLabel>
#include <QProcess>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>

#include <memory>

static constexpr int fixtureTimeoutMs = 2000;

namespace
{

const QString blockInterface = QStringLiteral("org.freedesktop.UDisks2.Block");
const QString driveInterface = QStringLiteral("org.freedesktop.UDisks2.Drive");
const QDBusObjectPath diskA(QStringLiteral("/org/freedesktop/UDisks2/block_devices/test_a"));
const QDBusObjectPath diskB(QStringLiteral("/org/freedesktop/UDisks2/block_devices/test_b"));
const QDBusObjectPath driveA(QStringLiteral("/org/freedesktop/UDisks2/drives/test_a"));
const QDBusObjectPath driveB(QStringLiteral("/org/freedesktop/UDisks2/drives/test_b"));

void addDisk(rufus::UDisksObjects& objects, const QDBusObjectPath& disk,
    const QDBusObjectPath& drive, const QByteArray& path)
{
    // These paths are metadata only. Neither the fixture nor client opens them.
    objects[disk][blockInterface] = {
        {QStringLiteral("Drive"), QVariant::fromValue(drive)},
        {QStringLiteral("Device"), path + '\0'},
        {QStringLiteral("Size"), QVariant::fromValue(16ULL * 1024 * 1024 * 1024 + 123)},
        {QStringLiteral("ReadOnly"), false}, {QStringLiteral("HintSystem"), false},
        {QStringLiteral("HintIgnore"), false}, {QStringLiteral("Id"), disk.path()},
        {QStringLiteral("DeviceNumber"), QVariant::fromValue(quint64(2048))}
    };
    objects[drive][driveInterface] = {
        {QStringLiteral("ConnectionBus"), QStringLiteral("usb")},
        {QStringLiteral("Removable"), true}, {QStringLiteral("MediaRemovable"), true},
        {QStringLiteral("MediaAvailable"), true}, {QStringLiteral("Optical"), false},
        {QStringLiteral("Vendor"), QStringLiteral("Test")},
        {QStringLiteral("Model"), QStringLiteral("USB")},
        {QStringLiteral("Serial"), drive.path()},
        {QStringLiteral("TimeDetected"), QVariant::fromValue(quint64(1234567890000000))},
        {QStringLiteral("TimeMediaDetected"), QVariant::fromValue(quint64(1234567890000001))}
    };
}

void stopProcess(QProcess& process)
{
    if (process.state() == QProcess::NotRunning)
        return;
    process.terminate();
    if (!process.waitForFinished(fixtureTimeoutMs))
    {
        process.kill();
        process.waitForFinished(fixtureTimeoutMs);
    }
}

} // namespace

class TestUDisksClient : public QObject
{
    Q_OBJECT

private:
    QDBusMessage control(const QString& method, const QList<QVariant>& args = {})
    {
        QDBusMessage request = QDBusMessage::createMethodCall(QStringLiteral("org.rufus.TestUDisks"),
            QStringLiteral("/org/freedesktop/UDisks2"), QStringLiteral("org.rufus.TestUDisks"), method);
        request.setArguments(args);
        const QDBusMessage reply = connection_->call(request, QDBus::Block, fixtureTimeoutMs);
        if (reply.type() != QDBusMessage::ReplyMessage)
            QTest::qFail(qPrintable(reply.errorMessage()), __FILE__, __LINE__);
        return reply;
    }

    void setObjects()
    {
        control(QStringLiteral("SetObjects"), {QVariant::fromValue(objects_)});
    }

    int stat(const QString& key)
    {
        const QDBusMessage reply = control(QStringLiteral("Stats"));
        if (reply.arguments().isEmpty())
            return -1;
        return qdbus_cast<QVariantMap>(reply.arguments().at(0)).value(key).toInt();
    }

    void notifyAdded(const QDBusObjectPath& path)
    {
        control(QStringLiteral("Added"), {QVariant::fromValue(path), QVariant::fromValue(objects_.value(path))});
    }

    void notifyRemoved(const QDBusObjectPath& path, const QString& interface)
    {
        control(QStringLiteral("Removed"), {QVariant::fromValue(path), QStringList{interface}});
    }

    void notifyChanged(const QDBusObjectPath& path, const QString& interface, const QVariantMap& changed,
        const QStringList& invalidated = {})
    {
        control(QStringLiteral("Changed"), {path.path(), interface, changed, invalidated});
    }

    const rufus::Selection& selection() const
    {
        return controller_->selection();
    }

    QProcess daemon_;
    QProcess fixture_;
    QTemporaryDir busDirectory_;
    std::unique_ptr<QDBusConnection> connection_;
    std::unique_ptr<rufus::UDisksClient> client_;
    std::unique_ptr<rufus::SelectionController> controller_;
    std::unique_ptr<rufus::MainWindow> window_;
    rufus::UDisksObjects objects_;

private slots:
    void initTestCase()
    {
        QVERIFY(busDirectory_.isValid());
        qDBusRegisterMetaType<rufus::UDisksInterfaces>();
        qDBusRegisterMetaType<rufus::UDisksObjects>();
    }

    void init()
    {
        daemon_.start(QStringLiteral(RUFUS_DBUS_DAEMON), {QStringLiteral("--session"),
            QStringLiteral("--nofork"), QStringLiteral("--print-address=1"),
            QStringLiteral("--address=unix:tmpdir=%1").arg(busDirectory_.path())});
        QVERIFY(daemon_.waitForStarted(fixtureTimeoutMs));
        QVERIFY2(daemon_.waitForReadyRead(fixtureTimeoutMs), daemon_.readAllStandardError().constData());
        const QString address = QString::fromUtf8(daemon_.readLine()).trimmed();
        QVERIFY(address.startsWith(QStringLiteral("unix:")));
        connection_ = std::make_unique<QDBusConnection>(QDBusConnection::connectToBus(address,
            QStringLiteral("rufus-private-test")));
        QVERIFY(connection_->isConnected());
        fixture_.start(QCoreApplication::applicationDirPath() + QStringLiteral("/udisks_fixture"), {address});
        QVERIFY(fixture_.waitForStarted(fixtureTimeoutMs));
        QVERIFY2(fixture_.waitForReadyRead(fixtureTimeoutMs), fixture_.readAllStandardError().constData());
        QCOMPARE(fixture_.readLine(), QByteArray("ready\n"));
        objects_.clear();
        addDisk(objects_, diskA, driveA, "/dev/test-a");
        setObjects();
        controller_ = std::make_unique<rufus::SelectionController>();
        client_ = std::make_unique<rufus::UDisksClient>(*connection_);
        connect(client_.get(), &rufus::UDisksClient::devicesChanged,
            controller_.get(), &rufus::SelectionController::setDiscovery);
    }

    void cleanup()
    {
        window_.reset();
        client_.reset();
        controller_.reset();
        if (connection_ && connection_->isConnected() && fixture_.state() == QProcess::Running)
        {
            const int unexpected = stat(QStringLiteral("unexpected"));
            control(QStringLiteral("Quit"));
            const bool finished = fixture_.waitForFinished(fixtureTimeoutMs);
            connection_.reset();
            QDBusConnection::disconnectFromBus(QStringLiteral("rufus-private-test"));
            stopProcess(fixture_);
            stopProcess(daemon_);
            QVERIFY(finished);
            QVERIFY2(fixture_.exitCode() == 0, fixture_.readAllStandardError().constData());
            QCOMPARE(unexpected, 0);
            return;
        }
        connection_.reset();
        QDBusConnection::disconnectFromBus(QStringLiteral("rufus-private-test"));
        stopProcess(fixture_);
        stopProcess(daemon_);
    }

    void cleanupTestCase()
    {
        connection_.reset();
        QDBusConnection::disconnectFromBus(QStringLiteral("rufus-private-test"));
        stopProcess(fixture_);
        stopProcess(daemon_);
    }

    void enumerationFeedsWindow()
    {
        addDisk(objects_, diskB, driveB, "/dev/test-b");
        setObjects();
        window_ = std::make_unique<rufus::MainWindow>(*controller_);
        window_->show();
        client_->start();
        QTRY_VERIFY2_WITH_TIMEOUT(selection().devices.size() == 2,
            qPrintable(selection().discoveryProblem), fixtureTimeoutMs);
        QVERIFY(selection().discoveryAvailable);
        QCOMPARE(selection().devices.at(0).size, 16ULL * 1024 * 1024 * 1024 + 123);
        QCOMPARE(selection().devices.at(0).path, QStringLiteral("/dev/test-a"));
        QCOMPARE(selection().devices.at(0).objectPath, diskA.path());
        QCOMPARE(selection().devices.at(0).number, quint64(2048));
        QVERIFY(selection().devices.at(0).serviceOwner.startsWith(QLatin1Char(':')));
        QVERIFY(!selection().devices.at(0).identity.isEmpty());
        QCOMPARE(selection().devices.at(0).name, selection().devices.at(1).name);
        QVERIFY(selection().devices.at(0).id != selection().devices.at(1).id);
        auto* combo = window_->findChild<QComboBox*>(QStringLiteral("deviceCombo"));
        auto* start = window_->findChild<QPushButton*>(QStringLiteral("startButton"));
        auto* status = window_->findChild<QLabel*>(QStringLiteral("statusLabel"));
        QVERIFY(combo && start && status);
        QCOMPARE(combo->count(), 2);
        QCOMPARE(combo->currentIndex(), -1);
        QVERIFY(status->text().contains(QStringLiteral("2 device(s) found")));
        combo->setFocus();
        QTest::keyClick(combo, Qt::Key_Home);
        QCOMPARE(selection().deviceId, selection().devices.at(0).id);
        QVERIFY(!start->isEnabled());
    }

    void excludedDevices_data()
    {
        QTest::addColumn<QString>("reason");
        const QStringList reasons = {QStringLiteral("internal"), QStringLiteral("fixed-usb"),
            QStringLiteral("readonly"), QStringLiteral("system"), QStringLiteral("ignored"),
            QStringLiteral("empty"), QStringLiteral("optical"), QStringLiteral("partition"),
            QStringLiteral("loop"), QStringLiteral("missing-drive"), QStringLiteral("missing-flags"),
            QStringLiteral("zero-size"), QStringLiteral("overflow-size"),
            QStringLiteral("unterminated-path"), QStringLiteral("embedded-nul"), QStringLiteral("non-device-path")};
        for (const QString& reason : reasons)
            QTest::newRow(qPrintable(reason)) << reason;
    }

    void excludedDevices()
    {
        QFETCH(QString, reason);
        QVariantMap& block = objects_[diskA][blockInterface];
        QVariantMap& drive = objects_[driveA][driveInterface];
        if (reason == QStringLiteral("internal"))
            drive[QStringLiteral("ConnectionBus")] = QStringLiteral("ata");
        else if (reason == QStringLiteral("fixed-usb"))
            drive[QStringLiteral("MediaRemovable")] = false;
        else if (reason == QStringLiteral("readonly"))
            block[QStringLiteral("ReadOnly")] = true;
        else if (reason == QStringLiteral("system"))
            block[QStringLiteral("HintSystem")] = true;
        else if (reason == QStringLiteral("ignored"))
            block[QStringLiteral("HintIgnore")] = true;
        else if (reason == QStringLiteral("empty"))
            drive[QStringLiteral("MediaAvailable")] = false;
        else if (reason == QStringLiteral("optical"))
            drive[QStringLiteral("Optical")] = true;
        else if (reason == QStringLiteral("partition"))
            objects_[diskA][QStringLiteral("org.freedesktop.UDisks2.Partition")] = {};
        else if (reason == QStringLiteral("loop"))
            block[QStringLiteral("Drive")] = QVariant::fromValue(QDBusObjectPath(QStringLiteral("/")));
        else if (reason == QStringLiteral("missing-drive"))
            objects_.remove(driveA);
        else if (reason == QStringLiteral("missing-flags"))
            block.remove(QStringLiteral("ReadOnly"));
        else if (reason == QStringLiteral("zero-size"))
            block[QStringLiteral("Size")] = QVariant::fromValue(quint64(0));
        else if (reason == QStringLiteral("overflow-size"))
            block[QStringLiteral("Size")] = QVariant::fromValue(~quint64(0));
        else if (reason == QStringLiteral("unterminated-path"))
            block[QStringLiteral("Device")] = QByteArray("/dev/test-a");
        else if (reason == QStringLiteral("embedded-nul"))
            block[QStringLiteral("Device")] = QByteArray("/dev/test\0a\0", 12);
        else if (reason == QStringLiteral("non-device-path"))
            block[QStringLiteral("Device")] = QByteArray("/test/a\0", 8);
        setObjects();
        client_->start();
        QTRY_VERIFY_WITH_TIMEOUT(selection().discoveryAvailable, fixtureTimeoutMs);
        QVERIFY(selection().devices.isEmpty());
    }

    void noDevicesIsSuccessfulDiscovery()
    {
        objects_.clear();
        setObjects();
        client_->start();
        QTRY_VERIFY_WITH_TIMEOUT(selection().discoveryAvailable, fixtureTimeoutMs);
        QVERIFY(selection().devices.isEmpty());
        QVERIFY(selection().discoveryProblem.isEmpty());
    }

    void hotplugPreservesUnrelatedSelection()
    {
        client_->start();
        QTRY_COMPARE_WITH_TIMEOUT(selection().devices.size(), 1, fixtureTimeoutMs);
        const QString id = selection().devices.first().id;
        controller_->selectDevice(id);
        addDisk(objects_, diskB, driveB, "/dev/test-b");
        setObjects();
        notifyAdded(driveB);
        notifyAdded(diskB);
        QTRY_COMPARE_WITH_TIMEOUT(selection().devices.size(), 2, fixtureTimeoutMs);
        QCOMPARE(selection().deviceId, id);
        objects_[driveA][driveInterface][QStringLiteral("Model")] = QStringLiteral("Renamed");
        setObjects();
        notifyChanged(driveA, driveInterface, {{QStringLiteral("Model"), QStringLiteral("Renamed")}});
        QTRY_COMPARE_WITH_TIMEOUT(selection().devices.first().name, QStringLiteral("Test Renamed"), fixtureTimeoutMs);
        QCOMPARE(selection().deviceId, id);
        objects_.remove(diskA);
        setObjects();
        notifyRemoved(diskA, blockInterface);
        QTRY_VERIFY_WITH_TIMEOUT(selection().deviceId.isEmpty(), fixtureTimeoutMs);
        QTRY_COMPARE_WITH_TIMEOUT(selection().devices.size(), 1, fixtureTimeoutMs);
        QCOMPARE(selection().devices.first().path, QStringLiteral("/dev/test-b"));
    }

    void replacementAtSamePathClearsSelection()
    {
        client_->start();
        QTRY_COMPARE_WITH_TIMEOUT(selection().devices.size(), 1, fixtureTimeoutMs);
        const QString id = selection().devices.first().id;
        controller_->selectDevice(id);
        // Even identical metadata cannot preserve an identity after removal.
        notifyRemoved(diskA, blockInterface);
        notifyAdded(diskA);
        QTRY_VERIFY_WITH_TIMEOUT(selection().deviceId.isEmpty(), fixtureTimeoutMs);
        QTRY_COMPARE_WITH_TIMEOUT(selection().devices.size(), 1, fixtureTimeoutMs);
        QVERIFY(selection().devices.first().id != id);
    }

    void changedMediaInvalidatesBeforeReply()
    {
        client_->start();
        QTRY_COMPARE_WITH_TIMEOUT(selection().devices.size(), 1, fixtureTimeoutMs);
        const QString id = selection().devices.first().id;
        controller_->selectDevice(id);
        control(QStringLiteral("Hold"), {true});
        objects_[driveA][driveInterface][QStringLiteral("TimeMediaDetected")] = QVariant::fromValue(quint64(99));
        setObjects();
        notifyChanged(driveA, driveInterface, {}, {QStringLiteral("TimeMediaDetected")});
        QTRY_VERIFY_WITH_TIMEOUT(selection().deviceId.isEmpty(), fixtureTimeoutMs);
        QVERIFY(selection().devices.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(stat(QStringLiteral("held")), 1, fixtureTimeoutMs);
        control(QStringLiteral("ReplyOldest"));
        QTRY_COMPARE_WITH_TIMEOUT(selection().devices.size(), 1, fixtureTimeoutMs);
        QVERIFY(selection().devices.first().id != id);
    }

    void staleReplyCannotRestoreRemovedDevice()
    {
        control(QStringLiteral("Hold"), {true});
        client_->start();
        QTRY_COMPARE_WITH_TIMEOUT(stat(QStringLiteral("held")), 1, fixtureTimeoutMs);
        objects_.clear();
        setObjects();
        control(QStringLiteral("Hold"), {false});
        notifyRemoved(diskA, blockInterface);
        QTRY_VERIFY_WITH_TIMEOUT(selection().discoveryAvailable, fixtureTimeoutMs);
        QVERIFY(selection().devices.isEmpty());
        QSignalSpy snapshots(client_.get(), &rufus::UDisksClient::devicesChanged);
        control(QStringLiteral("ReplyOldest"));
        addDisk(objects_, diskB, driveB, "/dev/test-b");
        setObjects();
        notifyAdded(diskB);
        QTRY_COMPARE_WITH_TIMEOUT(selection().devices.size(), 1, fixtureTimeoutMs);
        QCOMPARE(selection().devices.first().path, QStringLiteral("/dev/test-b"));
        for (const QList<QVariant>& snapshot : snapshots)
        {
            const QList<rufus::Device> devices = qvariant_cast<QList<rufus::Device>>(snapshot.at(0));
            for (const rufus::Device& device : devices)
                QVERIFY(device.path != QStringLiteral("/dev/test-a"));
        }
    }

    void serviceRestartClearsIdentityAndObsoleteReply()
    {
        client_->start();
        QTRY_COMPARE_WITH_TIMEOUT(selection().devices.size(), 1, fixtureTimeoutMs);
        const QString id = selection().devices.first().id;
        controller_->selectDevice(id);
        control(QStringLiteral("Hold"), {true});
        notifyChanged(driveA, driveInterface, {{QStringLiteral("Model"), QStringLiteral("Test USB")}});
        QTRY_COMPARE_WITH_TIMEOUT(stat(QStringLiteral("held")), 1, fixtureTimeoutMs);
        control(QStringLiteral("ReleaseName"));
        QTRY_VERIFY_WITH_TIMEOUT(!selection().discoveryAvailable, fixtureTimeoutMs);
        QVERIFY(selection().devices.isEmpty());
        QVERIFY(selection().deviceId.isEmpty());
        QVERIFY(!selection().discoveryProblem.isEmpty());
        control(QStringLiteral("Hold"), {false});
        control(QStringLiteral("AcquireName"));
        QTRY_COMPARE_WITH_TIMEOUT(selection().devices.size(), 1, fixtureTimeoutMs);
        QVERIFY(selection().devices.first().id != id);
        const QString replacement = selection().devices.first().id;
        control(QStringLiteral("ReplyOldest"));
        objects_[driveA][driveInterface][QStringLiteral("Model")] = QStringLiteral("After restart");
        setObjects();
        notifyChanged(driveA, driveInterface, {{QStringLiteral("Model"), QStringLiteral("After restart")}});
        QTRY_COMPARE_WITH_TIMEOUT(selection().devices.first().name, QStringLiteral("Test After restart"), fixtureTimeoutMs);
        QCOMPARE(selection().devices.first().id, replacement);
    }

    void failedReplies_data()
    {
        QTest::addColumn<QString>("mode");
        QTest::newRow("service-error") << QStringLiteral("error");
        QTest::newRow("bad-signature") << QStringLiteral("malformed");
    }

    void failedReplies()
    {
        QFETCH(QString, mode);
        client_->start();
        QTRY_COMPARE_WITH_TIMEOUT(selection().devices.size(), 1, fixtureTimeoutMs);
        controller_->selectDevice(selection().devices.first().id);
        control(QStringLiteral("Mode"), {mode});
        notifyChanged(driveA, driveInterface, {{QStringLiteral("Model"), QStringLiteral("USB")}});
        QTRY_VERIFY_WITH_TIMEOUT(!selection().discoveryAvailable, fixtureTimeoutMs);
        QVERIFY(selection().devices.isEmpty());
        QVERIFY(selection().deviceId.isEmpty());
        QVERIFY(!selection().discoveryProblem.isEmpty());
        control(QStringLiteral("Mode"), {QString()});
        // Recovery uses the bounded retry, with no hotplug or user action.
        QTRY_COMPARE_WITH_TIMEOUT(selection().devices.size(), 1, 4000);
        QVERIFY(selection().discoveryAvailable);
    }

    void pendingDiscoveryKeepsWindowResponsive()
    {
        control(QStringLiteral("Hold"), {true});
        window_ = std::make_unique<rufus::MainWindow>(*controller_);
        window_->show();
        client_->start();
        QTRY_COMPARE_WITH_TIMEOUT(stat(QStringLiteral("held")), 1, fixtureTimeoutMs);
        auto* log = window_->findChild<QToolButton*>(QStringLiteral("logButton"));
        QVERIFY(log);
        QTest::mouseClick(log, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(window_->findChild<QWidget*>(QStringLiteral("logWindow")), 1000);
        control(QStringLiteral("ReplyOldest"));
        QTRY_VERIFY_WITH_TIMEOUT(selection().discoveryAvailable, fixtureTimeoutMs);
    }

    void disconnectedBusDoesNotFallBack()
    {
        const QDBusConnection disconnected = QDBusConnection::connectToBus(
            QStringLiteral("unix:path=%1/absent").arg(busDirectory_.path()), QStringLiteral("rufus-absent-test"));
        QVERIFY(!disconnected.isConnected());
        rufus::UDisksClient absent(disconnected);
        QSignalSpy result(&absent, &rufus::UDisksClient::devicesChanged);
        absent.start();
        QCOMPARE(result.size(), 1);
        QVERIFY(qvariant_cast<QList<rufus::Device>>(result.first().at(0)).isEmpty());
        QVERIFY(!result.first().at(1).toString().isEmpty());
        QCOMPARE(stat(QStringLiteral("calls")), 0);
        QDBusConnection::disconnectFromBus(QStringLiteral("rufus-absent-test"));
    }

    void missingServiceRecoversWhenStarted()
    {
        control(QStringLiteral("ReleaseName"));
        client_->start();
        QTRY_VERIFY_WITH_TIMEOUT(!selection().discoveryProblem.isEmpty(), fixtureTimeoutMs);
        QVERIFY(!selection().discoveryAvailable);
        QVERIFY(selection().devices.isEmpty());
        control(QStringLiteral("AcquireName"));
        QTRY_COMPARE_WITH_TIMEOUT(selection().devices.size(), 1, fixtureTimeoutMs);
        QVERIFY(selection().discoveryProblem.isEmpty());
    }

    void timedOutRequestClearsPreviousDevices()
    {
        client_->start();
        QTRY_COMPARE_WITH_TIMEOUT(selection().devices.size(), 1, fixtureTimeoutMs);
        controller_->selectDevice(selection().devices.first().id);
        control(QStringLiteral("Hold"), {true});
        notifyChanged(driveA, driveInterface, {{QStringLiteral("Model"), QStringLiteral("USB")}});
        QTRY_COMPARE_WITH_TIMEOUT(stat(QStringLiteral("held")), 1, fixtureTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(!selection().discoveryAvailable, 6500);
        QVERIFY(selection().devices.isEmpty());
        QVERIFY(selection().deviceId.isEmpty());
        QVERIFY(!selection().discoveryProblem.isEmpty());
    }

    void snapshotIdentityChangeClearsSelection()
    {
        client_->start();
        QTRY_COMPARE_WITH_TIMEOUT(selection().devices.size(), 1, fixtureTimeoutMs);
        const QString id = selection().devices.first().id;
        controller_->selectDevice(id);
        objects_[driveA][driveInterface][QStringLiteral("Serial")] = QStringLiteral("replacement");
        setObjects();
        // Even without a removal signal, fresh identity metadata invalidates selection.
        notifyChanged(driveA, driveInterface, {{QStringLiteral("Model"), QStringLiteral("USB")}});
        QTRY_VERIFY_WITH_TIMEOUT(selection().deviceId.isEmpty(), fixtureTimeoutMs);
        QCOMPARE(selection().devices.size(), 1);
        QVERIFY(selection().devices.first().id != id);
    }

    void busDisconnectionClearsSelection()
    {
        client_->start();
        QTRY_COMPARE_WITH_TIMEOUT(selection().devices.size(), 1, fixtureTimeoutMs);
        controller_->selectDevice(selection().devices.first().id);
        QCOMPARE(stat(QStringLiteral("unexpected")), 0);
        stopProcess(daemon_);
        QTRY_VERIFY_WITH_TIMEOUT(!selection().discoveryAvailable, fixtureTimeoutMs);
        QVERIFY(selection().devices.isEmpty());
        QVERIFY(selection().deviceId.isEmpty());
        QVERIFY(selection().discoveryProblem.contains(QStringLiteral("connection lost")));
    }
};

QTEST_MAIN(TestUDisksClient)
#include "test_udisks_client.moc"
