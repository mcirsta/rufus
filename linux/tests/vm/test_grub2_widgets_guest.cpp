// SPDX-License-Identifier: GPL-3.0-or-later
#include "qt/main_window.h"
#include "qt/selection_controller.h"
#include "platform/udisks_client.h"
#include "platform/syslinux_install.h"
#include <QApplication>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusReply>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QMessageBox>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalSpy>
#include <QTest>
#include <QToolButton>
#include <cerrno>
#include <fcntl.h>
#include <linux/mount.h>
#include <sys/stat.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <unistd.h>

static int completionBeeps, completionAlerts;
extern "C" void __wrap__ZN12QApplication4beepEv() { ++completionBeeps; }
extern "C" void __wrap__ZN12QApplication5alertEP7QWidgeti(QWidget*, int) { ++completionAlerts; }

extern "C" uid_t __real_getuid();
extern "C" uid_t __wrap_getuid()
{
    // This PID-1 root driver models the same UID 1000 source caller as its
    // guest-only authorization launcher. No filesystem/device I/O is wrapped.
    const bool sourceCaller = QFile::exists(QStringLiteral("/etc/rufus-winpe-gui-fixture"))
        || QFile::exists(QStringLiteral("/etc/rufus-grub4dos-gui-fixture"))
        || QFile::exists(QStringLiteral("/etc/rufus-grub4dos-image-gui-fixture"));
    return sourceCaller ? 1000 : __real_getuid();
}

