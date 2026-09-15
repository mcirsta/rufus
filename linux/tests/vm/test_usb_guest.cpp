// SPDX-License-Identifier: GPL-3.0-or-later
#include "qt/main_window.h"
#include "qt/selection_controller.h"
#include "platform/udisks_client.h"

#include <QApplication>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QMessageBox>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QScreen>
#include <QTest>
#include <fcntl.h>
#include <unistd.h>

namespace
{
using State = rufus::WriteState;

QByteArray readFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}

bool fixtureGuest()
{
    return geteuid() == 1000
        && readFile(QStringLiteral("/etc/rufus-usb-fixture")) == "RUFUS-TEST-USB\n"
        && readFile(QStringLiteral("/sys/devices/virtual/dmi/id/product_name")).trimmed() == "RufusUSBFixture"
        && readFile(QStringLiteral("/proc/cmdline")).trimmed().split(' ').contains("rufus.usb_fixture=1");
}

QByteArray xdo(const QStringList& args)
{
    QProcess process;
    process.start(QStringLiteral("xdotool"), args);
    if (!process.waitForFinished(3000)) return {};
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) return {};
    return process.readAllStandardOutput().trimmed();
}
}

class TestUsbGuest : public QObject
{
    Q_OBJECT
    rufus::SelectionController controller_;
    rufus::UDisksClient devices_{QDBusConnection::systemBus()};
    rufus::MainWindow window_{controller_};
    QProcess agent_;
    QByteArray authWindow_;
    bool wrote_ = false;
    bool cancelled_ = false;
    bool reattached_ = false;
    rufus::Device removedDevice_;
    rufus::RawWritePlan stalePlan_;
    State state() { return controller_.writer().status().state; }

    QByteArray authWindow()
    {
        return xdo({QStringLiteral("search"), QStringLiteral("--onlyvisible"),
            QStringLiteral("--name"), QStringLiteral("^Authentication Required$")}).split('\n').value(0);
    }

    void waitForAuthentication()
    {
        QElapsedTimer timer;
        timer.start();
        authWindow_.clear();
        while (timer.elapsed() < 10000 && state() == State::Authorizing)
        {
            authWindow_ = authWindow();
            if (!authWindow_.isEmpty()) return;
            QTest::qWait(50);
        }
        QVERIFY(state() == State::Writing || !authWindow_.isEmpty());
    }

