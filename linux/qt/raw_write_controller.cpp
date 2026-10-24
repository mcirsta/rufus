// SPDX-License-Identifier: GPL-3.0-or-later
#include "raw_write_controller.h"
#include "device_policy.h"

#include "platform/device_selection.h"
#include "platform/prepare_source.h"
#include <QFileInfo>
#include <cerrno>
#include <cstring>
#include <unistd.h>

namespace rufus
{

RawWriteController::RawWriteController(QObject* parent) : QObject(parent)
{
}

RawWriteController::~RawWriteController()
{
    if (process_)
    {
        disconnect(process_.get(), nullptr, this, nullptr);
        process_.reset(); // EOF cancellation and helper exit precede plan destruction.
    }
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
    return connection_ && connection_->isConnected() && helperAvailable();
}

void RawWriteController::setConnection(const QDBusConnection& connection)
{
    if (isActive())
        return;
    reset();
    connection_ = connection;
    emit changed();
}

void RawWriteController::setHelper(const QString& path)
{
    if (!isActive())
    {
        helper_ = path;
        emit changed();
    }
}

bool RawWriteController::helperAvailable() const
{
    const QFileInfo helper(helper_);
    return helper.isAbsolute() && helper.isFile() && helper.isExecutable();
}

void RawWriteController::reset()
{
    if (isActive())
        return;
    process_.reset();
    helperActive_ = false;
    sparseImage_ = false;
    zeroMode_ = ZERO_NONE;
    progressTotal_ = 0;
    badBlocksProcessed_ = badBlocksTotal_ = 0;
    access_.reset();
    plan_.reset();
    status_ = {};
    invalidated_.clear();
    hasResult_ = false;
}

void RawWriteController::publish()
{
    emit logMessage(statusText());
    if (!isActive() && status_.outcome.io.bytes_written != 0)
        emit logMessage(tr("Written: %1 bytes; verified: %2 / %3 bytes.")
            .arg(status_.outcome.io.bytes_written).arg(status_.outcome.io.bytes_verified)
            .arg(sparseImage_ ? status_.outcome.io.bytes_written : status_.size));
    emit changed();
}

void RawWriteController::prepare(const QString& path, const Device& device, const PrepareImageRequest* expected, bool allowFixedUsb)
{
    if (isActive())
        return;
    reset();
    if (!isAvailable())
    {
        status_.state = WriteState::Failed;
        status_.outcome.problem = tr("The device service or preparation helper is unavailable. Try again after restoring it.");
        publish();
        return;
    }
    plan_ = std::make_unique<RawWritePlan>();
    status_.outcome.problem = plan_->prepare(path, device, expected, allowFixedUsb);
    if (!status_.outcome.problem.isEmpty())
    {
        status_.state = WriteState::Failed;
        publish();
        return;
    }
    diskSequence_ = plan_->targetDiskSequence();
    zeroMode_ = plan_->imageMetadata().zeroMode;
    sparseImage_ = zeroMode_ == ZERO_FAST || plan_->imageMetadata().imageCompression == BLED_COMPRESSION_VTSI
        || plan_->imageMetadata().imageCompression == IMG_COMPRESSION_FFU;
    if (diskSequence_ == 0) diskSequence_ = ReadDiskSequence(device.number);
    status_.size = plan_->writeBytes();
    progressTotal_ = plan_->imageMetadata().imageCompression == IMG_COMPRESSION_FFU ? device.size : status_.size;
    status_.state = WriteState::Checking;
    access_ = std::make_unique<UDisksTarget>(*connection_, *plan_);
    connect(access_.get(), &UDisksTarget::stateChanged, this, &RawWriteController::accessChanged);
    publish();
    access_->check(true);
}

void RawWriteController::prepareZero(const Device& device, int mode, bool allowFixedUsb, int badBlocksPattern, bool detectFakes)
{
    PrepareImageRequest request;
    request.zeroMode = mode;
    request.badBlocksPattern = badBlocksPattern; request.detectFakes = detectFakes;
    prepare({}, device, &request, allowFixedUsb);
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
        if (!helperAvailable())
        {
            invalidate(tr("The matching preparation helper is required for image writing."));
            return;
        }
        status_.state = WriteState::AwaitingConfirmation;
        publish();
        emit confirmationRequired();
    }
    else if (state == UDisksTarget::State::Ready && status_.state == WriteState::Authorizing)
        startHelper();
}

