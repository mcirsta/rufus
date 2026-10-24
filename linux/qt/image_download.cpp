// SPDX-License-Identifier: GPL-3.0-or-later
#include "image_download.h"
#include "platform/image_save_destination.h"
#include "platform/iso_export.h"
#include "platform/rufus_reporting.h"
#include <QEventLoop>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <cerrno>
#include <cstring>
#include <limits>
#include <unistd.h>
namespace rufus
{
namespace
{
constexpr qint64 transferChunkBytes=64*1024;
constexpr int cancelPollMs=50,reportIntervalMs=100;
QString downloadProblem(int error)
{
    if (error==0) return {};
    if (error==ECANCELED) return ImageDownload::tr("Download cancelled.");
    return ImageDownload::tr("Could not save the downloaded image: %1").arg(QString::fromLocal8Bit(std::strerror(error)));
}
}
ImageDownloadJob::ImageDownloadJob(quint64 request,const QUrl& url,const QString& path,int timeoutMs)
    : network_(this),timer_(this),request_(request),url_(url),path_(path),timeoutMs_(timeoutMs)
{
    qRegisterMetaType<ImageDownloadResult>();
    timer_.setInterval(cancelPollMs); connect(&timer_,&QTimer::timeout,this,&ImageDownloadJob::poll);
}
ImageDownloadJob::~ImageDownloadJob()
{
    active_=false;
    if (reply_) { disconnect(reply_,nullptr,this,nullptr); reply_->abort(); }
}
void ImageDownloadJob::execute()
{
    destination_=std::make_unique<ImageSaveDestination>(path_);
    int error=destination_->open();
    if (error==0) error=destination_->create(ImageSaveIso);
    if (error==0 && IsRufusJobCancelled()) error=ECANCELED;
    if (error==0) {
        QNetworkRequest request(url_);
        request.setRawHeader("Accept-Encoding","identity"); request.setRawHeader("User-Agent","Rufus/Linux");
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,QNetworkRequest::NoLessSafeRedirectPolicy);
        request.setAttribute(QNetworkRequest::CookieLoadControlAttribute,QNetworkRequest::Manual);
        request.setAttribute(QNetworkRequest::CookieSaveControlAttribute,QNetworkRequest::Manual);
        request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,QNetworkRequest::AlwaysNetwork);
        request.setAttribute(QNetworkRequest::CacheSaveControlAttribute,false);
        reply_=network_.get(request); reply_->setReadBufferSize(transferChunkBytes);
        connect(reply_,&QNetworkReply::metaDataChanged,this,&ImageDownloadJob::consume);
        connect(reply_,&QNetworkReply::readyRead,this,&ImageDownloadJob::consume);
        connect(reply_,&QNetworkReply::finished,this,&ImageDownloadJob::replyFinished);
        QEventLoop loop; connect(this,&ImageDownloadJob::transferDone,&loop,&QEventLoop::quit,Qt::DirectConnection);
        active_=true; activity_.start(); report_.start(); timer_.start(); loop.exec(); if (active_) stop(ECANCELED); error=error_;
    }
    if (error==0) error=destination_->commit();
    const int cleanup=destination_->close(); if (cleanup!=0) error=cleanup;
    ImageDownloadResult result; result.error=error; result.path=path_; result.bytes=bytes_; result.problem=problem_.isEmpty() ? downloadProblem(error) : problem_;
    if (error!=0 && !destination_->recoveryPath().isEmpty())
        result.problem+=tr(" The previous file is preserved at: %1").arg(destination_->recoveryPath());
    if (error==0) result.sha256=hash_.result();
    emit imageReady(request_,result);
}
void ImageDownloadJob::consume()
{
    if (!active_) return;
    if (IsRufusJobCancelled()) { stop(ECANCELED); return; }
    const int status=reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status==0 || (status>=300 && status<400)) return;
    if (status!=200) { problem_=tr("The download server returned HTTP %1.").arg(status); stop(status==403 ? EACCES : EIO); return; }
    if (!headers_) {
        bool valid=false;
        const qint64 length=reply_->rawHeader("Content-Length").toLongLong(&valid);
        const QByteArray encoding=reply_->rawHeader("Content-Encoding").trimmed().toLower();
        if (!valid || length<=0 || (!encoding.isEmpty() && encoding!="identity")) { stop(EBADMSG); return; }
        total_=quint64(length); headers_=true;
    }
    while (active_ && reply_->bytesAvailable()>0) {
        if (IsRufusJobCancelled()) { stop(ECANCELED); return; }
        const QByteArray data=reply_->read(transferChunkBytes);
        if (data.isEmpty()) { stop(EIO); return; }
        if (quint64(data.size())>total_-bytes_) { stop(EFBIG); return; }
        qsizetype done=0;
        while (done<data.size()) {
            const ssize_t count=write(destination_->descriptor(),data.constData()+done,size_t(data.size()-done));
            if (count<0 && errno==EINTR) continue;
            if (count<=0) { stop(count<0 ? errno : EIO); return; }
            done+=count;
        }
        hash_.addData(data); bytes_+=quint64(data.size()); activity_.restart();
        if (report_.elapsed()>=reportIntervalMs) { emit received(request_,bytes_,total_); report_.restart(); }
    }
}
void ImageDownloadJob::replyFinished()
{
    if (!active_) return;
    consume(); if (!active_) return;
    if (reply_->error()!=QNetworkReply::NoError) {
        const auto error=reply_->error();
        problem_=tr("Download connection failed: %1").arg(reply_->errorString().left(512));
        stop(error==QNetworkReply::SslHandshakeFailedError || error==QNetworkReply::InsecureRedirectError ? EPROTO : EIO); return;
    }
    if (!headers_ || bytes_!=total_) { stop(EBADMSG); return; }
    emit received(request_,bytes_,total_); stop(0);
}
void ImageDownloadJob::poll()
{
    if (IsRufusJobCancelled()) stop(ECANCELED);
    else if (activity_.elapsed()>=timeoutMs_) stop(ETIMEDOUT);
}
void ImageDownloadJob::stop(int error)
{
    if (!active_) return;
    active_=false; error_=error; timer_.stop();
    disconnect(reply_,nullptr,this,nullptr); reply_->abort(); reply_->deleteLater(); reply_=nullptr;
    emit transferDone();
}
ImageDownload::ImageDownload(QObject* parent) : QObject(parent)
{
    connect(&thread_,&QThread::finished,this,&ImageDownload::threadFinished,Qt::QueuedConnection);
}
ImageDownload::~ImageDownload()
{
    thread_.requestInterruption(); thread_.quit(); thread_.wait();
}
bool ImageDownload::start(const QUrl& url,const QString& path,int timeoutMs)
{
    if (isActive()) return false;
    const bool endpoint=url.isValid() && !url.host().isEmpty() && url.userInfo().isEmpty() && !url.hasFragment();
    const bool transport=url.scheme()=="https" || (url.scheme()=="http" && url.host()=="127.0.0.1");
    if (!endpoint || !transport || path.isEmpty() || path.contains(QChar::Null) || timeoutMs<=0) return false;
    ++request_; pending_.reset(); result_={}; bytes_=total_=0; state_=ImageDownloadState::Running;
    auto* job=new ImageDownloadJob(request_,url,path,timeoutMs); job->moveToThread(&thread_);
    connect(&thread_,&QThread::started,job,&RufusJob::run);
    connect(job,&ImageDownloadJob::imageReady,this,&ImageDownload::acceptResult,Qt::QueuedConnection);
    connect(job,&ImageDownloadJob::received,this,&ImageDownload::acceptProgress,Qt::QueuedConnection);
    connect(job,&RufusJob::finished,&thread_,&QThread::quit,Qt::DirectConnection);
    connect(&thread_,&QThread::finished,job,&QObject::deleteLater);
    thread_.start(); emit changed(); return true;
}
void ImageDownload::cancel()
{
    if (state_!=ImageDownloadState::Running) return;
    state_=ImageDownloadState::Cancelling; pending_.reset(); thread_.requestInterruption(); emit changed();
}
void ImageDownload::reset()
{
    if (isActive() || state_==ImageDownloadState::Idle) return;
    state_=ImageDownloadState::Idle; pending_.reset(); result_={}; bytes_=total_=0; emit changed();
}
void ImageDownload::acceptResult(quint64 request,ImageDownloadResult result)
{
    if (request==request_ && state_==ImageDownloadState::Running) pending_=std::move(result);
}
void ImageDownload::acceptProgress(quint64 request,quint64 bytes,quint64 total)
{
    if (request!=request_ || state_!=ImageDownloadState::Running || bytes<bytes_ || bytes>total) return;
    bytes_=bytes; total_=total; emit changed();
}
void ImageDownload::threadFinished()
{
    thread_.wait();
    if (state_==ImageDownloadState::Cancelling) { result_={}; result_.error=ECANCELED; }
    else if (pending_) result_=*pending_;
    else { result_={}; result_.error=EIO; }
    pending_.reset(); if (result_.problem.isEmpty()) result_.problem=downloadProblem(result_.error);
    state_=result_.error==0 ? ImageDownloadState::Complete : result_.error==ECANCELED ? ImageDownloadState::Cancelled : ImageDownloadState::Failed;
    emit changed(); emit finished();
}
}
