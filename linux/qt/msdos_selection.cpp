// SPDX-License-Identifier: GPL-3.0-or-later
#include "msdos_selection.h"
#include <QDir>
#include <cerrno>
#include <unistd.h>

namespace rufus
{
MsDosSelection::MsDosSelection(QObject* parent) : QObject(parent), download_(this)
{
    connect(&download_,&HttpDownload::completed,this,&MsDosSelection::downloaded);
    connect(&download_,&HttpDownload::progress,this,&MsDosSelection::progress);
}
int MsDosSelection::start(const QString& directory, const QUrl& url)
{
    if (active_) return EBUSY;
    const bool secure = url.scheme() == QStringLiteral("https");
    const bool loopback = url.scheme() == QStringLiteral("http") && url.host() == QStringLiteral("127.0.0.1");
    const bool endpoint = url.isValid() && !url.host().isEmpty() && url.userInfo().isEmpty() && !url.hasQuery() && !url.hasFragment();
    if (!QDir::isAbsolutePath(directory) || directory.contains(QChar::Null) || !endpoint || (!secure && !loopback)) return EINVAL;
    directory_ = directory; url_ = url; active_ = true; waiting_ = false;
    QByteArray bytes;
    const int error = ReadMsDosImage(QDir(directory).filePath(QStringLiteral("diskcopy.dll")),getuid(),bytes);
    if (error == ENOENT) { waiting_ = true; emit downloadRequired(); }
    else finish(error,std::move(bytes));
    return 0;
}
void MsDosSelection::choose(bool accept)
{
    if (!active_ || !waiting_) return;
    waiting_ = false;
    if (!accept) { finish(ECANCELED); return; }
    const int error = download_.start(url_,DISKCOPY_SIZE,10000,true,{},SYMBOL_SERVER_USER_AGENT);
    if (error != 0) finish(error);
}
void MsDosSelection::downloaded(HttpDownloadResult result)
{
    if (!active_) return;
    int error = result.error;
    if (error == 0) error = StoreMsDosImage(directory_,result.bytes);
    if (error == 0) {
        QByteArray checked;
        error = ReadMsDosImage(QDir(directory_).filePath(QStringLiteral("diskcopy.dll")),getuid(),checked);
        if (error == 0 && checked != result.bytes) error = ESTALE;
    }
    finish(error,std::move(result.bytes));
}
void MsDosSelection::cancel()
{
    if (!active_) return;
    active_ = false; download_.cancel(); active_ = true; finish(ECANCELED);
}
void MsDosSelection::finish(int error, QByteArray bytes)
{
    if (!active_) return;
    MsDosSelectionResult result; result.error = error;
    if (error == 0) {
        result.path = QDir(directory_).filePath(QStringLiteral("diskcopy.dll"));
        result.bytes = std::move(bytes);
    }
    active_ = waiting_ = false; emit completed(std::move(result));
}
}
