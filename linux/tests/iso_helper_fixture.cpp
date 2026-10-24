// SPDX-License-Identifier: GPL-3.0-or-later
// Protocol-only child for Widgets lifecycle tests. Never opens the target.
#include "qt/prepare_image_protocol.h"
#include "platform/format_options.h"
#include "unattend.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QCryptographicHash>
#include <QFile>
#include <cerrno>
#include <cstdio>
#include <iostream>
#include <string>

extern "C" int IsRufusJobCancelled(void) { return 0; }
extern "C" int IsIsoScanCancelled(void) { return 0; }
extern "C" void uprintf(const char*, ...) {}
extern "C" void _UpdateProgressWithInfo(int, int, uint64_t, uint64_t, int) {}

static void send(const QJsonObject& object)
{
    const QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
    fwrite(bytes.constData(), 1, size_t(bytes.size()), stdout);
    fflush(stdout);
}

static int runRaw(const rufus::PrepareImageRequest& request, const QString& mode)
{
    rufus::PrepareImageOutcome outcome;
    outcome.rawWrite = outcome.started = true;
    const quint64 total = rufus::PrepareRawWriteBytes(request);
    send({{"event", "phase"}, {"phase", "checking"}});
    send(rufus::EncodeRawProgress(RAW_WRITE_COPY, 0, total));
    send(rufus::EncodeRawProgress(RAW_WRITE_COPY, total / 2, total));
    std::string line;
    if ((mode == QStringLiteral("raw-steps") || mode == QStringLiteral("zero-empty-steps")) || mode == QStringLiteral("raw-hold-copy"))
    {
        if (!std::getline(std::cin, line))
            outcome.error = ECANCELED;
    }
    if (mode == "write-error") outcome.error = ENOSPC;
    outcome.raw.bytes_written = outcome.error == 0 ? total : total / 2;
    outcome.raw.phase = RAW_WRITE_COPY;
    if (outcome.error == 0)
        send(rufus::EncodeRawProgress(RAW_WRITE_COPY, total, total));
    send(rufus::EncodeRawProgress(RAW_WRITE_SYNC, 0, 0));
    if (mode == QStringLiteral("sync-error"))
    {
        outcome.error = outcome.raw.sync_error = EIO;
        outcome.raw.phase = RAW_WRITE_SYNC;
    }
    if (((mode == "raw-steps" || mode == "zero-empty-steps") || mode == "raw-hold-sync") && outcome.error == 0)
        if (!std::getline(std::cin, line)) outcome.error = ECANCELED;
    if (outcome.error == 0)
    {
        send(rufus::EncodeRawProgress(RAW_WRITE_VERIFY, 0, total));
        send(rufus::EncodeRawProgress(RAW_WRITE_VERIFY, total / 2, total));
        outcome.raw.phase = RAW_WRITE_VERIFY;
        outcome.raw.bytes_verified = total / 2;
        if ((mode == "raw-steps" || mode == "zero-empty-steps") || mode == "raw-hold-verify")
            if (!std::getline(std::cin, line)) outcome.error = ECANCELED;
        if (mode == "verify-error") outcome.error = EBADMSG;
        if (outcome.error == 0)
        {
            send(rufus::EncodeRawProgress(RAW_WRITE_VERIFY, total, total));
            outcome.raw.bytes_verified = total;
            outcome.raw.phase = RAW_WRITE_DONE;
        }
    }
    if (request.zeroMode == ZERO_FAST && mode == QStringLiteral("zero-empty-steps"))
        outcome.raw.bytes_written = outcome.raw.bytes_verified = 0;
    send(rufus::EncodePrepareOutcome(outcome));
    if ((mode == QStringLiteral("raw-steps") || mode == QStringLiteral("zero-empty-steps")))
        std::getline(std::cin, line); // Keep the child alive after its successful result.
    return outcome.error == 0 ? 0 : 1;
}

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    QString mode = application.arguments().value(1);
    const bool fixedUsb = mode.startsWith(QStringLiteral("fixed-"));
    if (fixedUsb) mode.remove(0, 6);
    if (mode == QStringLiteral("deny")) return 127;
    if (mode == QStringLiteral("dismiss")) return 126;
    if (mode == QStringLiteral("unsupported")) return 1;
    if (mode == QStringLiteral("hold-authorization"))
    {
        std::string wait;
        std::getline(std::cin, wait);
        return 126;
    }
    send({{"event", "ready"}});
    std::string line;
    if (!std::getline(std::cin, line))
        return 1;
    rufus::PrepareImageRequest request;
    const QJsonDocument document = QJsonDocument::fromJson(QByteArray::fromStdString(line));
    if (!rufus::DecodePrepareRequest(document.object(), request) || !request.isDevice)
        return 1;
    if (fixedUsb && !request.allowFixedUsb)
        return 1;
    if (request.rawWrite)
        return runRaw(request, mode);
    const bool standaloneSyslinux = request.isStandaloneSyslinux();
    const bool standaloneGrub4dos = request.grub4dos && request.isSourceFree();
    const bool syslinuxNtfs = request.syslinux && request.fileSystem == FS_NTFS;
    if (request.grub2)
    {
        const bool fat = request.fileSystem == FS_FAT16 || request.fileSystem == FS_FAT32;
        const bool ntfs = request.fileSystem == FS_NTFS;
        const bool ext = request.fileSystem == FS_EXT2 || request.fileSystem == FS_EXT3;
        const QString operation = ext ? QStringLiteral("prepare-grub2-ext%1-device-v1").arg(request.fileSystem == FS_EXT2 ? 2 : 3)
            : ntfs ? (request.usesUefiNtfs() ? QStringLiteral("prepare-grub2-ntfs-uefi-device-v1")
                : QStringLiteral("prepare-grub2-ntfs-device-v1"))
            : QStringLiteral("prepare-grub2-fat%1-device-v1").arg(request.fileSystem == FS_FAT16 ? 16 : 32);
        if (!(fat || ntfs || ext) || !request.mbr
            || document.object().value("operation") != operation)
            return 1;
    }
    if (request.grub4dos)
    {
        const bool fat = request.fileSystem == FS_FAT16 || request.fileSystem == FS_FAT32;
        const bool ntfs = request.fileSystem == FS_NTFS;
        const bool standaloneLoader = standaloneGrub4dos && request.mbr && (fat || ntfs) && request.hasGrub2Core();
        const bool imageLoader = !request.isSourceFree() && request.mbr
            && (fat || ntfs) && !request.hasGrub2Core();
        const QString operation = ntfs ? (request.usesUefiNtfs() ? QStringLiteral("prepare-grub4dos-ntfs-uefi-device-v1")
            : QStringLiteral("prepare-grub4dos-ntfs-device-v1"))
            : QStringLiteral("prepare-grub4dos-fat%1-device-v1").arg(request.fileSystem == FS_FAT16 ? 16 : 32);
        const bool exactOperation = document.object().value("operation") == operation;
        if (!(standaloneLoader || imageLoader) || !exactOperation)
            return 1;
    }
    const QByteArray expectedCore = QByteArray::fromHex(application.arguments().value(3).toLatin1());
    if (!expectedCore.isEmpty())
    {
        QFile core(request.grub2CorePath);
        if (!request.hasGrub2Core() || !core.open(QIODevice::ReadOnly))
            return 1;
        const QByteArray bytes = core.readAll();
        const QByteArray hash = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
        if (hash != expectedCore || hash != request.grub2CoreSha256 || quint64(bytes.size()) != request.grub2CoreSize)
            return 1;
    }
    if (request.syslinux)
    {
        const bool supportedFilesystem = request.fileSystem == FS_FAT16 || request.fileSystem == FS_FAT32
            || (syslinuxNtfs && request.syslinuxSelection != BT_SYSLINUX_V4);
        const bool plainLoader = !request.enhancedInstall && request.windowsFlags == 0;
        const bool retainedSource = !request.imagePath.isEmpty() && request.imageSize != 0;
        const bool validSource = standaloneSyslinux ? request.imagePath.isEmpty() && request.imageSize == 0 : retainedSource;
        if (!supportedFilesystem || !plainLoader || !validSource || request.hasWindowsInstallSettings())
            return 1;
        const QString ntfsImage = request.uefiNtfs ? QStringLiteral("prepare-ntfs-syslinux-uefi-device-v1")
            : QStringLiteral("prepare-ntfs-syslinux-device-v1");
        const QString operation = syslinuxNtfs ? (standaloneSyslinux ? QStringLiteral("prepare-syslinux6-ntfs-device-v1") : ntfsImage) : standaloneSyslinux
            ? QStringLiteral("prepare-syslinux%1-fat%2-device-v1")
                .arg(request.syslinuxSelection == BT_SYSLINUX_V4 ? 4 : 6).arg(request.fileSystem == FS_FAT16 ? 16 : 32)
            : request.fileSystem == FS_FAT16 ? QStringLiteral("prepare-fat16-syslinux-device-v1")
            : QStringLiteral("prepare-syslinux-device-v1");
        if (document.object().value("operation") != operation)
            return 1;
    }
    if (request.fileSystem == FS_FAT16 && !request.syslinux && !request.nonBootable
        && !request.freeDos && !request.grub2 && !request.grub4dos && !request.windowsBios)
    {
        const bool fat16Source = document.object().value("operation") == "prepare-fat16-device-v1"
            && !request.imagePath.isEmpty() && request.imageSize != 0;
        if (!fat16Source)
            return 1;
    }
    if (request.windowsBios && request.fileSystem == FS_FAT16)
    {
        const bool source = !request.imagePath.isEmpty() && request.imageSize != 0;
        if (!request.mbr || !request.enhancedInstall || !source
            || document.object().value("operation") != "prepare-windows-fat16-bios-device-v1") return 1;
    }
    if (request.kolibriOs)
    {
        const QString operation = request.fileSystem == FS_NTFS ? QStringLiteral("prepare-kolibri-ntfs-device-v1")
            : QStringLiteral("prepare-kolibri-fat32-device-v1");
        if (!request.mbr || request.imagePath.isEmpty() || request.imageSize == 0
            || document.object().value("operation") != operation) return 1;
    }
    const bool enhanced = mode == QStringLiteral("enhanced-fat32") || mode == QStringLiteral("enhanced-ntfs");
    if (enhanced)
    {
        const bool ntfs = mode == QStringLiteral("enhanced-ntfs");
        if (!request.enhancedInstall || (request.fileSystem == FS_NTFS) != ntfs || request.windowsFlags != 0)
            return 1;
    }
    if (mode == QStringLiteral("s-mode-settings"))
    {
        if (request.windowsFlags != UNATTEND_FORCE_S_MODE || !request.windowsUsername.isEmpty())
            return 1;
        if (request.hasWindowsInstallSettings())
            return 1;
    }
    if (mode == QStringLiteral("silent-settings"))
    {
        const int expectedFlags = UNATTEND_SILENT_INSTALL | UNATTEND_SET_USER |
            UNATTEND_NO_DATA_COLLECTION | UNATTEND_DUPLICATE_LOCALE;
        const bool flagsAndAccount = request.windowsFlags == expectedFlags
            && request.windowsUsername == QStringLiteral("Măriuș_Test") && request.windowsSilentConfirmed;
        const bool selection = request.windowsInstallPath == QStringLiteral("/sources/install.wim") && request.windowsEdition == 1;
        const auto& region = request.windowsRegion;
        const bool locales = region.uiLanguage == QStringLiteral("en-US") && region.uiLanguageFallback.isEmpty()
            && region.systemLocale == QStringLiteral("ro-RO") && region.userLocale == QStringLiteral("en-GB");
        const bool regional = region.inputLocale == QStringLiteral("0409:00020409;ro-RO")
            && region.timezone == QStringLiteral("GTB Standard Time");
        if (!flagsAndAccount || !selection || !locales || !regional)
            return 1;
    }
    for (const char* phase : {"checking", "partitioning", "formatting"})
        send({{"event", "phase"}, {"phase", QLatin1String(phase)}});
    if (mode == QStringLiteral("hold-formatting"))
    {
        std::getline(std::cin, line);
        rufus::PrepareImageOutcome cancelled;
        cancelled.started = true;
        cancelled.error = ECANCELED;
        send(rufus::EncodePrepareOutcome(cancelled));
        return 1;
    }
    if (syslinuxNtfs && standaloneSyslinux)
    {
        send({{"event", "phase"}, {"phase", "mounting"}});
        const bool cancelMount = mode == QStringLiteral("hold-mounting");
        const bool mountError = mode == QStringLiteral("mount-error");
        if (cancelMount || mountError)
        {
            if (cancelMount) std::getline(std::cin, line);
            send({{"event", "phase"}, {"phase", "finalizing"}});
            rufus::PrepareImageOutcome failed;
            failed.started = true;
            failed.error = cancelMount ? ECANCELED : EIO;
            send(rufus::EncodePrepareOutcome(failed));
            return 1;
        }
    }
    if (!request.windowsToGo && (request.usesUefiNtfs() || request.freeDos || request.grub2 || request.grub4dos || request.windowsBios || request.kolibriOs || standaloneSyslinux))
        send({{"event", "phase"}, {"phase", "bootloader"}});
    const bool cancelBoot = mode == QStringLiteral("hold-bootloader");
    if (cancelBoot)
        std::getline(std::cin, line);
    const bool formatOnly = request.nonBootable || request.isStandaloneUefiNtfs() || (request.grub2 && request.isSourceFree()) || standaloneSyslinux;
    bool cancelCopy = false;
    if (standaloneSyslinux && !cancelBoot)
    {
        // Windows reserves three slots but emits only these two copy events.
        constexpr int syslinuxCopySteps = 2;
        for (int i = 0; i < syslinuxCopySteps; ++i)
        {
            send({{"event", "step"}, {"operation", OP_FILE_COPY}});
            if (mode == QStringLiteral("syslinux-steps") && !std::getline(std::cin, line))
            {
                cancelCopy = true;
                break;
            }
        }
    }
    if (!formatOnly)
    {
        if (!request.windowsToGo) send({{"event", "phase"}, {"phase", "mounting"}});
        send({{"event", "phase"}, {"phase", "extracting"}});
        if (request.freeDos)
        {
            // The original DOS path has one initial step plus five copy steps.
            constexpr int dosCopySteps = 6;
            for (int i = 0; i < dosCopySteps; ++i)
            {
                send({{"event", "step"}, {"operation", OP_FILE_COPY}});
                if (mode == QStringLiteral("freedos-steps") && !std::getline(std::cin, line))
                {
                    cancelCopy = true;
                    break;
                }
            }
        }
        else if (!standaloneGrub4dos)
        {
            send(rufus::EncodePrepareProgress(OP_FILE_COPY, 0, 10));
            send(rufus::EncodePrepareProgress(OP_FILE_COPY, 5, 10));
            if (mode == QStringLiteral("iso-steps") && !std::getline(std::cin, line))
                cancelCopy = true;
        }
    }
    if (mode == QStringLiteral("progress-backwards"))
        send(rufus::EncodePrepareProgress(OP_FILE_COPY, 4, 10));
    if (mode == QStringLiteral("progress-changed-total"))
        send(rufus::EncodePrepareProgress(OP_FILE_COPY, 6, 11));
    if (mode == QStringLiteral("progress-overrun"))
        send(rufus::EncodePrepareProgress(OP_FILE_COPY, 11, 10));
    rufus::PrepareImageOutcome outcome;
    outcome.started = true;
    outcome.offset = 1048576;
    const bool ntfsImage = request.fileSystem == FS_NTFS && !formatOnly;
    outcome.size = ntfsImage ? 64929792 : 66028032;
    if (!ntfsImage)
        outcome.size -= outcome.size % request.clusterSize;
    if (formatOnly && request.size == 64ULL * 1024 * 1024 * 1024)
        outcome.size = 68718395392; // 64-GiB GPT fixture with 32-KiB clusters.
    if (request.syslinux || (request.mbr && mode != QStringLiteral("gpt-layout")))
    {
        outcome.size = 66060288; // 129024 sectors: MBR track rounding, no GPT tail.
        if (request.fileSystem == FS_EXT2 || request.fileSystem == FS_EXT3)
            outcome.size = 334494720; // 320 MiB: partition rounded down to 63-sector tracks.
        if (request.size == 64ULL * 1024 * 1024 * 1024)
            outcome.size = 68718395392; // 64-GiB MBR fixture with 32-KiB clusters.
    }
    if (request.sfd && mode != QStringLiteral("gpt-layout"))
    {
        outcome.offset = 0;
        outcome.size = request.size;
    }
    if (request.uefiNtfs)
    {
        // Independent 64-MiB layout expectations: the 1-MiB boot payload is
        // track-rounded, then the main volume is track/cluster-rounded.
        const bool mbr = (request.mbr || request.syslinux) && mode != QStringLiteral("gpt-layout");
        outcome.size = mbr ? 64962560 : 64929792;
        outcome.size -= outcome.size % request.clusterSize;
    }
    if (cancelBoot || cancelCopy)
        outcome.error = ECANCELED;
    if (mode == QStringLiteral("wrong-layout"))
        outcome.size -= request.clusterSize;
    const bool nativeFormat = request.fileSystem == FS_NTFS || request.fileSystem == FS_EXT2 || request.fileSystem == FS_EXT3;
    if (request.windowsToGo)
    {
        GPT_IMAGE_RESULT planned = {};
        if (rufus::PlanFormatPartition(request, planned) != 0) return 1;
        outcome.offset = planned.offset;
        outcome.size = planned.size;
    }
    const bool exactFormatSize = request.fileSystem == FS_EXFAT || request.windowsToGo || (nativeFormat && (formatOnly || request.grub2 || request.grub4dos));
    outcome.filesystemBytes = exactFormatSize ? outcome.size : outcome.size - 512;
    outcome.copiedBytes = formatOnly ? 0 : standaloneGrub4dos ? request.grub2CoreSize
        : application.arguments().value(2).toULongLong();
    if (mode == QStringLiteral("short-format"))
        outcome.filesystemBytes -= 512;
    if (mode == QStringLiteral("short-copy"))
        outcome.copiedBytes = 1;
    if (mode == QStringLiteral("hold"))
    {
        std::getline(std::cin, line); // EOF must reach us through GUI cancellation.
        outcome.error = ECANCELED;
    }
    if (mode == QStringLiteral("cleanup-error"))
    {
        outcome.error = EBUSY;
        outcome.cleanupError = EBUSY;
    }
    if (mode == QStringLiteral("sync-error"))
    {
        outcome.error = EIO;
        outcome.syncError = EIO;
    }
    const bool extractionFailed = mode == QStringLiteral("extract-error");
    if (extractionFailed)
        outcome.error = EIO;
    if (!formatOnly && !request.freeDos && !standaloneGrub4dos && mode != QStringLiteral("hold"))
        send(rufus::EncodePrepareProgress(OP_FILE_COPY, 10, 10));
    if (request.windowsToGo) send({{"event", "phase"}, {"phase", "bootloader"}});
    if (request.windowsFlags != 0 && !extractionFailed && mode != QStringLiteral("skip-customization"))
    {
        send({{"event", "phase"}, {"phase", "customizing"}});
        if (mode == QStringLiteral("customization-error"))
            outcome.error = ENOSPC;
        if (mode == QStringLiteral("customization-cancel"))
            outcome.error = ECANCELED;
    }
    if (request.extendedLabel && formatOnly && !(standaloneSyslinux && request.fileSystem == FS_NTFS))
        send({{"event", "phase"}, {"phase", "mounting"}});
    outcome.appearanceDone = request.extendedLabel && outcome.error == 0 && mode != QStringLiteral("missing-appearance");
    if (mode != QStringLiteral("skip-finalizing"))
        send({{"event", "phase"}, {"phase", "finalizing"}});
    if (mode == QStringLiteral("progress-wrong-phase"))
        send(rufus::EncodePrepareProgress(OP_FILE_COPY, 10, 10));
    if (mode == QStringLiteral("iso-steps"))
        std::getline(std::cin, line);
    if (mode == QStringLiteral("hold-finalizing"))
    {
        std::getline(std::cin, line);
        outcome.error = ECANCELED;
    }
    outcome.windowsToGoDone = request.windowsToGo && outcome.error == 0 && mode != QStringLiteral("missing-togo");
    send(rufus::EncodePrepareOutcome(outcome));
    const bool holdResult = mode == QStringLiteral("hold-after-result") || mode == QStringLiteral("freedos-steps")
        || mode == QStringLiteral("syslinux-steps") || mode == QStringLiteral("iso-steps");
    if (holdResult)
        std::getline(std::cin, line); // Test releases EOF after observing the result.
    return outcome.error == 0 ? 0 : 1;
}