void RawWriteController::confirm()
{
    if (status_.state != WriteState::AwaitingConfirmation)
        return;
    status_.state = WriteState::Authorizing;
    publish();
    access_->recheck();
}

void RawWriteController::startHelper()
{
    if (!helperAvailable())
    {
        invalidate(tr("The preparation helper is required for this target."));
        return;
    }
    const Device& target = plan_->target();
    PrepareImageRequest request = plan_->imageMetadata();
    request.rawWrite = request.isDevice = true;
    request.allowFixedUsb = plan_->allowsFixedUsb();
    request.path = target.path;
    request.device = target.number;
    request.size = target.size;
    request.objectPath = target.objectPath;
    request.serviceOwner = target.serviceOwner;
    request.identity = target.identity;
    request.diskSequence = diskSequence_;
    FreezeDevicePolicy(request);
    int error = 0;
    if (zeroMode_ == ZERO_NONE)
    {
        error = CapturePrepareSource(plan_->sourcePath(), request);
        if (error == 0) error = CheckPrepareSourceIdentity(request, plan_->source());
    }
    const bool sameDisk = diskSequence_ != 0 && ReadDiskSequence(target.number) == diskSequence_;
    if (error == 0 && !sameDisk) error = ESTALE;
    if (error == 0 && zeroMode_ == ZERO_NONE)
    {
        PrepareIsoSource source(request);
        error = source.open(getuid()); // Same ownership/path checks as the authorized helper.
    }
    if (error != 0)
    {
        invalidate(tr("Cannot prepare the target: %1").arg(QString::fromLocal8Bit(strerror(error))));
        return;
    }
    process_ = std::make_unique<PrepareImageProcess>(helper_);
    process_->setReportDirectory(reportDirectory_);
    connect(process_.get(), &PrepareImageProcess::progressChanged, this, &RawWriteController::badBlocksProgress);
    connect(process_.get(), &PrepareImageProcess::badBlocksReviewRequired, this, &RawWriteController::badBlocksReviewRequired);
    connect(process_.get(), &PrepareImageProcess::changed, this, &RawWriteController::helperChanged);
    connect(process_.get(), &PrepareImageProcess::rawProgressChanged, this, &RawWriteController::helperProgress);
    connect(process_.get(), &PrepareImageProcess::logMessage, this, &RawWriteController::logMessage);
    connect(process_.get(), &PrepareImageProcess::finished, this, &RawWriteController::helperFinished);
    helperActive_ = true;
    if (!process_->start(request))
    {
        helperActive_ = false;
        invalidate(process_->problem().isEmpty() ? tr("The DD helper request could not be started.") : process_->problem());
    }
}

void RawWriteController::helperChanged()
{
    if (process_->state() == PrepareImageProcess::State::Running && status_.state == WriteState::Authorizing)
    {
        status_.state = WriteState::Writing;
        publish();
    }
}

void RawWriteController::badBlocksProgress(int operation, quint64 processed, quint64 total)
{
    if (operation != OP_BADBLOCKS) return;
    badBlocksProcessed_ = processed; badBlocksTotal_ = total;
    emit changed();
}

