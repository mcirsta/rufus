// SPDX-License-Identifier: GPL-3.0-or-later
#include "application_storage.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMetaType>
#include <QStandardPaths>
#include <QDebug>
#include <cerrno>

namespace rufus
{
namespace
{
QString portableDirectory;

QString userSettingsFile()
{
    QSettings settings(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("Rufus"), QStringLiteral("Linux"));
    return settings.fileName();
}

QString canonicalKey(const QString& key)
{
    // Retain preferences written by older Linux previews while using the
    // original Windows INI names for files shared between the two platforms.
    if (key.compare(QLatin1String("DisableRufusMbr"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("DisableRufusMBR");
    if (key.compare(QLatin1String("WindowsUserExperience"), Qt::CaseInsensitive) == 0
        || key.compare(QLatin1String("WindowsUserExperienceOptions"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("WindowsUserExperienceOptions");
    return key;
}

QString storedKey(const QSettings& settings, const QString& key)
{
    if (settings.contains(key)) return key;
    const QString canonical = canonicalKey(key);
    if (settings.contains(canonical)) return canonical;
    for (const QString& present : settings.allKeys())
        if (canonicalKey(present).compare(canonical, Qt::CaseInsensitive) == 0) return present;
    return canonical;
}

bool portableName(const QString& executable)
{
    const QString name = QFileInfo(executable).fileName();
    const qsizetype capital = name.indexOf(QLatin1Char('P'));
    const bool alphaMarker = capital >= 0 && capital + 1 < name.size() && name[capital + 1] == QLatin1Char('H');
    return name.contains(QLatin1Char('p')) || (capital >= 0 && !alphaMarker);
}
}

int ConfigureApplicationStorage(const QString& executable, bool portableRequested, QString& problem)
{
    problem.clear();
    portableDirectory.clear();
    const bool validExecutable = !executable.isEmpty() && !executable.contains(QChar::Null);
    if (!validExecutable) { problem = QStringLiteral("Invalid Rufus executable path."); return EINVAL; }
    const QString directory = QFileInfo(executable).absolutePath();
    const QString path = QDir(directory).filePath(QStringLiteral("rufus.ini"));
    const bool create = portableRequested || portableName(executable);
    if (!create && !QFileInfo::exists(path)) return 0;
    QFile file(path);
    if (!file.open(create ? QIODevice::WriteOnly | QIODevice::Append : QIODevice::ReadOnly)) {
        problem = QStringLiteral("Cannot use portable settings %1: %2").arg(path, file.errorString());
        return file.error() == QFileDevice::PermissionsError ? EACCES : EIO;
    }
    file.close();
    if (file.error() != QFileDevice::NoError) {
        problem = QStringLiteral("Cannot close portable settings %1: %2").arg(path, file.errorString());
        return EIO;
    }
    portableDirectory = directory;
    return 0;
}

bool IsPortableApplication() { return !portableDirectory.isEmpty(); }

QString ApplicationSettingsFile()
{
    return IsPortableApplication() ? QDir(portableDirectory).filePath(QStringLiteral("rufus.ini")) : userSettingsFile();
}

QString ApplicationFilesDirectory()
{
    if (IsPortableApplication()) return QDir(portableDirectory).filePath(QStringLiteral("rufus_files"));
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath(QStringLiteral("files"));
}

QString ApplicationReportDirectory()
{
    return IsPortableApplication() ? ApplicationFilesDirectory() : QDir::homePath();
}

int ResetUserApplicationSettings()
{
    // Win32 Alt+R deletes the registry, including while portable INI mode is
    // active. Leave the portable file and current in-memory choices intact.
    QSettings settings(userSettingsFile(), QSettings::IniFormat);
    settings.clear();
    settings.sync();
    return settings.status() == QSettings::NoError ? 0 : EIO;
}

ApplicationSettings::ApplicationSettings() : QSettings(ApplicationSettingsFile(), QSettings::IniFormat) {}

ApplicationSettings::~ApplicationSettings()
{
    sync();
    if (status() != NoError) qWarning().noquote() << "Cannot save Rufus settings:" << fileName();
}

QVariant ApplicationSettings::value(const QString& key, const QVariant& fallback) const
{
    const QVariant stored = QSettings::value(storedKey(*this, key), fallback);
    const bool integral = fallback.metaType().id() == QMetaType::Int || fallback.metaType().id() == QMetaType::UInt;
    if (IsPortableApplication() && integral) {
        bool numeric = false;
        const qlonglong number = stored.toString().toLongLong(&numeric, 0);
        if (numeric) return QVariant::fromValue(number);
    }
    if (fallback.metaType().id() != QMetaType::Bool || stored.metaType().id() == QMetaType::Bool) return stored;
    bool numeric = false;
    const qlonglong number = stored.toString().toLongLong(&numeric, 0);
    // Accept old Qt boolean spellings too; portable INIs use Windows's 0/1.
    return numeric ? QVariant(number != 0) : QVariant(stored.toString().compare(QLatin1String("true"), Qt::CaseInsensitive) == 0);
}

void ApplicationSettings::setValue(const QString& key, const QVariant& data)
{
    const QString canonical = IsPortableApplication() ? canonicalKey(key) : key;
    const QString previous = storedKey(*this, key);
    if (previous != canonical) remove(previous);
    const bool portableBoolean = IsPortableApplication() && data.metaType().id() == QMetaType::Bool;
    QSettings::setValue(canonical, portableBoolean ? QVariant(data.toBool() ? 1 : 0) : data);
}
}
