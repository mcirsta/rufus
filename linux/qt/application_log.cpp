// SPDX-License-Identifier: GPL-3.0-or-later
#include "application_log.h"
#include <QFile>
#include <QSaveFile>
#include <array>

namespace rufus
{
namespace
{
constexpr qint64 maximumLogBytes = 0x7ffffffe; // Win32 MAX_LOG_SIZE.

bool copyLog(QIODevice& source, QIODevice& destination, QString& problem)
{
    std::array<char, 64 * 1024> buffer;
    while (!source.atEnd()) {
        const qint64 count = source.read(buffer.data(), buffer.size());
        if (count < 0) { problem = source.errorString(); return false; }
        if (count == 0) { problem = QStringLiteral("Cannot finish reading the session log."); return false; }
        if (destination.write(buffer.data(), count) != count) { problem = destination.errorString(); return false; }
    }
    return true;
}
}

bool ApplicationLog::append(const QString& message)
{
    if (!problem_.isEmpty()) return false;
    if (!spool_.isOpen() && !spool_.open()) { problem_ = spool_.errorString(); return false; }
    const QByteArray bytes = message.toUtf8() + '\n';
    const qint64 used = spool_.size();
    const bool fits = used >= 0 && used <= maximumLogBytes && bytes.size() <= maximumLogBytes - used;
    if (!fits) { problem_ = QStringLiteral("The session log exceeds Rufus's size limit."); return false; }
    if (!spool_.seek(used) || spool_.write(bytes) != bytes.size()) { problem_ = spool_.errorString(); return false; }
    return true;
}

bool ApplicationLog::clear()
{
    if (spool_.isOpen() && (!spool_.resize(0) || !spool_.seek(0))) { problem_ = spool_.errorString(); return false; }
    problem_.clear();
    return true;
}

bool ApplicationLog::save(const QString& path, bool appendPrevious, QString& problem)
{
    problem = problem_;
    if (!problem.isEmpty()) return false;
    QSaveFile destination(path);
    if (!destination.open(QIODevice::WriteOnly)) { problem = destination.errorString(); return false; }
    // Copy the previous log into the atomic replacement. An export failure
    // must leave the earlier sessions intact, including on a full disk.
    QFile previous(path);
    if (appendPrevious && previous.exists()) {
        if (!previous.open(QIODevice::ReadOnly)) { problem = previous.errorString(); return false; }
        if (!copyLog(previous, destination, problem)) return false;
        if (previous.size() > 0 && destination.write("\n", 1) != 1) { problem = destination.errorString(); return false; }
    }
    if (spool_.isOpen()) {
        if (!spool_.seek(0)) { problem = spool_.errorString(); return false; }
        if (!copyLog(spool_, destination, problem)) return false;
    }
    if (!destination.commit()) { problem = destination.errorString(); return false; }
    return true;
}
}
