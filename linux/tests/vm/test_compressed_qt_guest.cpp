// SPDX-License-Identifier: GPL-3.0-or-later
// Only called by the guarded MBR guest driver, with its live private bus/home.
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
thread_local bool callerUid = false;

void require(bool valid, const char* message)
{
    if (!valid) qFatal("Compressed Qt guest failed: %s", message);
}

QByteArray readFile(const char* path)
{
    QFile file(QString::fromLatin1(path));
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
}

extern "C" uid_t __real_getuid();
extern "C" uid_t __wrap_getuid() { return callerUid ? 1000 : __real_getuid(); }

int main(int argc, char** argv)
{
    const QByteArray parent = QByteArray("/proc/") + QByteArray::number(getppid()) + "/exe";
    const bool guarded = getuid() == 0 && geteuid() == 0
        && readFile("/sys/class/dmi/id/product_name") == "RufusMBRFixture\n"
        && readFile("/etc/rufus-compressed-qt-fixture") == "compressed-qt\n"
        && QFile::symLinkTarget(QString::fromLatin1(parent)).endsWith(QStringLiteral("/test_mbr_handoff_guest"));
    if (!guarded) return 77;
    QCoreApplication application(argc, argv);
    QFile input;
    require(input.open(stdin, QIODevice::ReadOnly), "request pipe");
    const QByteArray packet = input.readLine(rufus::PrepareMessageBytes + 1);
    rufus::PrepareImageRequest expected;
    require(rufus::DecodePrepareRequest(QJsonDocument::fromJson(packet).object(), expected)
        && expected.rawWrite, "fixture source/target identity");
    const bool zero = expected.zeroMode != ZERO_NONE;
    const bool compressed = expected.hasCompressedImage();
    const bool xz = expected.imageCompression == BLED_COMPRESSION_XZ;
    const bool bzip2 = expected.imageCompression == BLED_COMPRESSION_BZIP2;
    const bool lzma = expected.imageCompression == BLED_COMPRESSION_LZMA;
    const bool lzw = expected.imageCompression == BLED_COMPRESSION_LZW;
    const bool zip = expected.imageCompression == BLED_COMPRESSION_ZIP;
    const bool zstd = expected.imageCompression == BLED_COMPRESSION_ZSTD;
    const bool ffu = expected.imageCompression == IMG_COMPRESSION_FFU;
    const bool vtsi = expected.imageCompression == BLED_COMPRESSION_VTSI;
    const bool vhd = expected.imageCompression == IMG_COMPRESSION_VHD || expected.imageCompression == IMG_COMPRESSION_VHDX || expected.imageCompression == IMG_COMPRESSION_FFU;
    const bool lzwFixture = QFile::exists(QStringLiteral("/etc/rufus-lzw-qt-fixture"));
    require(!compressed || xz || bzip2 || lzma || lzw || zip || zstd || vtsi || vhd || expected.imageCompression == BLED_COMPRESSION_GZIP, "qualified compressed provider");
    const bool largerImage = xz || bzip2 || lzma || lzwFixture || zip || zstd || vtsi || vhd;
    const int analysisTimeoutMs = largerImage ? 180000 : 90000;
    const int helperTimeoutMs = lzma || lzwFixture || zip || zstd || vtsi || vhd ? 600000 : largerImage ? 300000 : 90000;
    rufus::UDisksObjects objects;
    rufus::Device device;
    require(rufus::ReadPrepareDeviceSnapshot(QDBusConnection::systemBus(), expected, objects, device) == 0,
        "fresh native target selection");
    device.serviceOwner = expected.serviceOwner;
    device.id = device.objectPath;
    // Model the caller only in this front-end thread. The exec'd production
    // helper and all native kernel/owner/mount/claim operations remain real.
    callerUid = true;
    rufus::SelectionController controller;
    controller.writer().setConnection(QDBusConnection::systemBus());
    controller.writer().setHelper(QStringLiteral("/usr/bin/rufus-prepare-helper"));
    controller.setDiscovery({device}, {});
    controller.setExtendedLabel(expected.extendedLabel);
    controller.setBadBlocksEnabled(expected.badBlocksPattern >= 0);
    if (expected.badBlocksPattern >= 0) controller.setBadBlocksPattern(expected.badBlocksPattern);
    if (controller.detectFakes() != expected.detectFakes) controller.toggleFakeDetection();
    if (controller.ignoreBootMarker() != expected.ignoreBootMarker) controller.toggleBootMarker();
    if (zero) controller.startZero(expected.zeroMode == ZERO_FAST);
    else controller.selectImage(expected.imagePath);
    QElapsedTimer timer;
    timer.start();
    while (controller.isAnalyzing())
    {
        require(timer.elapsed() < analysisTimeoutMs, "compressed analysis deadline");
        QCoreApplication::processEvents();
        usleep(1000);
    }
    if (controller.selection().imageState == rufus::ImageState::ChoosingStore)
    {
        require(ffu && expected.ffuStore != 0 && !controller.canStartWrite(), "explicit FFU store before target access");
        require(!QFile::exists(QStringLiteral("/run/format-launches")), "no authorization before FFU store selection");
        controller.chooseFfuStore(expected.ffuStore);
        timer.restart();
        while (controller.isAnalyzing())
        {
            require(timer.elapsed() < analysisTimeoutMs, "selected FFU store analysis deadline");
            QCoreApplication::processEvents();
            usleep(1000);
        }
        require(controller.selection().imageSource.ffuStore == expected.ffuStore, "selected FFU store frozen after analysis");
    }
    if (!zero)
    {
        const auto& selected = controller.selection();
        require(selected.rawImage && selected.imageState == rufus::ImageState::Complete, "ordinary compressed selection");
        require(selected.imageSource.imageCompression == expected.imageCompression
            && selected.imageSize == expected.imageSize, "ordinary source type/stored size");
        if (compressed)
        require(selected.imageAnalysis.report.compression_type == expected.imageCompression
            && selected.imageAnalysis.report.image_size == expected.imageSize
            && selected.imageAnalysis.report.projected_size == expected.decodedImageSize, "compressed report facts");
        require(selected.imageSource.decodedImageSize == expected.decodedImageSize
            && selected.imageSource.decodedImageSha256 == expected.decodedImageSha256, "frozen decoded identity");
        require(selected.imageSource.imageLayoutSha256 == expected.imageLayoutSha256, "frozen VTSI extent map");
        require(selected.imageSource.ffuMinimumSize == expected.ffuMinimumSize, "optimized FFU minimum frozen after analysis");
        require(selected.imageSource.ignoreBootMarker == expected.ignoreBootMarker, "frozen explicit marker choice");
        require(controller.canStartWrite(), "ordinary START admission");
        require(readFile("/run/format-launches").isEmpty(), "no authorization during analysis");
        controller.startWrite();
    }
    auto& writer = controller.writer();
    timer.restart();
    while (writer.status().state == rufus::WriteState::Checking)
    {
        require(timer.elapsed() < 10000, "read-only target plan deadline");
        QCoreApplication::processEvents();
        usleep(1000);
    }
    require(writer.status().state == rufus::WriteState::AwaitingConfirmation, "native geometry before confirmation");
    require(writer.plan() && writer.plan()->imageMetadata().imageSectorSize == expected.imageSectorSize
        && writer.plan()->imageMetadata().imageLayoutSha256 == expected.imageLayoutSha256
        && writer.status().size == rufus::PrepareRawWriteBytes(expected), "actual target geometry and decoded progress total");
    require(writer.plan()->imageMetadata().ffuStore == controller.selection().imageSource.ffuStore, "FFU store retained for authorization");
    require(writer.plan()->imageMetadata().ffuMinimumSize == expected.ffuMinimumSize, "FFU fit bound retained for authorization");
    require(readFile("/run/format-launches").isEmpty(), "no authorization before confirmation");
    writer.confirm();
    timer.restart();
    bool held = false, held99 = false;
    qint64 nextDiagnosticMs = 15000;
    while (writer.isActive())
    {
        if (timer.elapsed() >= nextDiagnosticMs)
        {
            const auto& status = writer.status();
            printf("RUFUS_COMPRESSED_QT_WAIT TYPE=%d ELAPSED_MS=%lld STATE=%d PHASE=%d WRITTEN=%llu VERIFIED=%llu HELD=%d STATUS=%s\n",
                expected.imageCompression, static_cast<long long>(timer.elapsed()), int(status.state), int(status.outcome.io.phase),
                static_cast<unsigned long long>(status.outcome.io.bytes_written),
                static_cast<unsigned long long>(status.outcome.io.bytes_verified), held, qPrintable(writer.statusText()));
            fflush(stdout);
            nextDiagnosticMs += 15000;
        }
        require(timer.elapsed() < helperTimeoutMs, "ordinary helper completion deadline");
        QCoreApplication::processEvents();
        if (QFile::exists(QStringLiteral("/run/format-exit-held")) && writer.isActive())
        {
            require(writer.status().state == rufus::WriteState::Writing, "client waits for launcher exit");
            held = true;
            held99 = held99 || writer.percent() == 99;
        }
        usleep(1000);
    }
    if (writer.status().state != rufus::WriteState::Complete)
        qFatal("Compressed Qt guest: %s", qPrintable(writer.statusText()));
    const quint64 written = writer.status().outcome.io.bytes_written;
    const quint64 verified = writer.status().outcome.io.bytes_verified;
    const bool physicalComplete = zero ? (expected.zeroMode == ZERO_FAST ? written <= expected.size : written == expected.size) && verified == written : !compressed ? written == expected.imageSize && verified == written : vtsi || ffu ? written > 0 && written <= (ffu ? expected.size : expected.decodedImageSize) && verified == written
        : vhd ? written == expected.decodedImageSize && verified == written
        : written == expected.size && verified == expected.size;
    require(held && held99 && writer.percent() == 100 && physicalComplete, "joined verified result");
    const QByteArray launches = readFile("/run/format-launches");
    require(launches == "launch\n", "single authorized helper");
    require(QFile::remove(QStringLiteral("/run/format-launches"))
        && QFile::remove(QStringLiteral("/run/format-exit-held")), "launcher markers released");
    printf("RUFUS_COMPRESSED_QT_DONE WRITTEN=%llu VERIFIED=%llu SHA256=%s TYPE=%d STORED=%llu DECODED=%llu SECTOR=%u HELD=%d HELD99=%d PERCENT=%d LAUNCHES=%lld ZERO=%d\n",
        static_cast<unsigned long long>(writer.status().outcome.io.bytes_written),
        static_cast<unsigned long long>(writer.status().outcome.io.bytes_verified),
        expected.decodedImageSha256.toHex().constData(), expected.imageCompression,
        static_cast<unsigned long long>(expected.imageSize), static_cast<unsigned long long>(expected.decodedImageSize),
        expected.imageSectorSize, held, held99, writer.percent(), static_cast<long long>(launches.count("launch\n")), expected.zeroMode);
    return 0;
}
