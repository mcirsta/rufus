// SPDX-License-Identifier: GPL-3.0-or-later
#include "native_disk_fixture.h"
#include "qt/main_window.h"
#include "qt/selection_controller.h"
#include "qt/application_storage.h"
#include <QShortcut>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <sys/sysmacros.h>

class TestDeviceOptions : public QObject
{
    Q_OBJECT
    QTemporaryDir directory_;
private slots:
    void initTestCase()
    {
        QVERIFY(directory_.isValid()); QString problem;
        QCOMPARE(rufus::ConfigureApplicationStorage(directory_.filePath("rufus-linux"), true, problem), 0);
    }
    void init() { rufus::ApplicationSettings settings; settings.clear(); settings.sync(); ResetTestNativeDisks(); }
    void protocol()
    {
        rufus::PrepareImageRequest request;
        request.isDevice = request.nonBootable = true; request.path = QStringLiteral("/test/no-device");
        request.device = makedev(8, 239); request.size = 64ULL * 1024 * 1024; request.diskSequence = 42;
        request.objectPath = QStringLiteral("/org/freedesktop/UDisks2/block_devices/fixture");
        request.serviceOwner = QStringLiteral(":1.42"); request.identity = {QStringLiteral("fixture")};
        request.fileSystem = FS_FAT32; request.clusterSize = 4096; request.label = QStringLiteral("OPTIONS");
        for (int kind : {1, 2, 4, 8, 16}) {
            request.deviceClass = rufus::DeviceClass(kind);
            const auto packet = rufus::EncodePrepareRequest(request); rufus::PrepareImageRequest decoded;
            QVERIFY(!packet.isEmpty()); QVERIFY(rufus::DecodePrepareRequest(packet, decoded));
            QCOMPARE(decoded.deviceClass, request.deviceClass); QCOMPARE(packet.contains("deviceClass"), kind != 1);
        }
        auto packet = rufus::EncodePrepareRequest(request);
        for (const QJsonValue value : {QJsonValue(0), QJsonValue(1), QJsonValue(3), QJsonValue(32),
            QJsonValue(-1), QJsonValue(2.5), QJsonValue(true), QJsonValue("2"), QJsonValue(QJsonObject())}) {
            packet.insert("deviceClass", value); rufus::PrepareImageRequest decoded;
            QVERIFY(!rufus::DecodePrepareRequest(packet, decoded));
        }
        request.deviceClass = rufus::DeviceClass(3); QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        request.deviceClass = rufus::DeviceClass::Virtual; request.isDevice = false;
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
    }
    void shortcutsAndPreferences()
    {
        rufus::SelectionController controller; rufus::MainWindow window(controller);
        QCOMPARE(controller.deviceClasses(), quint32(19));
        struct Shortcut { int key; quint32 expected; };
        const Shortcut cases[] = {{Qt::ALT | Qt::Key_W, 23}, {Qt::ALT | Qt::Key_G, 17},
            {Qt::ALT | Qt::Key_G, 23}, {Qt::CTRL | Qt::ALT | Qt::Key_F, 31},
            {Qt::CTRL | Qt::ALT | Qt::Key_F, 23}};
        for (const auto& test : cases) {
            QShortcut* found = nullptr;
            for (QShortcut* shortcut : window.findChildren<QShortcut*>())
                if (shortcut->key() == QKeySequence(test.key)) found = shortcut;
            QVERIFY(found); QVERIFY(QMetaObject::invokeMethod(found, "activated", Qt::DirectConnection));
            QCOMPARE(controller.deviceClasses(), test.expected);
        }
        QVERIFY(!controller.selection().allowFixedUsb);
        controller.setAllowFixedUsb(true); controller.toggleNonUsbRemovable();
        controller.setAllowFixedUsb(false); controller.toggleNonUsbRemovable();
        QVERIFY(controller.selection().allowFixedUsb);
        controller.toggleVirtualDisks();
        rufus::SelectionController restored;
        QCOMPARE(restored.deviceClasses(), quint32(17)); QVERIFY(!restored.selection().allowFixedUsb);
        restored.toggleVirtualDisks(); QCOMPARE(restored.deviceClasses(), quint32(23));
    }
    void eligibility_data()
    {
        QTest::addColumn<int>("kind"); QTest::addColumn<bool>("visible");
        QTest::newRow("usb") << 1 << true; QTest::newRow("virtual") << 2 << true;
        QTest::newRow("vmware") << 4 << false; QTest::newRow("non-usb-removable") << 8 << false;
        QTest::newRow("card") << 16 << true;
    }
    void eligibility()
    {
        QFETCH(int, kind); QFETCH(bool, visible);
        const QString block = QStringLiteral("/org/freedesktop/UDisks2/block_devices/fixture");
        const QString drive = QStringLiteral("/org/freedesktop/UDisks2/drives/fixture");
        const QString blockInterface = QStringLiteral("org.freedesktop.UDisks2.Block");
        const QString driveInterface = QStringLiteral("org.freedesktop.UDisks2.Drive");
        constexpr quint64 number = 2287;
        auto& facts = TestNativeDisk(number); facts.deviceClass = rufus::DeviceClass(kind); facts.usbTransport = kind == 1;
        rufus::UDisksObjects objects;
        objects[QDBusObjectPath(block)][blockInterface] = {{"Drive", QVariant::fromValue(QDBusObjectPath(drive))},
            {"Device", QByteArray("/dev/test-device\0", 17)}, {"Size", quint64(64 * 1024 * 1024)},
            {"DeviceNumber", number}, {"ReadOnly", false}, {"HintSystem", kind != 1}, {"HintIgnore", false}};
        objects[QDBusObjectPath(drive)][driveInterface] = {{"ConnectionBus", kind == 1 ? "usb" : "ata"},
            {"Removable", true}, {"MediaRemovable", true}, {"MediaAvailable", true}, {"Optical", false}};
        rufus::Device device;
        QCOMPARE(rufus::DescribeUDisksDevice(objects, block, device), visible);
        QVERIFY(rufus::DescribeUDisksDevice(objects, block, device, true, rufus::DeviceAccess::Write, 31));
        QCOMPARE(device.deviceClass, rufus::DeviceClass(kind));
        if (kind != 1) QVERIFY(device.requiresNativeProtection);
        QVERIFY(rufus::CheckUDisksTargetUse(objects, device, 0, true).isEmpty());
        QVERIFY(!rufus::CheckUDisksTargetUse(objects, device, number, true).isEmpty());
        objects[QDBusObjectPath(block)][blockInterface]["ReadOnly"] = true;
        QVERIFY(!rufus::DescribeUDisksDevice(objects, block, device, true, rufus::DeviceAccess::Write, 31));
        QVERIFY(rufus::DescribeUDisksDevice(objects, block, device, true, rufus::DeviceAccess::Read, 31));
        objects[QDBusObjectPath(block)][blockInterface]["HintIgnore"] = true;
        QVERIFY(!rufus::DescribeUDisksDevice(objects, block, device, true, rufus::DeviceAccess::Read, 31));
    }
};
QTEST_MAIN(TestDeviceOptions)
#include "test_device_options.moc"
