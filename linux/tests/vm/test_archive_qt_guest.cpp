// SPDX-License-Identifier: GPL-3.0-or-later
// Ordinary supplementary ZIP selection on an owned disposable guest only.
#include "qt/selection_controller.h"
#include "platform/device_selection.h"
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
void require(bool value, const char* text) { if (!value) qFatal("ZIP Qt guest: %s", text); }
QByteArray read(const char* path)
{
    QFile file(QString::fromLatin1(path));
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
}
extern "C" uid_t __real_getuid();
extern "C" uid_t __wrap_getuid() { return caller ? 1000 : __real_getuid(); }
int main(int argc, char** argv)
{
    const QString parent = QStringLiteral("/proc/%1/exe").arg(getppid());
    const bool guarded = getuid() == 0 && geteuid() == 0
        && read("/sys/class/dmi/id/product_name") == "RufusMBRFixture\n"
        && read("/etc/rufus-zip-supplement-fixture") == "zip-supplement\n"
        && QFile::symLinkTarget(parent).endsWith(QStringLiteral("/test_mbr_handoff_guest"));
    if (!guarded) return 77;
    QCoreApplication application(argc, argv);
    QFile input; require(input.open(stdin, QIODevice::ReadOnly), "identity pipe");
    rufus::PrepareImageRequest expected;
    require(rufus::DecodePrepareRequest(QJsonDocument::fromJson(input.readLine(rufus::PrepareMessageBytes)).object(), expected)
        && expected.nonBootable && expected.archive.present() && !expected.extendedLabel, "archive format request");
    rufus::UDisksObjects objects; rufus::Device device;
    require(rufus::ReadPrepareDeviceSnapshot(QDBusConnection::systemBus(), expected, objects, device) == 0, "fresh target");
    device.serviceOwner = expected.serviceOwner; device.id = device.objectPath;
    caller = true; // Model the caller in the GUI and its read-only worker threads.
    rufus::SelectionController controller;
    controller.isoWriter().configure(QDBusConnection::systemBus(), QStringLiteral("/usr/bin/rufus-prepare-helper"));
    controller.setDiscovery({device}, {}); controller.setBootType(BT_NON_BOOTABLE);
    controller.setExtendedLabel(false); controller.setFileSystem(FS_FAT32);
    controller.setPartitionScheme(rufus::PartitionScheme::Mbr); controller.setVolumeLabel(expected.label);
    controller.selectArchive(expected.archive.path);
    require(controller.canStartWrite(), "ordinary START"); controller.startWrite();
    auto& writer = controller.isoWriter();
    QSignalSpy logs(&writer, &rufus::IsoWriteController::logMessage);
    QElapsedTimer timer; timer.start();
    while (writer.state() == rufus::IsoWriteState::Checking) {
        QCoreApplication::processEvents(); require(timer.elapsed() < 60000, "preflight deadline"); usleep(1000);
    }
    if (writer.state() != rufus::IsoWriteState::AwaitingConfirmation) qFatal("ZIP Qt preflight: %s", qPrintable(writer.statusText()));
    require(writer.request().archive.path == expected.archive.path && writer.check().archiveFiles == 4, "archive frozen and validated");
    require(read("/run/format-launches").isEmpty(), "no authorization before confirmation");
    writer.confirm(); timer.restart(); bool held = false;
    while (writer.isActive()) {
        QCoreApplication::processEvents(); require(timer.elapsed() < 180000, "creation deadline");
        if (QFile::exists(QStringLiteral("/run/format-exit-held")) && writer.isActive()) {
            require(writer.state() != rufus::IsoWriteState::Complete && writer.progressPosition() < MAX_PROGRESS, "READY waits for launcher exit");
            held = true;
        }
        usleep(1000);
    }
    if (writer.state() != rufus::IsoWriteState::Complete) {
        for (const auto& entry : logs) fprintf(stderr, "ARCHIVE_QT_LOG %s\n", qPrintable(entry.first().toString()));
        qFatal("ZIP Qt creation: %s (started=%d)", qPrintable(writer.statusText()), int(writer.outcome().started));
    }
    require(held && writer.outcome().archiveDone && writer.outcome().archiveBytes == writer.check().archiveBytes,
        "verified archive and joined helper");
    require(writer.progressPosition() == MAX_PROGRESS && read("/run/format-launches") == "launch\n", "one authorization, full READY");
    puts("RUFUS_ARCHIVE_QT_DONE AUTH=1 JOINED=1 READY=100 ARCHIVE=1");
    return 0;
}
