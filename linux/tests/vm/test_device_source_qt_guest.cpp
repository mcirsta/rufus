// SPDX-License-Identifier: GPL-3.0-or-later
#include "qt/selection_controller.h"
#include "qt/device_policy.h"
#include "qt/application_storage.h"
#include "platform/device_selection.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QSignalSpy>
#include <atomic>
#include <cstdio>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
namespace
{
std::atomic_bool caller{false};
void require(bool condition, const char* message) { if (!condition) qFatal("Device/source Qt: %s", message); }
QByteArray read(const char* path) { QFile file(QString::fromLatin1(path)); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray(); }
}
extern "C" uid_t __real_getuid();
extern "C" uid_t __real_geteuid();
extern "C" uid_t __wrap_getuid() { return caller ? 1000 : __real_getuid(); }
extern "C" uid_t __wrap_geteuid() { return caller ? 1000 : __real_geteuid(); }
int main(int argc, char** argv)
{
    const bool captureArgument = argc == 2 && QByteArray(argv[1]) == "capture";
    const bool expectedUser = captureArgument ? getuid() == 1000 && geteuid() == 1000 : getuid() == 0 && geteuid() == 0;
    if (!expectedUser || argc != 2 || read("/etc/rufus-device-source-fixture") != "device-source\n"
        || read("/sys/class/dmi/id/product_name") != "RufusMBRFixture\n"
        || (!captureArgument && !QFile::symLinkTarget(QStringLiteral("/proc/%1/exe").arg(getppid())).endsWith(QStringLiteral("/test_mbr_handoff_guest")))) return 77;
    QCoreApplication app(argc, argv);
    QFile input; require(input.open(stdin, QIODevice::ReadOnly), "request pipe");
    rufus::PrepareImageRequest request;
    require(rufus::DecodePrepareRequest(QJsonDocument::fromJson(input.readLine(rufus::PrepareMessageBytes)).object(), request), "typed request");
    const QString command = QString::fromLatin1(argv[1]);
    const bool capture = command == QLatin1String("capture");
    if (capture) request.deviceClass = rufus::DeviceClass::Optical;
    rufus::UDisksObjects objects; rufus::Device device;
    const int checked = capture ? rufus::ReadSaveDeviceSnapshot(QDBusConnection::systemBus(), request, objects, device)
        : rufus::ReadPrepareDeviceSnapshot(QDBusConnection::systemBus(), request, objects, device);
    require(checked == 0, "fresh native snapshot");
    device.id = device.objectPath; device.serviceOwner = request.serviceOwner;
    if (capture) {
        const int forbidden = open(QFile::encodeName(request.path).constData(), O_RDONLY | O_CLOEXEC);
        require(forbidden < 0 && errno == EACCES, "unprivileged optical caller has no raw access");
    }
    caller = true;
    QString problem;
    require(rufus::ConfigureApplicationStorage(QStringLiteral("/run/caller-home/rufus-linux"), true, problem) == 0, "private settings");
    rufus::SelectionController controller;
    controller.configureDeviceOperations(QStringLiteral("/usr/bin/rufus-prepare-helper"));
    controller.setDiscovery({device}, {});
    QSignalSpy logs(&controller, &rufus::SelectionController::logMessage);
    QElapsedTimer timer; timer.start();
    bool held = false;
    if (capture) {
        require(controller.startSaveDevice(QStringLiteral("/run/caller-home/disc.iso")), "optical save starts");
        while (controller.saver().isActive()) {
            QCoreApplication::processEvents(); require(timer.elapsed() < 120000, "capture deadline"); usleep(1000);
        }
        if (controller.saver().state() != rufus::ImageSaveState::Complete) {
            for (const auto& entry : logs) fprintf(stderr, "SOURCE_LOG %s\n", qPrintable(entry.first().toString()));
            qFatal("Optical capture: %s", qPrintable(controller.saver().statusText()));
        }
        require(controller.saver().progressPosition() == MAX_PROGRESS, "capture READY after join");
        require(read("/run/format-launches").isEmpty(), "optical uses one UDisks acquisition without helper authorization");
    } else if (command == QLatin1String("restore")) {
        controller.writer().setConnection(QDBusConnection::systemBus());
        controller.writer().setHelper(QStringLiteral("/usr/bin/rufus-prepare-helper"));
        controller.setBootType(BT_IMAGE);
        controller.selectImage(QStringLiteral("/mapped-child.vhdx"));
        while (controller.isAnalyzing()) { QCoreApplication::processEvents(); require(timer.elapsed() < 120000, "mapped source analysis"); usleep(1000); }
        require(controller.canStartWrite(), "mapped parent source accepted"); controller.startWrite();
        while (controller.writer().status().state == rufus::WriteState::Checking) {
            QCoreApplication::processEvents(); require(timer.elapsed() < 180000, "mapped restore preflight"); usleep(1000);
        }
        require(controller.writer().status().state == rufus::WriteState::AwaitingConfirmation, "mapped restore confirmation");
        controller.writer().confirm();
        while (controller.writer().isActive()) {
            QCoreApplication::processEvents(); require(timer.elapsed() < 480000, "mapped restore deadline");
            if (QFile::exists(QStringLiteral("/run/format-exit-held")) && controller.writer().isActive()) {
                require(controller.writer().status().state != rufus::WriteState::Complete, "mapped READY waits for exit"); held = true;
                QFile release(QStringLiteral("/run/format-exit-release"));
                require(release.open(QIODevice::WriteOnly) && release.write("release\n") == 8, "mapped launcher release");
            }
            usleep(1000);
        }
        if (controller.writer().status().state != rufus::WriteState::Complete) {
            for (const auto& entry : logs) fprintf(stderr, "RESTORE_LOG %s\n", qPrintable(entry.first().toString()));
            qFatal("Mapped restore failed: %s", qPrintable(controller.writer().statusText()));
        }
        require(held && read("/run/format-launches") == "launch\n", "mapped restore one authorization and joined exit");
    } else {
        if (command == QLatin1String("unlocked")) rufus::SetDeviceLocking(false);
        require(controller.startDeviceOperation(request.deviceOperation), "device command starts");
        while (controller.deviceOperation()->isActive()) {
            QCoreApplication::processEvents(); require(timer.elapsed() < 120000, "operation deadline");
            if (QFile::exists(QStringLiteral("/run/format-exit-held")) && controller.deviceOperation()->isActive()) {
                require(controller.deviceOperation()->state() != rufus::PrepareImageProcess::State::Complete, "READY waits for launcher exit"); held = true;
                QFile release(QStringLiteral("/run/format-exit-release"));
                require(release.open(QIODevice::WriteOnly) && release.write("release\n") == 8, "explicit operation launcher release");
            }
            usleep(1000);
        }
        if (controller.deviceOperation()->state() != rufus::PrepareImageProcess::State::Complete) {
            for (const auto& entry : logs) fprintf(stderr, "DEVICE_LOG %s\n", qPrintable(entry.first().toString()));
            qFatal("Device command: %s", qPrintable(controller.deviceOperation()->problem()));
        }
        require(held && read("/run/format-launches") == "launch\n", "one authorization and joined launcher");
    }
    printf("RUFUS_DEVICE_SOURCE_QT COMMAND=%s AUTH=1 JOINED=1 READY=100\n", argv[1]);
    return 0;
}
