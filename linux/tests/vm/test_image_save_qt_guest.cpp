// SPDX-License-Identifier: GPL-3.0-or-later
// The guarded root guest launches this process with real uid/gid 1000.
#include "qt/selection_controller.h"
#include "platform/device_selection.h"
#include <QCoreApplication>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusReply>
#include <QElapsedTimer>
#include <QFile>
#include <QSignalSpy>
#include <QThread>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>

namespace {
void require(bool valid, const char* message) { if (!valid) qFatal("Image saving guest: %s", message); }
QDBusMessage control(const QString& method, const QList<QVariant>& args = {})
{
    QDBusMessage message = QDBusMessage::createMethodCall(QStringLiteral("org.rufus.TestUDisks"),
        QStringLiteral("/org/freedesktop/UDisks2"), QStringLiteral("org.rufus.TestUDisks"), method);
    message.setArguments(args);
    const QDBusMessage reply = QDBusConnection::systemBus().call(message);
    require(reply.type() == QDBusMessage::ReplyMessage, "fixture control");
    return reply;
}
void printLog(const QString& message) { fprintf(stderr, "%s\n", qPrintable(message)); }
QVariantMap stats() { return qdbus_cast<QVariantMap>(control(QStringLiteral("Stats")).arguments().at(0)); }
}
int main(int argc, char** argv)
{
    if (getuid() != 1000 || geteuid() != 1000 || getgid() != 1000 || argc != 4) return 77;
    QFile guard(QStringLiteral("/etc/rufus-save-qt-fixture"));
    if (!guard.open(QIODevice::ReadOnly) || guard.readAll() != "save-qt\n") return 77;
    QCoreApplication app(argc, argv);
    const QString mode = QString::fromLatin1(argv[1]), path = QString::fromLocal8Bit(argv[2]);
    const QByteArray expected = QByteArray::fromHex(argv[3]);
    const bool exportBatch = QFile::exists(QStringLiteral("/etc/rufus-save-export-fixture"));
    const bool readOnlyBatch = QFile::exists(QStringLiteral("/etc/rufus-save-read-only-fixture"));
    const bool iso = path.endsWith(QLatin1String(".iso"));
    const bool helperSource = exportBatch && (!readOnlyBatch || iso || mode == QLatin1String("mounted-vhdx"));
    if (exportBatch) qputenv("PATH", QByteArray("/run/save-tools:") + qgetenv("PATH"));
    require(expected.size() == 32, "expected raw disk digest");
    const int forbidden = open("/dev/sda", O_RDONLY | O_CLOEXEC);
    require(forbidden < 0 && errno == EACCES, "unprivileged caller has no raw device access");
    qDBusRegisterMetaType<rufus::UDisksInterfaces>(); qDBusRegisterMetaType<rufus::UDisksObjects>();
    QDBusMessage query = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.UDisks2"),
        QStringLiteral("/org/freedesktop/UDisks2"), QStringLiteral("org.freedesktop.DBus.ObjectManager"), QStringLiteral("GetManagedObjects"));
    const QDBusReply<rufus::UDisksObjects> snapshot = QDBusConnection::systemBus().call(query);
    if (!snapshot.isValid()) qFatal("Image saving guest discovery: %s", qPrintable(snapshot.error().message()));
    rufus::Device device;
    require(rufus::DescribeUDisksDevice(snapshot.value(), QStringLiteral("/org/freedesktop/UDisks2/block_devices/sda"), device, false, rufus::DeviceAccess::Read), "selected USB");
    device.id = device.objectPath;
    device.serviceOwner = QDBusConnection::systemBus().interface()->serviceOwner(QStringLiteral("org.freedesktop.UDisks2")).value();
    rufus::SelectionController controller;
    QObject::connect(&controller, &rufus::SelectionController::logMessage, printLog);
    controller.saver().setConnection(QDBusConnection::systemBus());
    if (exportBatch) controller.saver().setHelper(QStringLiteral("/usr/bin/rufus-save-guest-helper"));
    rufus::UDisksClient discovery(QDBusConnection::systemBus());
    QObject::connect(&discovery, &rufus::UDisksClient::devicesChanged, &controller, &rufus::SelectionController::setDiscovery);
    discovery.start();
    QElapsedTimer discoveryTimer; discoveryTimer.start();
    while (!controller.selection().discoveryAvailable || controller.selection().devices.isEmpty()) {
        require(discoveryTimer.elapsed() < 10000, "read-only discovery deadline");
        QCoreApplication::processEvents(); QThread::msleep(1);
    }
    require(controller.selection().devices.size() == 1, "single eligible source from production discovery");
    if (readOnlyBatch) {
        require(controller.selection().devices.first().readOnly, "read-only discovery preserved");
        controller.setBootType(BT_NON_BOOTABLE);
        require(!controller.canStartWrite(), "read-only source cannot be formatted");
        controller.startWrite();
        require(!controller.isBusy(), "read-only START refuses before authorization");
        controller.setBootType(BT_IMAGE);
    }
    require(controller.canSaveDevice(), "ordinary save admission without selecting an image");
    if (mode == "cancel") control(QStringLiteral("HoldOpen"), {true});
    if (mode == "deny") control(QStringLiteral("OpenMode"), {QStringLiteral("deny")});
    const int opens = stats().value(QStringLiteral("opens")).toInt();
    QSignalSpy finished(&controller.saver(), &rufus::ImageSaveController::finished);
    require(controller.startSaveDevice(path), "ordinary save start");
    require(controller.isBusy() && !controller.canStartWrite() && !controller.canSaveDevice(), "controls locked through job");
    QElapsedTimer timer; timer.start();
    bool cancelled = false, sawProgress = false, heldExit = false;
    qint64 diagnostic = 15000;
    while (controller.saver().isActive()) {
        require(timer.elapsed() < 300000, "capture completion deadline");
        require(controller.saver().progressPosition() < MAX_PROGRESS, "no full READY before worker joins");
        QCoreApplication::processEvents();
        heldExit = heldExit || QFile::exists(QStringLiteral("/run/save-exit-held"));
        sawProgress = sawProgress || controller.saver().state() == rufus::ImageSaveState::Saving
            || controller.saver().state() == rufus::ImageSaveState::Verifying;
        if (mode == "helper-cancel" && !cancelled && sawProgress) {
            controller.saver().cancel(); cancelled = true;
        }
        if (mode == "cancel" && !cancelled && stats().value(QStringLiteral("openHeld")).toInt() == 1) {
            controller.saver().cancel();
            require(controller.saver().isActive() && controller.saver().state() == rufus::ImageSaveState::Cancelling, "late authorization cancellation waits");
            control(QStringLiteral("ReplyOpen")); control(QStringLiteral("HoldOpen"), {false}); cancelled = true;
        }
        if (timer.elapsed() >= diagnostic) {
            printf("RUFUS_SAVE_QT_WAIT %s ELAPSED=%lld STATE=%d\n", qPrintable(mode), static_cast<long long>(timer.elapsed()), int(controller.saver().state()));
            fflush(stdout); diagnostic += 15000;
        }
        QThread::msleep(1);
    }
    require(finished.size() == 1 && !controller.isBusy(), "one joined terminal result");
    const auto& saved = controller.saver().result();
    printf("RUFUS_SAVE_QT_RESULT %s ERROR=%d COMMITTED=%d STATUS=%s\n", qPrintable(mode), saved.error, saved.committed, qPrintable(controller.saver().statusText())); fflush(stdout);
    if (mode == "cancel" || mode == "helper-cancel" || mode == "deny" || mode == "busy" || mode == "iso-cleanup") {
        require(saved.error == ((mode == "cancel" || mode == "helper-cancel") ? ECANCELED : mode == "deny" ? EACCES : EBUSY) && !saved.committed, "cancellation/denial result");
        QFile old(path); require(old.open(QIODevice::ReadOnly) && old.readAll() == "OLD BACKUP", "original output preserved");
    } else {
        if (saved.error != 0) qFatal("Image saving guest result: %s", qPrintable(controller.saver().statusText()));
        require(sawProgress && saved.committed && (iso ? saved.bytes > 0 && saved.sha256.size() == 32
            : saved.bytes == device.size && saved.sha256 == expected), "verified saved disk or tree");
        require(controller.saver().state() == rufus::ImageSaveState::Complete && controller.saver().progressPosition() == MAX_PROGRESS, "full green READY after join");
        controller.setDiscovery({}, {});
        require(controller.saver().state() == rufus::ImageSaveState::Complete && finished.size() == 1, "unplug preserves success");
    }
    require(stats().value(QStringLiteral("opens")).toInt() == opens + (helperSource ? 0 : 1) && stats().value(QStringLiteral("unexpected")).toInt() == 0,
        "one interactive read-only acquisition per whole capture");
    if (helperSource) require(heldExit, "source helper result held until process exit");
    printf("RUFUS_SAVE_QT_DONE %s ERROR=%d COMMITTED=%d BYTES=%llu SHA256=%s OPENS=%d UID=%u HELD=%d\n", qPrintable(mode),
        saved.error, saved.committed, static_cast<unsigned long long>(saved.bytes), saved.sha256.toHex().constData(), helperSource ? 0 : 1, unsigned(getuid()), heldExit);
    return 0;
}
