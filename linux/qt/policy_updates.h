// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "http_download.h"
#include "platform/secure_boot_cache.h"
#include <QObject>

namespace rufus
{
struct PolicyUpdateSources
{
    QUrl textRoot = QUrl(QStringLiteral("https://rufus.ie/"));
    QList<DbxSource> dbx = BundledDbxSources();
};
struct PolicyUpdateResult
{
    int error = 0, listUpdates = 0, dbxUpdates = 0, failures = 0;
    bool dbxDeclined = false;
    SecureBootPolicy policy;
};
// One cancellable unprivileged policy refresh, no device/helper operations.
// At most one DBX consent request per start; text lists use the update policy.
class PolicyUpdates final : public QObject
{
    Q_OBJECT
public:
    explicit PolicyUpdates(QObject* parent = nullptr);
    ~PolicyUpdates() override;
    int start(const QString& directory, const SecureBootPolicy& current,
        const PolicyUpdateSources& sources = {}, int timeoutMs = 3500);
    void answerDbx(bool accept);
    void cancel();
    bool isActive() const { return active_; }
    bool waitingForDbx() const { return active_ && waiting_; }
signals:
    void dbxAvailable();
    void progress(QString message);
    void logMessage(QString message);
    void completed(rufus::PolicyUpdateResult result);
private:
    void next();
    void downloaded(rufus::HttpDownloadResult result);
    void finish(int error);
    int request(const QUrl& url, qsizetype maximum, const QByteArray& accept = {});
    HttpDownload download_;
    PolicyUpdateSources sources_;
    SecureBootPolicy policy_;
    QString directory_;
    int stage_ = 0, architectureIndex_ = 0, timeoutMs_ = 3500;
    int listUpdates_ = 0, dbxUpdates_ = 0, failures_ = 0;
    quint64 candidateTime_ = 0;
    bool active_ = false, waiting_ = false, dbxAccepted_ = false, dbxDeclined_ = false;
};
}
Q_DECLARE_METATYPE(rufus::PolicyUpdateResult)
