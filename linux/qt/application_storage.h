// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QSettings>
#include <QString>
#include <QStringList>

namespace rufus
{
// Select storage once, before constructing controllers or worker threads.
// An adjacent rufus.ini or Rufus's portable filename marker selects INI mode.
// Explicit portable failures are reported instead of writing user settings.
int ConfigureApplicationStorage(const QString& executable, bool portableRequested, QString& problem);
bool IsPortableApplication();
QString ApplicationSettingsFile();
QString ApplicationFilesDirectory();
QString ApplicationReportDirectory();
int ResetUserApplicationSettings();
int ClearApplicationResources(const QStringList& protectedPaths, QString& problem);

class ApplicationSettings final : public QSettings
{
public:
    ApplicationSettings();
    ~ApplicationSettings() override;
    QVariant value(const QString& key, const QVariant& fallback = {}) const;
    void setValue(const QString& key, const QVariant& value);
};
}
