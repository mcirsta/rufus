// SPDX-License-Identifier: GPL-3.0-or-later
#include "grub2_core_selection.h"
#include "grub_inspect.h"
#include "../../res/grub/grub_version.h"
#include <QDir>
#include <cerrno>
#include <unistd.h>
#include <utility>

namespace rufus
{
namespace
{
constexpr quint64 sectorBytes = 512;
constexpr quint64 maximumPartitionOffset = 1024 * 1024;

bool validServerRoot(const QUrl& server)
{
    const bool http = server.scheme() == QStringLiteral("http") || server.scheme() == QStringLiteral("https");
    return server.isValid() && http && !server.host().isEmpty()
        && server.userInfo().isEmpty() && !server.hasQuery() && !server.hasFragment();
}
}

Grub2CoreSelection::Grub2CoreSelection(QObject* parent) : QObject(parent), download_(this)
{
    connect(&download_, &SignedDownload::completed, this, &Grub2CoreSelection::downloadFinished);
    connect(&download_, &SignedDownload::progress, this, &Grub2CoreSelection::progress);
}

Grub2CoreSelection::~Grub2CoreSelection()
{
    active_ = false;
    download_.cancel();
}

int Grub2CoreSelection::start(const PrepareImageRequest& source, const RUFUS_IMG_REPORT& report,
    const QString& filesDirectory, bool mbr, bool bios, quint64 partitionOffset, const QUrl& server)
{
    if (active_)
        return EBUSY;
    const bool sourcePath = QDir::isAbsolutePath(source.imagePath) && !source.imagePath.contains(QChar(u'\0'));
    const bool cacheDirectory = QDir::isAbsolutePath(filesDirectory) && !filesDirectory.contains(QChar(u'\0'));
    if (!sourcePath || !cacheDirectory)
        return EINVAL;
    const bool gap = partitionOffset > sectorBytes && partitionOffset <= maximumPartitionOffset
        && partitionOffset % sectorBytes == 0;
    const bool serverRoot = validServerRoot(server);
    if (!gap || !serverRoot)
        return EINVAL;
    grub4dos_ = false;
    request_ = source;
    report_ = report;
    filesDirectory_ = filesDirectory;
    server_ = server;
    partitionOffset_ = partitionOffset;
    mbr_ = mbr;
    bios_ = bios;
    before_ = sizeof(report_.grub2_version);
    result_ = {};
    active_ = true;
    waiting_ = false;
    signatureRejected_ = false;
    source_ = std::make_unique<PrepareIsoSource>(request_);
    const int sourceError = source_->open(getuid());
    if (sourceError != 0)
    {
        finish(sourceError);
        return 0;
    }
    Grub2CachedCore core;
    const int error = ReadGrub2CoreCache(filesDirectory_, report_, mbr_, bios_, partitionOffset_, core);
    result_.cachePath = core.path;
    result_.candidateError = error;
    const bool stop = error == ECANCELED || error == ESTALE;
    if (stop)
        finish(error);
    else if (core.state == Grub2CacheState::Missing)
    {
        waiting_ = true;
        emit downloadRequired(QString::fromLatin1(report_.grub2_version));
    }
    else
    {
        if (core.state == Grub2CacheState::Ready)
        {
            result_.origin = Grub2CoreOrigin::Cache;
            result_.bytes = std::move(core.bytes);
            result_.sha256 = std::move(core.sha256);
        }
        finish(0);
    }
    return 0;
}

int Grub2CoreSelection::startGrub4dos(const QString& filesDirectory, const QUrl& server)
{
    if (active_)
        return EBUSY;
    const bool cacheDirectory = QDir::isAbsolutePath(filesDirectory) && !filesDirectory.contains(QChar(u'\0'));
    if (!cacheDirectory || !validServerRoot(server))
        return EINVAL;
    grub4dos_ = true;
    request_ = {};
    report_ = {};
    source_.reset();
    filesDirectory_ = filesDirectory;
    server_ = server;
    result_ = {};
    active_ = true;
    waiting_ = false;
    signatureRejected_ = false;
    Grub2CachedCore loader;
    const int error = ReadGrub4dosLoaderCache(filesDirectory_, loader);
    result_.cachePath = loader.path;
    result_.candidateError = error;
    if (error == ECANCELED || error == ESTALE)
        finish(error);
    else if (loader.state == Grub2CacheState::Missing)
    {
        waiting_ = true;
        emit downloadRequired(QStringLiteral(GRUB4DOS_VERSION));
    }
    else
    {
        if (error == 0)
        {
            result_.origin = Grub2CoreOrigin::Cache;
            result_.bytes = std::move(loader.bytes);
            result_.sha256 = std::move(loader.sha256);
        }
        finish(error);
    }
    return 0;
}

int Grub2CoreSelection::choose(Grub2DownloadChoice choice)
{
    if (!waiting_)
        return EALREADY;
    const bool valid = choice == Grub2DownloadChoice::Yes || choice == Grub2DownloadChoice::No
        || choice == Grub2DownloadChoice::Cancel;
    if (!valid)
        return EINVAL;
    waiting_ = false;
    if (choice == Grub2DownloadChoice::Cancel)
        finish(ECANCELED);
    else if (choice == Grub2DownloadChoice::No)
        // GRUB4DOS has no embedded grldr fallback. Stop before target writes.
        finish(grub4dos_ ? ENOENT : 0);
    else
        nextCandidate();
    return 0;
}

int Grub2CoreSelection::checkSource() const
{
    const int error = source_ ? source_->check() : 0;
    const bool disappeared = error == ENOENT || error == ENOTDIR;
    return disappeared ? ESTALE : error;
}

void Grub2CoreSelection::nextCandidate()
{
    const int sourceError = checkSource();
    if (sourceError != 0)
    {
        finish(sourceError);
        return;
    }
    QByteArray candidate;
    if (grub4dos_)
    {
        result_.downloadVersion = QStringLiteral(GRUB4DOS_VERSION);
        candidate = "grub4dos-" GRUB4DOS_VERSION "/grldr";
    }
    else
    {
        before_ = GetGrub2DownloadVersionLength(&report_, before_);
        if (before_ == 0)
        {
            // Win32 resets ErrorStatus per request, but a final missing/invalid
            // signature sets ERROR_BAD_SIGNATURE. Stop before any helper starts.
            finish(signatureRejected_ ? EBADMSG : 0);
            return;
        }
        result_.downloadVersion = QString::fromLatin1(report_.grub2_version, qsizetype(before_));
        candidate = "grub-" + result_.downloadVersion.toLatin1() + "/core.img";
    }
    QByteArray root = server_.toEncoded();
    if (!root.endsWith('/')) root += '/';
    const QUrl url = QUrl::fromEncoded(root + candidate);
    const quint64 maximumBytes = grub4dos_ ? PrepareCoreMaximumBytes : partitionOffset_ - sectorBytes;
    const int error = download_.start(url, qsizetype(maximumBytes));
    if (error != 0)
        finish(error);
}

void Grub2CoreSelection::downloadFinished(SignedDownloadResult result)
{
    if (!active_)
        return;
    if (result.error == ECANCELED)
    {
        finish(ECANCELED);
        return;
    }
    const int sourceError = checkSource();
    if (sourceError != 0)
    {
        finish(sourceError);
        return;
    }
    signatureRejected_ = result.signatureRequest && result.error != 0;
    int error = result.error;
    if (error == 0)
        error = grub4dos_ ? StoreGrub4dosLoaderCache(filesDirectory_, result.bytes)
            : StoreGrub2CoreCache(filesDirectory_, report_, mbr_, bios_, partitionOffset_, result.bytes);
    result_.candidateError = error;
    if (error != 0 && grub4dos_)
        finish(error);
    else if (error == ECANCELED || error == ESTALE)
        finish(error);
    else if (error != 0)
        nextCandidate();
    else
    {
        // Match Windows's read-after-install decision. The reader independently
        // checks the cache, but retain the verified snapshot rather than trusting
        // bytes substituted between commit and readback.
        Grub2CachedCore core;
        error = grub4dos_ ? ReadGrub4dosLoaderCache(filesDirectory_, core)
            : ReadGrub2CoreCache(filesDirectory_, report_, mbr_, bios_, partitionOffset_, core);
        const bool sameBytes = core.state == Grub2CacheState::Ready && core.bytes == result.bytes;
        if (error == 0 && !sameBytes)
            error = ESTALE;
        result_.candidateError = error;
        if (error == 0)
        {
            result_.origin = Grub2CoreOrigin::Download;
            result_.bytes = std::move(result.bytes);
            result_.sha256 = std::move(result.sha256);
        }
        const bool stop = grub4dos_ || error == ECANCELED || error == ESTALE;
        finish(stop ? error : 0);
    }
}

void Grub2CoreSelection::cancel()
{
    if (active_)
        finish(ECANCELED);
}

void Grub2CoreSelection::invalidate()
{
    if (active_)
        finish(ESTALE);
}

void Grub2CoreSelection::finish(int error)
{
    if (!active_)
        return;
    if (error == 0 && source_ && source_->retainedSource().fd >= 0)
        error = checkSource();
    result_.error = error;
    if (error != 0)
    {
        result_.origin = Grub2CoreOrigin::Embedded;
        result_.bytes.clear();
        result_.sha256.clear();
    }
    active_ = false;
    waiting_ = false;
    source_.reset();
    download_.cancel();
    Grub2CoreSelectionResult result = std::move(result_);
    emit completed(std::move(result));
}
}
