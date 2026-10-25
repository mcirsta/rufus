// SPDX-License-Identifier: GPL-3.0-or-later
#include "secure_boot_cache.h"
#include "rufus_reporting.h"
#include "../../res/dbx/dbx_info.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QUrlQuery>
#include <QTimeZone>
#include <cstring>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace rufus
{
namespace
{
constexpr qsizetype maximumDbxBytes = 16 * 1024 * 1024, maximumMetadataBytes = 24 * 1024 * 1024;
const char* names[] = {"ia32", "x64", "arm", "aa64", "ia64", "riscv64", "loongarch64"};
int cachePath(const QString& directory, int architecture, QString& path)
{
    if (!QDir::isAbsolutePath(directory) || directory.contains(QChar::Null) || architecture < 1 || architecture > 7) return EINVAL;
    path = QDir(directory).filePath(QStringLiteral("dbx_%1.json").arg(QString::fromLatin1(names[architecture-1]))); return 0;
}
int readFile(const QString& path, qsizetype limit, QByteArray& bytes)
{
    bytes.clear();
    const int descriptor = open(QFile::encodeName(path).constData(), O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW);
    if (descriptor < 0) return errno;
    struct stat before = {}, after = {};
    int error = fstat(descriptor, &before) == 0 ? 0 : errno;
    const bool regular = S_ISREG(before.st_mode) && before.st_uid == getuid() && before.st_nlink == 1;
    if (error == 0 && !regular) error = ENOTSUP;
    if (error == 0 && (before.st_size <= 0 || before.st_size > limit)) error = EFBIG;
    if (error == 0) bytes.resize(qsizetype(before.st_size));
    qsizetype done = 0;
    while (error == 0 && done < bytes.size()) {
        const ssize_t count = read(descriptor, bytes.data() + done, size_t(bytes.size() - done));
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) error = count < 0 ? errno : EIO;
        else done += count;
    }
    if (error == 0 && fstat(descriptor, &after) != 0) error = errno;
    const bool changed = before.st_size != after.st_size || before.st_mtim.tv_sec != after.st_mtim.tv_sec
        || before.st_mtim.tv_nsec != after.st_mtim.tv_nsec || before.st_ctim.tv_sec != after.st_ctim.tv_sec
        || before.st_ctim.tv_nsec != after.st_ctim.tv_nsec;
    if (error == 0 && changed) error = ESTALE;
    if (close(descriptor) != 0 && error == 0) error = errno;
    if (error != 0) bytes.clear();
    return error;
}
int writeFile(const QString& path, const QByteArray& bytes)
{
    struct stat info = {};
    if (lstat(QFile::encodeName(path).constData(), &info) == 0) {
        if (!S_ISREG(info.st_mode) || info.st_nlink != 1 || info.st_uid != getuid()) return ENOTSUP;
    } else if (errno != ENOENT) return errno;
    QSaveFile file(path); file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) return EIO;
    QByteArray check; const int error = readFile(path, bytes.size(), check);
    return error != 0 ? error : check == bytes ? 0 : EIO;
}
}
QList<DbxSource> BundledDbxSources()
{
    QList<DbxSource> result;
    for (int index = 0; index < 7; ++index) result.append({index + 1, QUrl(QString::fromLatin1(dbx_info[index].url)), dbx_info[index].timestamp});
    return result;
}
QUrl DbxCommitUrl(const QUrl& content)
{
    QUrl url = content; const QString path = content.path();
    const qsizetype marker = path.indexOf(QStringLiteral("/contents/"));
    if (marker < 0 || content.hasQuery() || content.hasFragment() || !content.userInfo().isEmpty()) return {};
    url.setPath(path.left(marker) + QStringLiteral("/commits"));
    QUrlQuery query; query.addQueryItem(QStringLiteral("path"), path.mid(marker + 10));
    query.addQueryItem(QStringLiteral("page"), QStringLiteral("1")); query.addQueryItem(QStringLiteral("per_page"), QStringLiteral("1"));
    url.setQuery(query); return url;
}
quint64 DbxCommitTimestamp(const QByteArray& bytes)
{
    if (bytes.isEmpty() || bytes.size() > 1024 * 1024) return 0;
    const auto array = QJsonDocument::fromJson(bytes).array(); if (array.isEmpty()) return 0;
    const QString text = array.first().toObject().value(QStringLiteral("commit")).toObject()
        .value(QStringLiteral("author")).toObject().value(QStringLiteral("date")).toString();
    QDateTime date = QDateTime::fromString(text, Qt::ISODate);
    date.setTimeZone(QTimeZone(QByteArrayLiteral("UTC")));
    if (!date.isValid() || date.toString(QStringLiteral("yyyy-MM-dd'T'HH:mm:ss'Z'")) != text || date.toSecsSinceEpoch() <= 0) return 0;
    return quint64(date.toSecsSinceEpoch());
}
int ReadSecureBootDbxCache(const QString& directory, int architecture, QByteArray& bytes, quint64& timestamp)
{
    bytes.clear(); timestamp = 0; QString path;
    int error = cachePath(directory, architecture, path); if (error != 0) return error;
    QByteArray metadata; error = readFile(path, maximumMetadataBytes, metadata);
    if (error != 0) return error;
    const auto object = QJsonDocument::fromJson(metadata).object(); bool number = false;
    const quint64 time = object.value(QStringLiteral("timestamp")).toString().toULongLong(&number);
    const QByteArray hash = object.value(QStringLiteral("sha256")).toString().toLatin1();
    if (object.size() != 4 || object.value(QStringLiteral("architecture")).toInt() != architecture || !number || time == 0 || hash.size() != 64) return EBADMSG;
    const QByteArray encoded = object.value(QStringLiteral("data")).toString().toLatin1();
    if (encoded.size() > ((maximumDbxBytes + 2) / 3) * 4) return EFBIG;
    bytes = QByteArray::fromBase64(encoded, QByteArray::AbortOnBase64DecodingErrors);
    if (QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex() != hash) error = EBADMSG;
    if (error == 0) error = ValidateSecureBootDbx(bytes);
    if (error != 0) bytes.clear(); else timestamp = time;
    return error;
}
int StoreSecureBootDbxCache(const QString& directory, int architecture, const QByteArray& bytes, quint64 timestamp)
{
    if (timestamp == 0) return EINVAL;
    int error = ValidateSecureBootDbx(bytes); if (error != 0) return error;
    QString path; error = cachePath(directory, architecture, path); if (error != 0) return error;
    QByteArray old; quint64 oldTime = 0;
    if (ReadSecureBootDbxCache(directory, architecture, old, oldTime) == 0 && oldTime >= timestamp) return EALREADY;
    if (!QDir().mkpath(directory)) return EIO;
    const QJsonObject metadata{{QStringLiteral("architecture"),architecture}, {QStringLiteral("timestamp"),QString::number(timestamp)},
        {QStringLiteral("sha256"),QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex())},
        {QStringLiteral("data"),QString::fromLatin1(bytes.toBase64())}};
    return writeFile(path, QJsonDocument(metadata).toJson(QJsonDocument::Compact));
}
int LoadCachedSecureBootPolicy(const QString& directory, SecureBootPolicy& policy)
{
    int error = LoadBundledSecureBootPolicy(policy); if (error != 0) return error;
    for (const auto& source : BundledDbxSources()) {
        QByteArray bytes; quint64 timestamp = 0;
        error = ReadSecureBootDbxCache(directory, source.architecture, bytes, timestamp);
        if (error == 0 && timestamp > source.timestamp) policy.dbx.insert(source.architecture, bytes);
        else if (error != 0 && error != ENOENT) uprintf("Ignoring unusable DBX cache for architecture %d: %s", source.architecture, strerror(error));
    }
    return 0;
}
}
