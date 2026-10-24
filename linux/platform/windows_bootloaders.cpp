// SPDX-License-Identifier: GPL-3.0-or-later
#include "windows_bootloaders.h"
#include "ntfs_attributes.h"
#include "rufus_reporting.h"
#include "image_file.h"
#include "target_io.h"
#include "pe_arch.h"

#include <QDir>
#include <QBuffer>
#include <QFile>
#include <QList>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QtEndian>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/xattr.h>
#include <sys/ioctl.h>
#include <linux/magic.h>
#include <linux/msdos_fs.h>
#include <unistd.h>
#include <limits>

namespace rufus
{
namespace
{
constexpr int directoryFlags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;
constexpr size_t copyBufferBytes = 64 * 1024;

int protectBootFile(int root, const char* path, quint32 flags, bool optional, dev_t device, bool ntfs)
{
    const QList<QByteArray> parts = QByteArray(path).split('/');
    int parent = fcntl(root, F_DUPFD_CLOEXEC, 0);
    if (parent < 0) return errno;
    int error = 0;
    for (qsizetype i = 0; i + 1 < parts.size(); ++i)
    {
        const int child = openat(parent, parts[i].constData(), directoryFlags);
        if (child < 0) error = errno;
        if (close(parent) != 0 && error == 0) error = errno;
        parent = child;
        if (error != 0) break;
    }
    const int file = error == 0 ? openat(parent, parts.last().constData(),
        O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK) : -1;
    if (error == 0 && file < 0) error = errno;
    const bool missingOptional = optional && error == ENOENT;
    struct stat info = {};
    if (error == 0 && fstat(file, &info) != 0) error = errno;
    const bool ordinary = S_ISREG(info.st_mode) || S_ISDIR(info.st_mode);
    if (error == 0 && (!ordinary || info.st_dev != device)) error = ESTALE;
    constexpr char ntfsAttributes[] = "system.ntfs_attrib";
    quint32 attributes = 0, encoded = 0;
    if (error == 0 && ntfs)
    {
        const ssize_t read = fgetxattr(file, ntfsAttributes, &encoded, sizeof(encoded));
        attributes = qFromLittleEndian(encoded);
        if (read < 0) error = errno;
        else if (read != sizeof(attributes)) {
            uprintf("Boot attributes for %s have %lld bytes; expected %zu.", path, (long long)read, sizeof(attributes));
            error = EBADMSG;
        }
    }
    else if (error == 0 && ioctl(file, FAT_IOCTL_GET_ATTRIBUTES, &attributes) != 0) error = errno;
    constexpr quint32 normalAttribute = 0x80; // NORMAL excludes all other DOS flags.
    // A freshly created NTFS directory can initially omit DIRECTORY in its
    // exposed DOS flags. The driver restores that structural bit on set;
    // request the actual inode type explicitly and verify the complete result.
    constexpr quint32 directoryAttribute = 0x10;
    const quint32 structural = S_ISDIR(info.st_mode) ? directoryAttribute : 0;
    const quint32 expected = (attributes | flags | structural) & ~normalAttribute;
    if (error == 0 && IsRufusJobCancelled()) error = ECANCELED;
    if (error == 0 && ntfs)
    {
        const quint32 encodedExpected = qToLittleEndian(expected);
        if (fsetxattr(file, ntfsAttributes, &encodedExpected, sizeof(encodedExpected), 0) != 0) error = errno;
        if (error == 0)
        {
            const ssize_t read = fgetxattr(file, ntfsAttributes, &encoded, sizeof(encoded));
            attributes = qFromLittleEndian(encoded);
            if (read < 0) error = errno;
            else if (read != sizeof(attributes)) {
                uprintf("Boot attribute readback for %s has %lld bytes; expected %zu.", path, (long long)read, sizeof(attributes));
                error = EBADMSG;
            }
        }
    }
    else if (error == 0)
    {
        if (ioctl(file, FAT_IOCTL_SET_ATTRIBUTES, &expected) != 0) error = errno;
        if (error == 0 && ioctl(file, FAT_IOCTL_GET_ATTRIBUTES, &attributes) != 0) error = errno;
    }
    if (error == 0 && attributes != expected) {
        uprintf("Boot attribute readback for %s is 0x%08x; expected 0x%08x.", path, attributes, expected);
        error = EBADMSG;
    }
    if (error == 0 && fsync(file) != 0) error = errno;
    if (missingOptional) error = 0;
    if (file >= 0 && close(file) != 0 && error == 0) error = errno;
    if (parent >= 0 && close(parent) != 0 && error == 0) error = errno;
    return error;
}

struct BootFile
{
    QByteArray path;
    QByteArray name;
};

int collectBootFile(const struct wimlib_dir_entry* entry, void* context)
{
    if (IsRufusJobCancelled())
        return WIMLIB_ERR_ABORTED_BY_PROGRESS;
    const unsigned unsupported = WIMLIB_FILE_ATTRIBUTE_DEVICE |
        WIMLIB_FILE_ATTRIBUTE_REPARSE_POINT | WIMLIB_FILE_ATTRIBUTE_ENCRYPTED;
    if (entry->attributes & unsupported)
        return WIMLIB_ERR_NOT_A_REGULAR_FILE;
    // Rufus lists only the immediate font files, not subdirectories.
    if (entry->attributes & WIMLIB_FILE_ATTRIBUTE_DIRECTORY)
        return 0;
    const QByteArray name(entry->filename);
    const bool basename = !name.isEmpty() && name != "." && name != ".." &&
        !name.contains('/') && !name.contains('\\');
    if (!basename || entry->full_path == nullptr)
        return WIMLIB_ERR_INVALID_PARAM;
    static_cast<QList<BootFile>*>(context)->append({entry->full_path, name});
    return 0;
}

int openBootDirectory(int root, const QByteArray& path, bool create = false)
{
    int parent = fcntl(root, F_DUPFD_CLOEXEC, 0);
    if (parent < 0)
        return -1;
    for (const QByteArray& part : path.split('/'))
    {
        if (create && mkdirat(parent, part.constData(), 0755) != 0 && errno != EEXIST)
        {
            const int error = errno;
            close(parent);
            errno = error;
            return -1;
        }
        const int child = openat(parent, part.constData(), directoryFlags);
        const int error = child < 0 ? errno : 0;
        close(parent);
        if (error != 0)
        {
            errno = error;
            return -1;
        }
        parent = child;
    }
    return parent;
}

int replaceBootFile(QIODevice& input, int parent, const QByteArray& name)
{
    struct stat info;
    if (fstatat(parent, name.constData(), &info, AT_SYMLINK_NOFOLLOW) == 0)
    {
        if (!S_ISREG(info.st_mode))
            return ENOTSUP;
        if (info.st_nlink != 1)
            return EMLINK;
    }
    else if (errno != ENOENT)
        return errno;
    int error = 0;
    const QByteArray target = "/proc/self/fd/" + QByteArray::number(parent) + '/' + name;
    QSaveFile output(QFile::decodeName(target));
    output.setDirectWriteFallback(false);
    bool restoreCompression = false;
    error = PrepareNtfsUncompressedCreation(parent, &restoreCompression);
    if (error == 0 && !output.open(QIODevice::WriteOnly))
        error = EIO;
    if (restoreCompression) {
        const int restored = SetNtfsDirectoryCompression(parent, true);
        if (error == 0) error = restored;
    }
    char buffer[copyBufferBytes];
    while (error == 0)
    {
        if (IsRufusJobCancelled())
        {
            error = ECANCELED;
            break;
        }
        const qint64 count = input.read(buffer, sizeof(buffer));
        if (count <= 0)
        {
            error = count == 0 ? 0 : EIO;
            break;
        }
        for (ssize_t done = 0; done < count;)
        {
            if (IsRufusJobCancelled())
            {
                error = ECANCELED;
                break;
            }
            const ssize_t written = write(output.handle(), buffer + done, size_t(count - done));
            if (written < 0 && errno == EINTR)
                continue;
            if (written <= 0)
            {
                error = written == 0 ? EIO : errno;
                break;
            }
            done += written;
        }
    }
    // Qt owns the temporary file and atomic rename. Check fsync ourselves:
    // commit() does not report every native synchronization failure.
    if (error == 0 && fsync(output.handle()) != 0)
        error = errno;
    if (error == 0 && IsRufusJobCancelled())
        error = ECANCELED;
    if (error == 0 && !output.commit())
        error = EIO;
    return error;
}

int replaceBootFile(const QByteArray& source, int parent, const QByteArray& name)
{
    const int fd = open(source.constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0)
        return errno;
    struct stat info;
    int error = fstat(fd, &info) == 0 ? 0 : errno;
    if (error == 0 && !S_ISREG(info.st_mode))
        error = ENOTSUP;
    QFile input;
    if (error == 0 && !input.open(fd, QIODevice::ReadOnly, QFileDevice::DontCloseHandle))
        error = EIO;
    if (error == 0)
        error = replaceBootFile(input, parent, name);
    input.close();
    if (close(fd) != 0 && error == 0)
        error = errno;
    return error;
}

int extractAndReplace(WIMStruct* wim, int image, const BootFile& file,
    const QByteArray& temporary, int parent, const QByteArray& name)
{
    if (IsRufusJobCancelled())
        return ECANCELED;
    const char* path = file.path.constData();
    const int flags = WIMLIB_EXTRACT_FLAG_NO_PRESERVE_DIR_STRUCTURE |
        WIMLIB_EXTRACT_FLAG_NO_ACLS | WIMLIB_EXTRACT_FLAG_NO_ATTRIBUTES | WIMLIB_EXTRACT_FLAG_NORPFIX;
    const int result = wimlib_extract_paths(wim, image, temporary.constData(), &path, 1, flags);
    if (result != 0)
    {
        uprintf("Could not extract CA 2023 file '%s': %s", path,
            wimlib_get_error_string(static_cast<wimlib_error_code>(result)));
        return result == WIMLIB_ERR_ABORTED_BY_PROGRESS ? ECANCELED : EIO;
    }
    const int error = replaceBootFile(temporary + '/' + file.name, parent, name);
    if (error != 0)
        uprintf("Could not replace CA 2023 file '%s': %s", name.constData(), strerror(error));
    return error;
}

int replaceLoader(WIMStruct* wim, int image, const char* path,
    const QByteArray& temporary, int parent, const QByteArray& name)
{
    QList<BootFile> files;
    int result = wimlib_iterate_dir_tree(wim, image, path, 0, collectBootFile, &files);
    if (result == 0 && files.size() != 1)
        result = WIMLIB_ERR_NOT_A_REGULAR_FILE;
    if (result != 0)
    {
        uprintf("Could not locate CA 2023 file '%s': %s", path,
            wimlib_get_error_string(static_cast<wimlib_error_code>(result)));
        return result == WIMLIB_ERR_ABORTED_BY_PROGRESS ? ECANCELED : EIO;
    }
    return extractAndReplace(wim, image, files.first(), temporary, parent, name);
}

int bootName(int directory, const QByteArray& requested, QByteArray& name)
{
    name = requested;
    const int descriptor = openat(directory, ".", directoryFlags);
    if (descriptor < 0) return errno;
    DIR* entries = fdopendir(descriptor);
    if (entries == nullptr) { const int error = errno; close(descriptor); return error; }
    const QString folded = QString::fromUtf8(requested);
    int error = 0;
    bool found = false;
    errno = 0;
    while (dirent* entry = readdir(entries))
    {
        if (QString::compare(QString::fromUtf8(entry->d_name), folded, Qt::CaseInsensitive) != 0) continue;
        if (found) { error = EEXIST; break; }
        name = entry->d_name;
        found = true;
    }
    if (error == 0 && errno != 0) error = errno;
    if (closedir(entries) != 0 && error == 0) error = errno;
    return error;
}

int bootDirectory(int root, const QByteArray& path, bool create)
{
    int parent = openat(root, ".", directoryFlags);
    if (parent < 0) return -1;
    for (const QByteArray& part : path.split('/'))
    {
        QByteArray name;
        int error = bootName(parent, part, name);
        if (error == 0 && create && mkdirat(parent, name.constData(), 0755) != 0 && errno != EEXIST)
            error = errno;
        const int child = error == 0 ? openat(parent, name.constData(), directoryFlags) : -1;
        if (child < 0 && error == 0) error = errno;
        if (close(parent) != 0 && error == 0) error = errno;
        if (error != 0)
        {
            if (child >= 0) close(child);
            errno = error;
            return -1;
        }
        parent = child;
    }
    return parent;
}

int copyBootRegular(int source, const QByteArray& sourceName, int destination,
    const QByteArray& destinationName, WindowsBootFilesResult& result)
{
    const int descriptor = openat(source, sourceName.constData(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (descriptor < 0) return errno;
    struct stat before, after;
    int error = fstat(descriptor, &before) == 0 ? 0 : errno;
    if (error == 0 && (!S_ISREG(before.st_mode) || before.st_size < 0)) error = ENOTSUP;
    if (error == 0 && quint64(before.st_size) > std::numeric_limits<quint64>::max() - result.bytes) error = EFBIG;
    QByteArray name;
    if (error == 0) error = bootName(destination, destinationName, name);
    QFile input;
    if (error == 0 && !input.open(descriptor, QIODevice::ReadOnly, QFileDevice::DontCloseHandle)) error = EIO;
    if (error == 0) error = replaceBootFile(input, destination, name);
    input.close();
    if (error == 0 && fstat(descriptor, &after) != 0) error = errno;
    if (error == 0)
    {
        const bool size = before.st_size == after.st_size;
        const bool modified = before.st_mtim.tv_sec == after.st_mtim.tv_sec && before.st_mtim.tv_nsec == after.st_mtim.tv_nsec;
        const bool changed = before.st_ctim.tv_sec == after.st_ctim.tv_sec && before.st_ctim.tv_nsec == after.st_ctim.tv_nsec;
        if (!size || !modified || !changed) error = ESTALE;
    }
    if (close(descriptor) != 0 && error == 0) error = errno;
    if (error == 0) { ++result.files; result.bytes += quint64(before.st_size); }
    return error;
}

int copyBootTree(int source, int destination, bool exNames, int depth,
    const QByteArray& prefix, WindowsBootFilesResult& result)
{
    constexpr int maximumBootDepth = 128;
    if (depth > maximumBootDepth) return ELOOP;
    struct stat parent;
    if (fstat(source, &parent) != 0) return errno;
    const int descriptor = openat(source, ".", directoryFlags);
    if (descriptor < 0) return errno;
    DIR* entries = fdopendir(descriptor);
    if (entries == nullptr) { const int error = errno; close(descriptor); return error; }
    int error = 0;
    while (error == 0)
    {
        errno = 0;
        dirent* entry = readdir(entries);
        if (entry == nullptr) { error = errno; break; }
        const QByteArray name(entry->d_name);
        if (name == "." || name == "..") continue;
        if (IsRufusJobCancelled()) { error = ECANCELED; break; }
        const QByteArray path = prefix + '/' + name;
        result.path = QString::fromUtf8(path);
        struct stat info;
        if (fstatat(source, name.constData(), &info, AT_SYMLINK_NOFOLLOW) != 0) { error = errno; break; }
        if (info.st_dev != parent.st_dev) { error = EXDEV; break; }
        QByteArray outputName;
        const QByteArray mapped = exNames ? QByteArray(name).replace("_EX", "") : name;
        error = bootName(destination, mapped, outputName);
        if (error != 0) break;
        if (S_ISDIR(info.st_mode))
        {
            const int input = openat(source, name.constData(), directoryFlags);
            if (input < 0) { error = errno; break; }
            if (mkdirat(destination, outputName.constData(), 0755) != 0 && errno != EEXIST) error = errno;
            const int output = error == 0 ? openat(destination, outputName.constData(), directoryFlags) : -1;
            if (output < 0 && error == 0) error = errno;
            if (error == 0) error = copyBootTree(input, output, exNames, depth + 1, path, result);
            if (output >= 0 && close(output) != 0 && error == 0) error = errno;
            if (close(input) != 0 && error == 0) error = errno;
        }
        else if (S_ISREG(info.st_mode)) error = copyBootRegular(source, name, destination, outputName, result);
        else error = ENOTSUP;
    }
    if (closedir(entries) != 0 && error == 0) error = errno;
    if (error == 0 && fsync(destination) != 0) error = errno;
    return error;
}

int copyBootBranch(int source, const QByteArray& branch, int stage, const QByteArray& path,
    bool optional, bool exNames, WindowsBootFilesResult& result)
{
    const int input = bootDirectory(source, branch, false);
    if (input < 0) return optional && errno == ENOENT ? 0 : errno;
    const int output = bootDirectory(stage, path, true);
    int error = output < 0 ? errno : copyBootTree(input, output, exNames, 0, path, result);
    if (output >= 0 && close(output) != 0 && error == 0) error = errno;
    if (close(input) != 0 && error == 0) error = errno;
    return error;
}

int writeBootBcd(int stage, const QByteArray& path, const QByteArray& bytes, WindowsBootFilesResult& result)
{
    const int directory = bootDirectory(stage, path, true);
    if (directory < 0) return errno;
    QByteArray name;
    int error = bootName(directory, "BCD", name);
    QBuffer input;
    input.setData(bytes);
    if (error == 0) error = input.open(QIODevice::ReadOnly) ? replaceBootFile(input, directory, name) : EIO;
    if (error == 0 && fsync(directory) != 0) error = errno;
    if (close(directory) != 0 && error == 0) error = errno;
    if (error == 0) { ++result.files; result.bytes += quint64(bytes.size()); }
    return error;
}

int stageBiosRoot(int stage, const char* requested, const char* output, bool optional)
{
    const int boot = bootDirectory(stage, "Boot", false);
    if (boot < 0) return errno;
    QByteArray name;
    int error = bootName(boot, requested, name);
    if (error == 0 && renameat(boot, name.constData(), stage, output) != 0)
        error = optional && errno == ENOENT ? 0 : errno;
    if (close(boot) != 0 && error == 0) error = errno;
    return error;
}

int stageEfiEntry(int stage, int architecture, WindowsBootFilesResult& result)
{
    result.path = QStringLiteral("EFI/Microsoft/Boot/bootmgfw.efi");
    const int source = bootDirectory(stage, "EFI/Microsoft/Boot", false);
    if (source < 0) return errno;
    QByteArray manager;
    int error = bootName(source, "bootmgfw.efi", manager);
    const int descriptor = error == 0 ? openat(source, manager.constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK) : -1;
    if (descriptor < 0 && error == 0) error = errno;
    QFile input;
    struct stat info;
    if (error == 0 && fstat(descriptor, &info) != 0) error = errno;
    constexpr qint64 maximumEfiBytes = 128 * 1024 * 1024;
    if (error == 0 && (!S_ISREG(info.st_mode) || info.st_size < 0 || info.st_size > maximumEfiBytes)) error = EFBIG;
    if (error == 0 && !input.open(descriptor, QIODevice::ReadOnly, QFileDevice::DontCloseHandle)) error = EIO;
    if (error == 0)
    {
        const QByteArray bytes = input.readAll();
        if (input.error() != QFileDevice::NoError) error = EIO;
        else if (bytes.size() != info.st_size || FindPeArch(reinterpret_cast<const uint8_t*>(bytes.constData()), bytes.size()) != architecture)
            error = EBADMSG;
    }
    input.close();
    if (descriptor >= 0 && close(descriptor) != 0 && error == 0) error = errno;
    const int destination = error == 0 ? bootDirectory(stage, "EFI/Boot", true) : -1;
    if (destination < 0 && error == 0) error = errno;
    if (error == 0)
        error = copyBootRegular(source, manager, destination, QByteArray("boot") + efi_archname[architecture] + ".efi", result);
    if (destination >= 0 && close(destination) != 0 && error == 0) error = errno;
    if (close(source) != 0 && error == 0) error = errno;
    return error;
}

int copyBootFiles(WIMStruct* wim, int image, int root, const QByteArray& temporary,
    const QList<BootFile>& fonts, QStringList* modified)
{
    int error = 0;
    int replaced = 0;
    if (!fonts.isEmpty())
    {
        const int parent = openBootDirectory(root, "efi/microsoft/boot/fonts");
        if (parent < 0)
            return errno;
        for (const BootFile& font : fonts)
        {
            const QByteArray name = QByteArray(font.name).replace("_EX", "");
            const bool basename = !name.isEmpty() && name != "." && name != "..";
            error = basename ? extractAndReplace(wim, image, font, temporary, parent, name) : EINVAL;
            if (error != 0)
                break;
            if (modified != nullptr) modified->append(QStringLiteral("efi/microsoft/boot/fonts/") + QString::fromUtf8(name));
            ++replaced;
        }
        close(parent);
        if (error != 0)
            return error;
    }
    const int boot = openBootDirectory(root, "efi/boot");
    if (boot < 0 && errno != ENOENT)
        return errno;
    if (boot >= 0)
    {
        for (int arch = 1; arch < ARCH_MAX; ++arch)
        {
            const QByteArray name = QByteArray("boot") + efi_archname[arch] + ".efi";
            struct stat info;
            if (fstatat(boot, name.constData(), &info, AT_SYMLINK_NOFOLLOW) != 0)
            {
                if (errno == ENOENT)
                    continue;
                error = errno;
                break;
            }
            error = replaceLoader(wim, image, "Windows/Boot/EFI_EX/bootmgfw_EX.efi", temporary, boot, name);
            if (error == 0) {
                if (modified != nullptr) modified->append(QStringLiteral("efi/boot/") + QString::fromLatin1(name));
                ++replaced;
            }
            break; // Same first-existing architecture rule as Win32 Rufus.
        }
        close(boot);
    }
    if (error == 0)
        error = replaceLoader(wim, image, "Windows/Boot/EFI_EX/bootmgr_EX.efi", temporary, root, "bootmgr.efi");
    if (error == 0)
    {
        if (modified != nullptr) modified->append(QStringLiteral("bootmgr.efi"));
        uprintf("Replaced %d EFI bootloader files with 'Windows UEFI CA 2023' compatible versions.", replaced + 1);
        uprintf("The target PC must have the 'Windows UEFI CA 2023' Secure Boot certificate installed.");
    }
    return error;
}
}

int InstallWindowsEfiRepair(const QByteArray& payload, const char* directory)
{
    if (payload.isEmpty() || directory == nullptr)
        return EINVAL;
    if (IsRufusJobCancelled())
        return ECANCELED;
    const int root = open(directory, directoryFlags);
    if (root < 0)
        return errno;
    const int boot = openBootDirectory(root, "EFI/BOOT", true);
    int error = boot < 0 ? errno : 0;
    if (error == 0)
    {
        QBuffer input;
        input.setData(payload);
        error = input.open(QIODevice::ReadOnly) ? replaceBootFile(input, boot, "BOOTX64.EFI") : EIO;
        if (close(boot) != 0 && error == 0)
            error = errno;
    }
    if (close(root) != 0 && error == 0)
        error = errno;
    return error;
}

int ApplyWindowsCa2023Bootloaders(WIMStruct* wim, int image, const char* directory, QStringList* modified)
{
    QList<BootFile> fonts;
    const char* const fontPath = "Windows/Boot/Fonts_EX";
    int result = wimlib_iterate_dir_tree(wim, image, fontPath, 0, collectBootFile, &fonts);
    if (result == 0 && !fonts.isEmpty())
        result = WIMLIB_ERR_INVALID_PARAM;
    if (result == 0)
        result = wimlib_iterate_dir_tree(wim, image, fontPath,
            WIMLIB_ITERATE_DIR_TREE_FLAG_CHILDREN, collectBootFile, &fonts);
    if (result != 0)
    {
        uprintf("Could not locate CA 2023 payloads: %s", wimlib_get_error_string(static_cast<wimlib_error_code>(result)));
        return result == WIMLIB_ERR_ABORTED_BY_PROGRESS ? ECANCELED : EIO;
    }
    QTemporaryDir temporary(QDir::tempPath() + QStringLiteral("/rufus-windows-XXXXXX"));
    if (!temporary.isValid())
        return EIO;
    const int root = open(directory, directoryFlags);
    if (root < 0)
        return errno;
    int error = copyBootFiles(wim, image, root, QFile::encodeName(temporary.path()), fonts, modified);
    if (close(root) != 0 && error == 0)
        error = errno;
    return error;
}

int StageWindowsToGoBootFiles(int windowsDirectory, int stageDirectory,
    const WindowsToGoBootOptions& options, WindowsBootFilesResult& result)
{
    result = {};
    if (!options.bios && !options.uefi) return EINVAL;
    if (options.uefi && (options.architecture <= ARCH_UNKNOWN || options.architecture >= ARCH_MAX)) return EINVAL;
    if (IsRufusJobCancelled()) return ECANCELED;
    int error = CheckEmptyTargetDirectory(stageDirectory);
    if (error != 0) return error;
    const int config = bootDirectory(windowsDirectory, "Windows/System32/config", false);
    if (config < 0) return errno;
    QByteArray name;
    error = bootName(config, "BCD-Template", name);
    const int source = error == 0 ? openat(config, name.constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK) : -1;
    if (source < 0 && error == 0) error = errno;
    WindowsBcdResult bios, efi;
    WindowsBcdOptions bcd = options.bcd;
    if (error == 0 && options.bios) { bcd.uefi = false; error = PrepareWindowsToGoBcd(source, bcd, bios); }
    if (error == 0 && options.uefi) { bcd.uefi = true; error = PrepareWindowsToGoBcd(source, bcd, efi); }
    if (source >= 0 && close(source) != 0 && error == 0) error = errno;
    if (close(config) != 0 && error == 0) error = errno;
    if (error != 0) return error;
    const QByteArray fonts = options.ca2023 ? "Windows/Boot/Fonts_EX" : "Windows/Boot/Fonts";
    if (options.bios)
    {
        // CA 2023 changes EFI loaders, not the BIOS PCAT set.
        error = copyBootBranch(windowsDirectory, "Windows/Boot/PCAT", stageDirectory, "Boot", false, false, result);
        if (error == 0) error = copyBootBranch(windowsDirectory, "Windows/Boot/Fonts", stageDirectory, "Boot/Fonts", false, false, result);
        if (error == 0) error = copyBootBranch(windowsDirectory, "Windows/Boot/Resources", stageDirectory, "Boot/Resources", true, false, result);
        if (error == 0) error = stageBiosRoot(stageDirectory, "bootmgr", "bootmgr", false);
        if (error == 0) error = stageBiosRoot(stageDirectory, "bootnxt", "BOOTNXT", true);
        if (error == 0) error = writeBootBcd(stageDirectory, "Boot", bios.bytes, result);
    }
    if (error == 0 && options.uefi)
    {
        const QByteArray efiSource = options.ca2023 ? "Windows/Boot/EFI_EX" : "Windows/Boot/EFI";
        error = copyBootBranch(windowsDirectory, efiSource, stageDirectory, "EFI/Microsoft/Boot", false, options.ca2023, result);
        if (error == 0) error = copyBootBranch(windowsDirectory, fonts, stageDirectory, "EFI/Microsoft/Boot/Fonts", false, options.ca2023, result);
        if (error == 0) error = copyBootBranch(windowsDirectory, "Windows/Boot/Resources", stageDirectory, "EFI/Microsoft/Boot/Resources", true, false, result);
        if (error == 0) error = stageEfiEntry(stageDirectory, options.architecture, result);
        if (error == 0) error = writeBootBcd(stageDirectory, "EFI/Microsoft/Boot", efi.bytes, result);
    }
    if (error == 0 && fsync(stageDirectory) != 0) error = errno;
    if (error == 0) result.path.clear();
    return error;
}

int InstallWindowsToGoBootFiles(int stageDirectory, int destination, WindowsBootFilesResult& result)
{
    result = {};
    struct stat source, target;
    if (fstat(stageDirectory, &source) != 0 || fstat(destination, &target) != 0) return errno;
    if (!S_ISDIR(source.st_mode) || !S_ISDIR(target.st_mode)) return ENOTDIR;
    if (source.st_dev == target.st_dev && source.st_ino == target.st_ino) return EINVAL;
    const int error = copyBootTree(stageDirectory, destination, false, 0, {}, result);
    if (error == 0) result.path.clear();
    return error;
}

int SetWindowsToGoBootAttributes(int directory, bool bios, bool uefi)
{
    struct stat root;
    struct statfs filesystem;
    if (fstat(directory, &root) != 0 || fstatfs(directory, &filesystem) != 0) return errno;
    if (!S_ISDIR(root.st_mode)) return ENOTDIR;
    constexpr long ntfsMagic = 0x5346544e;
    const bool ntfs = filesystem.f_type == ntfsMagic || filesystem.f_type == FUSE_SUPER_MAGIC;
    if (!ntfs && filesystem.f_type != MSDOS_SUPER_MAGIC) return ENOTSUP;
    constexpr quint32 readOnly = 1, hiddenSystem = 2 | 4;
    int error = 0;
    if (bios)
    {
        error = protectBootFile(directory, "bootmgr", hiddenSystem | readOnly, false, root.st_dev, ntfs);
        if (error == 0) error = protectBootFile(directory, "BOOTNXT", hiddenSystem | readOnly, true, root.st_dev, ntfs);
        if (error == 0) error = protectBootFile(directory, "Boot", hiddenSystem, false, root.st_dev, ntfs);
        if (error == 0) error = protectBootFile(directory, "Boot/BCD", hiddenSystem, false, root.st_dev, ntfs);
    }
    if (error == 0 && uefi)
        error = protectBootFile(directory, "EFI/Microsoft/Boot/BCD", hiddenSystem, false, root.st_dev, ntfs);
    return error;
}
}
