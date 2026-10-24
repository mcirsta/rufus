// SPDX-License-Identifier: GPL-3.0-or-later
#include "qt/main_window.h"
#include "qt/selection_controller.h"
#include "qt/image_mode_dialog.h"
#include "platform/udisks_client.h"
#include "platform/prepare_device.h"
#include "platform/raw_device.h"
#include "platform/uefi_ntfs.h"
#include <QApplication>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusReply>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QLineEdit>
#include <QMessageBox>
#include <QProcess>
#include <QProgressBar>
#include <QRadioButton>
#include <QDialogButtonBox>
#include <QtEndian>
#include <QPushButton>
#include <QSettings>
#include <QTest>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

static bool sourceCaller = true;
extern "C" uid_t __real_getuid();
extern "C" uid_t __wrap_getuid() { return sourceCaller ? 1000 : __real_getuid(); }
namespace
{
constexpr quint64 diskBytes = 2ULL * 1024 * 1024 * 1024;
constexpr quint64 persistenceBytes = 256ULL * 1024 * 1024;
constexpr int timeoutMs = 180000;
void require(bool valid, const char* message)
{
    if (!valid) qFatal("Persistence guest failed: %s (errno=%d)", message, errno);
}
QByteArray readFile(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
void logMessage(const QString& message) { fprintf(stderr, "%s\n", qPrintable(message)); }
void writeFile(const QString& path, const QByteArray& text)
{
    QFile file(path);
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(text) == text.size(), "write control");
    file.close();
}
rufus::Device publishDevice(const QDBusConnection& bus, quint64 capacity, bool uefiNtfs)
{
    qDBusRegisterMetaType<rufus::UDisksInterfaces>();
    qDBusRegisterMetaType<rufus::UDisksObjects>();
    const QDBusObjectPath disk(QStringLiteral("/org/freedesktop/UDisks2/block_devices/sda"));
    const QDBusObjectPath drive(QStringLiteral("/org/freedesktop/UDisks2/drives/fixture"));
    struct stat info;
    require(stat("/dev/sda", &info) == 0 && S_ISBLK(info.st_mode), "guest USB node");
    // The initramfs has devtmpfs but no udev. Supply its ordinary partition
    // links, including the trailing UEFI:NTFS partition when required.
    require(QDir().mkpath(QStringLiteral("/dev/block")), "partition link directory");
    const unsigned partitionCount = 3;
    Q_UNUSED(uefiNtfs);
    for (unsigned number = 1; number <= partitionCount; ++number)
    {
        const QByteArray link = "/dev/block/" + QByteArray::number(major(info.st_rdev)) + ':'
            + QByteArray::number(minor(info.st_rdev) + number);
        const QByteArray partition = "../sda" + QByteArray::number(number);
        require(symlink(partition.constData(), link.constData()) == 0 || errno == EEXIST, "guest partition link");
    }
    rufus::UDisksObjects objects;
    objects[disk][QStringLiteral("org.freedesktop.UDisks2.Block")] = {
        {QStringLiteral("Drive"), QVariant::fromValue(drive)},
        {QStringLiteral("Device"), QByteArray("/dev/sda\0", 9)},
        {QStringLiteral("Size"), QVariant::fromValue(capacity)},
        {QStringLiteral("DeviceNumber"), QVariant::fromValue(quint64(info.st_rdev))},
        {QStringLiteral("ReadOnly"), false}, {QStringLiteral("HintSystem"), false},
        {QStringLiteral("HintIgnore"), false}, {QStringLiteral("Id"), QStringLiteral("widgets-fixture")}};
    objects[drive][QStringLiteral("org.freedesktop.UDisks2.Drive")] = {
        {QStringLiteral("ConnectionBus"), QStringLiteral("usb")}, {QStringLiteral("Removable"), true},
        {QStringLiteral("MediaRemovable"), true}, {QStringLiteral("MediaAvailable"), true},
        {QStringLiteral("Optical"), false}, {QStringLiteral("Serial"), QStringLiteral("RUFUS-MBR-TEST")},
        {QStringLiteral("TimeDetected"), QVariant::fromValue(quint64(1))},
        {QStringLiteral("TimeMediaDetected"), QVariant::fromValue(quint64(2))}};
    auto message = QDBusMessage::createMethodCall(QStringLiteral("org.rufus.TestUDisks"),
        QStringLiteral("/org/freedesktop/UDisks2"), QStringLiteral("org.rufus.TestUDisks"), QStringLiteral("SetObjects"));
    message.setArguments({QVariant::fromValue(objects)});
    require(bus.call(message).type() == QDBusMessage::ReplyMessage, "publish device");
    rufus::Device device;
    require(rufus::DescribeUDisksDevice(objects, disk.path(), device), "describe native USB");
    device.serviceOwner = bus.interface()->serviceOwner(QStringLiteral("org.freedesktop.UDisks2")).value();
    device.id = QStringLiteral("guest-usb");
    return device;
}

void threePartitionBackend(const QDBusConnection& bus)
{
    const auto device = publishDevice(bus, diskBytes, true);
    rufus::PrepareImageRequest request;
    request.isDevice = true;
    request.path = device.path;
    request.device = device.number;
    request.size = device.size;
    request.diskSequence = rufus::ReadDiskSequence(device.number);
    request.objectPath = device.objectPath;
    request.serviceOwner = device.serviceOwner;
    request.identity = device.identity;
    request.fileSystem = FS_NTFS;
    request.clusterSize = 4096;
    request.label = QStringLiteral("RUFUS_NATIVE");
    request.persistenceSize = persistenceBytes;
    request.persistenceExtVersion = 4;
    const GPT_IMAGE_OPTIONS options = {diskBytes, 512, 63, 4096, device.number,
        1024 * 1024, persistenceBytes};
    GPT_IMAGE_RESULT layout = {};
    sourceCaller = false;
    {
        rufus::PrepareDeviceTarget target(request);
        require(target.open() == 0, "native three-partition claim");
        require(CreateGptImagePartition(target.descriptor(), &options, &layout) == 0, "native GPT table");
        FORMAT_RESULT formatted = {};
        const int persistenceError = target.formatPersistence(layout, false, QStringLiteral("/usr/bin/mke2fs"), formatted);
        if (persistenceError != 0)
            qFatal("Native persistence error=%d exit=%d sync=%d bytes=%llu", persistenceError,
                formatted.exit_code, formatted.sync_error, static_cast<unsigned long long>(formatted.filesystem_bytes));
        const rufus::NtfsFormatOptions ntfs = {layout.size, layout.offset, 4096, request.label};
        rufus::NtfsFormatResult ntfsResult = {};
        require(target.formatNtfs(layout, ntfs, QStringLiteral("/usr/bin/mkntfs"), ntfsResult) == 0,
            "native main format and UEFI partition three");
        IMAGE_FILE_SOURCE payload = {};
        payload.fd = open("/uefi-ntfs.img", O_RDONLY | O_CLOEXEC);
        require(payload.fd >= 0 && fstat(payload.fd, &payload.info) == 0, "payload source");
        RAW_WRITE_RESULT copied = {};
        require(WriteUefiNtfsImage(&payload, "/uefi-ntfs.img", target.descriptor(), &options, &copied) == 0,
            "native third partition payload readback");
        require(close(payload.fd) == 0 && target.check() == 0, "native completion");
    }
    sourceCaller = true;
    puts("RUFUS_PERSISTENCE_THREE_PARTITION_OK");
    fflush(stdout);
}

void runJob(const QDBusConnection& bus, int ext, bool casper, bool fail)
{
    QSettings settings(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("Rufus"), QStringLiteral("Linux"));
    settings.setValue(QStringLiteral("UseExtVersion"), ext);
    settings.sync();
    for (const char* name : {"/run/format-exit-held", "/run/format-exit-release", "/run/format-launches", "/run/fail-persistence"})
        QFile::remove(QString::fromLatin1(name));
    if (fail) writeFile(QStringLiteral("/run/fail-persistence"), "fail\n");
    rufus::SelectionController controller;
    QObject::connect(&controller, &rufus::SelectionController::logMessage, logMessage);
    controller.isoWriter().configure(bus, QStringLiteral("/usr/bin/rufus-prepare-helper"));
    controller.writer().setConnection(bus);
    controller.writer().setHelper(QStringLiteral("/usr/bin/rufus-prepare-helper"));
    rufus::MainWindow window(controller);
    controller.setDiscovery({publishDevice(bus, diskBytes, false)}, {});
    window.show();
    controller.selectImage(casper ? QStringLiteral("/casper.iso") : QStringLiteral("/debian.iso"));
    QElapsedTimer timer;
    timer.start();
    while (controller.isAnalyzing() && timer.elapsed() < timeoutMs) QTest::qWait(10);
    require(!controller.isAnalyzing() && controller.canSetPersistence(), "live source analysis");
    require(controller.selection().partition == rufus::PartitionScheme::Mbr, "Syslinux MBR selection");
    controller.setFileSystem(FS_FAT32);
    controller.setClusterSize(4096);
    controller.setVolumeLabel(QStringLiteral("RUFUS_LIVE"));
    auto* size = window.findChild<QLineEdit*>(QStringLiteral("persistenceSize"));
    auto* start = window.findChild<QPushButton*>(QStringLiteral("startButton"));
    auto* progress = window.findChild<QProgressBar*>(QStringLiteral("progressBar"));
    require(size && start && progress && size->isEnabled(), "persistence controls available");
    require(start->isEnabled(), "START available after analysis");
    size->setText(QStringLiteral("256"));
    require(QMetaObject::invokeMethod(size, "editingFinished"), "choose size");
    require(controller.selection().persistenceSize == persistenceBytes, "selected size");
    QTest::mouseClick(start, Qt::LeftButton);
    timer.restart();
    while (controller.isoWriter().state() == rufus::IsoWriteState::Checking && timer.elapsed() < timeoutMs)
        QTest::qWait(10);
    if (controller.isoWriter().state() != rufus::IsoWriteState::AwaitingConfirmation)
        qFatal("Persistence preflight failed: %s", qPrintable(controller.isoWriter().statusText()));
    require(controller.isoWriter().request().persistenceSize == persistenceBytes
        && controller.isoWriter().request().persistenceExtVersion == ext, "frozen persistence request");
    controller.setPersistenceSize(0);
    require(controller.selection().persistenceSize == persistenceBytes, "busy selection frozen");
    auto* confirmation = window.findChild<QMessageBox*>(QStringLiteral("isoWriteConfirmation"));
    require(confirmation && confirmation->isVisible(), "creation confirmation");
    require(readFile(QStringLiteral("/run/format-launches")).isEmpty(), "no premature authorization");
    QTest::mouseClick(confirmation->button(QMessageBox::Ok), Qt::LeftButton);
    timer.restart();
    while (readFile(QStringLiteral("/run/format-exit-held")).isEmpty() && timer.elapsed() < timeoutMs)
        QTest::qWait(10);
    require(!readFile(QStringLiteral("/run/format-exit-held")).isEmpty(), "helper finished before launcher exit");
    require(controller.isoWriter().isActive() && progress->value() < MAX_PROGRESS, "READY held before exit");
    require(QDir(QStringLiteral("/run")).entryList({QStringLiteral("rufus-*")}, QDir::Dirs).isEmpty(), "helper mount and seed cleanup");
    writeFile(QStringLiteral("/run/format-exit-release"), "release\n");
    timer.restart();
    while (controller.isoWriter().isActive() && timer.elapsed() < timeoutMs) QTest::qWait(10);
    const bool complete = controller.isoWriter().state() == rufus::IsoWriteState::Complete;
    if (complete != !fail)
        qFatal("Persistence job outcome: %s", qPrintable(controller.isoWriter().statusText()));
    require(readFile(QStringLiteral("/run/format-launches")) == "launch\n", "one authorization for complete job");
    if (!fail)
        require(progress->value() == MAX_PROGRESS && progress->format() == QStringLiteral("READY"), "successful READY");
    else
    {
        require(progress->format() != QStringLiteral("READY"), "failure remains failed");
        require(controller.isoWriter().outcome().error == EIO, "formatter failure propagated");
    }
    const int disk = open("/dev/sda", O_RDWR | O_EXCL | O_CLOEXEC);
    require(disk >= 0 && close(disk) == 0, "claim released after helper exit");
    printf("RUFUS_PERSISTENCE_JOB EXT=%d CASPER=%d FAILED=%d LAUNCHES=1 HELD=1\n", ext, casper, fail);
    fflush(stdout);
}
#include "iso_esp_cases.inc"
}
int main(int argc, char** argv)
{
    if (__real_getuid() != 0 || geteuid() != 0 || getppid() != 1
        || readFile(QStringLiteral("/sys/class/dmi/id/product_name")) != "RufusPersistenceFixture\n") return 77;
    QApplication application(argc, argv);
    QProcess daemon, fixture;
    daemon.setProcessChannelMode(QProcess::ForwardedErrorChannel);
    daemon.start(QStringLiteral("/usr/bin/dbus-daemon"), {QStringLiteral("--nofork"),
        QStringLiteral("--print-address=1"), QStringLiteral("--config-file=/etc/rufus-test-bus.conf")});
    require(daemon.waitForStarted() && daemon.waitForReadyRead(), "private bus");
    const QByteArray address = daemon.readLine().trimmed();
    qputenv("DBUS_SYSTEM_BUS_ADDRESS", address);
    fixture.start(QStringLiteral("/usr/bin/udisks_fixture"), {QString::fromUtf8(address)});
    require(fixture.waitForStarted() && fixture.waitForReadyRead(), "UDisks fixture");
    require(fixture.readLine().trimmed() == "ready", "fixture ready");
    const auto bus = QDBusConnection::systemBus();
    if (QFile::exists(QStringLiteral("/etc/rufus-iso-esp-fixture")))
    {
        for (int filesystem : {FS_FAT16, FS_FAT32})
            for (bool persistence : {false, true}) runIsoEspJob(bus, filesystem, persistence);
        fixture.terminate(); require(fixture.waitForFinished(), "ESP fixture joined");
        daemon.terminate(); require(daemon.waitForFinished(), "ESP bus joined");
        puts("RUFUS_ISO_ESP_GUEST_OK");
        return 0;
    }
    threePartitionBackend(bus);
    runJob(bus, 2, true, true);
    runJob(bus, 2, true, false);
    writeFile(QStringLiteral("/run/check-persistence"), "2 casper-rw\n");
    QProcess inspector;
    inspector.setProcessChannelMode(QProcess::ForwardedChannels);
    inspector.start(QStringLiteral("/usr/bin/bash"), {QStringLiteral("/inspect-persistence")});
    require(inspector.waitForFinished(timeoutMs) && inspector.exitCode() == 0, "ext2 readback");
    runJob(bus, 4, true, false);
    writeFile(QStringLiteral("/run/check-persistence"), "4 casper-rw\n");
    inspector.start(QStringLiteral("/usr/bin/bash"), {QStringLiteral("/inspect-persistence")});
    require(inspector.waitForFinished(timeoutMs) && inspector.exitCode() == 0, "ext4 readback");
    runJob(bus, 3, false, false);
    writeFile(QStringLiteral("/run/check-persistence"), "3 persistence\n");
    inspector.start(QStringLiteral("/usr/bin/bash"), {QStringLiteral("/inspect-persistence")});
    require(inspector.waitForFinished(timeoutMs) && inspector.exitCode() == 0, "ext3 readback");
    fixture.terminate(); require(fixture.waitForFinished(), "fixture joined");
    daemon.terminate(); require(daemon.waitForFinished(), "bus joined");
    puts("RUFUS_PERSISTENCE_GUEST_OK");
    return 0;
}
