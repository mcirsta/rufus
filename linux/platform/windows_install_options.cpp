// SPDX-License-Identifier: GPL-3.0-or-later
#include "windows_install_options.h"
#include "iso_file.h"
#include "rufus_reporting.h"

extern "C" {
#include "native_input.h"
}

#include <climits>
#include <utility>

namespace rufus
{
namespace
{
struct WimInput
{
    ISO_FILE_READER reader = {};
    WIMStruct* wim = nullptr;

    ~WimInput()
    {
        wimlib_free(wim);
        CloseIsoFileReader(&reader);
    }

    int open(const ISO_ANALYSIS_SOURCE& source, const char* path)
    {
        const int status = OpenIsoFileReader(&reader, source.iso, source.udf, path);
        if (status == ISO_SCAN_CANCELLED)
            return WIMLIB_ERR_ABORTED_BY_PROGRESS;
        if (status != ISO_SCAN_OK)
            return WIMLIB_ERR_OPEN;
        return OpenWimFromIsoReader(&reader, 0, &wim);
    }
};

int readEditions(WIMStruct* wim, WindowsInstallOptions& options)
{
    struct wimlib_wim_info info = {};
    const int error = wimlib_get_wim_info(wim, &info);
    if (error != 0)
        return error;
    // Native image queries take a signed int. Never wrap an archive's count.
    const bool validCount = info.image_count > 0 && info.image_count <= INT_MAX;
    if (!validCount)
        return WIMLIB_ERR_INVALID_IMAGE;
    for (uint32_t image = 1; image <= info.image_count; ++image)
    {
        if (IsIsoScanCancelled())
            return WIMLIB_ERR_ABORTED_BY_PROGRESS;
        // Same DISPLAYNAME -> DESCRIPTION -> unknown rule as GetEditions
        // in wue.c. NAME is not the user-facing fallback and labels need not be unique.
        const char* name = wimlib_get_image_property(wim, int(image), "DISPLAYNAME");
        if (name == nullptr)
        {
            options.nonstandardNames = true;
            name = wimlib_get_image_property(wim, int(image), "DESCRIPTION");
        }
        const QString label = name != nullptr ? QString::fromUtf8(name) : QStringLiteral("Unknown Windows Version");
        options.editions.append({int(image), label});
    }
    return 0;
}

int readSetupLanguage(WIMStruct* wim, WindowsInstallOptions& options)
{
    constexpr int setupImage = 2; // GetUILanguage reads the second IMAGE, not the boot index.
    struct wimlib_wim_info info = {};
    const int error = wimlib_get_wim_info(wim, &info);
    if (error != 0)
        return error;
    if (info.image_count < setupImage)
        return WIMLIB_ERR_INVALID_IMAGE;
    const char* language = wimlib_get_image_property(wim, setupImage, "WINDOWS/LANGUAGES/LANGUAGE[1]");
    if (language != nullptr)
    {
        // Retain the full tag. Win32's eight-byte buffer can truncate script tags.
        options.uiLanguage = QString::fromUtf8(language);
        options.languageFromImage = true;
    }
    return 0;
}
}

int ReadWindowsImageEditions(WIMStruct* wim, WindowsInstallOptions& options)
{
    options = {};
    if (wim == nullptr) return WIMLIB_ERR_INVALID_PARAM;
    if (IsIsoScanCancelled()) return WIMLIB_ERR_ABORTED_BY_PROGRESS;
    WindowsInstallOptions result;
    const int error = readEditions(wim, result);
    if (IsIsoScanCancelled()) return WIMLIB_ERR_ABORTED_BY_PROGRESS;
    if (error != 0) return error;
    options = std::move(result);
    return 0;
}

int ReadWindowsInstallOptions(const ISO_ANALYSIS_SOURCE& source, const char* installPath,
    WindowsInstallOptions& options)
{
    options = {};
    if (IsIsoScanCancelled())
        return WIMLIB_ERR_ABORTED_BY_PROGRESS;
    const bool oneView = (source.iso != nullptr) != (source.udf != nullptr);
    if (!oneView || installPath == nullptr || installPath[0] != '/')
        return WIMLIB_ERR_INVALID_PARAM;
    WindowsInstallOptions result;
    WimInput install, boot;
    int error = install.open(source, installPath);
    if (error == 0)
        error = ReadWindowsImageEditions(install.wim, result);
    if (error != 0)
        return error;
    result.languageError = boot.open(source, "/sources/boot.wim");
    if (result.languageError == 0)
        result.languageError = readSetupLanguage(boot.wim, result);
    const bool cancelled = IsIsoScanCancelled() || result.languageError == WIMLIB_ERR_ABORTED_BY_PROGRESS;
    if (cancelled)
        return WIMLIB_ERR_ABORTED_BY_PROGRESS;
    if (result.nonstandardNames)
        uprintf("WARNING: Nonstandard Windows image (missing <DISPLAYNAME> entries)");
    if (!result.languageFromImage)
        uprintf("Windows Setup language unavailable; using Rufus's en-US fallback");
    options = std::move(result);
    return 0;
}
}
