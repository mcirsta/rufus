// SPDX-License-Identifier: GPL-3.0-or-later
#include "device_content.h"
#include "drive_label.h"
#include <QStringConverter>
#include <cerrno>
#include <cstring>
#include <strings.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace rufus
{
namespace
{
constexpr size_t maximumDirectoryEntries = 4096;
constexpr qsizetype lineUnits = 1023; // Original wchar_t buf[1024], including NUL.
bool sameFile(const struct stat& first, const struct stat& second)
{
    return first.st_dev == second.st_dev && first.st_ino == second.st_ino && first.st_size == second.st_size
        && first.st_mtim.tv_sec == second.st_mtim.tv_sec && first.st_mtim.tv_nsec == second.st_mtim.tv_nsec
        && first.st_ctim.tv_sec == second.st_ctim.tv_sec && first.st_ctim.tv_nsec == second.st_ctim.tv_nsec;
}
int autorunName(int directory, QByteArray& name)
{
    const int fd = openat(directory,".",O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    if (fd < 0) return errno;
    DIR* stream = fdopendir(fd);
    if (stream == nullptr) { const int error = errno; close(fd); return error; }
    int error = 0; size_t count = 0;
    while (true) {
        errno = 0; const struct dirent* entry = readdir(stream);
        if (entry == nullptr) { error = errno; break; }
        if (++count > maximumDirectoryEntries) { error = EFBIG; break; }
        if (strcasecmp(entry->d_name,"autorun.inf") != 0) continue;
        if (!name.isEmpty()) { error = EBADMSG; break; }
        name = entry->d_name;
    }
    if (closedir(stream) != 0 && error == 0) error = errno;
    return error != 0 ? error : name.isEmpty() ? ENOENT : 0;
}
}
int ParseDriveLabel(const QByteArray& bytes, QString& label)
{
    label.clear();
    if (bytes.size() > DriveLabelFileBytes) return EFBIG;
    const bool utf8 = bytes.startsWith(QByteArray::fromHex("efbbbf"));
    if (bytes.startsWith(QByteArray::fromHex("feff"))) return EBADMSG;
    const qsizetype skip = utf8 ? 3 : bytes.startsWith(QByteArray::fromHex("fffe")) ? 2 : 0;
    if (!utf8 && (bytes.size() - skip) % 2 != 0) return EBADMSG;
    QStringDecoder decoder(utf8 ? QStringDecoder::Utf8 : QStringDecoder::Utf16LE);
    const QString text = decoder(QByteArrayView(bytes).sliced(skip));
    if (decoder.hasError() || text.contains(QChar::Null) || !QStringView(text).isValidUtf16()) return EBADMSG;
    qsizetype position = 0;
    while (position < text.size()) {
        qsizetype end = text.indexOf(QChar('\n'),position);
        if (end < 0) end = text.size(); else ++end;
        end = qMin(end,position + lineUnits);
        size_t start = 0, length = 0;
        const auto* line = reinterpret_cast<const uint16_t*>(text.utf16() + position);
        if (FindDriveLabel16(line,size_t(end-position),&start,&length)) {
            const QString result = text.mid(position+qsizetype(start),qsizetype(length));
            if (!QStringView(result).isValidUtf16()) return EBADMSG;
            for (const QChar character : result)
                if (character.unicode() < 32 || character.unicode() == 127) return EBADMSG;
            label = result; return 0;
        }
        position = end;
    }
    return ENOENT;
}
int ReadMountedDriveLabel(int directory, quint64 filesystemDevice, QString& label)
{
    label.clear(); struct stat root = {};
    if (fstat(directory,&root) != 0) return errno;
    if (!S_ISDIR(root.st_mode) || quint64(root.st_dev) != filesystemDevice) return ESTALE;
    QByteArray name; int error = autorunName(directory,name); if (error != 0) return error;
    const int fd = openat(directory,name.constData(),O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    if (fd < 0) return errno;
    struct stat before = {}, after = {}, named = {};
    error = fstat(fd,&before) == 0 ? 0 : errno;
    const bool regular = S_ISREG(before.st_mode) && before.st_dev == root.st_dev;
    if (error == 0 && !regular) error = ENOTSUP;
    if (error == 0 && (before.st_size < 0 || before.st_size > DriveLabelFileBytes)) error = EFBIG;
    QByteArray bytes; if (error == 0) bytes.resize(qsizetype(before.st_size));
    qsizetype done = 0;
    while (error == 0 && done < bytes.size()) {
        const ssize_t count = read(fd,bytes.data()+done,size_t(bytes.size()-done));
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) error = count < 0 ? errno : EIO;
        else done += count;
    }
    if (error == 0 && fstat(fd,&after) != 0) error = errno;
    if (error == 0 && fstatat(directory,name.constData(),&named,AT_SYMLINK_NOFOLLOW) != 0) error = errno;
    if (error == 0 && (!sameFile(before,after) || !sameFile(after,named))) error = ESTALE;
    if (close(fd) != 0 && error == 0) error = errno;
    if (error == 0) error = ParseDriveLabel(bytes,label);
    return error;
}
}
