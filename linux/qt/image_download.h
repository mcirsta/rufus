// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "rufus_job.h"
#include <QCryptographicHash>
#include <QNetworkAccessManager>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <memory>
#include <cerrno>
#include <optional>
class QNetworkReply;
namespace rufus
{
class ImageSaveDestination;
struct ImageDownloadResult { int error=EIO; QString path,problem; quint64 bytes=0; QByteArray sha256; };
enum class ImageDownloadState { Idle,Running,Cancelling,Complete,Cancelled,Failed };
class ImageDownloadJob final : public RufusJob
{
    Q_OBJECT
public:
    ImageDownloadJob(quint64 request,const QUrl& url,const QString& path,int timeoutMs);
    ~ImageDownloadJob() override;
signals:
    void received(quint64 request,quint64 bytes,quint64 total);
    void imageReady(quint64 request,rufus::ImageDownloadResult result);
    void transferDone();
protected:
    void execute() override;
private:
    void consume();
    void replyFinished();
    void poll();
    void stop(int error);
    QNetworkAccessManager network_;
    QTimer timer_;
    QNetworkReply* reply_=nullptr;
    std::unique_ptr<ImageSaveDestination> destination_;
    QCryptographicHash hash_{QCryptographicHash::Sha256};
    QElapsedTimer activity_,report_;
    const quint64 request_;
    const QUrl url_;
    const QString path_;
    const int timeoutMs_;
    quint64 bytes_=0,total_=0;
    int error_=EIO;
    QString problem_;
    bool headers_=false,active_=false;
};
class ImageDownload final : public QObject
{
    Q_OBJECT
public:
    explicit ImageDownload(QObject* parent=nullptr);
    ~ImageDownload() override;
    bool start(const QUrl& url,const QString& path,int timeoutMs=30000);
    void cancel();
    void reset();
    bool isActive() const { return state_==ImageDownloadState::Running || state_==ImageDownloadState::Cancelling; }
    ImageDownloadState state() const { return state_; }
    const ImageDownloadResult& result() const { return result_; }
    quint64 received() const { return bytes_; }
    quint64 total() const { return total_; }
signals:
    void changed();
    void finished(); // Only after sync/cleanup and worker exit.
private:
    void acceptResult(quint64 request,rufus::ImageDownloadResult result);
    void acceptProgress(quint64 request,quint64 bytes,quint64 total);
    void threadFinished();
    QThread thread_;
    ImageDownloadState state_=ImageDownloadState::Idle;
    ImageDownloadResult result_;
    std::optional<ImageDownloadResult> pending_;
    quint64 request_=0,bytes_=0,total_=0;
};
}
Q_DECLARE_METATYPE(rufus::ImageDownloadResult)
