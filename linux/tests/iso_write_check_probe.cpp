// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/iso_write_check.h"
#include "platform/prepare_source.h"
#include "platform/format_options.h"
#include "platform/secure_boot.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <cstdarg>
#include <cstdio>
#include <unistd.h>

extern "C" void _UpdateProgressWithInfo(int, int, uint64_t, uint64_t, int) {}
extern "C" int IsIsoScanCancelled(void) { return 0; }
extern "C" int IsRufusJobCancelled(void) { return 0; }
extern "C" void uprintf(const char* format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    vfprintf(stderr, format, arguments);
    va_end(arguments);
    fputc('\n', stderr);
}

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    const QStringList arguments = application.arguments();
    if (arguments.size() != 5 && arguments.size() != 6)
    {
        fputs("Usage: iso_write_check_probe IMAGE TARGET_BYTES CLUSTER_BYTES LABEL [ntfs|fat32-split|syslinux|syslinux-ntfs|syslinux-ntfs-uefi]\n", stderr);
        return 2;
    }
    rufus::PrepareImageRequest request;
    bool validSize, validCluster;
    request.size = arguments[2].toULongLong(&validSize);
    request.clusterSize = arguments[3].toUInt(&validCluster);
    request.label = arguments[4];
    if (arguments.size() == 6)
    {
        request.uefiNtfs = arguments[5] == QStringLiteral("syslinux-ntfs-uefi");
        const bool syslinuxNtfs = request.uefiNtfs || arguments[5] == QStringLiteral("syslinux-ntfs");
        request.fileSystem = arguments[5] == QStringLiteral("ntfs") || syslinuxNtfs ? FS_NTFS : FS_FAT32;
        request.enhancedInstall = arguments[5] == QStringLiteral("fat32-split");
        request.syslinux = arguments[5] == QStringLiteral("syslinux") || syslinuxNtfs;
        const bool esp = arguments[5] == QLatin1String("esp16") || arguments[5] == QLatin1String("esp32");
        if (esp) { request.isoEsp = true; request.fileSystem = arguments[5] == QLatin1String("esp16") ? FS_FAT16 : FS_FAT32; }
        if (request.fileSystem != FS_NTFS && !request.enhancedInstall && !request.syslinux && !esp)
            return 2;
    }
    if (!validSize || !validCluster)
        return 2;
    int error = wimlib_global_init(WIMLIB_INIT_FLAG_ASSUME_UTF8);
    if (error != 0)
        return 2;
    error = rufus::CapturePrepareSource(arguments[1], request);
    rufus::IsoWriteCheck result;
    ISO_ANALYSIS_SOURCE view = {};
    if (error == 0) error = rufus::CheckIsoWriteImage(request,getuid(),result,&view);
    int secureStatus = -1;
    if (!error) { rufus::SecureBootPolicy policy; secureStatus = rufus::LoadBundledSecureBootPolicy(policy); if (!secureStatus) secureStatus = rufus::ScanIsoSecureBoot(view,result.analysis,policy,false); }
    CloseIsoAnalysisSource(&view);
    QJsonArray choices;
    for (int i = 0; i < result.choices.count; ++i)
        choices.append(int(result.choices.mode[i]));
    const QJsonObject output = {{"error", error}, {"secureStatus",secureStatus}, {"securityPending",bool(result.analysis.pending & ISO_PENDING_SECURE_BOOT)}, {"extraPartitions", result.extraPartitions}, {"problem", result.problem},
        {"efi", result.efiPath}, {"pending", int(result.analysis.pending)}, {"choices", choices},
        {"files", QString::number(result.tree.files)}, {"bytes", QString::number(result.tree.bytes_written)},
        {"requiredBytes", QString::number(result.tree.required_bytes)}, {"splitInstallWim", result.splitInstallWim},
        {"offset", QString::number(result.partition.offset)}, {"size", QString::number(result.partition.size)},
        {"formatCluster", int(rufus::FormatFatClusterSize(request, result.partition))}};
    const QByteArray json = QJsonDocument(output).toJson();
    fwrite(json.constData(), 1, size_t(json.size()), stdout);
    wimlib_global_cleanup();
    return error == 0 ? 0 : 1;
}
