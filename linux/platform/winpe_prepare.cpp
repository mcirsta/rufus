// SPDX-License-Identifier: GPL-3.0-or-later
#include "winpe_prepare.h"
#include "config_patch.h"
#include "rufus_reporting.h"
#include "winpe_setup.h"
#include "image_report.h"
#include <QByteArray>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace rufus
{
namespace
{
constexpr int directoryFlags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;
constexpr qsizetype bufferBytes = 64 * 1024;
constexpr qsizetype maxBootFileBytes = CONFIG_PATCH_MAX_BYTES;
constexpr qsizetype excludedLoaderTail = 32;

struct SetupFile
{
    const char* directory;
    const char* name;
    const char* destination;
    int parent = -1;
    int input = -1;
    struct stat info = {};
    QByteArray directoryName = {}, fileName = {}, bytes = {};
};

int findName(int parent, const char* name, QByteArray& found)
{
    found.clear();
    // A new descriptor avoids changing the borrowed directory's scan position.
    const int fd = openat(parent, ".", directoryFlags);
    if (fd < 0)
        return errno;
    DIR* directory = fdopendir(fd);
    if (directory == nullptr)
    {
        const int error = errno;
        close(fd);
        return error;
    }
    int error = 0;
    while (error == 0)
    {
        if (IsRufusJobCancelled())
        {
            error = ECANCELED;
            break;
        }
        errno = 0;
        const dirent* entry = readdir(directory);
        if (entry == nullptr)
        {
            error = errno;
            break;
        }
        if (QByteArray(entry->d_name).compare(name, Qt::CaseInsensitive) != 0)
            continue;
        if (!found.isEmpty())
            error = EEXIST;
        else
            found = entry->d_name;
    }
    if (closedir(directory) != 0 && error == 0)
        error = errno;
    return error != 0 ? error : (found.isEmpty() ? ENOENT : 0);
}

int checkSource(int root, const SetupFile& file)
{
    QByteArray directoryName, fileName;
    int error = findName(root, file.directory, directoryName);
    if (error == 0)
        error = findName(file.parent, file.name, fileName);
    if (error != 0)
        return error == ENOENT ? ESTALE : error;
    if (directoryName != file.directoryName || fileName != file.fileName)
        return ESTALE;
    struct stat parent, pathParent, current, path;
    if (fstat(file.parent, &parent) != 0 ||
        fstatat(root, directoryName.constData(), &pathParent, AT_SYMLINK_NOFOLLOW) != 0)
        return errno;
    const bool sameDirectory = parent.st_dev == pathParent.st_dev && parent.st_ino == pathParent.st_ino;
    if (!sameDirectory)
        return ESTALE;
    if (fstat(file.input, &current) != 0 ||
        fstatat(file.parent, fileName.constData(), &path, AT_SYMLINK_NOFOLLOW) != 0)
        return errno;
    const bool sameFile = current.st_dev == path.st_dev && current.st_ino == path.st_ino;
    const bool sameSize = current.st_size == file.info.st_size;
    const bool sameModified = current.st_mtim.tv_sec == file.info.st_mtim.tv_sec &&
        current.st_mtim.tv_nsec == file.info.st_mtim.tv_nsec;
    const bool sameChanged = current.st_ctim.tv_sec == file.info.st_ctim.tv_sec &&
        current.st_ctim.tv_nsec == file.info.st_ctim.tv_nsec;
    if (!sameFile || !sameSize || !sameModified || !sameChanged)
        return ESTALE;
    return IsRufusJobCancelled() ? ECANCELED : 0;
}

int readSource(int root, SetupFile& file)
{
    int error = findName(root, file.directory, file.directoryName);
    if (error == 0)
    {
        file.parent = openat(root, file.directoryName.constData(), directoryFlags);
        if (file.parent < 0)
            error = errno;
    }
    if (error == 0)
        error = findName(file.parent, file.name, file.fileName);
    if (error != 0)
        return error;
    file.input = openat(file.parent, file.fileName.constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (file.input < 0)
        return errno;
    if (fstat(file.input, &file.info) != 0)
        return errno;
    if (!S_ISREG(file.info.st_mode))
        return ENOTSUP;
    if (file.info.st_nlink != 1)
        return EMLINK;
    if (file.info.st_size <= 0)
        return EBADMSG;
    if (file.info.st_size > maxBootFileBytes)
        return EFBIG;
    file.bytes.resize(qsizetype(file.info.st_size));
    for (qsizetype done = 0; done < file.bytes.size();)
    {
        if (IsRufusJobCancelled())
            return ECANCELED;
        const size_t count = size_t(qMin(bufferBytes, file.bytes.size() - done));
        const ssize_t read = pread(file.input, file.bytes.data() + done, count, done);
        if (read < 0 && errno == EINTR)
            continue;
        if (read <= 0)
            return read == 0 ? EIO : errno;
        done += read;
    }
    return checkSource(root, file);
}

int publishFile(int root, const char* temporary, const char* destination)
{
    if (renameat2(root, temporary, root, destination, RENAME_NOREPLACE) == 0)
        return 0;
    const int error = errno;
    const bool unsupported = error == EINVAL || error == ENOSYS || error == EOPNOTSUPP;
    if (!unsupported)
        return error;
    // NTFS-3G's FUSE interface can lack rename-with-flags. A same-directory
    // hard link also publishes without replacing any existing destination.
    // Remove the temporary name before returning success; never use plain rename.
    if (linkat(root, temporary, root, destination, 0) != 0)
        return errno;
    return unlinkat(root, temporary, 0) == 0 ? 0 : errno;
}

int storeFile(int root, const SetupFile& file, SetupFile files[], size_t count, SetupFile* replacement)
{
    char temporary[80];
    snprintf(temporary, sizeof(temporary), "/proc/self/fd/%d/.rufus-winpe-XXXXXX", root);
    const int output = mkostemp(temporary, O_CLOEXEC);
    if (output < 0)
        return errno;
    const char* temporaryName = strrchr(temporary, '/') + 1;
    int error = 0;
    if (replacement != nullptr)
    {
        struct stat created;
        if (fstat(output, &created) != 0)
            error = errno;
        else
        {
            // Match the config writer: fixed NTFS-3G metadata needs no chmod/chown.
            const struct stat& original = replacement->info;
            const bool sameOwner = created.st_uid == original.st_uid && created.st_gid == original.st_gid;
            const mode_t permissions = original.st_mode & 0777;
            const bool sameMode = (created.st_mode & 0777) == permissions;
            if (!sameOwner && fchown(output, original.st_uid, original.st_gid) != 0)
                error = errno;
            if (error == 0 && !sameMode && fchmod(output, permissions) != 0)
                error = errno;
        }
    }
    char actual[bufferBytes];
    // Same bounded transfer/readback approach as the native FreeDOS writer.
    for (int pass = 0; pass < 2 && error == 0; ++pass)
    {
        const bool verify = pass != 0;
        for (qsizetype done = 0; done < file.bytes.size();)
        {
            if (IsRufusJobCancelled())
            {
                error = ECANCELED;
                break;
            }
            const size_t count = size_t(qMin(bufferBytes, file.bytes.size() - done));
            const ssize_t transferred = verify ? pread(output, actual, count, done) :
                write(output, file.bytes.constData() + done, count);
            if (transferred < 0 && errno == EINTR)
                continue;
            if (transferred <= 0)
            {
                error = transferred == 0 ? EIO : errno;
                break;
            }
            if (verify && memcmp(actual, file.bytes.constData() + done, size_t(transferred)) != 0)
            {
                error = EBADMSG;
                break;
            }
            done += transferred;
        }
        if (error == 0 && !verify && fsync(output) != 0)
            error = errno;
    }
    struct stat actualInfo;
    if (error == 0 && fstat(output, &actualInfo) != 0)
        error = errno;
    if (error == 0 && actualInfo.st_size != file.bytes.size())
        error = EBADMSG;
    if (close(output) != 0 && error == 0)
        error = errno;
    int preparedInput = -1;
    if (error == 0 && replacement != nullptr)
    {
        preparedInput = openat(root, temporaryName, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (preparedInput < 0)
            error = errno;
    }
    for (size_t i = 0; i < count && error == 0; ++i)
        if (files[i].input >= 0)
            error = checkSource(root, files[i]);
    QByteArray existing;
    if (error == 0)
    {
        const int found = findName(root, file.destination, existing);
        if (replacement != nullptr)
            error = found == 0 ? (existing == replacement->fileName ? 0 : ESTALE)
                : (found == ENOENT ? ESTALE : found);
        else
            error = found == ENOENT ? 0 : (found == 0 ? EEXIST : found);
    }
    bool published = false;
    if (error == 0)
    {
        if (replacement != nullptr)
            error = renameat(root, temporaryName, root, replacement->fileName.constData()) == 0 ? 0 : errno;
        else
            error = publishFile(root, temporaryName, file.destination);
        published = error == 0;
    }
    if (published && replacement != nullptr)
    {
        // The root file now names the verified output. Retain its new
        // descriptor/identity for subsequent writes and completion checks.
        const int previousInput = replacement->input;
        replacement->input = preparedInput;
        preparedInput = -1;
        if (fstat(replacement->input, &replacement->info) != 0)
            error = errno;
        if (close(previousInput) != 0 && error == 0)
            error = errno;
    }
    if (preparedInput >= 0 && close(preparedInput) != 0 && error == 0)
        error = errno;
    if (error != 0 && !published)
        return unlinkat(root, temporaryName, 0) == 0 ? error : errno;
    if (error != 0)
        return error;
    return fsync(root) == 0 ? 0 : errno;
}
}

int PrepareWinPeRootFiles(int root, uint16_t winpe, bool usesMinint, bool optionsKnown)
{
    if (root < 0)
        return EINVAL;
    if (!optionsKnown)
        return ENOTSUP;
    const bool completeI386 = (winpe & WINPE_I386) == WINPE_I386;
    const bool completeAmd64 = (winpe & WINPE_AMD64) == WINPE_AMD64;
    const bool completeMinint = (winpe & WINPE_MININT) == WINPE_MININT;
    if (!completeI386 && !completeAmd64 && !completeMinint)
        return ENOTSUP;
    const WINPE_SETUP_PLAN plan = GetWinPeSetupPlan(winpe, usesMinint);
    if (plan.action == WINPE_UNSUPPORTED_LAYOUT)
        return ENOTSUP;
    if (IsRufusJobCancelled())
        return ECANCELED;
    SetupFile files[] = {
        {plan.loader_directory, "ntdetect.com", "ntdetect.com"},
        {plan.setup_directory, "txtsetup.sif", "TXTSETUP.SIF"},
        {plan.loader_directory, "setupldr.bin", "BOOTMGR"},
        {".", "ntdetect.com", nullptr},
        {".", "txtsetup.sif", nullptr},
        {".", "bootmgr", nullptr}
    };
    constexpr size_t fileCount = sizeof(files) / sizeof(files[0]);
    constexpr size_t detectorIndex = 0, sifIndex = 1, loaderIndex = 2, existingDetectorIndex = 3;
    constexpr size_t existingSifIndex = 4, existingLoaderIndex = 5;
    bool keepDetector = false, replaceSif = false, existingLoader = false;
    int error = 0;
    // Finish every required read/transform/collision check before creating files.
    for (size_t i = 0; i < existingDetectorIndex && error == 0; ++i)
    {
        if (i == sifIndex && !plan.copy_sif)
            continue;
        error = readSource(root, files[i]);
        QByteArray existing;
        if (error == 0)
        {
            const int found = findName(root, files[i].destination, existing);
            if (found == 0 && i == detectorIndex)
            {
                SetupFile& retained = files[existingDetectorIndex];
                error = readSource(root, retained);
                keepDetector = error == 0;
            }
            else if (found == 0 && i == sifIndex)
            {
                error = readSource(root, files[existingSifIndex]);
                replaceSif = error == 0;
            }
            else if (found == 0 && i == loaderIndex)
            {
                SetupFile& retained = files[existingLoaderIndex];
                error = readSource(root, retained);
                existingLoader = error == 0;
            }
            else
                error = found == ENOENT ? 0 : (found == 0 ? EEXIST : found);
        }
    }
    if (error == 0 && files[loaderIndex].bytes.size() <= excludedLoaderTail)
        error = EBADMSG;
    if (error == 0 && plan.copy_sif)
    {
        QByteArray edited;
        const QByteArray& original = files[replaceSif ? existingSifIndex : sifIndex].bytes;
        error = InsertConfigSectionText(original, WINPE_SETUP_SECTION, WINPE_SETUP_SOURCE_DEVICE, edited);
        if (error == 0)
            files[sifIndex].bytes.swap(edited);
    }
    if (error == 0 && plan.action == WINPE_PATCH_LOADER)
    {
        QByteArray& loader = files[loaderIndex].bytes;
        if (!PatchWinPeLoader(loader.data(), uint32_t(loader.size()), plan.index, usesMinint))
            error = EBADMSG;
    }
    if (error == 0 && existingLoader)
    {
        QByteArray rootLoader = files[existingLoaderIndex].bytes;
        const bool patch = plan.action == WINPE_PATCH_LOADER;
        if (patch && !PatchWinPeLoader(rootLoader.data(), uint32_t(rootLoader.size()), plan.index, usesMinint))
            error = EBADMSG;
        // One original pass per input: repeated patches need not be idempotent.
        if (error == 0 && rootLoader != files[loaderIndex].bytes)
            error = EEXIST;
        if (error == 0)
            files[loaderIndex].bytes.swap(rootLoader); // Publish the transformed root input.
    }
    for (size_t i = 0; i < fileCount && error == 0; ++i)
    {
        const bool retainedDetector = i == detectorIndex && keepDetector;
        const bool retainedLoader = i == loaderIndex && existingLoader && plan.action == WINPE_KEEP_LOADER;
        const bool needsOutput = files[i].input >= 0 && files[i].destination != nullptr
            && !retainedDetector && !retainedLoader;
        SetupFile* replacement = nullptr;
        if (i == sifIndex && replaceSif)
            replacement = &files[existingSifIndex];
        else if (i == loaderIndex && existingLoader)
            replacement = &files[existingLoaderIndex];
        if (needsOutput)
            error = storeFile(root, files[i], files, fileCount, replacement);
    }
    for (SetupFile& file : files)
    {
        if (error == 0 && file.input >= 0)
            error = checkSource(root, file);
        if (file.input >= 0 && close(file.input) != 0 && error == 0)
            error = errno;
        if (file.parent >= 0 && close(file.parent) != 0 && error == 0)
            error = errno;
    }
    return error;
}
}
