// SPDX-License-Identifier: GPL-3.0-or-later
#include "policy_updates.h"
#include <QDir>
#include <QSet>
#include <cerrno>

namespace rufus
{
namespace
{
const char* feeds[] = {"sbat_level.txt", "sb_active.txt", "sb_revoked.txt"};
constexpr int firstCommitStage = 3, dbxBodyStage = 4;
bool validEndpoint(const QUrl& url)
{
    const bool secure = url.scheme() == QStringLiteral("https");
    const bool loopback = url.scheme() == QStringLiteral("http")
        && (url.host() == QStringLiteral("127.0.0.1") || url.host() == QStringLiteral("localhost") || url.host() == QStringLiteral("::1"));
    return url.isValid() && !url.host().isEmpty() && (secure || loopback) && url.userInfo().isEmpty() && !url.hasFragment() && !url.hasQuery();
}
}
PolicyUpdates::PolicyUpdates(QObject* parent) : QObject(parent), download_(this)
{
    connect(&download_, &HttpDownload::completed, this, &PolicyUpdates::downloaded);
}
PolicyUpdates::~PolicyUpdates()
{
    active_ = false; disconnect(&download_, nullptr, this, nullptr); download_.cancel();
}
int PolicyUpdates::start(const QString& directory, const SecureBootPolicy& current, const PolicyUpdateSources& sources, int timeoutMs)
{
    if (active_) return EBUSY;
    if (!QDir::isAbsolutePath(directory) || directory.contains(QChar::Null) || !validEndpoint(sources.textRoot) || timeoutMs <= 0) return EINVAL;
    QSet<int> architectures;
    for (const auto& dbx : sources.dbx) {
        const bool architecture = dbx.architecture >= 1 && dbx.architecture <= 7 && !architectures.contains(dbx.architecture);
        if (!architecture || !validEndpoint(dbx.url) || !DbxCommitUrl(dbx.url).isValid()) return EINVAL;
        architectures.insert(dbx.architecture);
    }
    directory_ = directory; policy_ = current; sources_ = sources; timeoutMs_ = timeoutMs;
    stage_ = architectureIndex_ = listUpdates_ = dbxUpdates_ = failures_ = 0;
    active_ = true; waiting_ = dbxAccepted_ = dbxDeclined_ = false;
    next(); return 0;
}
int PolicyUpdates::request(const QUrl& url, qsizetype maximum, const QByteArray& accept)
{
    const int error = download_.start(url, maximum, timeoutMs_, false, accept);
    if (error == 0) emit progress(stage_ < firstCommitStage ? tr("Checking Secure Boot policy lists…") : tr("Checking for UEFI DBX updates…"));
    return error;
}
void PolicyUpdates::next()
{
    if (!active_ || waiting_) return;
    if (stage_ < firstCommitStage) {
        const int error = request(sources_.textRoot.resolved(QUrl(QString::fromLatin1(feeds[stage_]))), 1023);
        if (error != 0) finish(error);
        return;
    }
    if (architectureIndex_ >= sources_.dbx.size()) { finish(0); return; }
    const auto source = sources_.dbx[architectureIndex_];
    const QUrl url = stage_ == firstCommitStage ? DbxCommitUrl(source.url) : source.url;
    const int error = request(url, stage_ == firstCommitStage ? 1024 * 1024 : 16 * 1024 * 1024,
        stage_ == firstCommitStage ? QByteArray("application/vnd.github+json") : QByteArray("application/vnd.github.v3.raw"));
    if (error != 0) finish(error);
}
void PolicyUpdates::downloaded(HttpDownloadResult response)
{
    if (!active_) return;
    int error = response.error;
    if (stage_ < firstCommitStage) {
        if (error == 0) error = ValidateSecureBootList(stage_, response.bytes);
        if (error == 0) {
            if (stage_ == 0) policy_.sbat = response.bytes;
            else if (stage_ == 1) policy_.activeCertificates = response.bytes;
            else policy_.revokedCertificates = response.bytes;
            ++listUpdates_;
        } else { ++failures_; emit logMessage(tr("Could not update %1; keeping the current policy.").arg(QString::fromLatin1(feeds[stage_]))); }
        ++stage_; next(); return;
    }
    const auto source = sources_.dbx[architectureIndex_];
    if (stage_ == firstCommitStage) {
        candidateTime_ = error == 0 ? DbxCommitTimestamp(response.bytes) : 0;
        QByteArray cached; quint64 cachedTime = 0;
        if (ReadSecureBootDbxCache(directory_, source.architecture, cached, cachedTime) != 0) cachedTime = 0;
        const bool unavailableArchitecture = source.timestamp == 0 && policy_.dbx.value(source.architecture).isEmpty()
            && error == ENOENT;
        if (candidateTime_ == 0 && !unavailableArchitecture) ++failures_;
        if (candidateTime_ <= qMax(cachedTime, source.timestamp)) { ++architectureIndex_; next(); return; }
        stage_ = dbxBodyStage;
        if (!dbxAccepted_) { waiting_ = true; emit dbxAvailable(); return; }
        next(); return;
    }
    if (error == 0) error = StoreSecureBootDbxCache(directory_, source.architecture, response.bytes, candidateTime_);
    if (error == 0) { policy_.dbx.insert(source.architecture,response.bytes); ++dbxUpdates_; }
    else if (error != EALREADY) { ++failures_; emit logMessage(tr("Could not update DBX for architecture %1; keeping the current policy.").arg(source.architecture)); }
    ++architectureIndex_; stage_ = firstCommitStage; next();
}
void PolicyUpdates::answerDbx(bool accept)
{
    if (!active_ || !waiting_) return;
    waiting_ = false;
    if (!accept) { dbxDeclined_ = true; finish(0); return; }
    dbxAccepted_ = true; next();
}
void PolicyUpdates::cancel()
{
    if (!active_) return;
    active_ = false; download_.cancel(); active_ = true; finish(ECANCELED);
}
void PolicyUpdates::finish(int error)
{
    if (!active_) return;
    PolicyUpdateResult result;
    result.error = error; result.listUpdates = listUpdates_; result.dbxUpdates = dbxUpdates_;
    result.failures = failures_; result.dbxDeclined = dbxDeclined_; result.policy = policy_;
    active_ = waiting_ = false; emit completed(std::move(result));
}
}
