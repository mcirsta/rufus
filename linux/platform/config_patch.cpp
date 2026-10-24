// SPDX-License-Identifier: GPL-3.0-or-later
#include "config_patch.h"
#include "config_text.h"

#include <QByteArray>
#include <QStringConverter>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

extern "C" int IsIsoScanCancelled(void);

namespace
{
constexpr qsizetype maxConfigBytes = CONFIG_PATCH_MAX_BYTES;
constexpr qsizetype ioBytes = 64 * 1024;
constexpr auto decodeFlags = QStringConverter::Flag::Stateless | QStringConverter::Flag::ConvertInitialBom;

int decodeText(QByteArrayView bytes, QStringConverter::Encoding encoding, QString& text)
{
    QStringDecoder decoder(encoding, decodeFlags);
    text = decoder(bytes);
    if (decoder.hasError())
        return EILSEQ;
    if (!QStringView(text).isValidUtf16())
        return EILSEQ;
    const bool binaryMarker = text.contains(QChar(0)) || text.contains(QChar(0x1a));
    return binaryMarker ? ENOTSUP : 0;
}

int replaceText(QString& text, const QString& token, const QString& source, const QString& replacement,
    QString& output, bool& matched)
{
    wchar_t line[CONFIG_TEXT_LINE_CAPACITY], wideToken[CONFIG_TEXT_LINE_CAPACITY], wideSource[CONFIG_TEXT_LINE_CAPACITY];
    size_t offsets[CONFIG_TEXT_MAX_REPLACEMENTS + 1];
    // A longer token/source cannot fit Rufus's input chunk.
    if (token.size() >= CONFIG_TEXT_LINE_CAPACITY || source.size() >= CONFIG_TEXT_LINE_CAPACITY)
        return 0;
    for (qsizetype i = 0; i < token.size(); ++i)
        wideToken[i] = token.at(i).unicode();
    wideToken[token.size()] = 0;
    for (qsizetype i = 0; i < source.size(); ++i)
        wideSource[i] = source.at(i).unicode();
    wideSource[source.size()] = 0;
    // Windows text input collapses CRLF before fgetws applies its size limit.
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    for (qsizetype start = 0; start < text.size();)
    {
        if (IsIsoScanCancelled())
            return ECANCELED;
        qsizetype count = 0;
        const qsizetype available = qMin(qsizetype(CONFIG_TEXT_LINE_CAPACITY - 1), text.size() - start);
        while (count < available)
        {
            const QChar character = text.at(start + count);
            line[count++] = character.unicode();
            if (character == QLatin1Char('\n'))
                break;
        }
        line[count] = 0;
        const size_t matches = SplitConfigToken(line, wideToken, wideSource, offsets);
        for (size_t i = 0; i <= matches; ++i)
        {
            const qsizetype length = qsizetype(wcslen(line + offsets[i]));
            const qsizetype added = length + (i < matches ? replacement.size() : 0);
            if (added > maxConfigBytes - output.size())
                return EFBIG;
            output.append(text.mid(start + qsizetype(offsets[i]), length));
            if (i < matches)
                output.append(replacement);
        }
        matched = matched || matches != 0;
        start += count;
    }
    return 0;
}

int decodeConfig(const QByteArray& bytes, QStringConverter::Encoding& encoding, qsizetype& bomBytes,
    QString& text)
{
    const char utf8Bom[] = "\xef\xbb\xbf", utf16Bom[] = "\xff\xfe";
    encoding = QStringConverter::Utf8;
    bomBytes = 0;
    const bool utf32le = bytes.size() >= 4 && memcmp(bytes.constData(), "\xff\xfe\0\0", 4) == 0;
    const bool utf32be = bytes.size() >= 4 && memcmp(bytes.constData(), "\0\0\xfe\xff", 4) == 0;
    if (utf32le || utf32be || bytes.startsWith("\xfe\xff"))
        return ENOTSUP;
    if (bytes.startsWith(utf16Bom))
    {
        encoding = QStringConverter::Utf16LE;
        bomBytes = sizeof(utf16Bom) - 1;
    }
    else if (bytes.startsWith(utf8Bom))
        bomBytes = sizeof(utf8Bom) - 1;
    if (encoding == QStringConverter::Utf16LE)
    {
        const bool completeUnits = (bytes.size() - bomBytes) % qsizetype(sizeof(char16_t)) == 0;
        if (!completeUnits)
            return EILSEQ;
    }
    return decodeText(QByteArrayView(bytes).sliced(bomBytes), encoding, text);
}

int transformConfig(const QByteArray& bytes, const char* token, const char* source, const char* replacement,
    QByteArray& output, bool& matched)
{
    QStringConverter::Encoding encoding;
    qsizetype bomBytes = 0;
    QString text, wideToken, wideSource, wideReplacement, changed;
    int error = decodeConfig(bytes, encoding, bomBytes, text);
    if (error == 0)
        error = decodeText(QByteArrayView(token), QStringConverter::Utf8, wideToken);
    if (error == 0)
        error = decodeText(QByteArrayView(source), QStringConverter::Utf8, wideSource);
    if (error == 0)
        error = decodeText(QByteArrayView(replacement), QStringConverter::Utf8, wideReplacement);
    if (error == 0)
        error = replaceText(text, wideToken, wideSource, wideReplacement, changed, matched);
    if (error != 0 || !matched)
        return error;
    changed.remove(QChar('\r'));
    QStringEncoder encoder(encoding, QStringConverter::Flag::Stateless);
    output = bytes.left(bomBytes);
    output += encoder(changed);
    if (encoder.hasError())
        return EILSEQ;
    return output.size() <= maxConfigBytes ? 0 : EFBIG;
}

int transformPersistence(QByteArray& bytes, bool grub, bool& modified)
{
    const CONFIG_TEXT_PATCH patches[] = {
        {grub ? "linux" : "append", "file=/cdrom/preseed", "persistent file=/cdrom/preseed"},
        {grub ? "linux" : "append", "boot=casper", "boot=casper persistent"},
        {"linux", "/casper/vmlinuz", "/casper/vmlinuz persistent"},
        {"kernel", "/casper/vmlinuz", "/casper/vmlinuz persistent"},
        {grub ? "linux" : "append", "boot=live", "boot=live persistence"}
    };
    for (size_t i = 0; i < sizeof(patches) / sizeof(patches[0]); i++) {
        const CONFIG_TEXT_PATCH& patch = patches[i];
        QByteArray output; bool matched = false;
        int error = transformConfig(bytes, patch.token, patch.source, patch.replacement, output, matched);
        if (error != 0) return error;
        if (!matched) continue;
        bytes.swap(output); modified = true;
        if (i == 0 && grub) {
            error = transformConfig(bytes, "linux", "maybe-ubiquity", "", output, matched);
            if (error != 0) return error;
            if (matched) bytes.swap(output);
        }
        break;
    }
    return 0;
}

int transformPatches(QByteArray& bytes, const CONFIG_TEXT_PATCH* patches, size_t count, bool& modified,
    bool persistence = false, bool grub = false)
{
    if (persistence) {
        const int error = transformPersistence(bytes, grub, modified);
        if (error != 0) return error;
    }
    for (size_t i = 0; i < count; ++i)
    {
        const CONFIG_TEXT_PATCH& patch = patches[i];
        if (patch.source[0] == 0 || strcmp(patch.source, patch.replacement) == 0)
            continue;
        QByteArray output;
        bool matched = false;
        const int error = transformConfig(bytes, patch.token, patch.source, patch.replacement, output, matched);
        if (error != 0)
            return error;
        if (matched)
            bytes.swap(output);
        modified = modified || matched;
    }
    return 0;
}

int checkPatches(const CONFIG_TEXT_PATCH* patches, size_t count, bool& active)
{
    if (patches == nullptr && count != 0)
        return EINVAL;
    for (size_t i = 0; i < count; ++i)
    {
        const CONFIG_TEXT_PATCH& patch = patches[i];
        const bool missing = patch.token == nullptr || patch.source == nullptr || patch.replacement == nullptr;
        if (missing || patch.token[0] == 0)
            return EINVAL;
        const bool changesText = patch.source[0] != 0 && strcmp(patch.source, patch.replacement) != 0;
        active = active || changesText;
    }
    return 0;
}

int checkConfigFile(int parent, const char* name, int input, const struct stat& original)
{
    struct stat current, path;
    if (fstat(input, &current) != 0 || fstatat(parent, name, &path, AT_SYMLINK_NOFOLLOW) != 0)
        return errno;
    const bool samePath = path.st_dev == current.st_dev && path.st_ino == current.st_ino;
    const bool sameModified = current.st_mtim.tv_sec == original.st_mtim.tv_sec
        && current.st_mtim.tv_nsec == original.st_mtim.tv_nsec;
    const bool sameChanged = current.st_ctim.tv_sec == original.st_ctim.tv_sec
        && current.st_ctim.tv_nsec == original.st_ctim.tv_nsec;
    if (!samePath || !sameModified || !sameChanged || current.st_size != original.st_size)
        return ESTALE;
    return IsIsoScanCancelled() ? ECANCELED : 0;
}

int readConfig(int input, const struct stat& info, QByteArray& bytes)
{
    if (!S_ISREG(info.st_mode))
        return ENOTSUP;
    if (info.st_nlink != 1)
        return EMLINK;
    if (info.st_size < 0 || info.st_size > maxConfigBytes)
        return EFBIG;
    bytes.resize(qsizetype(info.st_size));
    qsizetype done = 0;
    while (done < bytes.size())
    {
        if (IsIsoScanCancelled())
            return ECANCELED;
        const size_t size = size_t(qMin(ioBytes, bytes.size() - done));
        const ssize_t count = pread(input, bytes.data() + done, size, done);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return count == 0 ? EIO : errno;
        done += count;
    }
    return 0;
}

int writeConfig(int output, const QByteArray& bytes)
{
    qsizetype done = 0;
    while (done < bytes.size())
    {
        if (IsIsoScanCancelled())
            return ECANCELED;
        const size_t size = size_t(qMin(ioBytes, bytes.size() - done));
        const ssize_t count = write(output, bytes.constData() + done, size);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return count == 0 ? EIO : errno;
        done += count;
    }
    while (fsync(output) != 0)
    {
        if (errno != EINTR)
            return errno;
        if (IsIsoScanCancelled())
            return ECANCELED;
    }
    return 0;
}

int storeConfig(int parent, const char* name, int input, const struct stat& info,
    const QByteArray& bytes, bool* modified)
{
    char temporary[64];
    snprintf(temporary, sizeof(temporary), "/proc/self/fd/%d/.rufus-config-XXXXXX", parent);
    const int output = mkostemp(temporary, O_CLOEXEC);
    if (output < 0)
        return errno;
    const char* temporaryName = strrchr(temporary, '/') + 1;
    struct stat created;
    int error = fstat(output, &created) == 0 ? 0 : errno;
    if (error == 0)
    {
        // NTFS-3G can supply fixed ownership/modes without implementing chown
        // or chmod. Preserve matching metadata without redundant changes.
        const bool sameOwner = created.st_uid == info.st_uid && created.st_gid == info.st_gid;
        const mode_t permissions = info.st_mode & 0777;
        const bool sameMode = (created.st_mode & 0777) == permissions;
        if (!sameOwner && fchown(output, info.st_uid, info.st_gid) != 0)
            error = errno;
        if (error == 0 && !sameMode && fchmod(output, permissions) != 0)
            error = errno;
    }
    if (error == 0)
        error = writeConfig(output, bytes);
    if (close(output) != 0 && error == 0)
        error = errno;
    if (error == 0)
        error = checkConfigFile(parent, name, input, info);
    if (error == 0 && renameat(parent, temporaryName, parent, name) != 0)
        error = errno;
    if (error != 0)
    {
        return unlinkat(parent, temporaryName, 0) == 0 ? error : errno;
    }
    *modified = true;
    while (fsync(parent) != 0)
    {
        if (errno != EINTR)
            return errno;
        if (IsIsoScanCancelled())
            return ECANCELED;
    }
    return 0;
}
}

