// SPDX-License-Identifier: GPL-3.0-or-later
#include "qt/selection_controller.h"
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

namespace
{
std::atomic<bool> caller = false;
void require(bool okay, const char* message) { if (!okay) qFatal("Native format Qt: %s", message); }
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
        && (read("/etc/rufus-native-formats-fixture") == "native-formats\n"
            || read("/etc/rufus-runtime-validation-fixture") == "runtime-validation\n"
            || read("/etc/rufus-device-options-fixture") == "device-options\n"
            || read("/etc/rufus-ntfs-options-fixture") == "ntfs-options\n"
            || read("/etc/rufus-iso-resources-fixture") == "iso-resources\n"
            || read("/etc/rufus-legacy-iso-fixture") == "legacy-iso\n"
            || read("/etc/rufus-geometry-fixture") == "geometry-batch\n"
            || read("/etc/rufus-iso-options-fixture") == "iso-options\n"
            || read("/etc/rufus-legacy-boot-fixture") == "legacy-boot\n")
        && QFile::symLinkTarget(parent).endsWith(QStringLiteral("/test_mbr_handoff_guest"));
    if (!guarded) return 77;
    QCoreApplication application(argc, argv);
    QFile input; require(input.open(stdin, QIODevice::ReadOnly), "identity pipe");
    const bool deviceOptions = read("/etc/rufus-device-options-fixture") == "device-options\n";
    const bool legacy = read("/etc/rufus-legacy-boot-fixture") == "legacy-boot\n";
    const bool ntfsOptions = read("/etc/rufus-ntfs-options-fixture") == "ntfs-options\n";
    const bool udfIndexing = read("/etc/rufus-udf-indexing-fixture") == "udf-indexing\n";
    const bool isoResources = read("/etc/rufus-iso-resources-fixture") == "iso-resources\n";
    const bool legacyIso = read("/etc/rufus-legacy-iso-fixture") == "legacy-iso\n";
    const bool geometry = read("/etc/rufus-geometry-fixture") == "geometry-batch\n";
    const bool isoOptions = isoResources || legacyIso || read("/etc/rufus-iso-options-fixture") == "iso-options\n";
    const bool runtime = read("/etc/rufus-runtime-validation-fixture") == "runtime-validation\n";
    rufus::PrepareImageRequest expected;
    require(rufus::DecodePrepareRequest(QJsonDocument::fromJson(input.readLine(rufus::PrepareMessageBytes)).object(), expected)
        && (geometry ? expected.nonBootable && expected.fileSystem >= FS_FAT16 && expected.fileSystem <= FS_EXT3
            : deviceOptions ? expected.nonBootable && expected.fileSystem == FS_FAT32 : ntfsOptions ? expected.fileSystem == FS_NTFS : isoOptions ? expected.msDos || !expected.imagePath.isEmpty() : legacy ? (expected.msDos || expected.reactOs) && expected.mbr : runtime ? expected.runtimeValidation && !expected.imagePath.isEmpty()
            : expected.nonBootable && (expected.fileSystem == FS_UDF || expected.fileSystem == FS_EXT2 || expected.fileSystem == FS_EXT3)), "native typed job");
    rufus::UDisksObjects objects; rufus::Device device;
    require(rufus::ReadPrepareDeviceSnapshot(QDBusConnection::systemBus(), expected, objects, device) == 0, "fresh device snapshot");
    device.serviceOwner = expected.serviceOwner; device.id = device.objectPath;
    caller = true;
    if (udfIndexing) {
        QString problem;
        require(rufus::ConfigureApplicationStorage(QStringLiteral("/run/caller-home/rufus-linux"), true, problem) == 0,
            "private UDF preference storage");
    }
    rufus::SelectionController controller;
    QSignalSpy logs(&controller, &rufus::SelectionController::logMessage);
    controller.writer().setConnection(QDBusConnection::systemBus());
    controller.writer().setHelper(QStringLiteral("/usr/bin/rufus-prepare-helper"));
    controller.isoWriter().configure(QDBusConnection::systemBus(), QStringLiteral("/usr/bin/rufus-prepare-helper"),
        legacy || isoOptions ? QStringLiteral("/run/caller-home/files") : QString());
    if (deviceOptions) {
        if (device.deviceClass == rufus::DeviceClass::Vmware) controller.toggleVmwareDisks();
        if (device.deviceClass == rufus::DeviceClass::NonUsbRemovable) controller.toggleNonUsbRemovable();
        controller.setExtendedLabel(false);
    }
    controller.setDiscovery({device}, {});
    if (geometry) controller.setExtendedLabel(false);
    if (legacyIso && controller.enhancedInstall() != expected.enhancedInstall) controller.toggleEnhancedInstall();
    if (ntfsOptions || udfIndexing) {
        if (ntfsOptions && controller.ntfsCompression() != expected.ntfsCompression) controller.toggleNtfsCompression();
        if (udfIndexing && controller.fileIndexing() == expected.fileIndexing) controller.toggleFileIndexing();
        if (controller.fileIndexing() != expected.fileIndexing) controller.toggleFileIndexing();
    }
    if (isoOptions) {
        if (!expected.joliet) controller.toggleJoliet();
        if (!expected.rockRidge) controller.toggleRockRidge();
        if (controller.preserveTimestamps() != expected.preserveTimestamps) controller.togglePreserveTimestamps();
        if (!expected.sizeChecks) controller.toggleSizeChecks();
    }
    if (runtime || (isoOptions && !expected.msDos) || (ntfsOptions && !expected.nonBootable)) {
        controller.selectImage(expected.imagePath);
        QElapsedTimer analysis; analysis.start();
        while (controller.isAnalyzing()) {
            QCoreApplication::processEvents(); require(analysis.elapsed() < 60000, "joined source analysis"); usleep(1000);
        }
    } else if (legacy || (isoOptions && expected.msDos)) {
        controller.setBootType(expected.msDos ? BT_MSDOS : BT_REACTOS); controller.setExtendedLabel(false);
    } else {
        controller.setBootType(BT_NON_BOOTABLE); controller.setAdvancedFormat(true);
    }
    controller.setPartitionScheme(isoResources || legacyIso ? rufus::PartitionScheme::Mbr : expected.sfd ? rufus::PartitionScheme::Sfd : expected.mbr ? rufus::PartitionScheme::Mbr : rufus::PartitionScheme::Gpt);
    controller.setFileSystem(expected.fileSystem); controller.setQuickFormat(expected.quickFormat);
    if (udfIndexing) {
        controller.setFileSystem(FS_NTFS);
        require(controller.fileIndexing() == expected.fileIndexing, "indexing choice survives NTFS switch");
        controller.setFileSystem(FS_UDF);
        rufus::SelectionController restored;
        require(controller.fileIndexing() == expected.fileIndexing && restored.fileIndexing() == expected.fileIndexing,
            "saved indexing choice survives UDF switch and controller reload");
    }
    controller.setVolumeLabel(expected.extendedLabel ? expected.displayLabel : expected.label);
    if (runtime) {
        controller.setClusterSize(expected.clusterSize); controller.setExtendedLabel(false);
        require(controller.canRuntimeValidate(), "runtime validation eligibility"); controller.setRuntimeValidation(true);
    }
    if (legacy || isoOptions || ntfsOptions || geometry) { controller.setClusterSize(expected.clusterSize); controller.setExtendedLabel(false); }
    if (expected.archive.present()) controller.selectArchive(expected.archive.path);
    require(controller.canStartWrite(), "ordinary START admission"); controller.startWrite();
    auto& writer = controller.isoWriter();
    QElapsedTimer timer; timer.start();
    if ((legacy || isoOptions) && expected.msDos) writer.chooseDosOptions(expected.dosKeyboard,expected.dosCodepage);
    while (writer.state() == rufus::IsoWriteState::Checking) {
        QCoreApplication::processEvents(); require(timer.elapsed() < 30000, "preflight deadline"); usleep(1000);
    }
    if ((runtime || isoOptions || ntfsOptions) && writer.state() == rufus::IsoWriteState::ChoosingMode) {
        writer.chooseMode(IMAGE_WRITE_ISO);
        while (writer.state() == rufus::IsoWriteState::Checking) {
            QCoreApplication::processEvents(); require(timer.elapsed() < 60000, "ISO preflight deadline"); usleep(1000);
        }
    }
    if (ntfsOptions && writer.state() == rufus::IsoWriteState::ChoosingWindowsOptions) {
        writer.chooseWindowsOptions(expected);
        while (writer.state() == rufus::IsoWriteState::Checking) {
            QCoreApplication::processEvents(); require(timer.elapsed() < 60000, "Windows option preflight deadline"); usleep(1000);
        }
    }
    if (legacyIso && writer.state() == rufus::IsoWriteState::ChoosingWindowsOptions) {
        writer.chooseWindowsOptions(expected);
        while (writer.state() == rufus::IsoWriteState::Checking) {
            QCoreApplication::processEvents(); require(timer.elapsed() < 60000, "B4 Windows option preflight deadline"); usleep(1000);
        }
    }
    if (isoResources && writer.state() == rufus::IsoWriteState::SelectingGrubCore) {
        qFatal("B3 missing staged source-specific cache");
    }
    if (writer.state() != rufus::IsoWriteState::AwaitingConfirmation) {
        for (const auto& entry : logs) fprintf(stderr, "NATIVE_LOG %s\n", qPrintable(entry.first().toString()));
        qFatal("Native preflight: %s", qPrintable(writer.statusText()));
    }
    require(writer.request().fileSystem == expected.fileSystem && writer.request().sfd == expected.sfd
        && writer.request().mbr == expected.mbr && writer.request().quickFormat == expected.quickFormat, "frozen format and layout");
    if (geometry) require(writer.request().logicalSectorSize == expected.logicalSectorSize
        && writer.request().physicalSectorSize == expected.physicalSectorSize
        && writer.request().clusterSize == expected.clusterSize, "frozen actual device geometry and cluster");
    if (legacyIso) require(writer.request().windowsBios == expected.windowsBios
        && writer.request().syslinux == expected.syslinux && writer.request().kolibriOs == expected.kolibriOs
        && writer.request().enhancedInstall == expected.enhancedInstall && writer.request().uefiNtfs == expected.uefiNtfs,"B4 frozen original dispatch");
    if (isoOptions) require(writer.request().joliet == expected.joliet && writer.request().rockRidge == expected.rockRidge
        && writer.request().preserveTimestamps == expected.preserveTimestamps && writer.request().sizeChecks == expected.sizeChecks
        && writer.request().clusterSize == expected.clusterSize, "frozen ISO options and DOS override");
    if (ntfsOptions) require(writer.request().ntfsCompression == expected.ntfsCompression
        && writer.request().fileIndexing == expected.fileIndexing, "frozen NTFS options");
    if (udfIndexing) require(writer.request().fileIndexing == expected.fileIndexing && !writer.request().ntfsCompression,
        "frozen UDF indexing choice without NTFS compression");
    require(writer.request().deviceClass == expected.deviceClass, "frozen native device class");
    require(writer.request().runtimeValidation == expected.runtimeValidation, "frozen runtime flag");
    require(writer.request().extendedLabel == expected.extendedLabel && writer.request().archive.present() == expected.archive.present(), "frozen files");
    require(read("/run/format-launches").isEmpty(), "no authorization before confirmation");
    writer.confirm(); timer.restart(); bool held = false;
    while (writer.isActive()) {
        QCoreApplication::processEvents(); require(timer.elapsed() < 120000, "creation deadline");
        if (QFile::exists(QStringLiteral("/run/format-exit-held")) && writer.isActive()) {
            require(writer.state() != rufus::IsoWriteState::Complete && writer.progressPosition() < MAX_PROGRESS, "READY waits for exit"); held = true;
            if (QFile::exists(QStringLiteral("/etc/rufus-device-source-fixture"))) {
                QFile release(QStringLiteral("/run/format-exit-release"));
                require(release.open(QIODevice::WriteOnly) && release.write("release\n") == 8, "explicit B2 launcher release");
            }
        }
        usleep(1000);
    }
    if (writer.state() != rufus::IsoWriteState::Complete) {
        for (const auto& entry : logs) fprintf(stderr, "NATIVE_LOG %s\n", qPrintable(entry.first().toString()));
        qFatal("Native format job: %s", qPrintable(writer.statusText()));
    }
    require(held && writer.progressPosition() == MAX_PROGRESS && read("/run/format-launches") == "launch\n", "joined helper, one authorization, full READY");
    require(!expected.extendedLabel || writer.outcome().appearanceDone, "appearance finished");
    require(!expected.archive.present() || writer.outcome().archiveDone, "supplement finished");
    if (udfIndexing) {
        int attempts = 0, unsupported = 0;
        for (const auto& entry : logs) {
            const QString line = entry.first().toString();
            if (line == QStringLiteral("Disabling file indexing...")) ++attempts;
            if (line == QStringLiteral("Could not disable UDF file content indexing: the Linux UDF provider does not expose the Windows NOT_CONTENT_INDEXED attribute.")) ++unsupported;
        }
        const int expectedAttempts = expected.fileIndexing ? 0 : 1;
        require(attempts == expectedAttempts && unsupported == expectedAttempts, "optional UDF indexing diagnostic follows saved choice");
        require(writer.outcome().error == 0 && writer.outcome().syncError == 0 && writer.outcome().cleanupError == 0,
            "optional indexing leaves successful sync and cleanup");
        printf("RUFUS_UDF_INDEXING_QT_DONE INDEXING=%d DIAGNOSTIC=%d AUTH=1 JOINED=1 READY=100\n",
            int(expected.fileIndexing), expectedAttempts);
    }
    if (geometry && expected.fileSystem == FS_NTFS) {
        int capability = -1, partitioning = -1, kernel = -1, fallback = -1;
        for (qsizetype index = 0; index < logs.size(); index++) {
            const QString line = logs[index].first().toString();
            if (line.contains(QStringLiteral("Non-512-byte NTFS file creation requires ntfs-3g."))) capability = int(index);
            if (line.contains(QStringLiteral("Partitioning..."))) partitioning = int(index);
            if (line.contains(QStringLiteral("NTFS file copying uses the kernel ntfs driver"))) kernel = int(index);
            if (line.contains(QStringLiteral("NTFS file copying uses ntfs-3g"))) fallback = int(index);
        }
        require(partitioning >= 0, "native NTFS partition phase recorded");
        const bool non512 = expected.logicalSectorSize != 512;
        require(non512 ? capability >= 0 && capability < partitioning && fallback > partitioning && kernel < 0
            : capability < 0 && kernel > partitioning && fallback < 0,
            "native NTFS provider selected by geometry before partitioning");
        printf("RUFUS_GEOMETRY_NTFS_PROVIDER LOGICAL=%u PHYSICAL=%u DRIVER=%s PREWRITE=1\n",
            expected.logicalSectorSize, expected.physicalSectorSize, non512 ? "ntfs-3g" : "ntfs");
    }
    if (geometry) printf("RUFUS_GEOMETRY_QT_DONE FS=%d LOGICAL=%u PHYSICAL=%u AUTH=1 JOINED=1 READY=100\n",
        expected.fileSystem, expected.logicalSectorSize, expected.physicalSectorSize);
    else if (deviceOptions) printf("RUFUS_DEVICE_OPTIONS_QT_DONE CLASS=%u AUTH=1 JOINED=1 READY=100\n", rufus::DeviceClassMask(expected.deviceClass));
    else if (ntfsOptions) printf("RUFUS_NTFS_OPTIONS_QT_DONE JOB=%d AUTH=1 JOINED=1 READY=100\n", expected.nonBootable ? (expected.fileIndexing ? 1 : 0) : 2);
    else if (legacyIso) printf("RUFUS_LEGACY_ISO_QT_DONE FS=%d AUTH=1 JOINED=1 READY=100\n", expected.fileSystem);
    else if (isoResources) printf("RUFUS_ISO_RESOURCES_QT_DONE FS=%d AUTH=1 JOINED=1 READY=100\n", expected.fileSystem);
    else if (isoOptions) printf("RUFUS_ISO_OPTIONS_QT_DONE FS=%d AUTH=1 JOINED=1 READY=100\n", expected.fileSystem);
    else if (legacy) printf("RUFUS_LEGACY_QT_DONE DOS=%d FS=%d AUTH=1 JOINED=1 READY=100\n",int(expected.msDos),expected.fileSystem);
    else if (runtime) printf("RUFUS_RUNTIME_QT_DONE FS=%d AUTH=1 JOINED=1 READY=100\n", expected.fileSystem);
    else printf("RUFUS_NATIVE_QT_DONE FS=%d SFD=%d MBR=%d QUICK=%d AUTH=1 JOINED=1 READY=100\n",
        expected.fileSystem, int(expected.sfd), int(expected.mbr), int(expected.quickFormat));
    return 0;
}
