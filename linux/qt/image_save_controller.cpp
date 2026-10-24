// SPDX-License-Identifier: GPL-3.0-or-later
#include "image_save_controller.h"
#include "device_policy.h"
#include "platform/iso_export.h"
#include "platform/device_selection.h"
#include <QFileInfo>
#include <cstring>

namespace rufus
{
ImageSaveController::ImageSaveController(QObject* parent) : QObject(parent), connection_(QDBusConnection::systemBus()), helper_(QStringLiteral(RUFUS_SAVE_HELPER_PATH))
{
    connect(&thread_, &QThread::finished, this, &ImageSaveController::threadFinished, Qt::QueuedConnection);
}
ImageSaveController::~ImageSaveController()
{
    thread_.requestInterruption(); thread_.quit(); thread_.wait();
}
void ImageSaveController::setConnection(const QDBusConnection& connection) { if (!isActive()) connection_ = connection; }
bool ImageSaveController::isActive() const
{
    return state_ == ImageSaveState::Checking || state_ == ImageSaveState::Authorizing || state_ == ImageSaveState::Saving
        || state_ == ImageSaveState::Verifying || state_ == ImageSaveState::Finalizing || state_ == ImageSaveState::Cancelling;
}
bool ImageSaveController::start(const Device& device, const QString& path, bool allowFixedUsb)
{
    if (isActive()) return false;
    const int format = GetImageSaveFormat(path);
    const bool supported = format == IMG_COMPRESSION_VHD || format == IMG_COMPRESSION_VHDX || format == IMG_COMPRESSION_FFU || format == ImageSaveIso;
    if (!supported || device.size == 0 || device.number == 0 || device.serviceOwner.isEmpty()) return false;
    const bool optical = device.deviceClass == DeviceClass::Optical;
    if (optical && format != ImageSaveIso) return false;
    iso_ = format == ImageSaveIso && !optical;
    result_ = {}; pending_.reset(); processed_ = 0; total_ = device.size; path_ = path; phase_ = ImageSavePhase::Copy;
    PrepareImageRequest source;
    source.isDevice = true; source.rawWrite = true; source.path = device.path; source.device = device.number;
    source.size = device.size; source.identity = device.identity; source.objectPath = device.objectPath;
    source.serviceOwner = device.serviceOwner; source.allowFixedUsb = allowFixedUsb; source.deviceClass = device.deviceClass;
    source.diskSequence = ReadDiskSequence(device.number);
    FreezeDevicePolicy(source);
    state_ = ImageSaveState::Checking;
    ImageSaveJob* job = new ImageSaveJob(connection_, source, path, format, helper_);
    job->moveToThread(&thread_);
    connect(&thread_, &QThread::started, job, &RufusJob::run);
    connect(job, &ImageSaveJob::stageChanged, this, &ImageSaveController::stageChanged, Qt::QueuedConnection);
    connect(job, &ImageSaveJob::saveProgress, this, &ImageSaveController::progress, Qt::QueuedConnection);
    connect(job, &ImageSaveJob::saved, this, &ImageSaveController::acceptResult, Qt::QueuedConnection);
    connect(job, &RufusJob::logMessage, this, &ImageSaveController::logMessage, Qt::QueuedConnection);
    connect(job, &RufusJob::finished, &thread_, &QThread::quit, Qt::DirectConnection);
    connect(&thread_, &QThread::finished, job, &QObject::deleteLater);
    thread_.start(); emit changed(); return true;
}
void ImageSaveController::cancel()
{
    if (!isActive() || state_ == ImageSaveState::Cancelling) return;
    state_ = ImageSaveState::Cancelling; thread_.requestInterruption(); emit changed();
}
void ImageSaveController::reset()
{
    if (isActive()) return;
    state_ = ImageSaveState::Idle; result_ = {}; pending_.reset(); processed_ = total_ = 0; path_.clear();
}
void ImageSaveController::stageChanged(ImageSaveState state)
{
    if (isActive() && state_ != ImageSaveState::Cancelling) { state_ = state; emit changed(); }
}
void ImageSaveController::progress(ImageSavePhase phase, quint64 processed, quint64 total)
{
    if (!isActive() || state_ == ImageSaveState::Cancelling || (!iso_ && total != total_) || processed > total) return;
    if (iso_) total_ = total;
    if (phase == phase_ && processed < processed_) return;
    phase_ = phase; processed_ = processed;
    state_ = phase == ImageSavePhase::Copy ? ImageSaveState::Saving : ImageSaveState::Verifying;
    emit changed();
}
void ImageSaveController::acceptResult(const ImageSaveResult& result) { if (isActive()) pending_ = result; }
void ImageSaveController::threadFinished()
{
    thread_.wait();
    result_ = pending_.value_or(ImageSaveResult{EIO, false, 0, {}}); pending_.reset();
    if (result_.error == 0 && !result_.committed) result_.error = EIO;
    if (result_.error == 0) {
        state_ = ImageSaveState::Complete; processed_ = total_;
        emit logMessage((iso_ ? tr("Saved '%1'\nSHA256 of filesystem manifest: %2") : tr("Saved '%1'\nSHA256 of captured disk: %2"))
            .arg(path_, QString::fromLatin1(result_.sha256.toHex())));
    } else {
        state_ = result_.error == ECANCELED ? ImageSaveState::Cancelled : ImageSaveState::Failed;
        emit logMessage(statusText());
    }
    emit changed(); emit finished();
}
int ImageSaveController::progressPosition() const
{
    if (state_ == ImageSaveState::Complete) return MAX_PROGRESS;
    if (total_ == 0) return 0;
    const int phase = phase_ == ImageSavePhase::Copy ? 0 : phase_ == ImageSavePhase::VerifyImage ? 1 : 2;
    return qMin(MAX_PROGRESS - 1, int((double(phase) + double(processed_) / double(total_)) * MAX_PROGRESS / 3.0));
}
QString ImageSaveController::statusText() const
{
    switch (state_) {
    case ImageSaveState::Checking: return tr("Checking device and destination...");
    case ImageSaveState::Authorizing: return tr("Waiting for authorization...");
    case ImageSaveState::Saving: return iso_ ? tr("Exporting filesystem to ISO...") : tr("Saving device image...");
    case ImageSaveState::Verifying: return phase_ == ImageSavePhase::VerifySource ? tr("Checking captured device consistency...") : tr("Verifying saved image...");
    case ImageSaveState::Finalizing: return tr("Finishing saved image...");
    case ImageSaveState::Cancelling: return tr("Cancelling...");
    case ImageSaveState::Complete: return tr("READY");
    case ImageSaveState::Cancelled: return tr("Operation cancelled by the user");
    case ImageSaveState::Failed:
        if (result_.committed) return tr("The image was saved, but final cleanup or sync failed: %1").arg(QString::fromLocal8Bit(strerror(result_.error)));
        if (result_.error == EBUSY) return tr("The source is busy or its mounts cannot be safely released. Close files using this device and retry.");
        if (result_.error == ENOTSUP) return tr("This image format cannot capture the device geometry or GPT layout. Choose VHDX for a raw disk image.");
        if (result_.error == ESTALE) return tr("The device or destination changed during saving.");
        return tr("Cannot save device image: %1").arg(QString::fromLocal8Bit(strerror(result_.error)));
    case ImageSaveState::Idle: return {};
    }
    return {};
}
}