void RawWriteController::helperProgress(int phase, quint64 processed, quint64)
{
    if (!helperActive_ || hasResult_)
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

void RawWriteController::helperFinished()
{
    const PrepareImageOutcome& outcome = process_->outcome();
    status_.outcome.io = outcome.raw;
    status_.outcome.error = outcome.error;
    status_.outcome.started = outcome.started;
    status_.outcome.problem = process_->problem();
    hasResult_ = true;
    helperActive_ = false;
    finishWrite(); // The process client emits finished only after helper/launcher exit.
}

void RawWriteController::finishWrite()
{
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
    const bool sparseComplete = process_ && process_->state() == PrepareImageProcess::State::Complete
        && (zeroMode_ == ZERO_FAST || status_.outcome.io.bytes_written > 0)
        && status_.outcome.io.bytes_verified == status_.outcome.io.bytes_written;
    const bool denseComplete = status_.outcome.io.bytes_written == status_.size
        && status_.outcome.io.bytes_verified == status_.size;
    const bool fullyVerified = sparseImage_ ? sparseComplete : denseComplete;
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
    status_.state = helperActive_ ? WriteState::Cancelling : WriteState::Cancelled;
    if (helperActive_)
        process_->cancel();
    else if (access_)
        access_->cancel();
    publish();
}

void RawWriteController::invalidate(const QString& problem)
{
    if (!isActive())
        return;
    invalidated_ = problem;
    if (helperActive_)
    {
        status_.state = WriteState::Cancelling;
        process_->cancel();
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
    if (progressTotal_ == 0)
        return 0;
    const double bytes = double(status_.outcome.io.bytes_written) + double(status_.outcome.io.bytes_verified);
    // Copying and verification each account for half; reserve 100% for completion.
    constexpr double percentPerPass = 50.0;
    const bool tested = plan_ && plan_->imageMetadata().badBlocksPattern >= 0;
    if (!tested) return qBound(0, int(percentPerPass * bytes / double(progressTotal_)), 99);
    const double test = badBlocksTotal_ == 0 ? 0 : 50.0 * double(badBlocksProcessed_) / double(badBlocksTotal_);
    return qBound(0, int(test + 25.0 * bytes / double(progressTotal_)), 99);
}

QString RawWriteController::statusText() const
{
    switch (status_.state)
    {
    case WriteState::Idle: return {};
    case WriteState::Checking: return zeroMode_ != ZERO_NONE ? tr("Checking the selected device...") : tr("Checking the selected image and device...");
    case WriteState::AwaitingConfirmation: return tr("Waiting for confirmation...");
    case WriteState::Authorizing: return tr("Waiting for device authorization...");
    case WriteState::Cancelling: return tr("Cancelling... Waiting for device I/O to finish.");
    case WriteState::Complete: return zeroMode_ != ZERO_NONE ? tr("Device cleared and verified.") : tr("Image written and verified.");
    case WriteState::Failed: return status_.outcome.problem;
    case WriteState::Cancelled:
        if (zeroMode_ != ZERO_NONE) return tr("Device clearing cancelled. Unverified data may remain.");
        return !status_.outcome.started && status_.outcome.io.bytes_written == 0 ? tr("Image writing cancelled.")
            : tr("Image writing cancelled. The device contains an incomplete or unverified image.");
    case WriteState::Writing:
        if (badBlocksTotal_ != 0 && status_.outcome.io.phase == RAW_WRITE_PREPARE)
            return tr("Checking for bad blocks and fake capacity...");
        if (status_.outcome.io.phase == RAW_WRITE_SYNC)
            return tr("Flushing written data...");
        if (status_.outcome.io.phase == RAW_WRITE_VERIFY)
            return tr("Verifying image: %1 / %2 bytes").arg(status_.outcome.io.bytes_verified).arg(progressTotal_);
        return (zeroMode_ != ZERO_NONE ? tr("Clearing device: %1 / %2 bytes") : tr("Writing image: %1 / %2 bytes"))
            .arg(status_.outcome.io.bytes_written).arg(progressTotal_);
    }
    return {};
}

} // namespace rufus
