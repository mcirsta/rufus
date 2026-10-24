// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/syslinux_source.h"
#include "platform/image_source.h"
#include "iso_file.h"
#include "format_policy.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <unistd.h>

static const char* fault;
static bool checking;
static int reads, cancellations;
extern "C" void uprintf(const char*, ...) {}
extern "C" int IsIsoScanCancelled(void)
{
    if (!checking)
        return 0;
    if (strcmp(fault, "cancel") == 0)
        return 1;
    return strcmp(fault, "cancel-walk") == 0 && ++cancellations >= 4;
}

extern "C" int IsRufusJobCancelled(void) { return IsIsoScanCancelled(); }

extern "C" int __real_ReadIsoFileRange(ISO_FILE_READER*, uint64_t, void*, size_t);
extern "C" int __wrap_ReadIsoFileRange(ISO_FILE_READER* reader, uint64_t offset, void* buffer, size_t size)
{
    if (checking)
    {
        reads++;
        if (strcmp(fault, "read-fail") == 0)
            return ISO_SCAN_PARTIAL;
        if (strcmp(fault, "cancel-read") == 0)
            return ISO_SCAN_CANCELLED;
    }
    return __real_ReadIsoFileRange(reader, offset, buffer, size);
}

static int descriptorCount()
{
    DIR* directory = opendir("/proc/self/fd");
    if (directory == nullptr)
        qFatal("Cannot count descriptors");
    int count = 0;
    struct dirent* entry;
    while ((entry = readdir(directory)) != nullptr)
        if (entry->d_name[0] != '.')
            count++;
    closedir(directory);
    return count;
}

int main(int argc, char** argv)
{
    if (argc != 3 && argc != 4)
        return 64;
    const int targetFilesystem = argc == 4 && strcmp(argv[3], "ntfs") == 0 ? FS_NTFS : FS_FAT32;
    fault = argv[2];
    const int before = descriptorCount();
    IMAGE_FILE_SOURCE retained = {};
    int error = OpenImageFileSource(argv[1], &retained);
    if (error != 0)
        return 65;
    char path[64];
    snprintf(path, sizeof(path), "/proc/self/fd/%d", retained.fd);
    ISO_ANALYSIS_SOURCE source = {};
    ISO_ANALYSIS analysis = {};
    const ISO_ANALYSIS_OPTIONS options = {true, false, {0x0407, 0x0604}};
    const int status = AnalyzeIsoImageWithSource(path, uint64_t(retained.info.st_size), &options, &analysis, &source);
    QString problem;
    const bool analyzed = status == ISO_SCAN_OK || status == ISO_SCAN_PARTIAL;
    if (analyzed)
    {
        if (strcmp(fault, "incomplete") == 0)
            analysis.tree_complete = false;
        if (strcmp(fault, "wrong-view") == 0)
            analysis.filesystem = ISO_FILESYSTEM_NONE;
        if (strcmp(fault, "missing-config") == 0)
            strcpy(analysis.report.cfg_path, "/absent.cfg");
        unsigned char reportBefore[sizeof(analysis)];
        memcpy(reportBefore, &analysis, sizeof(analysis));
        checking = true;
        error = rufus::CheckNativeSyslinuxSource(source, analysis, options.enable_rockridge,
            targetFilesystem, 512, 0, problem);
        checking = false;
        if (memcmp(reportBefore, &analysis, sizeof(analysis)) != 0)
            qFatal("Source eligibility changed analysis facts");
        if (strcmp(fault, "replace-path") == 0)
        {
            const QByteArray moved = QByteArray(argv[1]) + ".held";
            if (rename(argv[1], moved.constData()) != 0)
                qFatal("Cannot move fixture");
            FILE* replacement = fopen(argv[1], "wb");
            if (replacement == nullptr || fwrite("replacement", 1, 11, replacement) != 11 || fclose(replacement) != 0)
                qFatal("Cannot replace fixture");
        }
        const int identityError = CheckImageFileSource(&retained, argv[1]);
        if (error == 0 && identityError != 0)
        {
            error = identityError;
            problem = QStringLiteral("Source identity changed before accepting eligibility.");
        }
    }
    else
    {
        error = EIO;
        problem = QStringLiteral("Source analysis failed.");
    }
    CloseIsoAnalysisSource(&source);
    CloseImageFileSource(&retained);
    const int leaks = descriptorCount() - before;
    const QJsonObject result = {{"error", error}, {"problem", problem}, {"fds", leaks}, {"reads", reads},
        {"filesystem", int(analysis.filesystem)}, {"config", analysis.report.cfg_path},
        {"detected", int(analysis.report.sl_detected_version)}, {"selected", int(analysis.report.sl_version)},
        {"inferred", bool(analysis.report.sl_version_inferred)}};
    puts(QJsonDocument(result).toJson(QJsonDocument::Compact).constData());
    return 0;
}
