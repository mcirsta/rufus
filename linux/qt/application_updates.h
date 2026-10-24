// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "http_download.h"
#include <QObject>

namespace rufus
{
struct ApplicationRelease
{
    QString version, notes;
    QUrl page;
};
struct ApplicationUpdateSources
{
    QUrl api = QUrl(QStringLiteral("https://api.github.com"));
    QString repository = QStringLiteral("mcirsta/rufus");
};
struct ApplicationUpdateResult
{
    int error = 0;
    bool available = false, releaseFound = false;
    qint64 serverTime = 0;
    ApplicationRelease release;
};
// Native upstream triple uses the same unsigned-word ordering as Windows;
// a stable release follows previews of the same triple.
bool ParseApplicationVersion(const QString& text, quint16 (&upstream)[3], quint32& preview);
int CompareApplicationVersions(const QString& left, const QString& right);
bool ParseApplicationReleasePage(const QByteArray& data, const QString& repository,
    const QString& architecture, bool includeBetas, ApplicationRelease& newest, int& rows, QString& problem);
bool ApplicationUpdateDue(int interval, qint64 last, qint64 now);

class ApplicationUpdates final : public QObject
{
    Q_OBJECT
public:
    explicit ApplicationUpdates(QObject* parent = nullptr, const ApplicationUpdateSources& sources = {});
    int start(const QString& current, const QString& architecture, bool includeBetas, bool force = false, int timeoutMs = 3500);
    void cancel();
    void setVerbose(int level) { if (!active_) verbose_ = qBound(0,level,2); }
    bool isActive() const { return active_; }
signals:
    void completed(rufus::ApplicationUpdateResult result);
    void logMessage(QString message);
private:
    void requestPage();
    void downloaded(rufus::HttpDownloadResult result);
    void finish(int error);
    HttpDownload download_;
    ApplicationUpdateSources sources_;
    ApplicationRelease newest_;
    QString current_, architecture_;
    int page_ = 0, timeoutMs_ = 3500;
    bool includeBetas_ = false, force_ = false, active_ = false;
    qint64 serverTime_ = 0;
    int verbose_ = 0;
};
}
Q_DECLARE_METATYPE(rufus::ApplicationUpdateResult)
