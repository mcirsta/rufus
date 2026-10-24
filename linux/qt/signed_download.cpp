// SPDX-License-Identifier: GPL-3.0-or-later
#include "signed_download.h"
#include "platform/download_signature.h"
#include "download_key.h"
#include <QCryptographicHash>
#include <cerrno>
#include <utility>

namespace rufus
{
SignedDownload::SignedDownload(QObject* parent) : QObject(parent), transport_(this)
{
    connect(&transport_, &HttpDownload::completed, this, &SignedDownload::downloadFinished);
    connect(&transport_, &HttpDownload::progress, this, &SignedDownload::downloadProgress);
}
SignedDownload::~SignedDownload()
{
    active_ = false; disconnect(&transport_, nullptr, this, nullptr); transport_.cancel();
}
int SignedDownload::start(const QUrl& url, qsizetype maximumBytes, int idleTimeoutMs)
{
    if (active_) return EBUSY;
    const bool plainFile = !url.path().isEmpty() && !url.path().endsWith(QChar(u'/')) && !url.hasQuery();
    if (!plainFile || maximumBytes <= 0 || maximumBytes > 1024 * 1024) return EINVAL;
    const int error = transport_.start(url, maximumBytes, idleTimeoutMs);
    if (error != 0) return error;
    payloadUrl_ = url; payload_.clear(); signature_ = false; active_ = true;
    idleTimeoutMs_ = idleTimeoutMs; return 0;
}
void SignedDownload::downloadProgress(quint64 received, quint64 total)
{
    if (active_ && !signature_) emit progress(received, total);
}
void SignedDownload::downloadFinished(HttpDownloadResult response)
{
    if (!active_) return;
    httpStatus_ = response.httpStatus;
    if (response.error != 0) { finish(signature_ && response.error == EFBIG ? EBADMSG : response.error); return; }
    if (!signature_) {
        payload_ = std::move(response.bytes); signature_ = true;
        const int error = transport_.start(QUrl::fromEncoded(payloadUrl_.toEncoded() + ".sig"), RUFUS_DOWNLOAD_SIGNATURE_BYTES, idleTimeoutMs_);
        if (error != 0) { finish(error); return; }
        emit progress(quint64(payload_.size()), quint64(payload_.size())); return;
    }
    int error = response.bytes.size() == RUFUS_DOWNLOAD_SIGNATURE_BYTES ? 0 : EBADMSG;
    if (error == 0) error = VerifyRufusDownloadSignature(payload_.constData(), size_t(payload_.size()),
        response.bytes.constData(), size_t(response.bytes.size()));
    finish(error);
}
void SignedDownload::cancel() { if (active_) transport_.cancel(); }
void SignedDownload::finish(int error)
{
    if (!active_) return;
    SignedDownloadResult result; result.error = error; result.httpStatus = httpStatus_; result.signatureRequest = signature_;
    if (error == 0) { result.bytes = std::move(payload_); result.sha256 = QCryptographicHash::hash(result.bytes, QCryptographicHash::Sha256); }
    active_ = false; payload_.clear(); emit completed(std::move(result));
}
}
