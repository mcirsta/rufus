// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>

namespace rufus
{
struct WindowsRegionOptions
{
    QString inputLocale;
    QString systemLocale;
    QString userLocale;
    QString uiLanguage;
    QString uiLanguageFallback;
    QString timezone; // Windows ID, not an IANA ID or a translated display name.
};

// Explicit user inputs only. Check bounded, XML-safe language tags/input-layout
// syntax and a Qt-known Windows time-zone ID. This does not establish whether
// a language pack or keyboard is present in the selected Windows edition.
// Fallback language may be empty, as allowed by Windows Setup.
int CheckWindowsRegionOptions(const WindowsRegionOptions& options);
bool IsWindowsLanguageTag(const QString& value);
}