namespace
{
constexpr int timeoutMs = 60000;
constexpr quint64 diskBytes = 320ULL * 1024 * 1024;
constexpr quint64 legacyDiskBytes = 64ULL * 1024 * 1024;
constexpr quint32 imageFat16ClusterBytes = 2048, imageFat32ClusterBytes = 512;
constexpr quint32 imageNtfsClusterBytes = 4096;
rufus::IsoWriteController* imageWriter;
QList<int> imagePositions;
QStringList imageStatuses;

void require(bool valid, const char* message)
{
    if (!valid) qFatal("Boot widget guest failed: %s", message);
}

QByteArray readFile(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

void logMessage(const QString& message)
{
    fprintf(stderr, "%s\n", qPrintable(message));
}

void observeImageProgress()
{
    if (imageWriter->state() != rufus::IsoWriteState::Writing)
        return;
    const int position = imageWriter->progressPosition();
    require(imagePositions.isEmpty() || position >= imagePositions.last(), "monotonic native ISO GUI progress");
    imagePositions.append(position);
    const QString status = imageWriter->statusText();
    if (imageStatuses.isEmpty() || imageStatuses.last() != status)
    {
        imageStatuses.append(status);
        fprintf(stderr, "RUFUS_IMAGE_GUI_PROGRESS position=%d status=%s\n", position, qPrintable(status));
    }
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
    const unsigned partitionCount = uefiNtfs ? 2 : 1;
    for (unsigned number = 1; number <= partitionCount; ++number)
    {
        const QByteArray link = "/dev/block/" + QByteArray::number(major(info.st_rdev)) + ':'
            + QByteArray::number(minor(info.st_rdev) + number);
        const QByteArray partition = "../sda" + QByteArray::number(number);
        require(symlink(partition.constData(), link.constData()) == 0, "guest partition link");
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

void runGui(const QDBusConnection& bus, int fileSystem, bool legacy)
{
    const bool grub4dos = QFile::exists(QStringLiteral("/etc/rufus-grub4dos-gui-fixture"));
    const bool grub4dosImage = QFile::exists(QStringLiteral("/etc/rufus-grub4dos-image-gui-fixture"));
    const bool grub4dosDual = grub4dosImage && QFile::exists(QStringLiteral("/etc/rufus-grub4dos-image-uefi-fixture"));
    const bool grub4dosJob = grub4dos || grub4dosImage;
    const bool grub4dosNtfs = grub4dosJob && fileSystem == FS_NTFS;
    const quint32 imageCluster = fileSystem == FS_NTFS ? imageNtfsClusterBytes
        : fileSystem == FS_FAT16 ? imageFat16ClusterBytes : imageFat32ClusterBytes;
    const QByteArray ntfsDriver = grub4dosNtfs ? readFile(QStringLiteral("/etc/rufus-ntfs-mount-fixture")).trimmed() : QByteArray();
    const bool fallback = ntfsDriver == "lowntfs-3g";
    if (fallback)
    {
        const int context = int(syscall(SYS_fsopen, "ntfs", FSOPEN_CLOEXEC));
        require(context < 0 && errno == ENODEV, "kernel NTFS absent before Qt fallback job");
    }
    rufus::SelectionController controller;
    QSignalSpy logs(&controller.isoWriter(), &rufus::IsoWriteController::logMessage);
    QObject::connect(&controller.isoWriter(), &rufus::IsoWriteController::logMessage, logMessage);
    controller.isoWriter().configure(bus, QStringLiteral("/usr/bin/rufus-prepare-helper"),
        grub4dosJob ? QStringLiteral("/grub4dos-files") : QString());
    controller.writer().setConnection(bus);
    controller.writer().setHelper(QStringLiteral("/usr/bin/rufus-prepare-helper"));
    const auto device = publishDevice(bus, legacy || grub4dosJob ? legacyDiskBytes : diskBytes,
        grub4dosDual && grub4dosNtfs);
    controller.setDiscovery({device}, {});
    controller.selectDevice(device.id);
    rufus::MainWindow window(controller);
    window.show();
    QElapsedTimer timer;
    QMetaObject::Connection imageProgressConnection;
    const QByteArray windowsChoices = legacy ? readFile(QStringLiteral("/etc/rufus-windows-bios-fixture")) : QByteArray();
    const bool syslinuxData = windowsChoices.contains("syslinux-data");
    const bool grubData = windowsChoices.contains("grub-data-");
    const bool grub2Sbr = windowsChoices.contains("grub2-sbr");
    const bool grub4dosSbr = windowsChoices.contains("grub4dos-sbr");
    if (legacy)
    {
        const QByteArray mbr = windowsChoices.split(' ').first();
        if (mbr == "win7") controller.toggleWindowsMbr();
        if (syslinuxData || grub2Sbr || grub4dosSbr) controller.toggleEnhancedInstall();
        controller.selectImage(QStringLiteral("/source.iso"));
        timer.start();
        while (controller.isAnalyzing() && timer.elapsed() < timeoutMs) QTest::qWait(10);
        require(!controller.isAnalyzing() && controller.canSelectWindowsBios(), "legacy ISO analyzed");
        require(controller.selection().partition == rufus::PartitionScheme::Mbr, "legacy MBR default");
        const bool advancedKeep = syslinuxData || grub2Sbr || grub4dosSbr;
        const int defaultFileSystem = advancedKeep ? FS_FAT32 : FS_NTFS;
        const quint32 defaultCluster = advancedKeep ? 512 : 4096;
        require(controller.selection().fileSystem == defaultFileSystem && controller.selection().clusterSize == defaultCluster, "shared legacy filesystem default");
        require(window.findChild<QComboBox*>(QStringLiteral("partitionCombo"))->count() == 1, "legacy has no GPT option");
        auto* fs = window.findChild<QComboBox*>(QStringLiteral("fileSystemCombo"));
        require(fs->count() == (advancedKeep ? 2 : 1), "qualified legacy filesystem choices");
        if (fileSystem == FS_FAT32 && !advancedKeep)
        {
            controller.toggleEnhancedInstall();
            require(fs->count() == 2 && controller.selection().fileSystem == FS_NTFS, "Alt+E keeps NTFS until chosen");
        }
        require(fs->findData(fileSystem) >= 0, "legacy filesystem available");
        fs->setCurrentIndex(fs->findData(fileSystem));
        require(QMetaObject::invokeMethod(fs, "activated", Q_ARG(int, fs->currentIndex())), "choose legacy filesystem");
        const quint32 selectedCluster = fileSystem == FS_FAT32 ? 512 : 4096;
        require(controller.selection().fileSystem == fileSystem && controller.selection().clusterSize == selectedCluster, "legacy filesystem and cluster chosen");
        require(window.findChild<QComboBox*>(QStringLiteral("targetCombo"))->currentText() == QStringLiteral("BIOS (or UEFI-CSM)"), "legacy target stays BIOS");
        require(window.findChild<QWidget*>(QStringLiteral("imageOptions"))->isHidden(), "no modern installation options");
        controller.setVolumeLabel(QStringLiteral("RUFUS_TEST"));
    }
    else if (grub4dosImage)
    {
        controller.selectImage(QStringLiteral("/source.iso"));
        timer.start();
        while (controller.isAnalyzing() && timer.elapsed() < timeoutMs) QTest::qWait(10);
        require(!controller.isAnalyzing() && controller.canSelectGrub(), "ordinary GRUB4DOS ISO analyzed");
        const auto& report = controller.selection().imageAnalysis.report;
        require(report.has_grub4dos && !report.has_grub2 && !HAS_SYSLINUX(report), "image provider facts preserved");
        require(report.grub2_version[0] == '\0', "unknown image version stays unknown");
        auto* partition = window.findChild<QComboBox*>(QStringLiteral("partitionCombo"));
        if (grub4dosDual)
        {
            require(partition->count() == 2 && partition->currentText() == QStringLiteral("GPT"), "shared EFI default and MBR choice");
            partition->setCurrentIndex(partition->findData(int(rufus::PartitionScheme::Mbr)));
            require(QMetaObject::invokeMethod(partition, "activated", Q_ARG(int, partition->currentIndex())), "choose combined MBR");
        }
        else
            require(partition->count() == 1 && partition->currentText() == QStringLiteral("MBR"), "image MBR only");
        const QString target = grub4dosDual ? QStringLiteral("BIOS or UEFI") : QStringLiteral("BIOS (or UEFI-CSM)");
        require(window.findChild<QComboBox*>(QStringLiteral("targetCombo"))->currentText() == target, "image target reflects both loaders");
        auto* fs = window.findChild<QComboBox*>(QStringLiteral("fileSystemCombo"));
        if (grub4dosDual)
        {
            require(fs->count() == 3 && fs->currentData() == FS_FAT16 && fs->currentText() == QStringLiteral("FAT16 (Default)"), "combined FAT/NTFS choices use the shared small-device default");
            require(fs->itemText(fs->findData(FS_FAT16)) == QStringLiteral("FAT16 (Default)"), "shared FAT16 default is offered");
            require(controller.selection().defaultFileSystem == FS_FAT16, "Win32 marker precedes image filtering");
            require(controller.selection().clusterSize == imageFat16ClusterBytes, "native combined FAT16 cluster default");
        }
        else
        {
            require(fs->count() == 3 && fs->currentData() == FS_FAT16 && fs->currentText() == QStringLiteral("FAT16 (Default)"), "qualified image FAT16 default and FAT/NTFS choices");
            require(controller.selection().clusterSize == imageFat16ClusterBytes, "native image FAT16 cluster default");
        }
        require(fs->findData(fileSystem) >= 0, "requested image filesystem available");
        fs->setCurrentIndex(fs->findData(fileSystem));
        require(QMetaObject::invokeMethod(fs, "activated", Q_ARG(int, fs->currentIndex())), "choose image filesystem");
        require(controller.selection().fileSystem == fileSystem && controller.selection().clusterSize == imageCluster, "selected image filesystem geometry");
        require(controller.selection().volumeLabel == QStringLiteral("SOURCE"), "shared ISO label default");
        controller.setVolumeLabel(QStringLiteral("RUFUS_TEST"));
        imageWriter = &controller.isoWriter();
        imageProgressConnection = QObject::connect(imageWriter, &rufus::IsoWriteController::changed, observeImageProgress);
    }
    else if (grub4dos)
    {
        QTest::mouseClick(window.findChild<QToolButton*>(QStringLiteral("advancedDriveButton")), Qt::LeftButton);
        auto* boot = window.findChild<QComboBox*>(QStringLiteral("bootCombo"));
        boot->setCurrentIndex(boot->findText(QStringLiteral("Grub4DOS 0.4.6a")));
        require(QMetaObject::invokeMethod(boot, "activated", Q_ARG(int, boot->currentIndex())), "choose GRUB4DOS");
        auto* fs = window.findChild<QComboBox*>(QStringLiteral("fileSystemCombo"));
        require(fs->count() == 3 && fs->currentData() == FS_FAT16, "qualified FAT16 default and FAT16/FAT32/NTFS choices");
        require(fs->findData(fileSystem) >= 0, "requested GRUB4DOS filesystem available");
        fs->setCurrentIndex(fs->findData(fileSystem));
        require(QMetaObject::invokeMethod(fs, "activated", Q_ARG(int, fs->currentIndex())), "choose GRUB4DOS filesystem");
        require(controller.selection().fileSystem == fileSystem, "selected GRUB4DOS filesystem");
        require(window.findChild<QComboBox*>(QStringLiteral("partitionCombo"))->currentText() == QStringLiteral("MBR"), "standalone MBR");
        require(window.findChild<QComboBox*>(QStringLiteral("targetCombo"))->currentText() == QStringLiteral("BIOS (or UEFI-CSM)"), "standalone BIOS");
        controller.setVolumeLabel(grub4dosNtfs ? QString::fromUtf8("Rufus ș😀.test") : QStringLiteral("RUFUS_TEST"));
    }
    else
    {
        QTest::mouseClick(window.findChild<QToolButton*>(QStringLiteral("advancedDriveButton")), Qt::LeftButton);
        auto* boot = window.findChild<QComboBox*>(QStringLiteral("bootCombo"));
        boot->setCurrentIndex(boot->findText(QStringLiteral("Grub 2.14")));
        require(QMetaObject::invokeMethod(boot, "activated", Q_ARG(int, boot->currentIndex())), "choose GRUB2");
        QTest::mouseClick(window.findChild<QToolButton*>(QStringLiteral("advancedFormatButton")), Qt::LeftButton);
        auto* fs = window.findChild<QComboBox*>(QStringLiteral("fileSystemCombo"));
        require(fs->findData(fileSystem) >= 0, "advanced ext choice");
        fs->setCurrentIndex(fs->findData(fileSystem));
        require(QMetaObject::invokeMethod(fs, "activated", Q_ARG(int, fs->currentIndex())), "choose ext");
        controller.setVolumeLabel(QStringLiteral("RufusExt"));
        require(window.findChild<QComboBox*>(QStringLiteral("clusterCombo"))->currentText() == QStringLiteral("Default"), "default cluster");
    }
    auto* start = window.findChild<QPushButton*>(QStringLiteral("startButton"));
    require(start && start->isEnabled(), "START enabled after ISO load");
    const int descriptors = QDir(QStringLiteral("/proc/self/fd")).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size();
    QTest::mouseClick(start, Qt::LeftButton);
    timer.start();
    while (controller.isoWriter().state() == rufus::IsoWriteState::Checking && timer.elapsed() < timeoutMs)
        QTest::qWait(10);
    const bool hasGrub2 = windowsChoices.contains("grub-data-both") || windowsChoices.contains("grub-data-grub2");
    if (hasGrub2)
    {
        require(controller.isoWriter().state() == rufus::IsoWriteState::SelectingGrubCore, "original GRUB version lookup before confirmation");
        auto* download = window.findChild<QMessageBox*>(QStringLiteral("grubDownloadDialog"));
        require(download && download->isVisible(), "GRUB version difference prompt");
        require(readFile(QStringLiteral("/run/format-launches")).isEmpty(), "GRUB lookup before authorization");
        QTest::mouseClick(download->button(QMessageBox::No), Qt::LeftButton);
        timer.restart();
        while (controller.isoWriter().state() == rufus::IsoWriteState::SelectingGrubCore && timer.elapsed() < timeoutMs)
            QTest::qWait(10);
    }
    if (controller.isoWriter().state() != rufus::IsoWriteState::AwaitingConfirmation)
        qFatal("GUI preflight failed: %s", qPrintable(controller.isoWriter().statusText()));
    auto* confirmation = window.findChild<QMessageBox*>(QStringLiteral("isoWriteConfirmation"));
    const QString grub4dosFileSystem = grub4dosNtfs ? QStringLiteral("NTFS")
        : fileSystem == FS_FAT16 ? QStringLiteral("FAT16") : QStringLiteral("FAT32");
    const QString imageSystem = grub4dosDual ? QStringLiteral("BIOS or UEFI") : QStringLiteral("BIOS");
    const QString detail = grub4dosImage ? QStringLiteral("MBR, %1, %2").arg(grub4dosFileSystem, imageSystem) : grub4dos ? QStringLiteral("Grub4DOS 0.4.6a — MBR, %1").arg(grub4dosFileSystem) : legacy ? (fileSystem == FS_FAT32 ? QStringLiteral("MBR, FAT32, BIOS")
        : QStringLiteral("MBR, NTFS, BIOS")) : QStringLiteral("Cluster size: Default");
    require(confirmation && confirmation->informativeText().contains(detail), "GUI confirmation");
    require(!legacy || controller.isoWriter().check().legacyWinPe, "fresh legacy preflight accepted");
    if (grub2Sbr)
    {
        const auto& request = controller.isoWriter().request();
        const auto& report = controller.isoWriter().check().analysis.report;
        const auto& core = controller.isoWriter().grubCoreResult();
        require(request.windowsBios && request.enhancedInstall && request.fileSystem == fileSystem, "Windows Alt+E filesystem request");
        require(!request.grub2 && !request.syslinux && !HAS_SYSLINUX(report), "independent Windows MBR and GRUB2 SBR");
        require(GetSecondaryBootType(BT_IMAGE, &report, false) == BT_GRUB2, "fresh GRUB2 SBR dispatch");
        require(core.origin == rufus::Grub2CoreOrigin::Embedded && core.bytes.size() == 42742, "No freezes original embedded core");
        require(core.sha256.toHex() == QByteArray("9a2c946704017fa8dc4e03a8a58d754d2d1607c2d2cd74f0e2920133f1192809"), "frozen core identity");
        require(!request.hasGrub2Core(), "process stages the frozen payload after confirmation");
    }
    if (grub4dosSbr)
    {
        const auto& request = controller.isoWriter().request();
        const auto& report = controller.isoWriter().check().analysis.report;
        require(request.windowsBios && request.enhancedInstall && request.fileSystem == fileSystem, "Windows Alt+E GRUB4DOS request");
        require(!request.grub2 && !request.syslinux && !HAS_SYSLINUX(report), "independent Windows MBR and GRUB4DOS SBR");
        require(report.has_grub4dos && !report.has_grub2 && report.grub2_version[0] == '\0', "source provider facts preserved");
        require(GetSecondaryBootType(BT_IMAGE, &report, false) == BT_GRUB4DOS, "fresh GRUB4DOS SBR dispatch");
        require(!request.hasGrub2Core() && controller.isoWriter().grubCoreResult().bytes.isEmpty(), "no GRUB2 payload selected");
        require(!window.findChild<QMessageBox*>(QStringLiteral("grubDownloadDialog")), "no GRUB2 download prompt");
        require(readFile(QStringLiteral("/run/format-launches")).isEmpty(), "confirmation before authorization");
    }
    if (syslinuxData)
    {
        const auto& request = controller.isoWriter().request();
        const auto& report = controller.isoWriter().check().analysis.report;
        require(request.windowsBios && request.enhancedInstall && !request.usesUefiNtfs(), "mixed Windows Alt+E request");
        require(!request.syslinux && !request.grub2 && HAS_SYSLINUX(report), "mixed image retains separate loader facts");
        require(report.sl_detected_version == 0 && report.sl_version == 0 && !report.sl_version_inferred, "fresh source version remains unknown");
        if (grubData)
        {
            const bool both = windowsChoices.contains("grub-data-both");
            const bool grub2 = both || windowsChoices.contains("grub-data-grub2");
            const bool grub4dos = both || windowsChoices.contains("grub-data-grub4dos");
            require(bool(report.has_grub2) == grub2 && bool(report.has_grub4dos) == grub4dos, "fresh GRUB provider flags retained");
            require(GetSecondaryBootType(BT_IMAGE, &report, false) == BT_IMAGE, "original Syslinux precedence suppresses GRUB SBR");
            require(!request.hasGrub2Core(), "mixed Windows route selects no GRUB core");
        }
        for (int boot : {BT_SYSLINUX_V4, BT_SYSLINUX_V6})
        {
            const char* installer = rufus::NativeSyslinuxInstaller(boot, FS_FAT32);
            require(installer != nullptr && !QFile::exists(QString::fromLocal8Bit(installer)), "mixed Widgets guest has no Syslinux installer");
        }
        const char* installer = rufus::NativeSyslinuxInstaller(BT_SYSLINUX_V6, FS_NTFS);
        require(installer != nullptr && !QFile::exists(QString::fromLocal8Bit(installer)), "mixed Widgets guest has no Extlinux installer");
    }
    const QString childrenPath = QStringLiteral("/proc/self/task/%1/children").arg(getpid());
    const QByteArray children = readFile(childrenPath).simplified();
    const QByteArray sourceHash = grub4dosImage ? QCryptographicHash::hash(readFile(QStringLiteral("/source.iso")), QCryptographicHash::Sha256) : QByteArray();
    if (grub4dosImage)
    {
        const auto& request = controller.isoWriter().request();
        require(request.grub4dos && !request.grub2 && request.mbr && !request.isSourceFree(), "ISO-owned GRUB4DOS request");
        require(request.imagePath == QStringLiteral("/source.iso") && request.fileSystem == fileSystem
            && request.clusterSize == imageCluster, "frozen image geometry");
        require((request.volumeId == 0) == grub4dosNtfs, "native image formatter owns NTFS serial");
        const bool pairedNtfs = grub4dosDual && grub4dosNtfs;
        require(request.usesUefiNtfs() == pairedNtfs && !request.enhancedInstall && !request.hasGrub2Core(), "image has only its required payload and no standalone metadata");
        const auto& checked = controller.isoWriter().check();
        require(bool(IS_EFI_BOOTABLE(checked.analysis.report)) == grub4dosDual, "frozen BIOS/EFI source facts");
        require(checked.efiPath == (grub4dosDual ? QStringLiteral("/EFI/BOOT/BOOTX64.EFI") : QString()), "frozen source fallback path");
        if (grub4dosDual)
        {
            require(checked.extraPartitions == (pairedNtfs ? XP_UEFI_NTFS : 0)
                && (checked.partition.uefi_ntfs_size > 0) == pairedNtfs,
                "combined source freezes the required trailing payload geometry");
            puts("RUFUS_GRUB4DOS_IMAGE_DUAL_REQUEST_DONE");
        }
        require(controller.isoWriter().grubCoreResult().bytes.isEmpty(), "no selected standalone loader");
        require(!QFile::exists(QStringLiteral("/grub4dos-files")) && !QFile::exists(QStringLiteral("/selected-loader")), "no loader cache or payload in guest");
        require(readFile(QStringLiteral("/run/format-launches")).isEmpty(), "no image authorization before confirmation");
    }
    if (grub4dos)
    {
        const auto& request = controller.isoWriter().request();
        const auto& loader = controller.isoWriter().grubCoreResult();
        require(request.grub4dos && request.mbr && request.imagePath.isEmpty(), "source-free GRUB4DOS request");
        const quint32 clusterBytes = grub4dosNtfs ? 4096 : fileSystem == FS_FAT16 ? 2048 : 512;
        require(request.fileSystem == fileSystem && request.clusterSize == clusterBytes, "frozen filesystem geometry");
        require(!request.usesUefiNtfs() && !request.uefiNtfs, "standalone BIOS job excludes UEFI:NTFS");
        require((request.volumeId == 0) == grub4dosNtfs, "native NTFS formatter owns serial");
        const QString label = grub4dosNtfs ? QString::fromUtf8("Rufus ș😀_test") : QStringLiteral("RUFUS_TEST");
        require(request.label == label, "frozen shared label normalization");
        require(loader.origin == rufus::Grub2CoreOrigin::Cache && loader.bytes.size() == 322965, "original cached loader retained");
        require(loader.sha256.toHex() == QByteArray("19ba57329be745ceae46354c5d411b7d8efec6802cfd7f6fee986df4b763f6fe"), "original loader hash");
        require(request.grub2CoreSha256 == loader.sha256 && request.hasGrub2Core(), "strict frozen metadata");
        require(readFile(QStringLiteral("/run/format-launches")).isEmpty(), "no authorization before confirmation");
        // The cache is caller-owned. Replacing it cannot replace retained bytes.
        QFile cache(request.grub2CorePath);
        require(cache.open(QIODevice::WriteOnly | QIODevice::Truncate), "replace selected cache");
        require(cache.write(QByteArray(loader.bytes.size(), 'x')) == loader.bytes.size(), "cache replacement complete");
        cache.close();
    }
    QTest::mouseClick(confirmation->button(QMessageBox::Ok), Qt::LeftButton);
    timer.restart();
    bool fuseHeld = false;
    auto* progress = window.findChild<QProgressBar*>(QStringLiteral("progressBar"));
    while (readFile(QStringLiteral("/run/format-exit-held")).isEmpty() && timer.elapsed() < timeoutMs)
    {
        QTest::qWait(10);
        if (fallback && !fuseHeld && QFile::exists(QStringLiteral("/run/grub4dos-fuse-exit-held")))
        {
            require(controller.isoWriter().isActive(), "Qt waits for owned FUSE exit");
            require(progress && progress->value() < MAX_PROGRESS && progress->format() != QStringLiteral("READY"), "no READY before FUSE exit");
            require(completionBeeps == 0 && completionAlerts == 0, "no notification before FUSE exit");
            require(readFile(QStringLiteral("/run/grub4dos-fuse-exit-held")).trimmed() == "0", "real FUSE unmount/exit succeeded");
            const QList<QByteArray> pids = readFile(QStringLiteral("/run/grub4dos-fuse-processes")).simplified().split(' ');
            require(pids.size() == 3, "FUSE launcher/helper/native PIDs recorded");
            for (const QByteArray& pid : pids)
            {
                bool valid = false;
                require(pid.toLongLong(&valid) > 1 && valid, "valid owned FUSE process PID");
            }
            require(QFile::exists(QStringLiteral("/proc/%1/status").arg(QString::fromLatin1(pids.at(0)))), "owned FUSE launcher retained");
            require(!QFile::exists(QStringLiteral("/proc/%1/status").arg(QString::fromLatin1(pids.at(2)))), "native FUSE child reaped");
            const QByteArray mounts = readFile(QStringLiteral("/proc/%1/mountinfo").arg(QString::fromLatin1(pids.at(1))));
            require(!mounts.isEmpty() && !mounts.contains("/run/rufus-"), "private NTFS unmounted before FUSE join");
            // The mounted filesystem owns the device claim. After unmount,
            // production joins FUSE before reacquiring/rechecking the whole disk.
            const int snapshots = QDir(QStringLiteral("/tmp")).entryList({QStringLiteral("rufus-grub2-*")}, QDir::Dirs).size();
            require(snapshots == int(grub4dos), "only standalone jobs retain a loader snapshot through FUSE exit");
            QFile fuseRelease(QStringLiteral("/run/grub4dos-fuse-exit-release"));
            require(fuseRelease.open(QIODevice::WriteOnly) && fuseRelease.write("release\n") == 8, "release owned FUSE exit");
            fuseRelease.close();
            fuseHeld = true;
        }
    }
    require(!readFile(QStringLiteral("/run/format-exit-held")).isEmpty(), "helper completed and held before exit");
    const QByteArray helperOutput = readFile(QStringLiteral("/run/format-helper-output"));
    if (!helperOutput.isEmpty())
        fprintf(stderr, "Native helper output:\n%s\n", helperOutput.constData());
    // The marker is a filesystem event; allow queued helper output to arrive.
    while (progress && progress->format() != QStringLiteral("Finalizing, please wait...") && timer.elapsed() < timeoutMs)
        QTest::qWait(10);
    require(controller.isoWriter().state() == rufus::IsoWriteState::Writing, "GUI waits for helper exit");
    require(progress && progress->format() == QStringLiteral("Finalizing, please wait..."), "finalizing GUI");
    require(progress->value() < MAX_PROGRESS, "no premature full READY");
    if (grub4dosImage)
    {
        require(progress->value() == (grub4dosNtfs ? 64224 : 64879), "original ISO finalizing progress");
        require(imageStatuses.contains(QStringLiteral("Writing master boot record...")), "original MBR/SBR caption");
        require(imageStatuses.contains(QStringLiteral("Copying ISO files: 100.0%")), "full native ISO copy percentage");
        require(imagePositions.size() > 2, "native GUI progress observed");
        require(completionBeeps == 0 && completionAlerts == 0, "no image notification before launcher exit");
        require(QDir(QStringLiteral("/tmp")).entryList({QStringLiteral("rufus-grub2-*")}, QDir::Dirs).isEmpty(), "ISO job has no loader staging");
        require(QDir(QStringLiteral("/run")).entryList({QStringLiteral("rufus-*")}, QDir::Dirs).isEmpty(), "image unmounted before launcher exit");
    }
    if (grub4dos)
    {
        const int windowsFinalizingPosition = grub4dosNtfs ? 62414 : 60853;
        require(progress->value() == windowsFinalizingPosition, "original discrete filesystem progress");
        require(completionBeeps == 0 && completionAlerts == 0, "no notification before launcher exit");
        const QStringList staged = QDir(QStringLiteral("/tmp")).entryList({QStringLiteral("rufus-grub2-*")}, QDir::Dirs);
        require(staged.size() == 1, "loader held through launcher exit");
        const QString path = QStringLiteral("/tmp/") + staged.first() + QStringLiteral("/grldr");
        const QByteArray bytes = readFile(path);
        const QByteArray hash = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
        require(hash == controller.isoWriter().grubCoreResult().sha256, "private staged original bytes");
        struct stat info;
        const QByteArray encoded = QFile::encodeName(path);
        require(stat(encoded.constData(), &info) == 0, "staged loader stat");
        const bool ownedLoader = info.st_uid == 1000 && info.st_nlink == 1 && (info.st_mode & 0777) == 0400;
        require(ownedLoader, "caller-owned read-only loader");
        const QByteArray directory = QFile::encodeName(QStringLiteral("/tmp/") + staged.first());
        require(stat(directory.constData(), &info) == 0, "staging directory stat");
        const bool privateDirectory = info.st_uid == 1000 && (info.st_mode & 0777) == 0700;
        require(privateDirectory, "caller-private staging directory");
        require(QDir(QStringLiteral("/run")).entryList({QStringLiteral("rufus-*")}, QDir::Dirs).isEmpty(), "mount cleanup before launcher exit");
    }
    QFile release(QStringLiteral("/run/format-exit-release"));
    require(release.open(QIODevice::WriteOnly) && release.write("release\n") == 8, "release helper exit");
    release.close();
    timer.restart();
    while (controller.isoWriter().isActive() && timer.elapsed() < timeoutMs) QTest::qWait(10);
    if (controller.isoWriter().state() != rufus::IsoWriteState::Complete)
        qFatal("GUI job failed: %s", qPrintable(controller.isoWriter().statusText()));
    require(progress->value() == MAX_PROGRESS && progress->format() == QStringLiteral("READY"), "full green READY");
    require(readFile(QStringLiteral("/run/format-launches")) == "launch\n", "single authorization");
    const int fd = open("/dev/sda", O_RDWR | O_EXCL | O_CLOEXEC);
    require(fd >= 0 && close(fd) == 0, "claim released before READY");
    const auto& outcome = controller.isoWriter().outcome();
    require(outcome.filesystemBytes == outcome.size, "complete filesystem volume");
    if (legacy)
    {
        const auto& check = controller.isoWriter().check();
        require(outcome.error == 0 && outcome.syncError == 0 && outcome.cleanupError == 0, "legacy job clean completion");
        const quint64 copied = outcome.copiedBytes;
        require(copied == check.tree.bytes_written && copied > 0, "legacy copied byte denominator");
        if (grub2Sbr) puts("RUFUS_WINPE_GRUB2_SBR_DONE");
        if (grub4dosSbr)
        {
            bool installed = false, otherInstaller = false;
            for (const QList<QVariant>& entry : logs)
            {
                const QString message = entry.first().toString();
                installed |= message == QStringLiteral("Installing GRUB4DOS boot records.");
                otherInstaller |= message.startsWith(QStringLiteral("Installing GRUB2"))
                    || message.startsWith(QStringLiteral("Installing Syslinux"));
            }
            require(installed && !otherInstaller, "only bundled GRUB4DOS SBR installed");
            puts("RUFUS_WINPE_GRUB4DOS_SBR_DONE");
        }
        if (syslinuxData)
        {
            bool unknown = false, installed = false;
            for (const QList<QVariant>& entry : logs)
            {
                const QString message = entry.first().toString();
                unknown |= message.contains(QStringLiteral("Could not detect Isolinux version - No fallback version available"));
                installed |= message.startsWith(QStringLiteral("Installing Syslinux"));
                if (grubData) installed |= message.startsWith(QStringLiteral("Installing GRUB2"));
            }
            require(unknown && !installed, "mixed Widgets job copies configs without selecting a Syslinux version");
            puts("RUFUS_WINPE_SYSLINUX_DATA_DONE");
            if (grubData) puts("RUFUS_WINPE_GRUB_DATA_DONE");
        }
        const bool fat32 = fileSystem == FS_FAT32;
        printf("RUFUS_WINDOWS_IMAGE_DONE ERROR=0 OFFSET=%llu SIZE=%llu BYTES=%llu COPIED=%llu FAT=%d CLUSTER=%u\n",
            outcome.offset, outcome.size, outcome.filesystemBytes, copied, fat32 ? 32 : 0, fat32 ? 512u : 4096u);
    }
    else if (grub4dosJob)
    {
        require(outcome.error == 0 && outcome.syncError == 0 && outcome.cleanupError == 0, "clean GRUB4DOS completion");
        const quint64 copiedBytes = grub4dosImage ? controller.isoWriter().check().tree.bytes_written : 322965;
        require(outcome.copiedBytes == copiedBytes && copiedBytes > 0, "complete source bytes copied");
        require(readFile(childrenPath).simplified() == children, "helper and formatter descendants reaped");
        require(QDir(QStringLiteral("/tmp")).entryList({QStringLiteral("rufus-grub2-*")}, QDir::Dirs).isEmpty(), "private loader staging removed");
        require(QDir(QStringLiteral("/run")).entryList({QStringLiteral("rufus-*")}, QDir::Dirs).isEmpty(), "private mount removed");
        require(!readFile(QStringLiteral("/proc/self/mountinfo")).contains("/dev/sda"), "no target mount remains");
        require(completionBeeps == 1 && completionAlerts == 1, "one completion notification");
        if (grub4dosImage)
        {
            require(QDir(QStringLiteral("/proc/self/fd")).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size() == descriptors, "image descriptor count restored");
            require(QCryptographicHash::hash(readFile(QStringLiteral("/source.iso")), QCryptographicHash::Sha256) == sourceHash, "ISO source unchanged");
            require(!QFile::exists(QStringLiteral("/grub4dos-files")), "no loader cache created");
            require(!controller.isoWriter().request().hasGrub2Core(), "no staged loader metadata after completion");
            puts("RUFUS_IMAGE_GRUB4DOS_SBR_DONE");
        }
        if (fallback)
        {
            require(fuseHeld, "Qt observed owned FUSE exit before READY");
            const QList<QByteArray> pids = readFile(QStringLiteral("/run/grub4dos-fuse-processes")).simplified().split(' ');
            require(pids.size() == 3, "completed FUSE process audit");
            for (const QByteArray& pid : pids)
                require(!QFile::exists(QStringLiteral("/proc/%1/status").arg(QString::fromLatin1(pid))), "FUSE/helper descendants gone before READY");
            puts("RUFUS_GRUB4DOS_FUSE_JOINED");
        }
        printf("%s ERROR=0 OFFSET=%llu SIZE=%llu BYTES=%llu COPIED=%llu VOLUMEID=%u\n",
            grub4dosImage ? "RUFUS_GRUB4DOS_IMAGE_DONE" : "RUFUS_GRUB4DOS_DONE",
            outcome.offset, outcome.size, outcome.filesystemBytes, outcome.copiedBytes, controller.isoWriter().request().volumeId);
    }
    else
    {
        require(outcome.copiedBytes == 0, "source-free volume");
        printf("RUFUS_FORMAT_VOLUME OFFSET=%llu SIZE=%llu BYTES=%llu\n", outcome.offset, outcome.size, outcome.filesystemBytes);
    }
    controller.setDiscovery({}, {});
    require(progress->format() == QStringLiteral("READY"), "unplug preserves completed READY");
    if (grub4dosJob) require(completionBeeps == 1 && completionAlerts == 1, "unplug does not repeat notification");
    puts(grub4dosImage ? "RUFUS_GRUB4DOS_IMAGE_GUI_DONE" : grub4dos ? "RUFUS_GRUB4DOS_GUI_DONE" : legacy ? "RUFUS_WINPE_GUI_DONE" : "RUFUS_GRUB2_EXT_GUI_DONE");
    QObject::disconnect(imageProgressConnection);
    imageWriter = nullptr;
}
}

int main(int argc, char** argv)
{
    const bool rootFixture = __real_getuid() == 0 && geteuid() == 0 && getppid() == 1;
    if (!rootFixture || readFile(QStringLiteral("/sys/class/dmi/id/product_name")) != "RufusMBRFixture\n") return 77;
    const QByteArray type = readFile(QStringLiteral("/etc/rufus-grub2-ext-helper-fixture")).trimmed();
    const bool legacy = QFile::exists(QStringLiteral("/etc/rufus-winpe-gui-fixture"));
    const bool grub4dos = QFile::exists(QStringLiteral("/etc/rufus-grub4dos-gui-fixture"));
    const bool grub4dosImage = QFile::exists(QStringLiteral("/etc/rufus-grub4dos-image-gui-fixture"));
    if (!legacy && !grub4dos && !grub4dosImage && type != "2" && type != "3") return 77;
    require(prctl(PR_SET_CHILD_SUBREAPER, 1) == 0, "helper descendant audit");
    if (legacy || grub4dosImage) require(chown("/source.iso", 1000, 1000) == 0, "image caller ownership before analysis");
    QApplication application(argc, argv);
    QProcess daemon, fixture;
    daemon.setProcessChannelMode(QProcess::ForwardedErrorChannel);
    daemon.start(QStringLiteral("/usr/bin/dbus-daemon"), {QStringLiteral("--nofork"),
        QStringLiteral("--print-address=1"), QStringLiteral("--config-file=/etc/rufus-test-bus.conf")});
    require(daemon.waitForStarted() && daemon.waitForReadyRead(), "private bus");
    const auto address = daemon.readLine().trimmed();
    qputenv("DBUS_SYSTEM_BUS_ADDRESS", address);
    fixture.start(QStringLiteral("/usr/bin/udisks_fixture"), {QString::fromUtf8(address)});
    require(fixture.waitForStarted() && fixture.waitForReadyRead() && fixture.readLine() == "ready\n", "fixture service");
    const bool legacyFat32 = legacy && readFile(QStringLiteral("/etc/rufus-windows-bios-fixture")).contains("fat32");
    const int legacyFileSystem = legacyFat32 ? FS_FAT32 : FS_NTFS;
    const bool grub4dosNtfs = QFile::exists(QStringLiteral("/etc/rufus-grub4dos-ntfs-fixture"))
        || (grub4dosImage && QFile::exists(QStringLiteral("/etc/rufus-ntfs-mount-fixture")));
    const int grub4dosFileSystem = grub4dosNtfs ? FS_NTFS
        : QFile::exists(QStringLiteral("/etc/rufus-fat16-fixture")) ? FS_FAT16 : FS_FAT32;
    const int fileSystem = grub4dos || grub4dosImage ? grub4dosFileSystem : legacy ? legacyFileSystem : (type == "2" ? FS_EXT2 : FS_EXT3);
    runGui(QDBusConnection::systemBus(), fileSystem, legacy);
    fixture.terminate();
    require(fixture.waitForFinished(), "fixture reaped");
    daemon.terminate();
    require(daemon.waitForFinished(), "bus reaped");
    return 0;
}
