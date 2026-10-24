// SPDX-License-Identifier: GPL-3.0-or-later
#include "application_instance.h"
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <climits>
#include <unistd.h>

namespace rufus
{
namespace
{
QString runtimeDirectory(const QString& selected)
{
    if (!selected.isEmpty()) return selected;
    const QString runtime = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (!runtime.isEmpty()) return runtime;
    return QDir::temp().filePath(QStringLiteral("rufus-%1").arg(getuid()));
}
}

ApplicationInstance::ApplicationInstance(const QString& directory)
    : directory_(runtimeDirectory(directory)), lock_(QDir(directory_).filePath(QStringLiteral("rufus-linux.lock")))
{
    // A long-running write must never be treated as stale because of its age.
    // QLockFile still checks whether the owner process has exited.
    lock_.setStaleLockTime(0);
}

bool ApplicationInstance::acquire(int waitTicks, QString& problem)
{
    problem.clear();
    if (waitTicks < 0 || waitTicks > INT_MAX / 100) {
        problem = QStringLiteral("Invalid application wait timeout."); return false;
    }
    const QFileInfo directory(directory_);
    if (!QDir().mkpath(directory_) || directory.isSymLink()) {
        problem = QStringLiteral("Cannot create the application lock directory: %1").arg(directory_); return false;
    }
    const QFileInfo current(directory_);
    const bool privateDirectory = current.isDir() && !current.isSymLink() && current.ownerId() == getuid()
        && !(current.permissions() & (QFileDevice::WriteGroup | QFileDevice::WriteOther));
    if (!privateDirectory) {
        problem = QStringLiteral("The application lock directory is not private: %1").arg(directory_); return false;
    }
    if (lock_.tryLock(waitTicks * 100)) return true;
    problem = lock_.error() == QLockFile::LockFailedError
        ? QStringLiteral("Another Rufus instance is running.")
        : QStringLiteral("Cannot acquire the Rufus application lock in %1.").arg(directory_);
    return false;
}
}
