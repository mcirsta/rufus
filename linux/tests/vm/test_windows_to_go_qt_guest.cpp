// SPDX-License-Identifier: GPL-3.0-or-later
// Guarded ordinary selection/controller route; the real helper writes only the
// disposable QEMU USB. Authorization is supplied by the shared guest launcher.
#include "qt/selection_controller.h"
#include "platform/device_selection.h"
#include "unattend.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QSignalSpy>
#include <cstdio>
#include <unistd.h>

namespace
{
bool caller = false;
void require(bool valid, const char* text) { if (!valid) qFatal("To Go Qt guest: %s", text); }
QByteArray read(const char* path)
{
    QFile file(QString::fromLatin1(path));
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
void settle(rufus::IsoWriteController& writer, QElapsedTimer& timer)
{
    while (writer.state() == rufus::IsoWriteState::Checking) {
        QCoreApplication::processEvents(); require(timer.elapsed() < 60000, "preflight deadline"); usleep(1000);
    }
}
}
extern "C" uid_t __real_getuid();
extern "C" uid_t __wrap_getuid() { return caller ? 1000 : __real_getuid(); }
int main(int argc, char** argv)
{
    const QString parent = QStringLiteral("/proc/%1/exe").arg(getppid());
    const bool guarded = getuid() == 0 && geteuid() == 0
        && read("/sys/class/dmi/id/product_name") == "RufusMBRFixture\n"
        && read("/etc/rufus-windows-togo-fixture") == "windows-togo\n"
        && QFile::symLinkTarget(parent).endsWith(QStringLiteral("/test_mbr_handoff_guest"));
    if (!guarded) return 77;
    QCoreApplication application(argc, argv);
    QFile input; require(input.open(stdin, QIODevice::ReadOnly), "identity pipe");
    rufus::PrepareImageRequest expected;
    require(rufus::DecodePrepareRequest(QJsonDocument::fromJson(input.readLine(rufus::PrepareMessageBytes)).object(), expected)
        && expected.windowsToGo, "To Go request");
    rufus::UDisksObjects objects; rufus::Device device;
    require(rufus::ReadPrepareDeviceSnapshot(QDBusConnection::systemBus(), expected, objects, device) == 0, "fresh target");
    device.serviceOwner = expected.serviceOwner; device.id = device.objectPath;
    caller = true;
    rufus::SelectionController controller;
    controller.isoWriter().configure(QDBusConnection::systemBus(), QStringLiteral("/usr/bin/rufus-prepare-helper"));
    controller.setDiscovery({device}, {});
    controller.selectImage(expected.imagePath);
    QElapsedTimer timer; timer.start();
    while (controller.isAnalyzing()) {
        QCoreApplication::processEvents(); require(timer.elapsed() < 60000, "source scan deadline"); usleep(1000);
    }
    require(controller.canSelectWindowsToGo(), "Windows To Go offered");
    controller.setWindowsToGo(true);
    controller.setPartitionScheme(expected.mbr ? rufus::PartitionScheme::Mbr : rufus::PartitionScheme::Gpt);
    if (expected.mbr) controller.setWindowsToGoTarget(expected.windowsToGoTarget);
    controller.setExtendedLabel(false); controller.setVolumeLabel(expected.label);
    require(controller.canStartWrite(), "ordinary START"); controller.startWrite();
    auto& writer = controller.isoWriter();
    QSignalSpy logs(&writer, &rufus::IsoWriteController::logMessage);
    settle(writer, timer);
    if (writer.state() == rufus::IsoWriteState::ChoosingWindowsArchive) {
        writer.chooseWindowsArchive(expected.windowsInstallPath); settle(writer, timer);
    }
    require(writer.state() == rufus::IsoWriteState::ChoosingWindowsEdition, "native edition choice");
    writer.chooseWindowsEdition(2); settle(writer, timer);
    require(writer.state() == rufus::IsoWriteState::ChoosingWindowsOptions, "To Go options");
    rufus::PrepareImageRequest options;
    options.windowsFlags = UNATTEND_OFFLINE_INTERNAL_DRIVES;
    writer.chooseWindowsOptions(options); settle(writer, timer);
    if (writer.state() != rufus::IsoWriteState::AwaitingConfirmation)
        qFatal("To Go preflight: %s", qPrintable(writer.statusText()));
    require(writer.request().windowsEdition == 2 && writer.request().windowsToGo
        && writer.request().mbr == expected.mbr && writer.request().windowsToGoTarget == expected.windowsToGoTarget,
        "edition and layout frozen");
    require(read("/run/format-launches").isEmpty(), "no authorization before confirmation");
    writer.confirm(); timer.restart(); bool held = false;
    while (writer.isActive()) {
        QCoreApplication::processEvents(); require(timer.elapsed() < 240000, "creation deadline");
        if (QFile::exists(QStringLiteral("/run/format-exit-held")) && writer.isActive()) {
            require(writer.state() != rufus::IsoWriteState::Complete && writer.progressPosition() < MAX_PROGRESS,
                "READY waits for launcher exit");
            held = true;
        }
        usleep(1000);
    }
    if (writer.state() != rufus::IsoWriteState::Complete) {
        for (const auto& entry : logs) fprintf(stderr, "TOGO_QT_LOG %s\n", qPrintable(entry.first().toString()));
        qFatal("To Go creation: %s (started=%d)", qPrintable(writer.statusText()), int(writer.outcome().started));
    }
    require(held && writer.outcome().windowsToGoDone && writer.outcome().copiedBytes == writer.check().windowsApplyBytes,
        "applied bytes and joined helper");
    require(writer.progressPosition() == MAX_PROGRESS && read("/run/format-launches") == "launch\n", "one authorization, full READY");
    printf("RUFUS_TOGO_QT_DONE MBR=%d TARGET=%d AUTH=1 JOINED=1 READY=100 EDITION=2\n", int(expected.mbr), expected.windowsToGoTarget);
    return 0;
}
