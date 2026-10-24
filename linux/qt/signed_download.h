// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QByteArray>
#include "http_download.h"
#include <QObject>
#include <QUrl>

class QNetworkReply;

namespace rufus
{
struct SignedDownloadResult
{
    int error = 0;
    int httpStatus = 0; // Last response's actual HTTP status, not DownloadStatus.
    bool signatureRequest = false; // Identifies the failing request/verification stage.
    QByteArray bytes;
    QByteArray sha256;
};

// One unprivileged bootloader candidate, on this object's event-loop thread.
// Fetch URL then original URL + ".sig"; accept only verified owned bytes.
// No cache writes, prefix search, prompts, host detection or helper launches.
class SignedDownload final : public QObject
{
    Q_OBJECT
public:
    // Match Win32 NET_SESSION_TIMEOUT for connect/header/receive inactivity.
    static constexpr int defaultIdleTimeoutMs = 3500;
    explicit SignedDownload(QObject* parent = nullptr);
    ~SignedDownload() override;
    // 1..1 MiB payload limit. Require an HTTP(S) file URL without credentials,
    // query or fragment. Return EINVAL/EBUSY before starting, without a signal.
    int start(const QUrl& url, qsizetype maximumBytes, int idleTimeoutMs = defaultIdleTimeoutMs);
    void cancel();
    bool isActive() const { return active_; }

signals:
    // Payload transport progress only. Signature verification still follows 100%.
    void progress(quint64 received, quint64 total);
    // Exactly once for each accepted start; failures have empty bytes/hash.
    // Ready for another start before delivery. Destruction silently aborts.
    void completed(rufus::SignedDownloadResult result);

private:
    void downloadFinished(rufus::HttpDownloadResult result);
    void downloadProgress(quint64 received, quint64 total);
    void finish(int error);
    HttpDownload transport_;
    QUrl payloadUrl_;
    QByteArray payload_;
    int idleTimeoutMs_ = defaultIdleTimeoutMs;
    int httpStatus_ = 0;
    bool signature_ = false;
    bool active_ = false;
};
}
Q_DECLARE_METATYPE(rufus::SignedDownloadResult)
