// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "iso_analyze.h"
#include <QList>
#include <QString>
#include <wimlib.h>

namespace rufus
{
struct WindowsEdition
{
    int image = 0; // Native WIM index, starting at 1, independent of the label.
    QString name;
};

struct WindowsInstallOptions
{
    QList<WindowsEdition> editions;
    QString uiLanguage = QStringLiteral("en-US");
    bool nonstandardNames = false;
    bool languageFromImage = false;
    int languageError = 0;
};

// Edition labels/indexes from an already retained ordinary or embedded WIM.
// Windows To Go has no Setup boot.wim language to read. No host edition is
// inferred. The caller owns and serializes the handle; errors clear options.
int ReadWindowsImageEditions(WIMStruct* wim, WindowsInstallOptions& options);

// Read only the caller's selected install archive and /sources/boot.wim through
// the already selected ISO/UDF view. Initialize wimlib for UTF-8 first and keep
// that view stable/serialized through return. Results own their strings.
// Return wimlib error codes; install errors or cancellation clear the result.
// Boot-language errors retain Rufus's en-US fallback, with its provenance and
// error recorded separately. No edition is selected and no host locale is read.
int ReadWindowsInstallOptions(const ISO_ANALYSIS_SOURCE& source, const char* installPath,
    WindowsInstallOptions& options);
}
