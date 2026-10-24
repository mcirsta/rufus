// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/zip_extract.h"
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

static const char* fault = "none";
static const char* archivePath;
static unsigned writes;
static bool active, failedSync;
extern "C" int IsRufusJobCancelled(void)
{
    return active && (strcmp(fault, "cancel") == 0 || (strcmp(fault, "cancel-write") == 0 && writes >= 2));
}
extern "C" int IsIsoScanCancelled(void) { return IsRufusJobCancelled(); }
extern "C" void uprintf(const char*, ...) {}
extern "C" void uprintfs(const char*) {}
extern "C" void UpdateProgress(int, float) {}
extern "C" void _UpdateProgressWithInfo(int, int, uint64_t, uint64_t, int) {}
extern "C" ssize_t __real_write(int, const void*, size_t);
extern "C" ssize_t __wrap_write(int descriptor, const void* bytes, size_t size)
{
    if (!active) return __real_write(descriptor, bytes, size);
    writes++;
    if (strcmp(fault, "write-fail") == 0 && writes >= 2) { errno = ENOSPC; return -1; }
    if (strcmp(fault, "short") == 0 && size > 7) size = 7;
    const ssize_t result = __real_write(descriptor, bytes, size);
    if (strcmp(fault, "mutate-write") == 0 && writes == 1) {
        const int changed = open(archivePath, O_WRONLY | O_APPEND | O_CLOEXEC);
        if (changed < 0 || __real_write(changed, "changed", 7) != 7 || close(changed) != 0) abort();
    }
    return result;
}
extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int descriptor)
{
    if (active && writes != 0 && !failedSync && strcmp(fault, "sync-fail") == 0) {
        failedSync = true; errno = EIO; return -1;
    }
    return __real_fsync(descriptor);
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 4 || argc > 6) return 2;
    archivePath = argv[1]; fault = argv[3];
    IMAGE_FILE_SOURCE source;
    int error = OpenImageFileSource(argv[1], &source);
    if (error != 0) return 3;
    if (lseek(source.fd, 17, SEEK_SET) != 17) abort();
    const quint64 maximumFile = argc >= 5 ? QString::fromLocal8Bit(argv[4]).toULongLong() : quint64(INT64_MAX);
    const quint64 maximumTotal = argc >= 6 ? QString::fromLocal8Bit(argv[5]).toULongLong() : quint64(INT64_MAX);
    int root = -1;
    if (strcmp(argv[2], "-") != 0) {
        root = open(argv[2], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (root < 0) abort();
    }
    active = true;
    rufus::ZipExtractResult result;
    error = root < 0 ? rufus::CheckZipSupplement(source, QString::fromLocal8Bit(argv[1]), maximumFile, maximumTotal, result)
        : rufus::ExtractZipSupplement(source, QString::fromLocal8Bit(argv[1]), root, maximumFile, maximumTotal, result);
    active = false;
    const bool positionKept = lseek(source.fd, 0, SEEK_CUR) == 17;
    CloseImageFileSource(&source);
    if (root >= 0 && close(root) != 0 && error == 0) error = errno;
    const QJsonObject report = {{"error", error}, {"syncError", result.syncError}, {"files", double(result.files)},
        {"directories", double(result.directories)}, {"bytes", QString::number(result.bytes)},
        {"path", result.path}, {"positionKept", positionKept}};
    const QByteArray text = QJsonDocument(report).toJson(QJsonDocument::Compact) + '\n';
    if (fwrite(text.constData(), 1, size_t(text.size()), stdout) != size_t(text.size())) return 4;
    return 0;
}
