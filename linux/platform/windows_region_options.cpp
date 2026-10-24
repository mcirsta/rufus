// SPDX-License-Identifier: GPL-3.0-or-later
#include "windows_region_options.h"

#include <QRegularExpression>
#include <QStringList>
#include <QTimeZone>
#include <cerrno>

namespace rufus
{
namespace
{
constexpr int maxLocaleChars = 84; // Windows LOCALE_NAME_MAX_LENGTH excludes the terminator here.
constexpr int maxInputChars = 256;
constexpr int maxTimezoneChars = 256;

bool inputLocales(const QString& value)
{
    if (value.isEmpty() || value.size() > maxInputChars)
        return false;
    static const QRegularExpression layout(QStringLiteral("\\A[0-9A-Fa-f]{4}:[0-9A-Fa-f]{8}\\z"));
    const QStringList entries = value.split(QLatin1Char(';'));
    for (const QString& entry : entries)
    {
        if (!IsWindowsLanguageTag(entry) && !layout.match(entry).hasMatch())
            return false;
    }
    return true;
}
}

bool IsWindowsLanguageTag(const QString& value)
{
    static const QRegularExpression syntax(QStringLiteral("\\A[A-Za-z]{2,3}(?:-[A-Za-z0-9]{2,8}){1,3}\\z"));
    return value.size() <= maxLocaleChars && syntax.match(value).hasMatch();
}

int CheckWindowsRegionOptions(const WindowsRegionOptions& options)
{
    const bool validLocales = IsWindowsLanguageTag(options.systemLocale) && IsWindowsLanguageTag(options.userLocale);
    const bool validUi = IsWindowsLanguageTag(options.uiLanguage)
        && (options.uiLanguageFallback.isEmpty() || IsWindowsLanguageTag(options.uiLanguageFallback));
    if (!validLocales || !validUi || !inputLocales(options.inputLocale))
        return EINVAL;
    const bool boundedTimezone = !options.timezone.isEmpty() && options.timezone.size() <= maxTimezoneChars;
    if (!boundedTimezone)
        return EINVAL;
    const QByteArray windowsId = options.timezone.toUtf8();
    const QByteArray ianaId = QTimeZone::windowsIdToDefaultIanaId(windowsId);
    const bool knownTimezone = !ianaId.isEmpty() && QTimeZone::ianaIdToWindowsId(ianaId) == windowsId;
    return knownTimezone ? 0 : EINVAL;
}
}
