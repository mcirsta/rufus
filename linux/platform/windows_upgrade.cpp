// SPDX-License-Identifier: GPL-3.0-or-later
#include "windows_upgrade.h"
#include "pe_arch.h"
#include "rufus_reporting.h"

#include <QFile>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

static void initializeSetupResources()
{
    Q_INIT_RESOURCE(windows_setup);
}

namespace rufus
{
namespace
{
constexpr uint32_t WindowsSetupWrapperBuild = 26000;
constexpr int DirectoryFlags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;

int checkRegularFileAt(int parent, const char* name)
{
    struct stat info;
    if (fstatat(parent, name, &info, AT_SYMLINK_NOFOLLOW) != 0)
        return errno;
    const bool ordinaryFile = S_ISREG(info.st_mode) && info.st_nlink == 1;
    return ordinaryFile ? 0 : EINVAL;
}

int writeNewFileAt(int parent, const char* name, const QByteArray& bytes)
{
    const int fd = openat(parent, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0)
        return errno;
    int error = 0;
    qsizetype written = 0;
    while (written < bytes.size() && !IsRufusJobCancelled())
    {
        const ssize_t count = write(fd, bytes.constData() + written, bytes.size() - written);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
        {
            error = count < 0 ? errno : EIO;
            break;
        }
        written += count;
    }
    if (close(fd) != 0 && error == 0)
        error = errno;
    if (error == 0 && IsRufusJobCancelled())
        error = ECANCELED;
    if (error != 0 && unlinkat(parent, name, 0) != 0)
        uprintf("Could not remove incomplete %s: %s", name, strerror(errno));
    return error;
}

int prepareAppraiser(int sources, QStringList* modified)
{
    const char* original = "appraiserres.dll";
    const char* backup = "appraiserres.bak";
    const int sourceError = checkRegularFileAt(sources, original);
    if (sourceError != 0 && sourceError != ENOENT)
        return sourceError;
    const bool haveOriginal = sourceError == 0;
    if (haveOriginal)
    {
        const int backupError = checkRegularFileAt(sources, backup);
        if (backupError != 0 && backupError != ENOENT)
            return backupError;
        // Rufus replaces an existing appraiser backup. Both entries have been
        // checked and this helper exclusively owns the private target tree.
        if (renameat(sources, original, sources, backup) != 0)
            return errno;
    }
    const int error = writeNewFileAt(sources, original, {});
    if (error != 0 && haveOriginal && renameat(sources, backup, sources, original) != 0)
        uprintf("Could not restore %s: %s", original, strerror(errno));
    if (error == 0) {
        uprintf("Created sources/appraiserres.dll placeholder");
        if (modified != nullptr) {
            modified->append(QStringLiteral("sources/appraiserres.dll"));
            if (haveOriginal) modified->append(QStringLiteral("sources/appraiserres.bak"));
        }
    }
    return error;
}

int prepareSetupWrapper(int root, QStringList* modified)
{
    int error = checkRegularFileAt(root, "setup.exe");
    if (error != 0)
        return error;
    const int fd = openat(root, "setup.exe", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
        return errno;
    QFile original;
    if (!original.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle))
    {
        close(fd);
        return EIO;
    }
    // Map rather than copy the executable just to inspect Rufus's PE header.
    const qint64 size = original.size();
    if (size <= 0)
        return EBADMSG;
    uchar* bytes = original.map(0, size);
    if (bytes == nullptr)
        return EIO;
    const int arch = FindPeArch(bytes, size);
    original.unmap(bytes);
    const bool supportedArch = arch == ARCH_X86_64 || arch == ARCH_ARM_64;
    if (!supportedArch)
    {
        uprintf("Unsupported setup.exe architecture; in-place upgrade wrapper will not be added.");
        return 0; // Same architecture exception as Rufus.
    }
    initializeSetupResources();
    const QString resource = arch == ARCH_X86_64 ? QStringLiteral(":/rufus/setup/setup_x64.exe") :
        QStringLiteral(":/rufus/setup/setup_arm64.exe");
    QFile wrapper(resource);
    if (!wrapper.open(QIODevice::ReadOnly))
        return ENOENT;
    const QByteArray payload = wrapper.readAll();
    const bool complete = !payload.isEmpty() && payload.size() == wrapper.size();
    if (!complete)
        return EIO;
    error = checkRegularFileAt(root, "setup.dll");
    if (error != ENOENT)
        return error == 0 ? EEXIST : error;
    // Unlike the appraiser backup, Rufus never replaces setup.dll.
    if (renameat(root, "setup.exe", root, "setup.dll") != 0)
        return errno;
    error = writeNewFileAt(root, "setup.exe", payload);
    if (error != 0 && renameat(root, "setup.dll", root, "setup.exe") != 0)
        uprintf("Could not restore setup.exe: %s", strerror(errno));
    if (error == 0) {
        uprintf("Created setup.exe wrapper from the original Rufus resource");
        if (modified != nullptr) *modified << QStringLiteral("setup.exe") << QStringLiteral("setup.dll");
    }
    return error;
}
}

int PrepareWindowsUpgradeFiles(const char* directory, uint32_t build, QStringList* modified)
{
    if (IsRufusJobCancelled())
        return ECANCELED;
    const int root = open(directory, DirectoryFlags);
    if (root < 0)
        return errno;
    const int sources = openat(root, "sources", DirectoryFlags);
    int error = sources < 0 ? errno : prepareAppraiser(sources, modified);
    if (sources >= 0 && close(sources) != 0 && error == 0)
        error = errno;
    if (error != 0)
        uprintf("Could not prepare sources/appraiserres.dll: %s", strerror(error));
    if (error == 0 && build >= WindowsSetupWrapperBuild)
    {
        error = IsRufusJobCancelled() ? ECANCELED : prepareSetupWrapper(root, modified);
        if (error != 0)
            uprintf("Could not prepare the setup.exe wrapper: %s", strerror(error));
    }
    if (close(root) != 0 && error == 0)
        error = errno;
    return error;
}
}