    void approvePlan()
    {
        QTest::mouseClick(window_.findChild<QPushButton*>(QStringLiteral("startButton")), Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(state(), State::AwaitingConfirmation, 5000);
        auto* dialog = window_.findChild<QMessageBox*>(QStringLiteral("writeConfirmation"));
        QVERIFY(dialog && dialog->isVisible());
        QTest::mouseClick(dialog->button(QMessageBox::Ok), Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(state(), State::Authorizing, 5000);
    }

    void logMessage(const QString& message) { qInfo().noquote() << message; }

    void authenticate()
    {
        waitForAuthentication();
        QVERIFY(state() == State::Writing || !authWindow_.isEmpty());
        if (state() != State::Writing)
        {
            xdo({QStringLiteral("windowfocus"), QString::fromLatin1(authWindow_)});
            // Public fixture password; the separate native agent receives it.
            xdo({QStringLiteral("type"), QStringLiteral("--clearmodifiers"), QStringLiteral("rufus-fixture")});
            xdo({QStringLiteral("key"), QStringLiteral("Return")});
        }
        QTRY_COMPARE_WITH_TIMEOUT(state(), State::Writing, 15000);
    }

    void marker(const QString& name)
    {
        QFile file(QStringLiteral("/home/rufus/") + name);
        QVERIFY(file.open(QIODevice::WriteOnly));
    }

    void reselect()
    {
        QTRY_COMPARE_WITH_TIMEOUT(controller_.selection().devices.size(), 1, 5000);
        const rufus::Device device = controller_.selection().devices.first();
        QVERIFY(device.identity.contains(QStringLiteral("RUFUS-TEST-USB")));
        QCOMPARE(device.size, quint64(256 * 1024 * 1024));
        controller_.selectDevice(device.id);
    }

    void selectImage(const QString& path)
    {
        QTest::mouseClick(window_.findChild<QPushButton*>(QStringLiteral("selectImageButton")), Qt::LeftButton);
        auto* dialog = window_.findChild<QFileDialog*>(QStringLiteral("imageDialog"));
        QVERIFY(dialog && dialog->isVisible());
        dialog->setDirectory(QStringLiteral("/opt/rufus"));
        dialog->selectFile(path);
        auto* buttons = dialog->findChild<QDialogButtonBox*>();
        QVERIFY(buttons);
        QTRY_VERIFY_WITH_TIMEOUT(buttons->button(QDialogButtonBox::Open)->isEnabled(), 2000);
        QTest::mouseClick(buttons->button(QDialogButtonBox::Open), Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(controller_.canStartWrite(), 5000);
    }

private slots:
    void initTestCase()
    {
        QVERIFY(fixtureGuest());
        controller_.writer().setConnection(QDBusConnection::systemBus());
        connect(&devices_, &rufus::UDisksClient::devicesChanged,
            &controller_, &rufus::SelectionController::setDiscovery);
        connect(&controller_, &rufus::SelectionController::logMessage, this, &TestUsbGuest::logMessage);
        agent_.setProcessChannelMode(QProcess::ForwardedChannels);
        agent_.start(QStringLiteral("/usr/lib/polkit-kde-authentication-agent-1"), {});
        QVERIFY(agent_.waitForStarted(3000));
        window_.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window_));
        devices_.start();
        QTRY_COMPARE_WITH_TIMEOUT(controller_.selection().devices.size(), 1, 15000);
        const rufus::Device device = controller_.selection().devices.first();
        QCOMPARE(device.size, quint64(256 * 1024 * 1024));
        QVERIFY(device.identity.contains(QStringLiteral("RUFUS-TEST-USB")));
        QCOMPARE(device.path, QStringLiteral("/dev/sda"));
        // Prove the account itself has no raw-write permission.
        const int fd = open("/dev/sda", O_RDWR | O_CLOEXEC);
        if (fd >= 0) close(fd);
        QVERIFY(fd < 0 && errno == EACCES);
        controller_.selectDevice(device.id);
        selectImage(QStringLiteral("/opt/rufus/boot.img"));
    }

    void nativeAuthenticationAndWrite()
    {
        approvePlan();
        waitForAuthentication();
        QVERIFY(!authWindow_.isEmpty());
        qInfo("RUFUS_NATIVE_AUTH_PROMPT");
        qInfo().noquote() << "Native agent window:" << xdo({QStringLiteral("getwindowname"), QString::fromLatin1(authWindow_)})
            << xdo({QStringLiteral("getwindowgeometry"), QString::fromLatin1(authWindow_)});
        xdo({QStringLiteral("windowraise"), QString::fromLatin1(authWindow_)});
        xdo({QStringLiteral("windowfocus"), QString::fromLatin1(authWindow_)});
        QTest::qWait(300); // Let the separate agent paint its exposed window.
        QVERIFY(window_.screen()->grabWindow(0).save(QStringLiteral("/home/rufus/authentication.png")));
        xdo({QStringLiteral("key"), QStringLiteral("Escape")});
        QTRY_VERIFY_WITH_TIMEOUT(state() == State::Failed || state() == State::Cancelled, 10000);
        QCOMPARE(controller_.writer().status().outcome.io.bytes_written, quint64(0));
        qInfo("RUFUS_NATIVE_AUTH_DISMISSED_NO_WRITE");

        approvePlan();
        authenticate();
        QCOMPARE(state(), State::Writing);
        QTRY_VERIFY_WITH_TIMEOUT(!controller_.writer().isActive(), 60000);
        QCOMPARE(state(), State::Complete);
        QCOMPARE(controller_.writer().percent(), 100);
        QCOMPARE(controller_.writer().status().outcome.io.bytes_written, controller_.selection().imageSize);
        QCOMPARE(controller_.writer().status().outcome.io.bytes_verified, controller_.selection().imageSize);
        qInfo("RUFUS_NATIVE_WRITE_VERIFIED");
        // The kernel/UDisks should discover the written partition after last close.
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(QStringLiteral("/dev/disk/by-label/RUFUS_TEST")), 10000);
        QTRY_COMPARE_WITH_TIMEOUT(controller_.selection().devices.size(), 1, 5000);
        qInfo("RUFUS_NATIVE_PARTITION_DISCOVERED");
        QVERIFY(window_.screen()->grabWindow(0).save(QStringLiteral("/home/rufus/complete.png")));
        wrote_ = true;
    }

