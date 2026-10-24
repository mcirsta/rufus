// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QByteArray>
#include <QNetworkAccessManager>
#include <QObject>
#include <QTimer>
#include <QUrl>

class QNetworkReply;
namespace rufus
{
struct HttpDownloadResult
{
    int error = 0, httpStatus = 0;
    QByteArray bytes;
    QByteArray date;
};
// One bounded HTTP(S) response. TLS errors remain errors; redirects cannot
// downgrade HTTPS. Callers choose mandatory length and additional Accept data.
class HttpDownload final : public QObject
{
    Q_OBJECT
public:
    explicit HttpDownload(QObject* parent = nullptr);
    ~HttpDownload() override;
    int start(const QUrl& url, qsizetype maximumBytes, int idleTimeoutMs = 3500,
        bool requireLength = true, const QByteArray& accept = {}, const QByteArray& userAgent = {}, const QByteArray& referer = {}, bool allowEmpty = false);
    void cancel();
    bool isActive() const { return active_; }
signals:
    void progress(quint64 received, quint64 total);
    void completed(rufus::HttpDownloadResult result);
private:
    int consumeReply();
    void receiveData();
    void replyFinished();
    void timedOut();
    void finish(int error);
    void retireReply();
    QNetworkAccessManager network_;
    QTimer timer_;
    QNetworkReply* reply_ = nullptr;
    QByteArray body_;
    qsizetype maximumBytes_ = 0;
    qint64 declaredBytes_ = -1;
    int idleTimeoutMs_ = 3500, httpStatus_ = 0;
    bool active_ = false, requireLength_ = true, headersRead_ = false, allowEmpty_ = false;
};
}
Q_DECLARE_METATYPE(rufus::HttpDownloadResult)
