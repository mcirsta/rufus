// SPDX-License-Identifier: GPL-3.0-or-later
#include "native_disk_fixture.h"
#include "platform/udisks_client.h"
#include "qt/main_window.h"
#include "qt/selection_controller.h"

#include <QComboBox>
#include <QDBusArgument>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QLabel>
#include <QLineEdit>
#include <QProcess>
#include <QPushButton>
#include <QSignalSpy>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>

#include <memory>
#include <sys/stat.h>

static constexpr int fixtureTimeoutMs = 2000;

namespace
{

const QString blockInterface = QStringLiteral("org.freedesktop.UDisks2.Block");
const QString driveInterface = QStringLiteral("org.freedesktop.UDisks2.Drive");
const QString partitionTableInterface = QStringLiteral("org.freedesktop.UDisks2.PartitionTable");
const QString partitionInterface = QStringLiteral("org.freedesktop.UDisks2.Partition");
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
        {QStringLiteral("DeviceNumber"), QVariant::fromValue(quint64(disk == diskA ? 2048 : 2064))}
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
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, busDirectory_.filePath(QStringLiteral("settings")));
        qputenv("XDG_DATA_HOME", busDirectory_.filePath(QStringLiteral("application-data")).toUtf8());
        qDBusRegisterMetaType<rufus::UDisksInterfaces>();
        qDBusRegisterMetaType<rufus::UDisksObjects>();
    }

    void init()
    {
        ResetTestNativeDisks();
        QSettings(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("Rufus"), QStringLiteral("Linux")).clear();
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
        connect(controller_.get(), &rufus::SelectionController::deviceClassesChanged,
            client_.get(), &rufus::UDisksClient::setDeviceClasses);
        connect(controller_.get(), &rufus::SelectionController::fixedUsbChanged,
            client_.get(), &rufus::UDisksClient::setAllowFixedUsb);
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

    void explicitEnumerationDiagnostics()
    {
        QSignalSpy logs(client_.get(),&rufus::UDisksClient::logMessage);
        client_->start(); QTRY_COMPARE_WITH_TIMEOUT(selection().devices.size(),1,fixtureTimeoutMs);
        QCOMPARE(logs.size(),0);
        client_->setUsbDebug(true);
        QTRY_VERIFY_WITH_TIMEOUT(!logs.isEmpty(),fixtureTimeoutMs);
        bool listed=false;
        for (const auto& row : logs) {
            const QString text=row[0].toString();
            if (text.contains("/dev/test-a") && text.contains("eligible=1")) listed=true;
            QVERIFY(!text.contains(QChar::Null));
        }
        QVERIFY(listed); QCOMPARE(stat(QStringLiteral("opens")),0);
        client_->setUsbDebug(false);
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
        QCOMPARE(combo->currentIndex(), 0);
        QVERIFY(status->text().contains(QStringLiteral("2 device(s) found")));
        QCOMPARE(selection().deviceId, selection().devices.at(0).id);
        QCOMPARE(stat(QStringLiteral("opens")), 0);
        QVERIFY(!start->isEnabled());
    }

    void mountedLabelChangesPreserveSelection()
    {
        QTemporaryDir mount; QVERIFY(mount.isValid());
        QFile inf(mount.filePath(QStringLiteral("autorun.inf")));
        QVERIFY(inf.open(QIODevice::WriteOnly));
        const QByteArray bytes=QByteArray::fromHex("efbbbf")+"[autorun]\r\nlabel=Long installer name\r\n";
        QCOMPARE(inf.write(bytes),bytes.size()); inf.close();
        struct ::stat facts={}; QCOMPARE(::stat(QFile::encodeName(mount.path()).constData(),&facts),0);
        const QString fs=QStringLiteral("org.freedesktop.UDisks2.Filesystem");
        const QDBusObjectPath volume(QStringLiteral("/org/freedesktop/UDisks2/block_devices/test_a1"));
        objects_[volume][blockInterface]={{"IdUsage","filesystem"},{"IdLabel","SHORT"},{"IdType","vfat"},
            {"DeviceNumber",quint64(facts.st_dev)},{"HintSystem",false},{"HintIgnore",false}};
        objects_[volume][partitionInterface]={{"Table",QVariant::fromValue(diskA)},{"Offset",quint64(1048576)}};
        objects_[volume][fs]={{"MountPoints",QVariant::fromValue(QList<QByteArray>{QFile::encodeName(mount.path())+'\0'})}};
        setObjects(); client_->start();
        QTRY_COMPARE_WITH_TIMEOUT(selection().devices.size(),1,fixtureTimeoutMs);
        const QString id=selection().devices.first().id;
        controller_->selectDevice(id);
        QTRY_COMPARE_WITH_TIMEOUT(selection().devices.first().volumeLabel,QStringLiteral("Long installer name"),fixtureTimeoutMs);
        QCOMPARE(selection().devices.first().mountedVolumes,1); QCOMPARE(selection().deviceId,id);
        const QStringList identity=selection().devices.first().identity;
        objects_[volume][fs]["MountPoints"]=QVariant::fromValue(QList<QByteArray>{}); setObjects();
        notifyChanged(volume,fs,objects_[volume][fs]);
        QTRY_COMPARE_WITH_TIMEOUT(selection().devices.first().volumeLabel,QStringLiteral("SHORT"),fixtureTimeoutMs);
        QCOMPARE(selection().devices.first().mountedVolumes,0); QCOMPARE(selection().deviceId,id);
        QCOMPARE(selection().devices.first().identity,identity);
        // Mount notification alone refreshes extended metadata; no authorization.
        QVERIFY(inf.open(QIODevice::WriteOnly|QIODevice::Truncate));
        const QByteArray changed=QByteArray::fromHex("efbbbf")+"label=Replaced label";
        QCOMPARE(inf.write(changed),changed.size()); inf.close();
        objects_[volume][fs]["MountPoints"]=QVariant::fromValue(QList<QByteArray>{QFile::encodeName(mount.path())+'\0'}); setObjects();
        notifyChanged(volume,fs,objects_[volume][fs]);
        QTRY_COMPARE_WITH_TIMEOUT(selection().devices.first().volumeLabel,QStringLiteral("Replaced label"),fixtureTimeoutMs);
        QCOMPARE(selection().deviceId,id); QCOMPARE(stat(QStringLiteral("opens")),0);
    }

    void usbMediaClasses_data()
    {
        QTest::addColumn<bool>("removable");
        QTest::addColumn<bool>("mediaRemovable");
        QTest::newRow("removable-drive-and-media") << true << true;
        QTest::newRow("removable-enclosure-fixed-media") << true << false;
        QTest::newRow("fixed-drive-and-media") << false << false;
        QTest::newRow("fixed-drive-removable-media") << false << true;
    }

    void usbMediaClasses()
    {
        QFETCH(bool, removable); QFETCH(bool, mediaRemovable);
        objects_[driveA][driveInterface][QStringLiteral("Removable")] = removable;
        objects_[driveA][driveInterface][QStringLiteral("MediaRemovable")] = mediaRemovable;
        rufus::Device candidate;
        QVERIFY(rufus::DescribeUDisksDevice(objects_, diskA.path(), candidate, true));
        QCOMPARE(candidate.removable, removable);
        QCOMPARE(candidate.mediaRemovable, mediaRemovable);
        QVERIFY(!candidate.readOnly);
        // Read-only opt-in cannot leak into the default discovery/write checks.
        rufus::Device ordinary;
        const bool defaultEligible = true; // Physical flags do not replace the shared filter.
        QCOMPARE(rufus::DescribeUDisksDevice(objects_, diskA.path(), ordinary), defaultEligible);
        QStringList mounts;
        QCOMPARE(rufus::ListUDisksTargetMounts(objects_, candidate, 0, mounts).isEmpty(), defaultEligible);
        QVERIFY(rufus::ListUDisksTargetMounts(objects_, candidate, 0, mounts, true).isEmpty());
        QVERIFY(rufus::CheckUDisksTargetUse(objects_, candidate, 0, true).isEmpty());
        QVERIFY(!rufus::CheckUDisksTargetUse(objects_, candidate, candidate.number, true).isEmpty());
        objects_[diskA][blockInterface][QStringLiteral("HintSystem")] = true;
        QVERIFY(!rufus::CheckUDisksTargetUse(objects_, candidate, 0, true).isEmpty());
        objects_[diskA][blockInterface][QStringLiteral("HintSystem")] = false;
        setObjects();
        client_->start();
        QTRY_VERIFY_WITH_TIMEOUT(selection().discoveryAvailable, fixtureTimeoutMs);
        QCOMPARE(selection().devices.size(), defaultEligible ? 1 : 0);
        controller_->setAllowFixedUsb(true);
        QTRY_COMPARE_WITH_TIMEOUT(selection().devices.size(), 1, fixtureTimeoutMs);
        QCOMPARE(selection().devices.first().removable, removable);
        QCOMPARE(selection().devices.first().mediaRemovable, mediaRemovable);
        controller_->setAllowFixedUsb(false);
        QTRY_COMPARE_WITH_TIMEOUT(selection().devices.size(), defaultEligible ? 1 : 0, fixtureTimeoutMs);
        QCOMPARE(stat(QStringLiteral("opens")), 0);
    }

    void advancedDeviceClasses_data()
    {
        QTest::addColumn<int>("kind");
        for (int kind : {2, 4, 8, 16}) QTest::newRow(qPrintable(QString::number(kind))) << kind;
    }
    void advancedDeviceClasses()
    {
        QFETCH(int, kind);
        auto& facts = TestNativeDisk(2048); facts.deviceClass = rufus::DeviceClass(kind); facts.usbTransport = false;
        objects_[driveA][driveInterface]["ConnectionBus"] = QStringLiteral("ata");
        objects_[diskA][blockInterface]["HintSystem"] = true;
        setObjects(); client_->start();
        QTRY_VERIFY_WITH_TIMEOUT(selection().discoveryAvailable, fixtureTimeoutMs);
        const bool initiallyVisible = kind == 2 || kind == 16;
        QCOMPARE(selection().devices.size(), initiallyVisible ? 1 : 0);
        if (kind == 4) controller_->toggleVmwareDisks();
        if (kind == 8) controller_->toggleNonUsbRemovable();
        QTRY_COMPARE_WITH_TIMEOUT(selection().devices.size(), 1, fixtureTimeoutMs);
        controller_->selectDevice(selection().devices.first().id);
        QVERIFY(!selection().deviceId.isEmpty());
        if (kind == 4) controller_->toggleVmwareDisks();
        else if (kind == 8) controller_->toggleNonUsbRemovable();
        else if (kind == 2) controller_->toggleVirtualDisks();
        else return;
        QTRY_COMPARE_WITH_TIMEOUT(selection().devices.size(), 0, fixtureTimeoutMs);
        QVERIFY(selection().deviceId.isEmpty());
        QCOMPARE(stat(QStringLiteral("opens")), 0);
    }

    void nativeClassification_data()
    {
        QTest::addColumn<QString>("kind");
        QTest::addColumn<bool>("visible");
        QTest::addColumn<bool>("protectedAcquisition");
        QTest::newRow("fixed-small-flash") << QStringLiteral("fixed") << true << true;
        QTest::newRow("native-fixed-udisks-removable") << QStringLiteral("native-fixed") << true << true;
        QTest::newRow("udisks-fixed-native-removable") << QStringLiteral("udisks-fixed") << true << true;
        QTest::newRow("removable-large-hdd") << QStringLiteral("large") << false << false;
        QTest::newRow("unknown-removal") << QStringLiteral("unknown") << false << true;
        QTest::newRow("card-500-gib") << QStringLiteral("card") << true << true;
        QTest::newRow("card-over-500-gib") << QStringLiteral("large-card") << false << true;
        QTest::newRow("compatibility-card") << QStringLiteral("compatibility") << true << true;
        QTest::newRow("current-media-wins") << QStringLiteral("current") << false << true;
    }

    void nativeClassification()
    {
        QFETCH(QString, kind); QFETCH(bool, visible); QFETCH(bool, protectedAcquisition);
        auto& facts = TestNativeDisk(2048);
        QVariantMap& drive = objects_[driveA][driveInterface];
        QVariantMap& block = objects_[diskA][blockInterface];
        if (kind == QStringLiteral("fixed") || kind == QStringLiteral("native-fixed"))
            facts.mediaRemoval = rufus::MediaRemoval::Fixed;
        if (kind == QStringLiteral("fixed") || kind == QStringLiteral("udisks-fixed"))
            drive[QStringLiteral("MediaRemovable")] = false;
        if (kind == QStringLiteral("large"))
            block[QStringLiteral("Size")] = quint64(2000) * 1024 * 1024 * 1024;
        const bool cardCase = kind.contains(QStringLiteral("card")) || kind == QStringLiteral("compatibility")
            || kind == QStringLiteral("current");
        if (kind == QStringLiteral("unknown") || cardCase)
            facts.mediaRemoval = rufus::MediaRemoval::Unknown;
        if (cardCase)
        {
            block[QStringLiteral("Size")] = quint64(500) * 1024 * 1024 * 1024;
            drive[QStringLiteral("Media")] = QStringLiteral("flash_sdxc");
            if (kind == QStringLiteral("large-card"))
                block[QStringLiteral("Size")] = block.value(QStringLiteral("Size")).toULongLong() + 512;
            if (kind == QStringLiteral("compatibility") || kind == QStringLiteral("current"))
            {
                drive[QStringLiteral("MediaCompatibility")] = QStringList{QStringLiteral("flash_sdxc")};
                drive[QStringLiteral("Media")] = kind == QStringLiteral("current") ? QStringLiteral("thumb") : QString();
            }
        }
        rufus::Device candidate;
        QCOMPARE(rufus::DescribeUDisksDevice(objects_, diskA.path(), candidate), visible);
        QVERIFY(rufus::DescribeUDisksDevice(objects_, diskA.path(), candidate, true));
        QCOMPARE(candidate.defaultVisible, visible);
        QCOMPARE(candidate.requiresNativeProtection, protectedAcquisition);
        QCOMPARE(rufus::CheckUDisksTargetUse(objects_, candidate, 0).isEmpty(), visible);
        QVERIFY(rufus::CheckUDisksTargetUse(objects_, candidate, 0, true).isEmpty());
        setObjects();
        client_->start();
        QTRY_VERIFY_WITH_TIMEOUT(selection().discoveryAvailable, fixtureTimeoutMs);
        QCOMPARE(selection().devices.size(), visible ? 1 : 0);
        QVERIFY(!selection().allowFixedUsb);
        controller_->setAllowFixedUsb(true);
        QTRY_COMPARE_WITH_TIMEOUT(selection().devices.size(), 1, fixtureTimeoutMs);
        QCOMPARE(selection().devices.first().defaultVisible, visible);
        controller_->setAllowFixedUsb(false);
        QTRY_COMPARE_WITH_TIMEOUT(selection().devices.size(), visible ? 1 : 0, fixtureTimeoutMs);
        QCOMPARE(stat(QStringLiteral("opens")), 0);
    }

    void nativeSnapshotUnavailable()
    {
        testNativeDiskError = ESTALE;
        rufus::Device candidate;
        QVERIFY(!rufus::DescribeUDisksDevice(objects_, diskA.path(), candidate));
        QVERIFY(!rufus::DescribeUDisksDevice(objects_, diskA.path(), candidate, true));
        client_->start();
        QTRY_VERIFY_WITH_TIMEOUT(selection().discoveryAvailable, fixtureTimeoutMs);
        QVERIFY(selection().devices.isEmpty());
        controller_->setAllowFixedUsb(true);
        QSignalSpy refreshed(client_.get(), &rufus::UDisksClient::devicesChanged);
        QTRY_VERIFY_WITH_TIMEOUT(!refreshed.isEmpty(), fixtureTimeoutMs);
        QVERIFY(selection().devices.isEmpty());
    }

    void nativeChangeInvalidatesSelection()
    {
        client_->start();
        QTRY_COMPARE_WITH_TIMEOUT(selection().devices.size(), 1, fixtureTimeoutMs);
        const QString original = selection().devices.first().id;
        controller_->selectDevice(original);
        TestNativeDisk(2048).diskSequence++;
        // An ordinary content refresh must still compare native identity.
        notifyChanged(diskA, blockInterface, {{QStringLiteral("IdLabel"), QStringLiteral("new label")}});
        QTRY_VERIFY_WITH_TIMEOUT(selection().devices.first().id != original, fixtureTimeoutMs);
        QVERIFY(selection().deviceId.isEmpty());
        QCOMPARE(stat(QStringLiteral("opens")), 0);
    }

    void geometryIdentity()
    {
        rufus::Device before, current;
        QVERIFY(rufus::DescribeUDisksDevice(objects_, diskA.path(), before));
        QCOMPARE(before.logicalSectorSize, quint32(512));
        QCOMPARE(before.physicalSectorSize, quint32(512));
        TestNativeDisk(2048).physicalSectorSize = 4096;
        QVERIFY(rufus::DescribeUDisksDevice(objects_, diskA.path(), current));
        QCOMPARE(current.logicalSectorSize, quint32(512));
        QCOMPARE(current.physicalSectorSize, quint32(4096));
        QVERIFY(before.identity != current.identity);
        before = current;
        TestNativeDisk(2048).logicalSectorSize = 4096;
        QVERIFY(rufus::DescribeUDisksDevice(objects_, diskA.path(), current));
        QCOMPARE(current.logicalSectorSize, quint32(4096));
        QVERIFY(before.identity != current.identity);
    }

    void fixedUsbDiscoveryRefresh()
    {
        TestNativeDisk(2048).model = "HDD";
        objects_[driveA][driveInterface][QStringLiteral("MediaRemovable")] = false;
        addDisk(objects_, diskB, driveB, "/dev/test-b");
        setObjects();
        client_->start();
        QTRY_COMPARE_WITH_TIMEOUT(selection().devices.size(), 1, fixtureTimeoutMs);
        const QString original = selection().deviceId;
        controller_->setAllowFixedUsb(true);
        QTRY_COMPARE_WITH_TIMEOUT(selection().devices.size(), 2, fixtureTimeoutMs);
        QCOMPARE(selection().deviceId, original);
        QString fixedId;
        for (const auto& device : selection().devices)
            if (!device.mediaRemovable) fixedId = device.id;
        QVERIFY(!fixedId.isEmpty());
        controller_->selectDevice(fixedId);
        controller_->setAllowFixedUsb(false);
        QVERIFY(selection().deviceId.isEmpty()); // Never bind old consent to the remaining disk.
        QTRY_COMPARE_WITH_TIMEOUT(selection().devices.size(), 1, fixtureTimeoutMs);
        QCOMPARE(selection().devices.first().id, original);
        controller_->setAllowFixedUsb(true);
        QTRY_COMPARE_WITH_TIMEOUT(selection().devices.size(), 2, fixtureTimeoutMs);
        QVERIFY(selection().deviceId.isEmpty());
        // Rapid policy changes discard in-flight snapshots from the old policy.
        controller_->setAllowFixedUsb(false);
        controller_->setAllowFixedUsb(true);
        controller_->setAllowFixedUsb(false);
        QTRY_COMPARE_WITH_TIMEOUT(selection().devices.size(), 1, fixtureTimeoutMs);
        QVERIFY(selection().devices.first().mediaRemovable);
        QCOMPARE(stat(QStringLiteral("opens")), 0);
    }

    void fixedUsbOldReplyCannotRestoreOptIn()
    {
        TestNativeDisk(2048).model = "HDD";
        objects_[driveA][driveInterface][QStringLiteral("MediaRemovable")] = false;
        setObjects();
        client_->start();
        QTRY_VERIFY_WITH_TIMEOUT(selection().discoveryAvailable, fixtureTimeoutMs);
        QVERIFY(selection().devices.isEmpty());
        control(QStringLiteral("Hold"), {true});
        controller_->setAllowFixedUsb(true);
        QTRY_COMPARE_WITH_TIMEOUT(stat(QStringLiteral("held")), 1, fixtureTimeoutMs);
        control(QStringLiteral("Hold"), {false});
        QSignalSpy snapshots(client_.get(), &rufus::UDisksClient::devicesChanged);
        controller_->setAllowFixedUsb(false);
        control(QStringLiteral("ReplyOldest"));
        addDisk(objects_, diskB, driveB, "/dev/test-b");
        setObjects();
        notifyAdded(diskB);
        QTRY_COMPARE_WITH_TIMEOUT(selection().devices.size(), 1, fixtureTimeoutMs);
        QCOMPARE(selection().devices.first().path, QStringLiteral("/dev/test-b"));
        for (const QList<QVariant>& snapshot : snapshots)
        {
            const QList<rufus::Device> devices = qvariant_cast<QList<rufus::Device>>(snapshot.at(0));
            for (const auto& device : devices)
                QVERIFY(device.path != QStringLiteral("/dev/test-a"));
        }
        QCOMPARE(stat(QStringLiteral("opens")), 0);
    }

    void readOnlyDiscoveryAndTransitions()
    {
        objects_[diskA][blockInterface][QStringLiteral("ReadOnly")] = true;
        setObjects();
        window_ = std::make_unique<rufus::MainWindow>(*controller_);
        window_->show();
        client_->start();
        QTRY_COMPARE_WITH_TIMEOUT(selection().devices.size(), 1, fixtureTimeoutMs);
        const rufus::Device original = selection().devices.first();
        QVERIFY(original.readOnly && controller_->canSaveDevice());
        QVERIFY(!controller_->canStartWrite());
        auto* combo = window_->findChild<QComboBox*>(QStringLiteral("deviceCombo"));
        auto* save = window_->findChild<QToolButton*>(QStringLiteral("saveDeviceButton"));
        QVERIFY(combo && save && save->isEnabled());
        QVERIFY(combo->currentText().contains(QStringLiteral("[Read-only]")));
        rufus::Device target;
        QVERIFY(!rufus::DescribeUDisksDevice(objects_, diskA.path(), target));
        QVERIFY(rufus::DescribeUDisksDevice(objects_, diskA.path(), target, false, rufus::DeviceAccess::Read));
        for (bool readOnly : {false, true})
        {
            objects_[diskA][blockInterface][QStringLiteral("ReadOnly")] = readOnly;
            setObjects();
            notifyChanged(diskA, blockInterface, {{QStringLiteral("ReadOnly"), readOnly}});
            QTRY_COMPARE_WITH_TIMEOUT(selection().devices.first().readOnly, readOnly, fixtureTimeoutMs);
            QCOMPARE(selection().deviceId, original.id);
            QCOMPARE(selection().devices.first().identity, original.identity);
            QVERIFY(controller_->canSaveDevice());
        }
        QCOMPARE(stat(QStringLiteral("opens")), 0);
    }

    void fixedUsbVetoes_data()
    {
        QTest::addColumn<QString>("reason");
        for (const char* reason : {"internal", "missing-bus", "system", "ignored", "readonly", "optical",
             "empty", "partition", "missing-drive", "missing-readonly", "malformed-readonly", "missing-system", "missing-ignore",
             "missing-optical", "missing-removable", "missing-media-removable", "malformed-removable",
             "malformed-media-removable", "zero-size", "overflow-size", "bad-path"})
            QTest::newRow(reason) << QString::fromLatin1(reason);
    }

    void fixedUsbVetoes()
    {
        QFETCH(QString, reason);
        QVariantMap& block = objects_[diskA][blockInterface];
        QVariantMap& drive = objects_[driveA][driveInterface];
        drive[QStringLiteral("MediaRemovable")] = false;
        if (reason == QStringLiteral("internal")) drive[QStringLiteral("ConnectionBus")] = QStringLiteral("ata");
        if (reason == QStringLiteral("missing-bus")) drive.remove(QStringLiteral("ConnectionBus"));
        if (reason == QStringLiteral("system")) block[QStringLiteral("HintSystem")] = true;
        if (reason == QStringLiteral("ignored")) block[QStringLiteral("HintIgnore")] = true;
        if (reason == QStringLiteral("readonly")) block[QStringLiteral("ReadOnly")] = true;
        if (reason == QStringLiteral("optical")) drive[QStringLiteral("Optical")] = true;
        if (reason == QStringLiteral("empty")) drive[QStringLiteral("MediaAvailable")] = false;
        if (reason == QStringLiteral("partition")) objects_[diskA][partitionInterface] = {};
        if (reason == QStringLiteral("missing-readonly")) block.remove(QStringLiteral("ReadOnly"));
        if (reason == QStringLiteral("malformed-readonly")) block[QStringLiteral("ReadOnly")] = QStringLiteral("false");
        if (reason == QStringLiteral("missing-system")) block.remove(QStringLiteral("HintSystem"));
        if (reason == QStringLiteral("missing-ignore")) block.remove(QStringLiteral("HintIgnore"));
        if (reason == QStringLiteral("missing-optical")) drive.remove(QStringLiteral("Optical"));
        if (reason == QStringLiteral("missing-removable")) drive.remove(QStringLiteral("Removable"));
        if (reason == QStringLiteral("missing-media-removable")) drive.remove(QStringLiteral("MediaRemovable"));
        if (reason == QStringLiteral("malformed-removable")) drive[QStringLiteral("Removable")] = QStringLiteral("false");
        if (reason == QStringLiteral("malformed-media-removable")) drive[QStringLiteral("MediaRemovable")] = 0;
        if (reason == QStringLiteral("zero-size")) block[QStringLiteral("Size")] = quint64(0);
        if (reason == QStringLiteral("overflow-size")) block[QStringLiteral("Size")] = ~quint64(0);
        if (reason == QStringLiteral("bad-path")) block[QStringLiteral("Device")] = QByteArray("/tmp/disk") + '\0';
        if (reason == QStringLiteral("missing-drive")) objects_.remove(driveA);
        rufus::Device candidate;
        QVERIFY(!rufus::DescribeUDisksDevice(objects_, diskA.path(), candidate, true));
        QCOMPARE(rufus::DescribeUDisksDevice(objects_, diskA.path(), candidate, true, rufus::DeviceAccess::Read), reason == QStringLiteral("readonly"));
        if (reason == QStringLiteral("readonly")) QVERIFY(candidate.readOnly);
        QCOMPARE(stat(QStringLiteral("opens")), 0);
    }

    void excludedDevices_data()
    {
        QTest::addColumn<QString>("reason");
        const QStringList reasons = {QStringLiteral("internal"), QStringLiteral("fixed-usb"),
            QStringLiteral("system"), QStringLiteral("ignored"),
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
        {
            drive[QStringLiteral("MediaRemovable")] = false;
            TestNativeDisk(2048).model = "HDD";
        }
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

    void partitionWarningCount_data()
    {
        QTest::addColumn<QString>("table");
        QTest::addColumn<QString>("type");
        QTest::addColumn<QString>("name");
        QTest::addColumn<QString>("label");
        QTest::addColumn<QString>("filesystem");
        QTest::addColumn<int>("expected");
        QTest::newRow("gpt-data") << QStringLiteral("gpt") << QStringLiteral("ebd0a0a2-b9e5-4433-87c0-68b6b72699c7") << QStringLiteral("Data") << QStringLiteral("") << QStringLiteral("ntfs") << 2;
        QTest::newRow("gpt-efi") << QStringLiteral("gpt") << QStringLiteral("C12A7328-F81F-11D2-BA4B-00A0C93EC93B") << QStringLiteral("") << QStringLiteral("") << QStringLiteral("") << 1;
        QTest::newRow("gpt-msr") << QStringLiteral("gpt") << QStringLiteral("e3c9e316-0b5c-4db8-817d-f92df00215ae") << QStringLiteral("") << QStringLiteral("") << QStringLiteral("") << 1;
        QTest::newRow("gpt-uefi-ntfs") << QStringLiteral("gpt") << QStringLiteral("unknown") << QStringLiteral("UEFI:NTFS") << QStringLiteral("") << QStringLiteral("") << 1;
        QTest::newRow("gpt-name-case") << QStringLiteral("gpt") << QStringLiteral("unknown") << QStringLiteral("uefi:ntfs") << QStringLiteral("") << QStringLiteral("") << 2;
        QTest::newRow("gpt-missing-type") << QStringLiteral("gpt") << QStringLiteral("") << QStringLiteral("") << QStringLiteral("") << QStringLiteral("") << 2;
        QTest::newRow("mbr-data") << QStringLiteral("dos") << QStringLiteral("0x07") << QStringLiteral("") << QStringLiteral("") << QStringLiteral("ntfs") << 2;
        QTest::newRow("mbr-unused") << QStringLiteral("dos") << QStringLiteral("0x00") << QStringLiteral("") << QStringLiteral("") << QStringLiteral("") << 1;
        QTest::newRow("mbr-extra") << QStringLiteral("dos") << QStringLiteral("0xEA") << QStringLiteral("") << QStringLiteral("") << QStringLiteral("") << 1;
        QTest::newRow("mbr-uefi-ntfs") << QStringLiteral("dos") << QStringLiteral("0xef") << QStringLiteral("") << QStringLiteral("UEFI_NTFS") << QStringLiteral("vfat") << 1;
        QTest::newRow("mbr-rufus-boot") << QStringLiteral("dos") << QStringLiteral("0xef") << QStringLiteral("") << QStringLiteral("RUFUS_BOOT") << QStringLiteral("vfat") << 1;
        QTest::newRow("mbr-other-efi") << QStringLiteral("dos") << QStringLiteral("0xef") << QStringLiteral("") << QStringLiteral("OTHER") << QStringLiteral("vfat") << 2;
        QTest::newRow("mbr-unknown-fs") << QStringLiteral("dos") << QStringLiteral("0xef") << QStringLiteral("") << QStringLiteral("UEFI_NTFS") << QStringLiteral("") << 2;
        QTest::newRow("mbr-label-on-data") << QStringLiteral("dos") << QStringLiteral("0x07") << QStringLiteral("") << QStringLiteral("UEFI_NTFS") << QStringLiteral("ntfs") << 2;
        QTest::newRow("mbr-container") << QStringLiteral("dos") << QStringLiteral("0x0f") << QStringLiteral("") << QStringLiteral("") << QStringLiteral("") << 2;
        QTest::newRow("mbr-unknown-type") << QStringLiteral("dos") << QStringLiteral("invalid") << QStringLiteral("") << QStringLiteral("") << QStringLiteral("") << 2;
        QTest::newRow("unknown-table") << QStringLiteral("") << QStringLiteral("0xea") << QStringLiteral("") << QStringLiteral("") << QStringLiteral("") << 2;
    }

    void partitionWarningCount()
    {
        QFETCH(QString, table); QFETCH(QString, type); QFETCH(QString, name);
        QFETCH(QString, label); QFETCH(QString, filesystem); QFETCH(int, expected);
        objects_[diskA][partitionTableInterface][QStringLiteral("Type")] = table;
        rufus::Device device;
        QVERIFY(rufus::DescribeUDisksDevice(objects_, diskA.path(), device));
        QCOMPARE(device.partitionCount, 0); // No children, including whole-device filesystems.
        const QStringList identity = device.identity;
        const QDBusObjectPath data(diskA.path() + QStringLiteral("1"));
        const QDBusObjectPath extra(diskA.path() + QStringLiteral("2"));
        objects_[data][partitionInterface] = {{QStringLiteral("Table"), QVariant::fromValue(diskA)},
            {QStringLiteral("Type"), table == QStringLiteral("dos") ? QStringLiteral("0x07") : QStringLiteral("data")}};
        QVERIFY(rufus::DescribeUDisksDevice(objects_, diskA.path(), device));
        QCOMPARE(device.partitionCount, 1);
        objects_[extra][partitionInterface] = {{QStringLiteral("Table"), QVariant::fromValue(diskA)},
            {QStringLiteral("Type"), type}, {QStringLiteral("Name"), name}, {QStringLiteral("IsContainer"), true}};
        objects_[extra][blockInterface] = {{QStringLiteral("IdLabel"), label}, {QStringLiteral("IdType"), filesystem}};
        QVERIFY(rufus::DescribeUDisksDevice(objects_, diskA.path(), device));
        QCOMPARE(device.partitionCount, expected);
        QCOMPARE(device.identity, identity); // Content changes do not replace selection identity.
        objects_[extra][partitionInterface][QStringLiteral("Table")] = QVariant::fromValue(diskB);
        QVERIFY(rufus::DescribeUDisksDevice(objects_, diskA.path(), device));
        QCOMPARE(device.partitionCount, 1); // Never count another disk's partition.
    }

    void partitionPreviewRefreshesWithoutReplacingDevice_data()
    {
        QTest::addColumn<bool>("invalidated");
        QTest::newRow("changed") << false;
        QTest::newRow("invalidated") << true;
    }

    void partitionPreviewRefreshesWithoutReplacingDevice()
    {
        QFETCH(bool, invalidated);
        window_ = std::make_unique<rufus::MainWindow>(*controller_);
        window_->show();
        objects_[diskA][partitionTableInterface][QStringLiteral("Type")] = QStringLiteral("dos");
        setObjects();
        client_->start();
        QTRY_COMPARE_WITH_TIMEOUT(selection().devices.size(), 1, fixtureTimeoutMs);
        const rufus::Device original = selection().devices.first();
        QCOMPARE(original.partitionTableType, QStringLiteral("dos"));
        auto* partition = window_->findChild<QComboBox*>(QStringLiteral("partitionCombo"));
        auto* target = window_->findChild<QComboBox*>(QStringLiteral("targetCombo"));
        QVERIFY(partition && target);
        QCOMPARE(partition->currentText(), QStringLiteral("MBR"));
        QCOMPARE(target->currentText(), QStringLiteral("BIOS (or UEFI-CSM)"));

        objects_[diskA][partitionTableInterface][QStringLiteral("Type")] = QStringLiteral("gpt");
        setObjects();
        if (invalidated)
            notifyChanged(diskA, partitionTableInterface, {}, {QStringLiteral("Type")});
        else
            notifyChanged(diskA, partitionTableInterface, {{QStringLiteral("Type"), QStringLiteral("gpt")}});
        QTRY_COMPARE_WITH_TIMEOUT(partition->currentText(), QStringLiteral("GPT"), fixtureTimeoutMs);
        QCOMPARE(target->currentText(), QStringLiteral("UEFI (non CSM)"));
        QCOMPARE(selection().devices.first().partitionTableType, QStringLiteral("gpt"));
        QCOMPARE(selection().devices.first().identity, original.identity);
        QCOMPARE(selection().deviceId, original.id);

        objects_[diskA].remove(partitionTableInterface);
        setObjects();
        notifyRemoved(diskA, partitionTableInterface);
        QTRY_COMPARE_WITH_TIMEOUT(partition->currentText(), QStringLiteral("MBR"), fixtureTimeoutMs);
        QVERIFY(selection().devices.first().partitionTableType.isEmpty());
        QCOMPARE(selection().deviceId, original.id);
        QVERIFY(!partition->isEnabled());
        QVERIFY(!target->isEnabled());
        QVERIFY(!controller_->canStartWrite());
        QVERIFY(!selection().syslinux);

        objects_[diskA][partitionTableInterface][QStringLiteral("Type")] = QStringLiteral("gpt");
        setObjects();
        const rufus::UDisksInterfaces added = {{partitionTableInterface, objects_[diskA][partitionTableInterface]}};
        control(QStringLiteral("Added"), {QVariant::fromValue(diskA), QVariant::fromValue(added)});
        QTRY_COMPARE_WITH_TIMEOUT(partition->currentText(), QStringLiteral("GPT"), fixtureTimeoutMs);
        QCOMPARE(selection().deviceId, original.id);
        // A content notification must still detect changed physical identity
        // in the fresh snapshot and require explicit reselection.
        objects_[driveA][driveInterface][QStringLiteral("TimeMediaDetected")] = QVariant::fromValue(quint64(99999));
        setObjects();
        notifyChanged(diskA, partitionTableInterface, {}, {QStringLiteral("Type")});
        QTRY_VERIFY_WITH_TIMEOUT(selection().deviceId.isEmpty(), fixtureTimeoutMs);
        QCOMPARE(partition->count(), 0);
        QCOMPARE(target->count(), 0);
    }

    void volumeLabelComesFromFirstFilesystem_data()
    {
        QTest::addColumn<QString>("scenario");
        QTest::addColumn<QString>("expected");
        QTest::newRow("partition-order") << QStringLiteral("ordered") << QStringLiteral("FIRST");
        QTest::newRow("unlabeled-first") << QStringLiteral("unlabeled") << QString();
        QTest::newRow("whole-disk") << QStringLiteral("whole") << QStringLiteral("WHOLE");
        QTest::newRow("other-table") << QStringLiteral("other") << QStringLiteral("LATER");
        QTest::newRow("non-filesystem") << QStringLiteral("crypto") << QStringLiteral("LATER");
        QTest::newRow("container") << QStringLiteral("container") << QStringLiteral("LATER");
        QTest::newRow("missing-offset") << QStringLiteral("offset") << QStringLiteral("LATER");
    }

    void volumeLabelComesFromFirstFilesystem()
    {
        QFETCH(QString, scenario);
        QFETCH(QString, expected);
        // Reverse lexical ordering to require partition offsets, not map order.
        const QDBusObjectPath first(diskA.path() + QStringLiteral("_z"));
        const QDBusObjectPath later(diskA.path() + QStringLiteral("_a"));
        objects_[first][partitionInterface] = {{QStringLiteral("Table"), QVariant::fromValue(diskA)},
            {QStringLiteral("Offset"), QVariant::fromValue(quint64(1024 * 1024))}};
        objects_[first][blockInterface] = {{QStringLiteral("IdUsage"), QStringLiteral("filesystem")},
            {QStringLiteral("IdLabel"), QStringLiteral("FIRST")}};
        objects_[later] = objects_[first];
        objects_[later][partitionInterface][QStringLiteral("Offset")] = QVariant::fromValue(quint64(2 * 1024 * 1024));
        objects_[later][blockInterface][QStringLiteral("IdLabel")] = QStringLiteral("LATER");
        objects_[first][blockInterface][QStringLiteral("IdType")] = QStringLiteral("vfat");
        objects_[first][blockInterface][QStringLiteral("IdVersion")] = QStringLiteral("FAT32");
        objects_[later][blockInterface][QStringLiteral("IdType")] = QStringLiteral("exfat");
        if (scenario == QStringLiteral("unlabeled"))
            objects_[first][blockInterface][QStringLiteral("IdLabel")] = QString();
        else if (scenario == QStringLiteral("whole"))
        {
            objects_[diskA][blockInterface][QStringLiteral("IdUsage")] = QStringLiteral("filesystem");
            objects_[diskA][blockInterface][QStringLiteral("IdLabel")] = QStringLiteral("WHOLE");
            objects_[diskA][blockInterface][QStringLiteral("IdType")] = QStringLiteral("ntfs");
        }
        else if (scenario == QStringLiteral("other"))
            objects_[first][partitionInterface][QStringLiteral("Table")] = QVariant::fromValue(diskB);
        else if (scenario == QStringLiteral("crypto"))
            objects_[first][blockInterface][QStringLiteral("IdUsage")] = QStringLiteral("crypto");
        else if (scenario == QStringLiteral("container"))
            objects_[first][partitionInterface][QStringLiteral("IsContainer")] = true;
        else if (scenario == QStringLiteral("offset"))
            objects_[first][partitionInterface].remove(QStringLiteral("Offset"));
        rufus::Device device;
        QVERIFY(rufus::DescribeUDisksDevice(objects_, diskA.path(), device));
        QCOMPARE(device.volumeLabel, expected);
        const bool firstChosen = scenario == QStringLiteral("ordered") || scenario == QStringLiteral("unlabeled");
        const QString expectedType = firstChosen ? QStringLiteral("vfat")
            : scenario == QStringLiteral("whole") ? QStringLiteral("ntfs") : QStringLiteral("exfat");
        QCOMPARE(device.fileSystemType, expectedType);
        QCOMPARE(device.fileSystemVersion, firstChosen ? QStringLiteral("FAT32") : QString());
    }

    void filesystemRefreshPreservesManualChoice()
    {
        window_ = std::make_unique<rufus::MainWindow>(*controller_);
        // The generic discovery fixture deliberately has an unaligned size;
        // format choices require a sector-aligned synthetic target.
        objects_[diskA][blockInterface][QStringLiteral("Size")] = QVariant::fromValue(16ULL * 1024 * 1024 * 1024);
        objects_[diskA][blockInterface][QStringLiteral("IdUsage")] = QStringLiteral("filesystem");
        objects_[diskA][blockInterface][QStringLiteral("IdType")] = QStringLiteral("vfat");
        objects_[diskA][blockInterface][QStringLiteral("IdVersion")] = QStringLiteral("FAT32");
        setObjects();
        client_->start();
        QTRY_COMPARE_WITH_TIMEOUT(selection().devices.size(), 1, fixtureTimeoutMs);
        const rufus::Device original = selection().devices.first();
        auto* filesystem = window_->findChild<QComboBox*>(QStringLiteral("fileSystemCombo"));
        auto* cluster = window_->findChild<QComboBox*>(QStringLiteral("clusterCombo"));
        QVERIFY(filesystem && cluster);
        QCOMPARE(filesystem->currentText(), QStringLiteral("FAT32 (Default)"));
        objects_[diskA][blockInterface][QStringLiteral("IdType")] = QStringLiteral("ntfs");
        objects_[diskA][blockInterface][QStringLiteral("IdVersion")] = QString();
        setObjects();
        notifyChanged(diskA, blockInterface, {{QStringLiteral("IdType"), QStringLiteral("ntfs")}});
        QTRY_COMPARE_WITH_TIMEOUT(filesystem->currentText(), QStringLiteral("NTFS"), fixtureTimeoutMs);
        QCOMPARE(cluster->currentText(), QStringLiteral("4096 bytes (Default)"));
        QCOMPARE(selection().deviceId, original.id);
        QCOMPARE(selection().devices.first().identity, original.identity);
        controller_->setFileSystem(FS_FAT32);
        objects_[diskA][blockInterface][QStringLiteral("IdVersion")] = QStringLiteral("3.1");
        setObjects();
        notifyChanged(diskA, blockInterface, {}, {QStringLiteral("IdVersion")});
        QTRY_COMPARE_WITH_TIMEOUT(selection().devices.first().fileSystemVersion, QStringLiteral("3.1"), fixtureTimeoutMs);
        QCOMPARE(filesystem->currentText(), QStringLiteral("FAT32 (Default)"));
        QVERIFY(!filesystem->isEnabled());
        QVERIFY(!controller_->canStartWrite());
        QCOMPARE(stat(QStringLiteral("opens")), 0);
    }

    void volumeLabelRefreshPreservesSelectionAndManualEdit()
    {
        window_ = std::make_unique<rufus::MainWindow>(*controller_);
        const QDBusObjectPath volume(diskA.path() + QStringLiteral("_1"));
        objects_[volume][partitionInterface] = {{QStringLiteral("Table"), QVariant::fromValue(diskA)},
            {QStringLiteral("Offset"), QVariant::fromValue(quint64(1024 * 1024))}};
        objects_[volume][blockInterface] = {{QStringLiteral("IdUsage"), QStringLiteral("filesystem")},
            {QStringLiteral("IdLabel"), QStringLiteral("OLD")}};
        setObjects();
        client_->start();
        QTRY_COMPARE_WITH_TIMEOUT(selection().devices.size(), 1, fixtureTimeoutMs);
        const rufus::Device original = selection().devices.first();
        auto* label = window_->findChild<QLineEdit*>(QStringLiteral("volumeEdit"));
        QCOMPARE(label->text(), QStringLiteral("OLD"));
        objects_[volume][blockInterface][QStringLiteral("IdLabel")] = QStringLiteral("NEW");
        setObjects();
        notifyChanged(volume, blockInterface, {}, {QStringLiteral("IdLabel")});
        QTRY_COMPARE_WITH_TIMEOUT(label->text(), QStringLiteral("NEW"), fixtureTimeoutMs);
        QCOMPARE(selection().deviceId, original.id);
        QCOMPARE(selection().devices.first().identity, original.identity);
        objects_[volume][partitionInterface][QStringLiteral("Table")] = QVariant::fromValue(diskB);
        setObjects();
        notifyChanged(volume, partitionInterface, {}, {QStringLiteral("Table")});
        QTRY_COMPARE_WITH_TIMEOUT(label->text(), QStringLiteral("17 GB"), fixtureTimeoutMs);
        objects_[volume][partitionInterface][QStringLiteral("Table")] = QVariant::fromValue(diskA);
        setObjects();
        notifyChanged(volume, partitionInterface, {}, {QStringLiteral("Table")});
        QTRY_COMPARE_WITH_TIMEOUT(label->text(), QStringLiteral("NEW"), fixtureTimeoutMs);
        controller_->setVolumeLabel(QStringLiteral("MY LABEL"));
        objects_.remove(volume);
        setObjects();
        notifyRemoved(volume, blockInterface);
        QTRY_VERIFY_WITH_TIMEOUT(selection().devices.first().volumeLabel.isEmpty(), fixtureTimeoutMs);
        QCOMPARE(label->text(), QStringLiteral("MY LABEL"));
        QCOMPARE(selection().deviceId, original.id);
        QVERIFY(!controller_->canStartWrite());
        QCOMPARE(stat(QStringLiteral("opens")), 0);
    }

    void formattingMetadataPreservesSelection_data()
    {
        QTest::addColumn<QString>("interface");
        QTest::newRow("partition-table") << QStringLiteral("org.freedesktop.UDisks2.PartitionTable");
        QTest::newRow("filesystem") << QStringLiteral("org.freedesktop.UDisks2.Filesystem");
    }

    void formattingMetadataPreservesSelection()
    {
        QFETCH(QString, interface);
        client_->start();
        QTRY_COMPARE_WITH_TIMEOUT(selection().devices.size(), 1, fixtureTimeoutMs);
        const QString id = selection().devices.first().id;
        controller_->selectDevice(id);
        const rufus::UDisksInterfaces added = {{interface, {}}};
        objects_[diskA][interface] = {};
        const QVariantMap content = {{QStringLiteral("IdUsage"), QStringLiteral("partitiontable")},
            {QStringLiteral("IdType"), QStringLiteral("gpt")}, {QStringLiteral("IdUUID"), QStringLiteral("new-gpt-id")}};
        for (auto property = content.cbegin(); property != content.cend(); ++property)
            objects_[diskA][blockInterface][property.key()] = property.value();
        setObjects();
        QSignalSpy snapshots(client_.get(), &rufus::UDisksClient::devicesChanged);
        control(QStringLiteral("Added"), {QVariant::fromValue(diskA), QVariant::fromValue(added)});
        notifyChanged(diskA, blockInterface, content);
        QTRY_VERIFY_WITH_TIMEOUT(!snapshots.isEmpty(), fixtureTimeoutMs);
        QCOMPARE(selection().deviceId, id);
        QCOMPARE(selection().devices.first().id, id);
        snapshots.clear();
        objects_[diskA].remove(interface);
        setObjects();
        notifyRemoved(diskA, interface);
        QTRY_VERIFY_WITH_TIMEOUT(!snapshots.isEmpty(), fixtureTimeoutMs);
        QCOMPARE(selection().deviceId, id);
        QCOMPARE(selection().devices.first().id, id);
        // Content changes never excuse a changed physical identity in the
        // fresh snapshot, even when the notification only named content.
        objects_[driveA][driveInterface][QStringLiteral("TimeMediaDetected")] = QVariant::fromValue(quint64(99999));
        setObjects();
        notifyChanged(diskA, blockInterface, {{QStringLiteral("IdLabel"), QStringLiteral("NEW LABEL")}});
        QTRY_VERIFY_WITH_TIMEOUT(selection().deviceId.isEmpty(), fixtureTimeoutMs);
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
        QVERIFY(selection().deviceId.isEmpty());
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
        QVERIFY(selection().deviceId.isEmpty());
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
