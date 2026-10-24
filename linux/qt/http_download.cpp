// SPDX-License-Identifier: GPL-3.0-or-later
#include "http_download.h"
#include <QNetworkReply>
#include <QNetworkRequest>
#include <algorithm>
#include <cerrno>
#include <utility>

namespace rufus
{
namespace
{
constexpr qsizetype maximumPayloadBytes = 16 * 1024 * 1024;
constexpr qint64 readChunkBytes = 64 * 1024;
constexpr int httpOk = 200;

int networkError(QNetworkReply::NetworkError error)
{
    switch (error)
    {
    case QNetworkReply::HostNotFoundError: return EHOSTUNREACH;
    case QNetworkReply::ConnectionRefusedError: return ECONNREFUSED;
    case QNetworkReply::TimeoutError: return ETIMEDOUT;
    case QNetworkReply::OperationCanceledError: return ECANCELED;
    case QNetworkReply::TooManyRedirectsError: return ELOOP;
    case QNetworkReply::SslHandshakeFailedError:
    case QNetworkReply::InsecureRedirectError:
    case QNetworkReply::ProtocolUnknownError:
    case QNetworkReply::ProtocolInvalidOperationError:
    case QNetworkReply::ProtocolFailure: return EPROTO;
    default: return EIO;
    }
}
}

HttpDownload::HttpDownload(QObject* parent) : QObject(parent), network_(this), timer_(this)
{
    timer_.setSingleShot(true);
    connect(&timer_, &QTimer::timeout, this, &HttpDownload::timedOut);
}

HttpDownload::~HttpDownload()
{
    active_ = false;
    timer_.stop();
    retireReply();
}

int HttpDownload::start(const QUrl& url, qsizetype maximumBytes, int idleTimeoutMs,
    bool requireLength, const QByteArray& accept, const QByteArray& userAgent, const QByteArray& referer, bool allowEmpty)
{
    if (active_) return EBUSY;
    const bool http = url.scheme() == QStringLiteral("http") || url.scheme() == QStringLiteral("https");
    const bool endpoint = url.isValid() && http && !url.host().isEmpty() && url.userInfo().isEmpty() && !url.hasFragment();
    const bool bounded = maximumBytes > 0 && maximumBytes <= maximumPayloadBytes && idleTimeoutMs > 0;
    const bool safeReferer=referer.size()<=2048 && !referer.contains('\r') && !referer.contains('\n');
    if (!safeReferer) return EINVAL;
    if (!endpoint || !bounded || accept.contains('\r') || accept.contains('\n') || userAgent.contains('\r') || userAgent.contains('\n')) return EINVAL;
    maximumBytes_ = maximumBytes; idleTimeoutMs_ = idleTimeoutMs; requireLength_ = requireLength; allowEmpty_=allowEmpty;
    body_.clear(); declaredBytes_ = -1; headersRead_ = false; httpStatus_ = 0; active_ = true;
    QNetworkRequest request(url);
    request.setRawHeader("Accept-Encoding", "identity");
    request.setRawHeader("User-Agent", userAgent.isEmpty() ? QByteArray("Rufus/Linux") : userAgent);
    if (!accept.isEmpty()) request.setRawHeader("Accept", accept);
    if (!referer.isEmpty()) request.setRawHeader("Referer",referer);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    request.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setAttribute(QNetworkRequest::AutoDeleteReplyOnFinishAttribute, false);
    reply_ = network_.get(request); reply_->setReadBufferSize(readChunkBytes);
    connect(reply_, &QNetworkReply::metaDataChanged, this, &HttpDownload::receiveData);
    connect(reply_, &QNetworkReply::readyRead, this, &HttpDownload::receiveData);
    connect(reply_, &QNetworkReply::finished, this, &HttpDownload::replyFinished);
    timer_.start(idleTimeoutMs_); return 0;
}

int HttpDownload::consumeReply()
{
    httpStatus_ = reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    // Qt follows redirects internally. Intermediate headers/bodies are not
    // payload bytes; finished() handles an unfollowed redirect as a failure.
    const bool redirect = httpStatus_ >= 300 && httpStatus_ < 400;
    if (redirect || httpStatus_ == 0)
        return 0;
    if (httpStatus_ != httpOk && !(allowEmpty_ && httpStatus_==204))
    {
        if (httpStatus_ == 404) return ENOENT;
        if (httpStatus_ == 401 || httpStatus_ == 403) return EACCES;
        return EPROTO;
    }
    if (!headersRead_)
    {
        const QByteArray encoding = reply_->rawHeader("Content-Encoding").trimmed();
        const bool identity = encoding.isEmpty() || encoding.compare("identity", Qt::CaseInsensitive) == 0;
        if (!identity || reply_->attribute(QNetworkRequest::OriginalContentLengthAttribute).isValid()) return EBADMSG;
        const QByteArray length = reply_->rawHeader("Content-Length");
        if (length.isEmpty()) {
            if (requireLength_) return EBADMSG;
        } else {
            bool valid = false; const quint64 size = length.toULongLong(&valid);
            if (!valid || (size == 0 && !allowEmpty_)) return EBADMSG;
            if (size > quint64(maximumBytes_)) return EFBIG;
            declaredBytes_ = qint64(size); body_.reserve(qsizetype(size));
        }
        headersRead_ = true;
    }
    while (reply_->bytesAvailable() > 0)
    {
        const qint64 remaining = (declaredBytes_ < 0 ? maximumBytes_ : declaredBytes_) - body_.size();
        if (reply_->bytesAvailable() > remaining)
            return EBADMSG;
        const QByteArray chunk = reply_->read(std::min(readChunkBytes, reply_->bytesAvailable()));
        if (chunk.isEmpty())
            return EIO;
        body_.append(chunk);
        timer_.start(idleTimeoutMs_);
    }
    return 0;
}

void HttpDownload::receiveData()
{
    if (!active_ || sender() != reply_)
        return;
    timer_.start(idleTimeoutMs_);
    const int error = consumeReply();
    if (error != 0)
        finish(error);
    else if (headersRead_)
        emit progress(quint64(body_.size()), declaredBytes_ > 0 ? quint64(declaredBytes_) : 0);
}

void HttpDownload::replyFinished()
{
    if (!active_ || sender() != reply_)
        return;
    int error = consumeReply();
    if (error == 0 && reply_->error() != QNetworkReply::NoError)
        error = networkError(reply_->error());
    if (error == 0 && httpStatus_ != httpOk && !(allowEmpty_ && httpStatus_==204))
        error = EPROTO;
    if (error == 0 && ((!allowEmpty_ && body_.isEmpty()) || (declaredBytes_ >= 0 && body_.size() != declaredBytes_)))
        error = EIO;
    if (error != 0)
    {
        finish(error);
        return;
    }
    finish(0);
}

void HttpDownload::cancel()
{
    if (active_)
        finish(ECANCELED);
}

void HttpDownload::timedOut()
{
    if (active_)
        finish(ETIMEDOUT);
}

void HttpDownload::retireReply()
{
    QNetworkReply* previous = reply_;
    reply_ = nullptr;
    if (previous == nullptr)
        return;
    disconnect(previous, nullptr, this, nullptr);
    if (!previous->isFinished())
        previous->abort();
    previous->deleteLater();
}

void HttpDownload::finish(int error)
{
    if (!active_)
        return;
    HttpDownloadResult result;
    result.error = error;
    result.httpStatus = httpStatus_;
    if (reply_ != nullptr) result.date = reply_->rawHeader("Date");
    if (error == 0) result.bytes = std::move(body_);
    active_ = false;
    timer_.stop();
    retireReply();
    body_.clear();
    emit completed(std::move(result));
}
}
