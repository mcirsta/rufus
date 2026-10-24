// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QTemporaryFile>

namespace rufus
{
// Keep the visible tail bounded without discarding the saved session log.
class ApplicationLog
{
public:
    bool append(const QString& message);
    bool clear();
    bool save(const QString& path, bool appendPrevious, QString& problem);
private:
    QTemporaryFile spool_;
    QString problem_;
};
}
