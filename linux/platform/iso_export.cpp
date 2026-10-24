// SPDX-License-Identifier: GPL-3.0-or-later
#include "iso_export.h"
#include "rufus_reporting.h"
#include "iso_file.h"
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QMap>
#include <QtEndian>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSet>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace rufus {
namespace {
constexpr int bufferBytes = 65536;
constexpr int maximumEntries = 262144;
constexpr int maximumDepth = 128;
constexpr quint64 maximumTreeBytes = quint64(8) << 40;
struct Entry { bool directory = false; quint64 size = 0; QByteArray hash; };
struct Tree {
    QMap<QByteArray, Entry> entries;
    quint64 total = 0, done = 0, names = 0, budget = 0;
    dev_t forbiddenDevice = 0; ino_t forbiddenInode = 0;
    ImageSaveProgress progress = nullptr;
    void* context = nullptr;
    ImageSavePhase phase = ImageSavePhase::Copy;
    void report() { if (progress) progress(context, phase, done, qMax(quint64(1), budget)); }
};
bool sameFile(const struct stat& a, const struct stat& b)
{
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_mode == b.st_mode && a.st_size == b.st_size
        && a.st_mtim.tv_sec == b.st_mtim.tv_sec && a.st_mtim.tv_nsec == b.st_mtim.tv_nsec
        && a.st_ctim.tv_sec == b.st_ctim.tv_sec && a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
}
int scanTree(int fd, const QByteArray& parent, int depth, Tree& tree, bool contents)
{
    if (depth > maximumDepth || parent.size() > 4096) return ENAMETOOLONG;
    struct stat before, after;
    if (fstat(fd, &before) != 0) return errno;
    const int duplicate = fcntl(fd, F_DUPFD_CLOEXEC, 3);
    if (duplicate < 0) return errno;
    DIR* directory = fdopendir(duplicate);
    if (!directory) { const int error = errno; close(duplicate); return error; }
    rewinddir(directory);
    int error = 0;
    for (;;) {
        if (IsRufusJobCancelled()) { error = ECANCELED; break; }
        errno = 0;
        dirent* entry = readdir(directory);
        if (!entry) { error = errno; break; }
        const QByteArray name(entry->d_name);
        if (name == "." || name == "..") continue;
        const QByteArray path = parent + '/' + name;
        if (QString::fromUtf8(name).toUtf8() != name) { error = EILSEQ; break; }
        tree.names += quint64(path.size());
        if (tree.entries.size() >= maximumEntries || tree.names > 64 * 1024 * 1024) { error = E2BIG; break; }
        struct stat info;
        if (fstatat(fd, name.constData(), &info, AT_SYMLINK_NOFOLLOW) != 0) { error = errno; break; }
        if (info.st_dev != before.st_dev) { error = EXDEV; break; }
        if (info.st_dev == tree.forbiddenDevice && info.st_ino == tree.forbiddenInode) { error = EBUSY; break; }
        const bool isDirectory = S_ISDIR(info.st_mode);
        if (!isDirectory && !S_ISREG(info.st_mode)) { error = ENOTSUP; break; }
        const int child = openat(fd, name.constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | (isDirectory ? O_DIRECTORY : 0));
        if (child < 0) { error = errno; break; }
        struct stat retained;
        if (fstat(child, &retained) != 0) error = errno;
        else if (!sameFile(info, retained)) error = ESTALE;
        Entry value{isDirectory, isDirectory ? 0 : quint64(info.st_size), {}};
        if (error == 0 && value.size > maximumTreeBytes - tree.total) error = EFBIG;
        if (error == 0) tree.total += value.size;
        if (error == 0 && isDirectory) error = scanTree(child, path, depth + 1, tree, contents);
        if (error == 0 && !isDirectory && contents) {
            QCryptographicHash hash(QCryptographicHash::Sha256);
            QByteArray buffer(bufferBytes, '\0');
            quint64 offset = 0;
            while (offset < value.size && error == 0) {
                if (IsRufusJobCancelled()) { error = ECANCELED; break; }
                const ssize_t count = pread(child, buffer.data(), size_t(qMin(quint64(bufferBytes), value.size - offset)), off_t(offset));
                if (count < 0 && errno == EINTR) continue;
                if (count <= 0) { error = count == 0 ? EIO : errno; break; }
                hash.addData(buffer.constData(), int(count));
                offset += quint64(count); tree.done += quint64(count); tree.report();
            }
            value.hash = hash.result();
        }
        if (error == 0 && fstat(child, &retained) != 0) error = errno;
        if (error == 0 && !sameFile(info, retained)) error = ESTALE;
        if (close(child) != 0 && error == 0) error = errno;
        if (error != 0) break;
        tree.entries.insert(path, value);
    }
    if (closedir(directory) != 0 && error == 0) error = errno;
    if (error == 0 && fstat(fd, &after) != 0) error = errno;
    if (error == 0 && !sameFile(before, after)) error = ESTALE;
    return error;
}
QByteArray manifestHash(const Tree& tree)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (auto item = tree.entries.cbegin(); item != tree.entries.cend(); ++item) {
        hash.addData(QByteArray::number(item.key().size()) + ':' + item.key() + ':'
            + (item->directory ? 'd' : 'f') + ':' + QByteArray::number(item->size) + ':');
        hash.addData(item->hash);
    }
    return hash.result();
}
int fileBytes(int fd, char* bytes, size_t size, quint64 offset, bool writing)
{
    while (size != 0) {
        if (IsRufusJobCancelled()) return ECANCELED;
        const ssize_t count = writing ? pwrite(fd, bytes, size, off_t(offset)) : pread(fd, bytes, size, off_t(offset));
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return count == 0 ? EIO : errno;
        size -= size_t(count); offset += quint64(count); bytes += count;
    }
    return 0;
}
int udfOnly(int fd)
{
    // mkisofs shares payload extents between its ISO and UDF trees. Remove
    // ISO recognition descriptors and move only the UDF VRS to sector 16.
    // All UDF anchor/partition/file addresses remain unchanged.
    QByteArray recognition(32 * 2048, '\0');
    const int error = fileBytes(fd, recognition.data(), size_t(recognition.size()), 16 * 2048, false);
    if (error != 0) return error;
    QByteArray udf;
    int end = -1;
    for (int i = 0; i < 32; i++) {
        const QByteArray id = recognition.mid(i * 2048 + 1, 5);
        if (id == "CD001" && udf.isEmpty()) continue;
        const QByteArray expected = udf.isEmpty() ? QByteArray("BEA01") : udf.size() == 2048 ? QByteArray("NSR02") : QByteArray("TEA01");
        if (id != expected) return EBADMSG;
        udf.append(recognition.constData() + i * 2048, 2048);
        if (id == "TEA01") { end = i; break; }
    }
    if (end < 2) return EBADMSG;
    recognition = QByteArray((end + 1) * 2048, '\0');
    memcpy(recognition.data(), udf.constData(), size_t(udf.size()));
    return fileBytes(fd, recognition.data(), size_t(recognition.size()), 16 * 2048, true);
}
int verifyDirectory(udf_t* image, udf_dirent_t* directory, const QByteArray& parent, int depth,
    const Tree& expected, Tree& actual, QSet<quint64>& visited)
{
    if (!directory) return EBADMSG;
    if (depth > maximumDepth) { udf_dirent_free(directory); return ELOOP; }
    while (directory) {
        const quint64 remaining = directory->dir_left;
        directory = udf_readdir(directory);
        if (!directory) return remaining == 0 ? 0 : EBADMSG;
        if (directory->b_parent) continue;
        if (IsRufusJobCancelled()) { udf_dirent_free(directory); return ECANCELED; }
        const QByteArray name(udf_get_filename(directory));
        const QByteArray path = parent + '/' + name;
        const auto expectedEntry = expected.entries.constFind(path);
        if (name.isEmpty() || name.contains('/') || expectedEntry == expected.entries.cend() || actual.entries.contains(path)) {
            udf_dirent_free(directory); return EBADMSG;
        }
        const bool isDirectory = udf_is_dir(directory);
        Entry value{isDirectory, isDirectory ? 0 : udf_get_file_length(directory), {}};
        int error = 0;
        if (isDirectory != expectedEntry->directory || value.size != expectedEntry->size) error = EBADMSG;
        if (error == 0 && isDirectory) {
            const quint64 location = quint64(directory->i_part_start) + qFromLittleEndian(directory->fid->icb.loc.lba);
            if (visited.contains(location)) error = ELOOP;
            else { visited.insert(location); error = verifyDirectory(image, udf_opendir(directory), path, depth + 1, expected, actual, visited); }
        }
        if (error == 0 && !isDirectory) {
            ISO_FILE_READER reader = {};
            if (OpenIsoFileReader(&reader, nullptr, image, path.constData()) != ISO_SCAN_OK) error = EBADMSG;
            QCryptographicHash hash(QCryptographicHash::Sha256);
            QByteArray buffer(bufferBytes, '\0');
            for (quint64 offset = 0; error == 0 && offset < value.size; offset += bufferBytes) {
                const size_t count = size_t(qMin(quint64(bufferBytes), value.size - offset));
                const int status = ReadIsoFileRange(&reader, offset, buffer.data(), count);
                if (status != ISO_SCAN_OK) error = IsRufusJobCancelled() ? ECANCELED : EBADMSG;
                else { hash.addData(buffer.constData(), int(count)); actual.done += count; actual.report(); }
            }
            CloseIsoFileReader(&reader);
            value.hash = hash.result();
            if (error == 0 && value.hash != expectedEntry->hash) error = EBADMSG;
        }
        if (error != 0) { udf_dirent_free(directory); return error; }
        actual.entries.insert(path, value);
    }
    return 0;
}
}
int GetImageSaveFormat(const QString& path)
{
    return QFileInfo(path).suffix().compare(QLatin1String("iso"), Qt::CaseInsensitive) == 0 ? ImageSaveIso
        : GetImageCompressionType(QFile::encodeName(path).constData());
}
int ExportIsoImage(int directory, int output, const QString& outputPath, const QString& label,
    const QString& encoder, ImageSaveProgress progress, void* context, ImageSaveResult& result)
{
    result = {};
    struct stat source, destination;
    if (fstat(directory, &source) != 0 || fstat(output, &destination) != 0) return errno;
    if (!S_ISDIR(source.st_mode) || !S_ISREG(destination.st_mode) || destination.st_size != 0) return EINVAL;
    if (!QDir::isAbsolutePath(encoder) || !QFileInfo(encoder).isExecutable()) return ENOENT;
    Tree plan;
    plan.forbiddenDevice = destination.st_dev; plan.forbiddenInode = destination.st_ino;
    int error = scanTree(directory, {}, 0, plan, false);
    Tree original;
    original.progress = progress; original.context = context; original.budget = plan.total;
    original.forbiddenDevice = destination.st_dev; original.forbiddenInode = destination.st_ino;
    if (error == 0) {
        original.report();
        error = scanTree(directory, {}, 0, original, true);
    }
    if (error == 0 && original.entries.size() != plan.entries.size()) error = ESTALE;
    if (error != 0) return error;
    QProcess encoderProcess;
    // The directory can belong to the helper's private mount namespace. Keep
    // a reachable cwd for tools using getcwd(), and traverse only the retained
    // descriptor through procfs. The final /. identifies a directory, not a
    // symlink entry for the encoder to skip.
    encoderProcess.setWorkingDirectory(QStringLiteral("/"));
    const QString sourcePath = QStringLiteral("/proc/%1/fd/%2/.").arg(getpid()).arg(directory);
    encoderProcess.setStandardOutputFile(QStringLiteral("/proc/self/fd/%1").arg(output));
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("LC_ALL"), QStringLiteral("C.UTF-8"));
    environment.insert(QStringLiteral("HOME"), QStringLiteral("/nonexistent"));
    environment.remove(QStringLiteral("MKISOFSRC"));
    encoderProcess.setProcessEnvironment(environment);
    encoderProcess.start(encoder, {QStringLiteral("-quiet"), QStringLiteral("-D"), QStringLiteral("-udf"), QStringLiteral("-iso-level"), QStringLiteral("3"),
        QStringLiteral("-input-charset"), QStringLiteral("utf-8"), QStringLiteral("-V"), label.left(32), sourcePath});
    if (!encoderProcess.waitForStarted(5000)) return EIO;
    bool stopping = false;
    while (encoderProcess.state() != QProcess::NotRunning && !encoderProcess.waitForFinished(50)) {
        // Drain stderr while the child runs; never retain an unbounded log.
        const QByteArray message = encoderProcess.readAllStandardError();
        if (!message.isEmpty()) uprintfs(message.left(4096).constData());
        if (IsRufusJobCancelled() && !stopping) { encoderProcess.terminate(); stopping = true; }
        if (stopping && !encoderProcess.waitForFinished(1000)) encoderProcess.kill();
    }
    const QByteArray message = encoderProcess.readAllStandardError();
    if (!message.isEmpty()) uprintfs(message.left(4096).constData());
    if (IsRufusJobCancelled()) return ECANCELED;
    if (encoderProcess.exitStatus() != QProcess::NormalExit || encoderProcess.exitCode() != 0) return EIO;
    error = udfOnly(output);
    if (error == 0 && fsync(output) != 0) error = errno;
    struct stat named;
    if (error == 0 && stat(QFile::encodeName(outputPath).constData(), &named) != 0) error = errno;
    if (error == 0 && (named.st_dev != destination.st_dev || named.st_ino != destination.st_ino)) error = ESTALE;
    udf_t* image = error == 0 ? udf_open(QFile::encodeName(outputPath).constData()) : nullptr;
    if (error == 0 && !image) error = EBADMSG;
    Tree verified;
    verified.budget = original.total; verified.progress = progress; verified.context = context; verified.phase = ImageSavePhase::VerifyImage;
    verified.report();
    QSet<quint64> visited;
    if (error == 0) error = verifyDirectory(image, udf_get_root(image, true, 0), {}, 0, original, verified, visited);
    char volume[512] = {};
    if (error == 0 && (udf_get_logical_volume_id(image, volume, sizeof(volume)) <= 0 || QString::fromUtf8(volume) != label.left(32))) error = EBADMSG;
    if (image && !udf_close(image) && error == 0) error = EIO;
    if (error == 0 && manifestHash(verified) != manifestHash(original)) error = EBADMSG;
    Tree fresh;
    fresh.budget = original.total;
    fresh.progress = progress; fresh.context = context; fresh.phase = ImageSavePhase::VerifySource;
    if (error == 0) error = scanTree(directory, {}, 0, fresh, true);
    if (error == 0 && manifestHash(fresh) != manifestHash(original)) error = ESTALE;
    if (error == 0 && IsRufusJobCancelled()) error = ECANCELED;
    if (error == 0) { result.bytes = original.total; result.sha256 = manifestHash(original); }
    result.error = error;
    return error;
}
}
