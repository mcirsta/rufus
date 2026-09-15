// SPDX-License-Identifier: GPL-3.0-or-later
#include "raw_write_controller.h"

#include <cerrno>

namespace rufus
{

RawWriteController::RawWriteController(QObject* parent) : QObject(parent)
{
    connect(&thread_, &QThread::finished, this, &RawWriteController::workerFinished, Qt::QueuedConnection);
}

RawWriteController::~RawWriteController()
{
    thread_.requestInterruption();
    thread_.quit();
    thread_.wait();
}

bool RawWriteController::isActive() const
{
    switch (status_.state)
    {
    case WriteState::Checking:
    case WriteState::AwaitingConfirmation:
    case WriteState::Authorizing:
    case WriteState::Writing:
    case WriteState::Cancelling:
        return true;
    default:
        return false;
    }
}

bool RawWriteController::isAvailable() const
{
    return connection_ && connection_->isConnected();
}

void RawWriteController::setConnection(const QDBusConnection& connection)
{
    if (isActive())
        return;
    reset();
    connection_ = connection;
    emit changed();
}

void RawWriteController::reset()
{
    if (isActive())
        return;
    access_.reset();
    plan_.reset();
    status_ = {};
    invalidated_.clear();
    hasResult_ = false;
    ++request_;
}

void RawWriteController::publish()
{
    emit logMessage(statusText());
    if (!isActive() && status_.outcome.io.bytes_written != 0)
        emit logMessage(tr("Written: %1 bytes; verified: %2 / %3 bytes.")
            .arg(status_.outcome.io.bytes_written).arg(status_.outcome.io.bytes_verified).arg(status_.size));
    emit changed();
}

void RawWriteController::prepare(const QString& path, const Device& device)
{
    if (isActive() || !isAvailable())
        return;
    reset();
    plan_ = std::make_unique<RawWritePlan>();
    status_.outcome.problem = plan_->prepare(path, device);
    if (!status_.outcome.problem.isEmpty())
    {
        status_.state = WriteState::Failed;
        publish();
        return;
    }
    status_.size = quint64(plan_->source().info.st_size);
    status_.state = WriteState::Checking;
    access_ = std::make_unique<UDisksTarget>(*connection_, *plan_);
    connect(access_.get(), &UDisksTarget::stateChanged, this, &RawWriteController::accessChanged);
    publish();
    access_->check();
}

void RawWriteController::accessChanged(UDisksTarget::State state)
{
    if (!isActive())
        return;
    if (state == UDisksTarget::State::Failed)
        invalidate(access_->problem());
    else if (state == UDisksTarget::State::Cancelled)
        cancel();
    else if (state == UDisksTarget::State::Prepared)
    {
        status_.state = WriteState::AwaitingConfirmation;
        publish();
        emit confirmationRequired();
    }
    else if (state == UDisksTarget::State::Ready && status_.state == WriteState::Authorizing)
        startWorker();
}

void RawWriteController::confirm()
{
    if (status_.state != WriteState::AwaitingConfirmation)
        return;
    status_.state = WriteState::Authorizing;
    publish();
    access_->authorize();
}

void RawWriteController::startWorker()
{
    QDBusUnixFileDescriptor descriptor = access_->takeDescriptor();
    if (!descriptor.isValid())
    {
        invalidate(tr("No authorized target descriptor is available."));
        return;
    }
    RawWriteJob* job = new RawWriteJob(request_, *plan_, descriptor);
    job->moveToThread(&thread_);
    connect(&thread_, &QThread::started, job, &RufusJob::run);
    connect(job, &RawWriteJob::writeProgress, this, &RawWriteController::acceptProgress, Qt::QueuedConnection);
    connect(job, &RawWriteJob::writeResult, this, &RawWriteController::acceptResult, Qt::QueuedConnection);
    connect(job, &RufusJob::finished, &thread_, &QThread::quit, Qt::DirectConnection);
    connect(&thread_, &QThread::finished, job, &QObject::deleteLater);
    workerActive_ = true;
    status_.state = WriteState::Writing;
    // Start before publishing: cancellation from a receiver must reach this run.
    thread_.start();
    publish();
}

void RawWriteController::acceptProgress(quint64 request, int phase, quint64 processed)
{
    if (request != request_ || !workerActive_ || hasResult_)
        return;
    const RAW_WRITE_PHASE previous = status_.outcome.io.phase;
    status_.outcome.io.phase = RAW_WRITE_PHASE(phase);
    if (phase == RAW_WRITE_COPY)
        status_.outcome.io.bytes_written = processed;
    else if (phase == RAW_WRITE_VERIFY)
        status_.outcome.io.bytes_verified = processed;
    if (previous != phase)
        emit logMessage(statusText());
    emit changed();
}

void RawWriteController::acceptResult(quint64 request, const RawWriteOutcome& outcome)
{
    if (request != request_ || !workerActive_)
        return;
    hasResult_ = true;
    status_.outcome = outcome;
    // Remain busy until the thread and descriptor have actually been released.
}

void RawWriteController::workerFinished()
{
    thread_.wait();
    workerActive_ = false;
    if (!invalidated_.isEmpty())
    {
        status_.outcome.problem = invalidated_ + QLatin1Char(' ') + status_.outcome.problem;
        status_.outcome.error = ESTALE;
    }
    else if (!hasResult_)
    {
        status_.outcome.problem = tr("Image writing ended without a result.");
        status_.outcome.error = EIO;
    }
    else if (status_.state == WriteState::Cancelling && status_.outcome.error == 0)
        status_.outcome.error = ECANCELED;
    const bool writeFailed = status_.outcome.error != 0 && status_.outcome.error != ECANCELED;
    const bool syncFailed = status_.outcome.io.sync_error != 0;
    const bool fullyVerified = status_.outcome.io.bytes_written == status_.size
        && status_.outcome.io.bytes_verified == status_.size;
    if (writeFailed || syncFailed)
        status_.state = WriteState::Failed;
    else if (status_.outcome.error == ECANCELED)
        status_.state = WriteState::Cancelled;
    else if (status_.outcome.io.phase == RAW_WRITE_DONE && fullyVerified)
        status_.state = WriteState::Complete;
    else
    {
        status_.state = WriteState::Failed;
        status_.outcome.error = EIO;
        status_.outcome.problem = tr("The written image was not completely verified.");
    }
    access_.reset();
    plan_.reset();
    publish();
}

void RawWriteController::cancel()
{
    if (!isActive() || status_.state == WriteState::Cancelling)
        return;
    status_.state = workerActive_ ? WriteState::Cancelling : WriteState::Cancelled;
    if (workerActive_)
        thread_.requestInterruption();
    else if (access_)
        access_->cancel();
    publish();
}

void RawWriteController::invalidate(const QString& problem)
{
    if (!isActive())
        return;
    invalidated_ = problem;
    if (workerActive_)
    {
        status_.state = WriteState::Cancelling;
        thread_.requestInterruption();
    }
    else
    {
        status_.state = WriteState::Failed;
        status_.outcome.problem = problem;
        if (access_)
            access_->cancel();
    }
    publish();
}

int RawWriteController::percent() const
{
    if (status_.state == WriteState::Complete)
        return 100;
    if (status_.size == 0)
        return 0;
    const double bytes = double(status_.outcome.io.bytes_written) + double(status_.outcome.io.bytes_verified);
    // Copying and verification each account for half; reserve 100% for completion.
    constexpr double percentPerPass = 50.0;
    return qBound(0, int(percentPerPass * bytes / double(status_.size)), 99);
}

QString RawWriteController::statusText() const
{
    switch (status_.state)
    {
    case WriteState::Idle: return {};
    case WriteState::Checking: return tr("Checking the selected image and device...");
    case WriteState::AwaitingConfirmation: return tr("Waiting for confirmation...");
    case WriteState::Authorizing: return tr("Waiting for device authorization...");
    case WriteState::Cancelling: return tr("Cancelling... Waiting for device I/O to finish.");
    case WriteState::Complete: return tr("Image written and verified.");
    case WriteState::Failed: return status_.outcome.problem;
    case WriteState::Cancelled:
        return status_.outcome.io.bytes_written == 0 ? tr("Image writing cancelled.")
            : tr("Image writing cancelled. The device contains an incomplete or unverified image.");
    case WriteState::Writing:
        if (status_.outcome.io.phase == RAW_WRITE_SYNC)
            return tr("Flushing written data...");
        if (status_.outcome.io.phase == RAW_WRITE_VERIFY)
            return tr("Verifying image: %1 / %2 bytes").arg(status_.outcome.io.bytes_verified).arg(status_.size);
        return tr("Writing image: %1 / %2 bytes").arg(status_.outcome.io.bytes_written).arg(status_.size);
    }
    return {};
}

} // namespace rufus