    void nativeExclusiveAccessAndCancellation()
    {
        QVERIFY(wrote_);
        reselect();
        approvePlan();
        authenticate();
        QCOMPARE(state(), State::Writing);
        QTRY_VERIFY_WITH_TIMEOUT(controller_.writer().status().outcome.io.bytes_written > 0, 5000);
        marker(QStringLiteral("exclusive-request"));
        QTRY_COMPARE_WITH_TIMEOUT(readFile(QStringLiteral("/home/rufus/exclusive-result")), QByteArray("busy\n"), 5000);
        QCOMPARE(state(), State::Writing);
        QTest::mouseClick(window_.findChild<QPushButton*>(QStringLiteral("closeButton")), Qt::LeftButton);
        auto* dialog = window_.findChild<QMessageBox*>(QStringLiteral("cancelConfirmation"));
        QVERIFY(dialog && dialog->isVisible());
        QTest::mouseClick(dialog->button(QMessageBox::Yes), Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(state(), State::Cancelled, 15000);
        QVERIFY(!controller_.writer().isActive());
        QVERIFY(controller_.writer().status().outcome.io.bytes_written > 0);
        QVERIFY(controller_.writer().status().outcome.io.bytes_written < controller_.selection().imageSize);
        QVERIFY(controller_.writer().percent() < 100);
        marker(QStringLiteral("exclusive-release"));
        QTRY_COMPARE_WITH_TIMEOUT(readFile(QStringLiteral("/home/rufus/release-result")), QByteArray("released\n"), 5000);
        qInfo("RUFUS_NATIVE_EXCLUSIVE_AND_CANCELLED");
        cancelled_ = true;
    }

    void nativeRemovalDuringWrite()
    {
        QVERIFY(cancelled_);
        reselect();
        removedDevice_ = controller_.selection().devices.first();
        QVERIFY(stalePlan_.prepare(QStringLiteral("/opt/rufus/boot.img"), removedDevice_).isEmpty());
        approvePlan();
        authenticate();
        QCOMPARE(state(), State::Writing);
        QTRY_VERIFY_WITH_TIMEOUT(controller_.writer().status().outcome.io.bytes_written > 0, 5000);
        // The host runner reacts by deleting this emulated USB device through QMP.
        qInfo("RUFUS_REMOVE_USB_NOW");
        QTRY_COMPARE_WITH_TIMEOUT(controller_.selection().devices.size(), 0, 15000);
        QTRY_COMPARE_WITH_TIMEOUT(state(), State::Failed, 15000);
        QVERIFY(!controller_.writer().isActive());
        QVERIFY(controller_.writer().percent() < 100);
        QVERIFY(controller_.writer().status().outcome.io.bytes_written < controller_.selection().imageSize);
        qInfo("RUFUS_NATIVE_REMOVAL_FAILED_CLEANLY");
    }

    void nativeReattachment()
    {
        QCOMPARE(state(), State::Failed);
        QVERIFY(!removedDevice_.id.isEmpty());
        qInfo("RUFUS_REATTACH_USB_NOW");
        QTRY_COMPARE_WITH_TIMEOUT(controller_.selection().devices.size(), 1, 15000);
        const rufus::Device device = controller_.selection().devices.first();
        QCOMPARE(device.path, removedDevice_.path);
        QCOMPARE(device.objectPath, removedDevice_.objectPath);
        QCOMPARE(device.size, removedDevice_.size);
        QVERIFY(device.identity.contains(QStringLiteral("RUFUS-TEST-USB")));
        QVERIFY(device.id != removedDevice_.id);
        QVERIFY(device.identity != removedDevice_.identity);
        QVERIFY(controller_.selection().deviceId.isEmpty());
        QVERIFY(!controller_.canStartWrite());
        controller_.selectDevice(removedDevice_.id);
        QVERIFY(controller_.selection().deviceId.isEmpty());
        rufus::UDisksTarget stale(QDBusConnection::systemBus(), stalePlan_);
        stale.check();
        QTRY_COMPARE_WITH_TIMEOUT(stale.state(), rufus::UDisksTarget::State::Failed, 5000);
        QVERIFY(!stale.problem().isEmpty());
        qInfo("RUFUS_NATIVE_REATTACH_REJECTED_STALE_PLAN");
        reattached_ = true;
    }

    void nativeLinuxImages_data()
    {
        QTest::addColumn<QString>("firmware");
        QTest::newRow("bios") << QStringLiteral("bios");
        QTest::newRow("uefi") << QStringLiteral("uefi");
    }

    void nativeLinuxImages()
    {
        QFETCH(QString, firmware);
        QVERIFY(reattached_);
        reselect();
        selectImage(QStringLiteral("/opt/rufus/openwrt-%1.img").arg(firmware));
        QVERIFY(controller_.canStartWrite());
        approvePlan();
        authenticate();
        QCOMPARE(state(), State::Writing);
        QTRY_VERIFY_WITH_TIMEOUT(!controller_.writer().isActive(), 90000);
        QCOMPARE(state(), State::Complete);
        QCOMPARE(controller_.writer().percent(), 100);
        QCOMPARE(controller_.writer().status().outcome.io.bytes_written, controller_.selection().imageSize);
        QCOMPARE(controller_.writer().status().outcome.io.bytes_verified, controller_.selection().imageSize);
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(QStringLiteral("/dev/sda2")), 10000);
        // The guest observer copies the closed device before another write can start.
        marker(QStringLiteral("snapshot-%1").arg(firmware));
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(QStringLiteral("/home/rufus/snapshot-%1-done").arg(firmware)), 15000);
        qInfo().noquote() << QStringLiteral("RUFUS_OPENWRT_WRITTEN_%1").arg(firmware);
    }

    void cleanupTestCase()
    {
        agent_.terminate();
        if (!agent_.waitForFinished(2000)) { agent_.kill(); agent_.waitForFinished(2000); }
    }
};

int main(int argc, char* argv[])
{
    // Check before QApplication, constructing discovery or connecting to any bus.
    if (!fixtureGuest()) { fprintf(stderr, "This test requires the disposable Rufus USB guest.\n"); return 77; }
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication application(argc, argv);
    TestUsbGuest test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_usb_guest.moc"
