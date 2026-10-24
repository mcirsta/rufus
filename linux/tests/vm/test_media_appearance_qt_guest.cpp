// SPDX-License-Identifier: GPL-3.0-or-later
// Only called by the guarded disposable guest; model the ordinary caller UID.
#include "qt/selection_controller.h"
#include "platform/device_selection.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <cstdio>
#include <cstring>
#include <unistd.h>

namespace
{
thread_local bool caller = false;
void require(bool okay, const char* message) { if (!okay) qFatal("Appearance Qt guest: %s", message); }
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
    const bool oldBios = argc == 2 && strcmp(argv[1], "--old-bios") == 0;
    const QString parent = QStringLiteral("/proc/%1/exe").arg(getppid());
    const bool guarded = getuid() == 0 && geteuid() == 0
        && read("/sys/class/dmi/id/product_name") == "RufusMBRFixture\n"
        && (oldBios ? read("/etc/rufus-old-bios-fixture") == "old-bios\n"
            : read("/etc/rufus-media-appearance-fixture") == "appearance\n")
        && QFile::symLinkTarget(parent).endsWith(QStringLiteral("/test_mbr_handoff_guest"));
    if (!guarded) return 77;
    QCoreApplication application(argc, argv);
    QFile input; require(input.open(stdin, QIODevice::ReadOnly), "identity pipe");
    rufus::PrepareImageRequest expected;
    require(rufus::DecodePrepareRequest(QJsonDocument::fromJson(input.readLine(rufus::PrepareMessageBytes)).object(), expected)
        && (oldBios ? expected.oldBiosFixes && expected.grub2 && !expected.extendedLabel
            : expected.nonBootable && expected.extendedLabel) && expected.fileSystem == FS_FAT32, "typed option job");
    rufus::UDisksObjects objects; rufus::Device device;
    require(rufus::ReadPrepareDeviceSnapshot(QDBusConnection::systemBus(), expected, objects, device) == 0, "fresh target");
    device.serviceOwner = expected.serviceOwner; device.id = device.objectPath;
    caller = true;
    rufus::SelectionController controller;
    require(controller.extendedLabelEnabled(), "Windows default is checked");
    controller.isoWriter().configure(QDBusConnection::systemBus(), QStringLiteral("/usr/bin/rufus-prepare-helper"));
    controller.setDiscovery({device}, {}); controller.setBootType(oldBios ? BT_GRUB2 : BT_NON_BOOTABLE);
    if (oldBios) { controller.setExtendedLabel(false); controller.setOldBiosFixes(true); }
    controller.setFileSystem(FS_FAT32);
    controller.setPartitionScheme(expected.mbr ? rufus::PartitionScheme::Mbr : rufus::PartitionScheme::Gpt);
    controller.setVolumeLabel(oldBios ? expected.label : expected.displayLabel);
    require(controller.canStartWrite(), "ordinary START admission");
    controller.startWrite();
    auto& writer = controller.isoWriter();
    QElapsedTimer timer; timer.start();
    while (writer.state() == rufus::IsoWriteState::Checking)
    {
        QCoreApplication::processEvents(); require(timer.elapsed() < 30000, "preflight deadline"); usleep(1000);
    }
    require(writer.state() == rufus::IsoWriteState::AwaitingConfirmation, "preflight before confirmation");
    require(oldBios ? writer.request().oldBiosFixes && writer.request().grub2
        : writer.request().displayLabel == expected.displayLabel && writer.request().extendedLabel, "frozen option");
    require(read("/run/format-launches").isEmpty(), "no authorization before confirmation");
    writer.confirm(); timer.restart(); bool held = false;
    while (writer.isActive())
    {
        QCoreApplication::processEvents(); require(timer.elapsed() < 90000, "creation deadline");
        if (QFile::exists(QStringLiteral("/run/format-exit-held")) && writer.isActive())
        {
            require(writer.state() != rufus::IsoWriteState::Complete && writer.progressPosition() < MAX_PROGRESS, "READY waits for launcher exit");
            held = true;
        }
        usleep(1000);
    }
    if (writer.state() != rufus::IsoWriteState::Complete) qFatal("Appearance Qt: %s", qPrintable(writer.statusText()));
    require(held && (oldBios || writer.outcome().appearanceDone) && writer.outcome().copiedBytes == 0, "verified and helper joined");
    require(writer.progressPosition() == MAX_PROGRESS && read("/run/format-launches") == "launch\n", "one authorization and full READY");
    puts(oldBios ? "RUFUS_OLD_BIOS_QT_DONE AUTH=1 JOINED=1 READY=100 FROZEN=1"
        : "RUFUS_APPEARANCE_QT_DONE AUTH=1 JOINED=1 READY=100 DEFAULT=1");
    return 0;
}
