// SPDX-License-Identifier: GPL-3.0-or-later
#include "windows_customization.h"
#include "qt/prepare_image_protocol.h"
#include "windows_bootloaders.h"
#include "windows_registry.h"
#include "windows_region_options.h"
#include "windows_upgrade.h"
#include "windows_wim.h"
#include "config_patch.h"
#include "rufus_reporting.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace rufus
{
int CheckWindowsCustomizationSettings(int flags, const QString& username)
{
    if ((flags & ~WindowsCustomizationFlags) != 0)
        return ENOTSUP;
    if (!(flags & UNATTEND_SET_USER))
        return username.isEmpty() ? 0 : EINVAL;
    const QByteArray encoded = username.toUtf8();
    const bool fits = encoded.size() < MAX_USERNAME_LENGTH;
    const bool validUnicode = QString::fromUtf8(encoded) == username;
    if (!fits || !validUnicode)
        return EINVAL;
    for (const QChar character : username)
    {
        const bool control = character.unicode() < ' ' || character.unicode() == 127;
        if (control)
            return EINVAL;
    }
    return 0;
}

int CheckWindowsCustomizationImage(const RUFUS_IMG_REPORT& report, int flags)
{
    if (flags == 0)
        return 0;
    int arch = ARCH_UNKNOWN;
    for (unsigned bits = report.has_efi >> 1; bits > 1; bits >>= 1)
        ++arch;
    const bool supportedArch = arch >= ARCH_X86_32 && arch <= ARCH_ARM_64;
    if (!supportedArch)
        return ENOTSUP;
    int supported = WindowsCustomizationFlags;
    if (flags & UNATTEND_SILENT_INSTALL)
        supported |= UNATTEND_SILENT_INSTALL | UNATTEND_DUPLICATE_LOCALE;
    // Expert mode controls visibility in the GUI, not native backend capability.
    const int allowed = GetWindowsUnattendFlags(&report, true) & supported;
    return (flags & ~allowed) == 0 ? 0 : ENOTSUP;
}

int CheckWindowsToGoCustomizationSettings(int flags, const QString& username)
{
    if (flags & ~WindowsToGoCustomizationFlags)
        return ENOTSUP;
    return CheckWindowsCustomizationSettings(flags & ~UNATTEND_OFFLINE_INTERNAL_DRIVES, username);
}

namespace
{
int checkSilentSettings(int flags, const QString& username, int edition, const WindowsRegionOptions& region)
{
    const int ordinaryFlags = flags & ~(UNATTEND_SILENT_INSTALL | UNATTEND_DUPLICATE_LOCALE);
    const int error = CheckWindowsCustomizationSettings(ordinaryFlags, username);
    if (error != 0)
        return error;
    const int prerequisites = UNATTEND_SET_USER | UNATTEND_NO_DATA_COLLECTION | UNATTEND_DUPLICATE_LOCALE;
    const bool havePrerequisites = (flags & prerequisites) == prerequisites;
    if (!havePrerequisites || edition < 1)
        return EINVAL;
    // A silent install cannot fall back to asking for an account when the
    // shared writer skips an empty or reserved name. Expect the UI's normalized name.
    if (username.isEmpty() || username != username.trimmed())
        return EINVAL;
    const QByteArray name = username.toUtf8();
    const bool invalidCharacters = strpbrk(name.constData(), USERNAME_INVALID_CHARS) != nullptr;
    if (invalidCharacters || IsUnattendUserNameReserved(name.constData()))
        return EINVAL;
    return CheckWindowsRegionOptions(region);
}

int checkSetupOptions(const RUFUS_UNATTEND_OPTIONS& options)
{
    if ((options.flags & UNATTEND_SET_USER) && options.username == nullptr)
        return EINVAL;
    const QString username = QString::fromUtf8(options.username);
    if (!(options.flags & UNATTEND_SILENT_INSTALL))
        return CheckWindowsCustomizationSettings(options.flags, username);
    const bool validUtf8 = username.toUtf8() == options.username;
    const bool validSetupLanguage = IsWindowsLanguageTag(QString::fromUtf8(options.winpe_ui_language));
    if (!validUtf8 || !validSetupLanguage || options.ui_language_fallback == nullptr)
        return EINVAL;
    const WindowsRegionOptions region = {QString::fromUtf8(options.input_locale),
        QString::fromUtf8(options.system_locale), QString::fromUtf8(options.user_locale),
        QString::fromUtf8(options.ui_language), QString::fromUtf8(options.ui_language_fallback),
        QString::fromUtf8(options.timezone)};
    return checkSilentSettings(options.flags, username, options.edition_index, region);
}

int openPantherDirectory(const char* root, bool windowsToGo = false)
{
    constexpr int directoryFlags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;
    const char* const installer[] = {"sources", "$OEM$", "$$", "Panther"};
    const char* const installed[] = {"Windows", "Panther"};
    const char* const* parts = windowsToGo ? installed : installer;
    const size_t count = windowsToGo ? sizeof(installed) / sizeof(installed[0]) : sizeof(installer) / sizeof(installer[0]);
    int parent = open(root, directoryFlags);
    if (parent < 0)
        return -1;
    for (size_t i = 0; i < count; ++i)
    {
        // sources must have come from the extracted installer. Reuse existing
        // OEM directories but verify each component before descending.
        int error = 0;
        if (i != 0 && mkdirat(parent, parts[i], 0755) != 0 && errno != EEXIST)
            error = errno;
        int child = -1;
        if (error == 0)
        {
            child = openat(parent, parts[i], directoryFlags);
            if (child < 0)
                error = errno;
        }
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

int writeAnswerFile(int parent, RUFUS_UNATTEND_OPTIONS& options, bool disableWinpe = false)
{
    const char* const name = "unattend.xml";
    const int fd = openat(parent, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0)
        return errno;
    FILE* stream = fdopen(fd, "w");
    int error = 0;
    if (stream == nullptr)
    {
        error = errno;
        close(fd);
    }
    else
    {
        uint32_t offsets[2];
        errno = 0;
        if (!WriteUnattendXml(stream, &options, offsets))
            error = errno != 0 ? errno : EIO;
        if (fclose(stream) != 0 && error == 0)
            error = errno;
    }
    if (error == 0 && IsRufusJobCancelled())
        error = ECANCELED;
    if (error == 0 && disableWinpe)
    {
        // Reuse the same token replacement as Rufus's post-registry path.
        bool modified = false;
        error = ReplaceConfigTokenAt(parent, name, "<settings", "windowsPE", "disabled", &modified);
        if (error == 0 && !modified)
            error = EBADMSG;
    }
    if (error != 0 && unlinkat(parent, name, 0) != 0)
        uprintf("Could not remove the incomplete Windows answer file: %s", strerror(errno));
    return error;
}

int findHiveName(const struct wimlib_dir_entry* entry, void* context)
{
    const unsigned unsupported = WIMLIB_FILE_ATTRIBUTE_DIRECTORY | WIMLIB_FILE_ATTRIBUTE_DEVICE |
        WIMLIB_FILE_ATTRIBUTE_REPARSE_POINT | WIMLIB_FILE_ATTRIBUTE_ENCRYPTED;
    if (entry->attributes & unsupported)
        return WIMLIB_ERR_NOT_A_REGULAR_FILE;
    if (entry->filename == nullptr || strcasecmp(entry->filename, "SYSTEM") != 0)
        return WIMLIB_ERR_INVALID_PARAM;
    *static_cast<QByteArray*>(context) = entry->filename;
    return 0;
}

int prepareSetupHive(WIMStruct* wim, int image, const QByteArray& work, const QByteArray& editedHive)
{
    const char* const systemPath = "Windows/System32/config/SYSTEM";
    QByteArray hiveName;
    int wimError = wimlib_iterate_dir_tree(wim, image, systemPath, 0, findHiveName, &hiveName);
    if (wimError == 0)
    {
        const int extractFlags = WIMLIB_EXTRACT_FLAG_NO_PRESERVE_DIR_STRUCTURE |
            WIMLIB_EXTRACT_FLAG_NO_ACLS | WIMLIB_EXTRACT_FLAG_NO_ATTRIBUTES | WIMLIB_EXTRACT_FLAG_NORPFIX;
        wimError = wimlib_extract_paths(wim, image, work.constData(), &systemPath, 1, extractFlags);
    }
    const QByteArray sourceHive = work + '/' + hiveName;
    const int hiveError = wimError == 0 ? PrepareWindowsSetupHive(sourceHive.constData(), editedHive.constData()) : EIO;
    const bool cancelled = IsRufusJobCancelled() || wimError == WIMLIB_ERR_ABORTED_BY_PROGRESS || hiveError == ECANCELED;
    if (cancelled)
        return ECANCELED;
    if (hiveError != 0)
    {
        const char* reason = wimError != 0 ? wimlib_get_error_string(static_cast<wimlib_error_code>(wimError)) : strerror(hiveError);
        uprintf("Could not prepare the offline SYSTEM hive: %s", reason);
        uprintf("Falling back to creating the registry keys through unattend.xml");
    }
    return hiveError;
}

int customizeSetupImage(WIMStruct* wim, int image, const char* directory, RUFUS_UNATTEND_OPTIONS& options)
{
    QTemporaryDir temporary(QDir::tempPath() + QStringLiteral("/rufus-windows-XXXXXX"));
    if (!temporary.isValid())
        return EIO;
    const QByteArray work = QFile::encodeName(temporary.path());
    const QByteArray editedHive = work + "/edited-system";
    const QByteArray answer = work + "/unattend.xml";
    const bool hardware = options.flags & UNATTEND_SECUREBOOT_TPM_MINRAM;
    const bool silent = options.flags & UNATTEND_SILENT_INSTALL;
    const int hiveError = hardware ? prepareSetupHive(wim, image, work, editedHive) : 0;
    if (hiveError == ECANCELED)
        return hiveError;
    const bool registryApplied = hardware && hiveError == 0;
    const bool useOem = registryApplied && !silent;
    const int parent = useOem ? openPantherDirectory(directory) :
        open(work.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (parent < 0)
        return errno;
    // Win32 removes only the bypass commands when silent installation still
    // needs WinPE. Generate that same answer by clearing their flag before writing.
    RUFUS_UNATTEND_OPTIONS answerOptions = options;
    if (registryApplied && silent)
        answerOptions.flags &= ~UNATTEND_SECUREBOOT_TPM_MINRAM;
    int error = writeAnswerFile(parent, answerOptions, useOem);
    if (error == 0)
    {
        struct wimlib_update_command commands[2] = {};
        size_t count = 0;
        if (registryApplied)
        {
            commands[count].op = WIMLIB_UPDATE_OP_ADD;
            commands[count].add.fs_source_path = const_cast<char*>(editedHive.constData());
            commands[count++].add.wim_target_path = const_cast<char*>("Windows/System32/config/SYSTEM");
        }
        if (!useOem)
        {
            commands[count].op = WIMLIB_UPDATE_OP_ADD;
            commands[count].add.fs_source_path = const_cast<char*>(answer.constData());
            commands[count++].add.wim_target_path = const_cast<char*>("Autounattend.xml");
        }
        const int wimError = UpdateWindowsSetupWim(wim, image, commands, count);
        if (wimError != 0)
        {
            uprintf("Could not update Windows Setup: %s", wimlib_get_error_string(static_cast<wimlib_error_code>(wimError)));
            error = wimError == WIMLIB_ERR_ABORTED_BY_PROGRESS ? ECANCELED : EIO;
            if (unlinkat(parent, "unattend.xml", 0) != 0)
                uprintf("Could not remove Windows answer file after the failed WIM update: %s", strerror(errno));
        }
    }
    if (close(parent) != 0 && error == 0)
        error = errno;
    if (error == 0)
        uprintf("Added %s", useOem ? "sources/$OEM$/$$/Panther/unattend.xml" : "boot.wim:Autounattend.xml");
    return error;
}

int applyOemCustomization(const char* directory, RUFUS_UNATTEND_OPTIONS& options)
{
    const int parent = openPantherDirectory(directory);
    if (parent < 0)
        return errno;
    int error = IsRufusJobCancelled() ? ECANCELED : writeAnswerFile(parent, options);
    if (close(parent) != 0 && error == 0)
        error = errno;
    if (error == 0)
        uprintf("Added sources/$OEM$/$$/Panther/unattend.xml");
    return error;
}

int applySetupCustomization(const char* directory, RUFUS_UNATTEND_OPTIONS& options, QStringList* modified)
{
    constexpr int directoryFlags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;
    const int root = open(directory, directoryFlags);
    if (root < 0)
        return errno;
    const int sources = openat(root, "sources", directoryFlags);
    int error = sources < 0 ? errno : 0;
    close(root);
    if (error != 0)
        return error;
    // Retain the checked parent through wimlib's append or rename commit.
    const QByteArray path = "/proc/self/fd/" + QByteArray::number(sources) + "/boot.wim";
    WIMStruct* wim = nullptr;
    int image = 0;
    const int wimError = OpenWindowsSetupWim(path.constData(), &wim, &image);
    if (wimError != 0)
    {
        uprintf("Could not open Windows Setup: %s", wimlib_get_error_string(static_cast<wimlib_error_code>(wimError)));
        error = wimError == WIMLIB_ERR_ABORTED_BY_PROGRESS ? ECANCELED : EIO;
    }
    else
    {
        // Read the CA payloads before any WIM commit invalidates this handle.
        // A payload failure must not silently discard the selected option.
        if (options.flags & UNATTEND_USE_MS2023_BOOTLOADERS)
            error = ApplyWindowsCa2023Bootloaders(wim, image, directory, modified);
        if (error == 0)
        {
            const bool needsWinpe = options.flags & UNATTEND_WINPE_SETUP_MASK;
            error = needsWinpe ? customizeSetupImage(wim, image, directory, options) :
                applyOemCustomization(directory, options);
            if (error == 0 && needsWinpe && modified != nullptr) modified->append(QStringLiteral("sources/boot.wim"));
        }
    }
    wimlib_free(wim);
    if (close(sources) != 0 && error == 0)
        error = errno;
    return error;
}

int copyHiveBytes(int source, int destination, quint64 bytes)
{
    char buffer[64 * 1024];
    for (quint64 offset = 0; offset < bytes;)
    {
        if (IsRufusJobCancelled()) return ECANCELED;
        const size_t count = size_t(qMin(bytes - offset, quint64(sizeof(buffer))));
        const ssize_t read = pread(source, buffer, count, off_t(offset));
        if (read < 0 && errno == EINTR) continue;
        if (read <= 0) return read == 0 ? EIO : errno;
        for (ssize_t done = 0; done < read;)
        {
            const ssize_t written = pwrite(destination, buffer + done, size_t(read - done), off_t(offset + quint64(done)));
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) return written == 0 ? EIO : errno;
            done += written;
        }
        offset += quint64(read);
    }
    return 0;
}

int applyToGoOfflinePolicies(const char* directory, int flags)
{
    constexpr int directoryFlags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;
    constexpr quint64 maximumHiveBytes = 256 * 1024 * 1024;
    int parent = open(directory, directoryFlags);
    if (parent < 0) return errno;
    int error = 0;
    const char* const parts[] = {"Windows", "System32", "config"};
    for (const char* part : parts)
    {
        const int child = openat(parent, part, directoryFlags);
        error = child < 0 ? errno : 0;
        if (close(parent) != 0 && error == 0) error = errno;
        parent = child;
        if (error != 0) break;
    }
    const int source = error == 0 ? openat(parent, "SYSTEM", O_RDWR | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC) : -1;
    if (error == 0 && source < 0) error = errno;
    struct stat original = {};
    if (error == 0 && fstat(source, &original) != 0) error = errno;
    const bool regularHive = S_ISREG(original.st_mode) && original.st_size > 0;
    if (error == 0 && !regularHive) error = EINVAL;
    if (error == 0 && quint64(original.st_size) > maximumHiveBytes) error = EFBIG;
    QTemporaryDir temporary(QDir::tempPath() + QStringLiteral("/rufus-togo-XXXXXX"));
    if (error == 0 && !temporary.isValid()) error = EIO;
    const QByteArray inputPath = QFile::encodeName(temporary.filePath(QStringLiteral("SYSTEM")));
    const QByteArray editedPath = QFile::encodeName(temporary.filePath(QStringLiteral("SYSTEM.edited")));
    const int input = error == 0 ? open(inputPath.constData(), O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600) : -1;
    if (error == 0 && input < 0) error = errno;
    if (error == 0) error = copyHiveBytes(source, input, quint64(original.st_size));
    if (input >= 0 && close(input) != 0 && error == 0) error = errno;
    if (error == 0) error = PrepareWindowsToGoHive(inputPath.constData(), editedPath.constData(), flags);
    const int edited = error == 0 ? open(editedPath.constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC) : -1;
    if (error == 0 && edited < 0) error = errno;
    struct stat changed = {}, current = {}, path = {};
    if (error == 0 && fstat(edited, &changed) != 0) error = errno;
    const bool validOutput = S_ISREG(changed.st_mode) && changed.st_size > 0
        && quint64(changed.st_size) <= maximumHiveBytes;
    if (error == 0 && !validOutput) error = EBADMSG;
    if (error == 0 && (fstat(source, &current) != 0 || fstatat(parent, "SYSTEM", &path, AT_SYMLINK_NOFOLLOW) != 0)) error = errno;
    const bool sameFile = current.st_dev == original.st_dev && current.st_ino == original.st_ino
        && path.st_dev == original.st_dev && path.st_ino == original.st_ino;
    const bool sameData = current.st_size == original.st_size
        && current.st_mtim.tv_sec == original.st_mtim.tv_sec && current.st_mtim.tv_nsec == original.st_mtim.tv_nsec
        && current.st_ctim.tv_sec == original.st_ctim.tv_sec && current.st_ctim.tv_nsec == original.st_ctim.tv_nsec;
    if (error == 0 && (!sameFile || !sameData)) error = ESTALE;
    const bool started = error == 0;
    if (error == 0) error = copyHiveBytes(edited, source, quint64(changed.st_size));
    if (error == 0 && ftruncate(source, changed.st_size) != 0) error = errno;
    if (started && fsync(source) != 0 && error == 0) error = errno;
    if (edited >= 0 && close(edited) != 0 && error == 0) error = errno;
    if (source >= 0 && close(source) != 0 && error == 0) error = errno;
    if (parent >= 0 && close(parent) != 0 && error == 0) error = errno;
    if (temporary.isValid() && !temporary.remove() && error == 0) error = EIO;
    return error;
}
}

int ApplyWindowsToGoCustomization(const char* directory, int arch, int flags, const QString& username)
{
    int error = CheckWindowsToGoCustomizationSettings(flags, username);
    if (error != 0 || flags == 0) return error;
    const bool validArch = arch >= ARCH_X86_32 && arch <= ARCH_ARM_64;
    if (directory == nullptr || *directory == 0 || !validArch) return EINVAL;
    if (IsRufusJobCancelled()) return ECANCELED;
    const int panther = openPantherDirectory(directory, true);
    if (panther < 0) return errno;
    // Preserve an existing image's answer and refuse before editing its hive.
    struct stat answer;
    if (fstatat(panther, "unattend.xml", &answer, AT_SYMLINK_NOFOLLOW) == 0) error = EEXIST;
    else if (errno != ENOENT) error = errno;
    const int offlineFlags = flags & UNATTEND_OFFLINE_SERVICING_MASK;
    if (error == 0 && offlineFlags != 0) error = applyToGoOfflinePolicies(directory, offlineFlags);
    QByteArray name = username.toUtf8();
    RUFUS_UNATTEND_OPTIONS options = {};
    options.arch = arch;
    options.flags = flags | UNATTEND_WINDOWS_TO_GO;
    options.username = name.data();
    if (error == 0) error = writeAnswerFile(panther, options);
    if (error == 0 && fsync(panther) != 0) error = errno;
    if (close(panther) != 0 && error == 0) error = errno;
    return error;
}

int CheckWindowsCustomizationSettings(const PrepareImageRequest& request)
{
    if (request.windowsToGo)
    {
        if (!ValidatePrepareWindowsToGo(request)) return EINVAL;
        return CheckWindowsToGoCustomizationSettings(request.windowsFlags, request.windowsUsername);
    }
    const bool silent = request.windowsFlags & UNATTEND_SILENT_INSTALL;
    if (!silent)
    {
        if (request.hasWindowsInstallSettings())
            return EINVAL;
        return CheckWindowsCustomizationSettings(request.windowsFlags, request.windowsUsername);
    }
    const bool confirmedSource = request.isDevice && !request.imagePath.isEmpty() && request.windowsSilentConfirmed;
    const bool archivePath = request.windowsInstallPath.startsWith(QLatin1Char('/'))
        && !request.windowsInstallPath.contains(QChar::Null)
        && QDir::cleanPath(request.windowsInstallPath) == request.windowsInstallPath;
    if (!confirmedSource || !archivePath)
        return EINVAL;
    return checkSilentSettings(request.windowsFlags, request.windowsUsername, request.windowsEdition, request.windowsRegion);
}

int ApplyWindowsCustomization(const char* directory, int arch, uint32_t build,
    const PrepareImageRequest& request, const QString& setupLanguage, QStringList* modified)
{
    const int error = CheckWindowsCustomizationSettings(request);
    if (error != 0)
        return error;
    QByteArray username = request.windowsUsername.toUtf8();
    const WindowsRegionOptions& region = request.windowsRegion;
    const QByteArray input = region.inputLocale.toUtf8();
    const QByteArray system = region.systemLocale.toUtf8();
    const QByteArray user = region.userLocale.toUtf8();
    const QByteArray display = region.uiLanguage.toUtf8();
    const QByteArray fallback = region.uiLanguageFallback.toUtf8();
    const QByteArray timezone = region.timezone.toUtf8();
    const QByteArray setup = setupLanguage.toUtf8();
    RUFUS_UNATTEND_OPTIONS options = {};
    options.arch = arch;
    options.flags = request.windowsFlags;
    options.edition_index = request.windowsEdition;
    options.username = username.data();
    options.input_locale = input.constData();
    options.system_locale = system.constData();
    options.user_locale = user.constData();
    options.ui_language = display.constData();
    options.ui_language_fallback = fallback.constData();
    options.timezone = timezone.constData();
    options.winpe_ui_language = setup.constData();
    return ApplyWindowsCustomization(directory, build, options, modified);
}

int ApplyWindowsCustomization(const char* directory, int arch, uint32_t build, int flags, const QString& username, QStringList* modified)
{
    const int error = CheckWindowsCustomizationSettings(flags, username);
    if (error != 0)
        return error;
    QByteArray name = username.toUtf8();
    RUFUS_UNATTEND_OPTIONS options = {};
    options.arch = arch;
    options.flags = flags;
    options.username = name.data();
    return ApplyWindowsCustomization(directory, build, options, modified);
}

int ApplyWindowsCustomization(const char* directory, uint32_t build, RUFUS_UNATTEND_OPTIONS& options, QStringList* modified)
{
    int error = checkSetupOptions(options);
    if (error != 0 || options.flags == 0)
        return error;
    const bool supportedArch = options.arch >= ARCH_X86_32 && options.arch <= ARCH_ARM_64;
    if (directory == nullptr || !supportedArch)
        return EINVAL;
    if (IsRufusJobCancelled())
        return ECANCELED;
    if (options.flags & UNATTEND_SECUREBOOT_TPM_MINRAM)
    {
        error = PrepareWindowsUpgradeFiles(directory, build, modified);
        if (error != 0)
            return error;
    }
    const bool needsSetupImage = options.flags & (UNATTEND_WINPE_SETUP_MASK | UNATTEND_USE_MS2023_BOOTLOADERS);
    return needsSetupImage ? applySetupCustomization(directory, options, modified) : applyOemCustomization(directory, options);
}
}
