// SPDX-License-Identifier: GPL-3.0-or-later
#include "msdos_image.h"
#include "rufus_reporting.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QtEndian>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace rufus
{
namespace
{
constexpr qsizetype sectorBytes = 512, rootOffset = 19 * sectorBytes, rootEntries = 224, entryBytes = 32;
static_assert(DISKCOPY_IMAGE_OFFSET + DISKCOPY_IMAGE_SIZE <= DISKCOPY_SIZE);
constexpr quint64 dataClusterBias = 31; // Original FAT12_CLUSTER_OFFSET.
int validate(const QByteArray& image)
{
    if (image.size() != DISKCOPY_SIZE) return EBADMSG;
    return QCryptographicHash::hash(image,QCryptographicHash::Sha256).toHex() == DISKCOPY_HASH ? 0 : EBADMSG;
}
}
int ReadMsDosFiles(const QByteArray& image, QVector<MsDosFile>& files)
{
    files.clear();
    const int error = validate(image); if (error != 0) return error;
    const auto* disk = reinterpret_cast<const unsigned char*>(image.constData() + DISKCOPY_IMAGE_OFFSET);
    QVector<MsDosFile> found;
    for (size_t index = 0; const auto* selected = GetMsDosFile(index); ++index) {
        const unsigned char* entry = nullptr;
        for (qsizetype row = 0; row < rootEntries; ++row) {
            const auto* candidate = disk + rootOffset + row * entryBytes;
            if (memcmp(candidate,selected->fat_name,11) != 0) continue;
            if (entry != nullptr) return EBADMSG;
            entry = candidate;
        }
        if (entry == nullptr) return ENOENT;
        const quint16 cluster = qFromLittleEndian<quint16>(entry + 26);
        const quint32 size = qFromLittleEndian<quint32>(entry + 28);
        const quint64 offset = (quint64(cluster) + dataClusterBias) * sectorBytes;
        const bool inBounds = cluster >= 2 && offset <= DISKCOPY_IMAGE_SIZE && size <= DISKCOPY_IMAGE_SIZE-offset;
        const bool regular = (entry[11] & 0x18) == 0 && size != 0;
        if (!inBounds || !regular) return EBADMSG;
        MsDosFile file;
        file.entry = selected; file.attributes = entry[11];
        file.accessDate = qFromLittleEndian<quint16>(entry + 18);
        file.writeTime = qFromLittleEndian<quint16>(entry + 22);
        file.writeDate = qFromLittleEndian<quint16>(entry + 24);
        file.bytes = QByteArray(reinterpret_cast<const char*>(disk + offset),qsizetype(size));
        if (!PatchMsDosFile(selected->name,reinterpret_cast<uint8_t*>(file.bytes.data()),size)) return EBADMSG;
        found.append(std::move(file));
    }
    files = std::move(found); return 0;
}
int ReadMsDosImage(const QString& path, uid_t owner, QByteArray& bytes)
{
    bytes.clear();
    if (!QDir::isAbsolutePath(path) || path.contains(QChar::Null)) return EINVAL;
    const int fd = open(QFile::encodeName(path).constData(),O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    if (fd < 0) return errno;
    struct stat before = {}, after = {};
    int error = fstat(fd,&before) == 0 ? 0 : errno;
    const bool ownedFile = S_ISREG(before.st_mode) && before.st_uid == owner && before.st_nlink == 1;
    if (error == 0 && !ownedFile) error = ENOTSUP;
    if (error == 0 && before.st_size != DISKCOPY_SIZE) error = EBADMSG;
    if (error == 0) bytes.resize(DISKCOPY_SIZE);
    qsizetype done = 0;
    while (error == 0 && done < bytes.size()) {
        if (IsRufusJobCancelled()) { error = ECANCELED; break; }
        const ssize_t count = read(fd,bytes.data()+done,size_t(bytes.size()-done));
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) error = count < 0 ? errno : EIO;
        else done += count;
    }
    if (error == 0 && fstat(fd,&after) != 0) error = errno;
    const bool changed = before.st_size != after.st_size || before.st_mtim.tv_sec != after.st_mtim.tv_sec
        || before.st_mtim.tv_nsec != after.st_mtim.tv_nsec || before.st_ctim.tv_sec != after.st_ctim.tv_sec
        || before.st_ctim.tv_nsec != after.st_ctim.tv_nsec;
    if (error == 0 && changed) error = ESTALE;
    if (close(fd) != 0 && error == 0) error = errno;
    if (error == 0) error = validate(bytes);
    if (error != 0) bytes.clear();
    return error;
}
int StoreMsDosImage(const QString& directory, const QByteArray& bytes)
{
    if (!QDir::isAbsolutePath(directory) || directory.contains(QChar::Null)) return EINVAL;
    const int error = validate(bytes); if (error != 0) return error;
    if (!QDir().mkpath(directory)) return EIO;
    const QString path = QDir(directory).filePath(QStringLiteral("diskcopy.dll"));
    struct stat info = {};
    if (lstat(QFile::encodeName(path).constData(),&info) == 0) {
        const bool owned = S_ISREG(info.st_mode) && info.st_uid == getuid() && info.st_nlink == 1;
        if (!owned) return ENOTSUP;
    } else if (errno != ENOENT) return errno;
    QSaveFile file(path); file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) return EIO;
    QByteArray checked;
    const int readError = ReadMsDosImage(path,getuid(),checked);
    return readError != 0 ? readError : checked == bytes ? 0 : ESTALE;
}
}
