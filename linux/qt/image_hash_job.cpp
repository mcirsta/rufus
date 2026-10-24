// SPDX-License-Identifier: GPL-3.0-or-later
#include "image_hash_job.h"
#include "platform/image_hash.h"
#include "platform/prepare_source.h"
#include "platform/rufus_reporting.h"

#include <QFile>
#include <cstring>

namespace rufus
{
namespace
{
struct ImageSourceScope
{
    IMAGE_FILE_SOURCE file = {-1, {}, false, false};
    ~ImageSourceScope() { CloseImageFileSource(&file); }
};

QString hashProblem(int error)
{
    if (error == 0)
        return {};
    if (error == ECANCELED)
        return ImageHashJob::tr("Operation cancelled by the user");
    if (error == ESTALE)
        return ImageHashJob::tr("The image changed. Select it again before computing checksums.");
    return ImageHashJob::tr("Could not compute image checksums: %1")
        .arg(QString::fromLocal8Bit(std::strerror(error)));
}
}

ImageHashJob::ImageHashJob(quint64 request, const PrepareImageRequest& source, bool extra)
    : request_(request), source_(source), extra_(extra)
{
    qRegisterMetaType<ImageHashResult>();
    connect(this, &RufusJob::progressWithInfo, this, &ImageHashJob::forwardProgress, Qt::DirectConnection);
    connect(this, &RufusJob::failed, this, &ImageHashJob::reportFailure, Qt::DirectConnection);
}

void ImageHashJob::forwardProgress(int operation, int message, quint64 processed, quint64 total, bool forced)
{
    (void)forced;
    if (operation == OP_NOOP_WITH_TASKBAR && message == 271)
        emit hashProgress(request_, processed, total);
}

void ImageHashJob::reportFailure(const QString& problem)
{
    ImageHashResult result;
    result.path = source_.imagePath;
    result.problem = problem;
    emit hashesReady(request_, result);
}

void ImageHashJob::execute()
{
    ImageSourceScope source;
    IMAGE_HASH_RESULT hashes = {};
    ImageHashResult result;
    result.path = source_.imagePath;
    const QByteArray path = QFile::encodeName(source_.imagePath);
    const bool validPath = !path.isEmpty() && !path.contains('\0');
    result.error = validPath ? OpenImageFileSource(path.constData(), &source.file) : EINVAL;
    if (result.error == 0)
        result.error = CheckPrepareSourceIdentity(source_, source.file);
    if (result.error == 0)
        result.error = HashImageFileSource(&source.file, path.constData(), extra_, &hashes);
    if (IsRufusJobCancelled())
        result.error = ECANCELED;
    result.problem = hashProblem(result.error);
    if (result.error == 0)
    {
        const int sizes[] = {MD5_HASHSIZE, SHA1_HASHSIZE, SHA256_HASHSIZE, SHA512_HASHSIZE};
        for (unsigned i = 0; i < hashes.count; i++)
            result.digests.append(QString::fromLatin1(QByteArray(
                reinterpret_cast<const char*>(hashes.digest[i]), sizes[i]).toHex()));
    }
    emit hashesReady(request_, result);
}

} // namespace rufus
