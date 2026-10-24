// SPDX-License-Identifier: GPL-3.0-or-later
#include "image_hash_controller.h"

namespace rufus
{

ImageHashController::ImageHashController(QObject* parent) : QObject(parent)
{
    connect(&thread_, &QThread::finished, this, &ImageHashController::threadFinished, Qt::QueuedConnection);
}

ImageHashController::~ImageHashController()
{
    thread_.requestInterruption();
    thread_.quit();
    thread_.wait();
}

bool ImageHashController::isActive() const
{
    return state_ == ImageHashState::Running || state_ == ImageHashState::Cancelling;
}

bool ImageHashController::start(const PrepareImageRequest& source, bool extra)
{
    if (isActive())
        return false;
    ++request_;
    result_ = {};
    pending_.reset();
    processed_ = 0;
    total_ = source.imageSize;
    state_ = ImageHashState::Running;
    ImageHashJob* job = new ImageHashJob(request_, source, extra);
    job->moveToThread(&thread_);
    connect(&thread_, &QThread::started, job, &RufusJob::run);
    connect(job, &ImageHashJob::hashesReady, this, &ImageHashController::acceptResult, Qt::QueuedConnection);
    connect(job, &ImageHashJob::hashProgress, this, &ImageHashController::acceptProgress, Qt::QueuedConnection);
    // Direct quit also permits teardown to join without a GUI event loop.
    connect(job, &RufusJob::finished, &thread_, &QThread::quit, Qt::DirectConnection);
    connect(&thread_, &QThread::finished, job, &QObject::deleteLater);
    thread_.start();
    emit changed();
    return true;
}

void ImageHashController::reset()
{
    if (isActive() || state_ == ImageHashState::Idle)
        return;
    ++request_;
    state_ = ImageHashState::Idle;
    result_ = {};
    pending_.reset();
    processed_ = 0;
    total_ = 0;
    emit changed();
}

void ImageHashController::cancel()
{
    if (state_ != ImageHashState::Running)
        return;
    state_ = ImageHashState::Cancelling;
    pending_.reset(); // A successful result may already be queued.
    thread_.requestInterruption();
    emit changed();
}

void ImageHashController::acceptResult(quint64 request, const ImageHashResult& result)
{
    if (request == request_ && state_ == ImageHashState::Running)
        pending_ = result;
}

void ImageHashController::acceptProgress(quint64 request, quint64 processed, quint64 total)
{
    if (request != request_ || state_ != ImageHashState::Running)
        return;
    const bool valid = total == total_ && processed >= processed_ && processed <= total;
    if (!valid)
        return;
    processed_ = processed;
    emit changed();
}

void ImageHashController::threadFinished()
{
    thread_.wait();
    if (state_ == ImageHashState::Cancelling)
    {
        result_ = {};
        result_.error = ECANCELED;
        result_.problem = tr("Operation cancelled by the user");
    }
    else if (pending_)
        result_ = *pending_;
    else
        result_.problem = tr("Checksum calculation ended without a result.");
    pending_.reset();
    if (result_.error == 0)
    {
        state_ = ImageHashState::Complete;
        processed_ = total_;
        const char* names[] = {"MD5", "SHA1", "SHA256", "SHA512"};
        for (qsizetype i = 0; i < result_.digests.size(); i++)
            emit logMessage(QStringLiteral("  %1: %2").arg(QLatin1String(names[i]), result_.digests[i]));
    }
    else
    {
        state_ = result_.error == ECANCELED ? ImageHashState::Cancelled : ImageHashState::Failed;
        emit logMessage(result_.problem);
    }
    emit changed();
    emit finished();
}

int ImageHashController::progressPosition() const
{
    // Reserve full READY for joined success, including an empty input.
    if (state_ == ImageHashState::Complete)
        return MAX_PROGRESS;
    if (total_ == 0)
        return 0;
    const double fraction = double(processed_) / double(total_);
    return qMin(MAX_PROGRESS - 1, int(fraction * MAX_PROGRESS));
}

QString ImageHashController::statusText() const
{
    switch (state_)
    {
    case ImageHashState::Running: return tr("Computing image hashes...");
    case ImageHashState::Cancelling: return tr("Cancelling...");
    case ImageHashState::Complete: return tr("READY");
    case ImageHashState::Cancelled:
    case ImageHashState::Failed: return result_.problem;
    case ImageHashState::Idle: return {};
    }
    return {};
}

} // namespace rufus