int InsertConfigSectionText(const QByteArray& bytes, const char* section, const char* data, QByteArray& output)
{
    output.clear();
    if (section == nullptr || data == nullptr)
        return EINVAL;
    if (section[0] == 0 || data[0] == 0)
        return EINVAL;
    if (bytes.size() > maxConfigBytes)
        return EFBIG;
    QString text, wideSection, wideData, changed;
    QStringConverter::Encoding encoding;
    qsizetype bomBytes = 0;
    int error = decodeConfig(bytes, encoding, bomBytes, text);
    // No host ANSI code page is inferred. ASCII without BOM is unambiguous;
    // non-ASCII SIF needs an explicit Unicode BOM before this adapter can edit it.
    if (error == 0 && bomBytes == 0)
    {
        for (unsigned char character : bytes)
            if (character >= 0x80)
                return ENOTSUP;
    }
    if (error == 0)
        error = decodeText(QByteArrayView(section), QStringConverter::Utf8, wideSection);
    if (error == 0)
        error = decodeText(QByteArrayView(data), QStringConverter::Utf8, wideData);
    if (error != 0)
        return error;
    if (wideSection.size() >= CONFIG_TEXT_LINE_CAPACITY)
        return EINVAL;
    wchar_t line[CONFIG_TEXT_LINE_CAPACITY], token[CONFIG_TEXT_LINE_CAPACITY];
    for (qsizetype i = 0; i < wideSection.size(); ++i)
        token[i] = wideSection.at(i).unicode();
    token[wideSection.size()] = 0;
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    bool matched = false;
    for (qsizetype start = 0; start < text.size();)
    {
        if (IsIsoScanCancelled())
            return ECANCELED;
        qsizetype count = 0;
        const qsizetype available = qMin(qsizetype(CONFIG_TEXT_LINE_CAPACITY - 1), text.size() - start);
        while (count < available)
        {
            const QChar character = text.at(start + count);
            line[count++] = character.unicode();
            if (character == QLatin1Char('\n'))
                break;
        }
        line[count] = 0;
        changed += QStringView(text).mid(start, count);
        if (IsConfigSection(line, token))
        {
            changed += wideData;
            changed += QLatin1Char('\n');
            matched = true;
        }
        if (changed.size() > maxConfigBytes)
            return EFBIG;
        start += count;
    }
    if (!matched)
        return ENOENT;
    // insert_section_data(..., FALSE): LF expands on output, bare CR remains.
    changed.replace(QStringLiteral("\n"), QStringLiteral("\r\n"));
    QStringEncoder encoder(encoding, QStringConverter::Flag::Stateless);
    QByteArray encoded = bytes.left(bomBytes) + encoder(changed);
    if (encoder.hasError())
        return EILSEQ;
    if (encoded.size() > maxConfigBytes)
        return EFBIG;
    output.swap(encoded);
    return 0;
}

