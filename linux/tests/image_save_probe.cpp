// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/image_save.h"
#include "platform/compressed_ffu.h"
#include "platform/iso_export.h"
#include "platform/image_save_destination.h"
#include "platform/rufus_reporting.h"
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <unistd.h>

namespace {
QString mode, sourcePath, outputPath;
volatile std::sig_atomic_t cancelled = 0;
bool injected = false;
void cancelSignal(int) { cancelled = 1; }
int input = -1, output = -1, directorySyncs = 0;
void progress(void*, rufus::ImageSavePhase phase, quint64 done, quint64)
{
    const bool copy = phase == rufus::ImageSavePhase::Copy && done > 0;
    const bool verify = phase == rufus::ImageSavePhase::VerifyImage && done == 0;
    const bool source = phase == rufus::ImageSavePhase::VerifySource && done == 0;
    if ((mode == "cancel-copy" && copy) || (mode == "cancel-verify" && verify)) cancelled = true;
    if (injected) return;
    if ((mode == "mutate" && source) || (mode == "corrupt" && verify)) {
        const bool mutate = mode == "mutate";
        const int fd = mutate ? open(QFile::encodeName(sourcePath).constData(), O_WRONLY) : output;
        const char value = char(0xee);
        if (fd < 0 || pwrite(fd, &value, 1, 1024) != 1) qFatal("fault mutation");
        if (mutate) close(fd);
        injected = true;
    }
    if (mode == "destination-race" && copy) {
        QFile file(outputPath);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write("RACED") != 5) qFatal("destination race");
        injected = true;
    }
}
}
extern "C" int IsRufusJobCancelled() { return cancelled; }
extern "C" int IsIsoScanCancelled() { return cancelled; }
extern "C" void uprintfs(const char* text) { fprintf(stderr, "%s\n", text); }
extern "C" ssize_t __real_pread64(int, void*, size_t, off64_t);
extern "C" ssize_t __real_pwrite64(int, const void*, size_t, off64_t);
extern "C" int __real_fsync(int);
extern "C" ssize_t __wrap_pread64(int fd, void* bytes, size_t count, off64_t offset)
{
    if (fd == input && mode == "read-fail") { errno = EIO; return -1; }
    if (fd == input && mode == "short" && !injected) { injected = true; errno = EINTR; return -1; }
    return __real_pread64(fd, bytes, mode == "short" ? qMin(count, size_t(1237)) : count, offset);
}
extern "C" ssize_t __wrap_pwrite64(int fd, const void* bytes, size_t count, off64_t offset)
{
    if (fd == output && mode == "write-fail") { errno = ENOSPC; return -1; }
    return __real_pwrite64(fd, bytes, mode == "short" ? qMin(count, size_t(1879)) : count, offset);
}
extern "C" int __wrap_fsync(int fd)
{
    struct stat info;
    if (fstat(fd, &info) == 0 && S_ISDIR(info.st_mode)) {
        directorySyncs++;
        if ((mode == "directory-sync-fail" && directorySyncs == 1) || (mode == "final-sync-fail" && directorySyncs == 2)) {
            errno = EIO; return -1;
        }
    }
    if (fd == output && mode == "sync-fail") { errno = EIO; return -1; }
    return __real_fsync(fd);
}
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    std::signal(SIGUSR1, cancelSignal);
    if ((argc == 4 || argc == 5) && QString::fromLocal8Bit(argv[1]) == QLatin1String("--ffu-plan")) {
        FFU_CAPTURE_GEOMETRY geometry = {};
        const int error = PlanFfuCapture(QString::fromLocal8Bit(argv[2]).toULongLong(),
            QString::fromLocal8Bit(argv[3]).toULongLong(),
            argc == 5 ? QString::fromLocal8Bit(argv[4]).toULongLong() : FFU_MAX_MANIFEST_BYTES, &geometry);
        QTextStream(stdout) << QJsonDocument(QJsonObject{{"error", error}, {"block", qint64(geometry.block_bytes)},
            {"blocks", qint64(geometry.blocks)}, {"image", qint64(geometry.image_bytes)},
            {"metadata", qint64(geometry.metadata_bytes)}, {"hashes", qint64(geometry.hash_bytes)},
            {"security", qint64(geometry.security_bytes)}, {"bytes", qint64(geometry.file_bytes)}}).toJson(QJsonDocument::Compact) << '\n';
        return 0;
    }
    if (argc != 6 && argc != 7) return 2;
    mode = QString::fromLocal8Bit(argv[1]); sourcePath = QString::fromLocal8Bit(argv[2]); outputPath = QString::fromLocal8Bit(argv[3]);
    const unsigned sector = QString::fromLocal8Bit(argv[4]).toUInt();
    quint64 size = QString::fromLocal8Bit(argv[5]).toULongLong();
    const int format = rufus::GetImageSaveFormat(outputPath);
    rufus::ImageSaveDestination destination(outputPath);
    rufus::ImageSaveResult result;
    int error = destination.open();
    if (error == 0) error = destination.create(format);
    output = destination.descriptor();
    input = open(QFile::encodeName(sourcePath).constData(), (mode == "source-rw" ? O_RDWR : O_RDONLY) | O_CLOEXEC | (format == rufus::ImageSaveIso && sector != 2048 ? O_DIRECTORY : 0));
    if (input < 0 && error == 0) error = errno;
    if (error == 0 && mode == "nonempty") { const char byte = 0; if (write(output, &byte, 1) != 1) return 3; }
    if (error == 0 && format == rufus::ImageSaveIso && sector != 2048) error = argc != 7 ? EINVAL
        : rufus::ExportIsoImage(input, output, destination.temporaryPath(), QStringLiteral("RUFUS EXPORT"), QString::fromLocal8Bit(argv[6]), progress, nullptr, result);
    else if (error == 0) error = rufus::SaveDeviceImage(input, size, sector, output, destination.temporaryPath(), format, progress, nullptr, result);
    if (input >= 0 && close(input) != 0 && error == 0) error = errno;
    if (error == 0) error = destination.commit();
    result.committed = destination.committed();
    const int cleanup = destination.close();
    if (cleanup != 0 && (error == 0 || error == ECANCELED)) error = cleanup;
    QTextStream(stdout) << QJsonDocument(QJsonObject{{"error", error}, {"committed", result.committed},
        {"bytes", qint64(result.bytes)}, {"sha256", QString::fromLatin1(result.sha256.toHex())}}).toJson(QJsonDocument::Compact) << '\n';
    return 0;
}