int ReplaceIsoConfigTokensAt(int parent, const char* name, const CONFIG_TEXT_PATCH* patches,
    size_t count, bool persistence, bool grub, bool* modified)
{
    if (modified == nullptr)
        return EINVAL;
    *modified = false;
    if (parent < 0 || name == nullptr)
        return EINVAL;
    const bool basename = name[0] != 0 && strchr(name, '/') == nullptr
        && strcmp(name, ".") != 0 && strcmp(name, "..") != 0;
    if (!basename)
        return EINVAL;
    bool active = false;
    int error = checkPatches(patches, count, active);
    if (error != 0 || (!active && !persistence))
        return error;
    if (IsIsoScanCancelled())
        return ECANCELED;
    const int input = openat(parent, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (input < 0)
        return errno;
    struct stat info;
    QByteArray bytes;
    bool matched = false;
    error = fstat(input, &info) == 0 ? 0 : errno;
    if (error == 0)
        error = readConfig(input, info, bytes);
    if (error == 0)
        error = transformPatches(bytes, patches, count, matched, persistence, grub);
    if (error == 0)
        error = checkConfigFile(parent, name, input, info);
    if (error == 0 && matched)
        error = storeConfig(parent, name, input, info, bytes, modified);
    if (close(input) != 0 && error == 0)
        error = errno;
    return error;
}

int ReplaceConfigTokenAt(int parent, const char* name, const char* token,
    const char* source, const char* replacement, bool* modified)
{
    if (source == nullptr || source[0] == 0)
    {
        if (modified != nullptr)
            *modified = false;
        return EINVAL;
    }
    const CONFIG_TEXT_PATCH patch = {token, source, replacement};
    return ReplaceConfigTokensAt(parent, name, &patch, 1, modified);
}

int CheckIsoConfigText(const void* data, size_t size, const CONFIG_TEXT_PATCH* patches, size_t count,
    bool persistence, bool grub, size_t* outputSize)
{
    if (outputSize == nullptr)
        return EINVAL;
    *outputSize = size;
    if (data == nullptr && size != 0)
        return EINVAL;
    bool active = false, modified = false;
    const int error = checkPatches(patches, count, active);
    if (error != 0 || (!active && !persistence))
        return error;
    if (size > CONFIG_PATCH_MAX_BYTES)
        return EFBIG;
    if (IsIsoScanCancelled())
        return ECANCELED;
    QByteArray bytes = QByteArray::fromRawData(size == 0 ? "" : static_cast<const char*>(data), qsizetype(size));
    const int transformed = transformPatches(bytes, patches, count, modified, persistence, grub);
    if (transformed == 0)
        *outputSize = size_t(bytes.size());
    return transformed;
}

int ReplaceConfigTokensAt(int parent, const char* name, const CONFIG_TEXT_PATCH* patches, size_t count, bool* modified)
{
    return ReplaceIsoConfigTokensAt(parent, name, patches, count, false, false, modified);
}

int CheckConfigText(const void* data, size_t size, const CONFIG_TEXT_PATCH* patches, size_t count, size_t* outputSize)
{
    return CheckIsoConfigText(data, size, patches, count, false, false, outputSize);
}
