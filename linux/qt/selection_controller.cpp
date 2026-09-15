// SPDX-License-Identifier: GPL-3.0-or-later
#include "selection_controller.h"

#include <QFile>
#include <QFileInfo>
#include <QSet>

#include <limits>

namespace rufus
{
namespace
{

const Device* findDevice(const QList<Device>& devices, const QString& id)
{
    for (const Device& device : devices)
    {
        if (device.id == id)
            return &device;
    }
    return nullptr;
}

} // namespace

SelectionController::SelectionController(QObject* parent)
    : QObject(parent)
{
    connect(&analysisThread_, &QThread::finished,
            this, &SelectionController::analysisFinished, Qt::QueuedConnection);
    connect(&writer_, &RawWriteController::changed, this, &SelectionController::selectionChanged);
    connect(&writer_, &RawWriteController::logMessage, this, &SelectionController::logMessage);
}

SelectionController::~SelectionController()
{
    // Window close normally cancels asynchronously. Teardown must still join
    // the worker if the event loop exits by another route.
    analysisThread_.requestInterruption();
    analysisThread_.quit();
    analysisThread_.wait();
}

const Selection& SelectionController::selection() const
{
    return selection_;
}

bool SelectionController::isAnalyzing() const
{
    return analysisActive_;
}

void SelectionController::setDiscovery(const QList<Device>& devices, const QString& problem)
{
    if (!problem.isEmpty() && problem != selection_.discoveryProblem)
        emit logMessage(problem);
    selection_.discoveryAvailable = problem.isEmpty();
    selection_.discoveryProblem = problem;
    setDevices(problem.isEmpty() ? devices : QList<Device>());
}

void SelectionController::setDevices(const QList<Device>& devices)
{
    QSet<QString> seen;
    QSet<QString> ambiguous;
    for (const Device& device : devices)
    {
        if (seen.contains(device.id))
            ambiguous.insert(device.id);
        seen.insert(device.id);
    }

    QList<Device> available;
    for (const Device& device : devices)
    {
        const bool writableMedia = device.removable && !device.readOnly;
        const bool supportedSize = device.size > 0 && device.size <= quint64(std::numeric_limits<qint64>::max());
        const bool identified = !device.id.isEmpty() && !device.path.isEmpty() && !ambiguous.contains(device.id);
        if (writableMedia && supportedSize && identified)
            available.append(device);
    }

    const Device* previous = findDevice(selection_.devices, selection_.deviceId);
    const Device* current = findDevice(available, selection_.deviceId);
    const bool sameMedia = previous && current
        && previous->path == current->path && previous->size == current->size;
    const bool sameIdentity = previous && current
        && previous->identity == current->identity && previous->number == current->number;
    const bool sameService = previous && current && previous->serviceOwner == current->serviceOwner;
    const bool selectionPreserved = sameMedia && sameIdentity && sameService;
    const bool invalidated = !selection_.deviceId.isEmpty() && !selectionPreserved;
    selection_.devices = available;
    if (invalidated)
    {
        selection_.deviceId.clear();
        writer_.invalidate(tr("The selected device changed or was removed."));
        emit logMessage(tr("The selected device changed or was removed. Select a device again."));
    }
    emit selectionChanged();
}

void SelectionController::selectDevice(const QString& id)
{
    if (writer_.isActive() || id == selection_.deviceId)
        return;
    writer_.reset();
    const Device* device = findDevice(selection_.devices, id);
    selection_.deviceId = device ? id : QString();
    if (device)
        emit logMessage(tr("Device selected: %1 (%2)").arg(device->name, device->path));
    else if (!id.isEmpty())
        emit logMessage(tr("The selected device is no longer available."));
    emit selectionChanged();
}

void SelectionController::selectImage(const QString& path)
{
    if (writer_.isActive() || path.isEmpty())
        return;
    writer_.reset();

    ++imageRequest_;
    if (analysisActive_)
        analysisThread_.requestInterruption();
    selection_.imagePath.clear();
    selection_.imageSize = 0;
    selection_.imageProblem.clear();
    selection_.imageAnalysis = {};
    selection_.windowsAnalysis = {};
    selection_.imageState = ImageState::None;
    selection_.rawImage = false;
    const QFileInfo info(path);
    QFile file(info.absoluteFilePath());
    if (!info.isFile())
        selection_.imageProblem = tr("Choose an existing image file.");
    else if (!file.open(QIODevice::ReadOnly))
        selection_.imageProblem = tr("Cannot read the image: %1").arg(file.errorString());
    else if (file.size() <= 0)
        selection_.imageProblem = tr("The selected image is empty or its size is unavailable.");
    else
    {
        selection_.imagePath = info.absoluteFilePath();
        selection_.imageSize = quint64(file.size());
        imageModified_ = info.lastModified();
        selection_.imageState = ImageState::Scanning;
        if (info.suffix().compare(QStringLiteral("img"), Qt::CaseInsensitive) == 0)
        {
            selection_.imageProblem = CheckRawImageFile(selection_.imagePath);
            selection_.rawImage = selection_.imageProblem.isEmpty();
            if (selection_.rawImage)
                selection_.imageState = ImageState::Complete;
        }
    }

    if (!selection_.imageProblem.isEmpty())
    {
        selection_.imageState = ImageState::Failed;
        emit logMessage(selection_.imageProblem);
    }
    else
    {
        emit logMessage(tr("Image selected: %1").arg(selection_.imagePath));
        if (!analysisActive_ && !selection_.rawImage)
            startAnalysis();
    }
    emit selectionChanged();
}

bool SelectionController::canStartWrite() const
{
    const Device* device = findDevice(selection_.devices, selection_.deviceId);
    const bool writerReady = !analysisActive_ && !writer_.isActive() && writer_.isAvailable();
    const bool imageReady = selection_.rawImage && selection_.imageState == ImageState::Complete;
    const bool targetReady = device && !device->identity.isEmpty() && device->size >= selection_.imageSize;
    if (!selection_.discoveryAvailable || !targetReady)
        return false;
    return writerReady && imageReady;
}

IMAGE_WRITE_CHOICES SelectionController::isoWriteChoices(quint64 persistenceSize, bool gpt, bool fat) const
{
    // A completed report supplies policy only. canStartWrite separately gates
    // execution until the chosen native installer has been implemented.
    if (analysisActive_ || selection_.imageState != ImageState::Complete)
        return {};
    return GetImageWriteChoices(&selection_.imageAnalysis.report, persistenceSize, gpt, fat);
}

void SelectionController::startWrite()
{
    if (!canStartWrite())
        return;
    const Device* device = findDevice(selection_.devices, selection_.deviceId);
    writer_.prepare(selection_.imagePath, *device);
}

void SelectionController::startAnalysis()
{
    runningRequest_ = imageRequest_;
    analysisActive_ = true;
    ImageAnalysisJob* job = new ImageAnalysisJob(runningRequest_, selection_.imagePath,
                                               selection_.imageSize, imageModified_);
    job->moveToThread(&analysisThread_);
    connect(&analysisThread_, &QThread::started, job, &RufusJob::run);
    connect(job, &ImageAnalysisJob::analysisReady,
            this, &SelectionController::acceptAnalysis, Qt::QueuedConnection);
    connect(job, &ImageAnalysisJob::analysisLog,
            this, &SelectionController::acceptAnalysisLog, Qt::QueuedConnection);
    // quit() is thread-safe. A direct call also permits joining during teardown
    // without depending on delivery of a queued GUI event.
    connect(job, &RufusJob::finished, &analysisThread_, &QThread::quit, Qt::DirectConnection);
    connect(&analysisThread_, &QThread::finished, job, &QObject::deleteLater);
    analysisThread_.start();
}

void SelectionController::cancelAnalysis()
{
    if (!analysisActive_ || selection_.imageState == ImageState::Cancelling)
        return;
    ++imageRequest_; // Discard a successful result that may already be queued.
    selection_.imageState = ImageState::Cancelling;
    selection_.imageAnalysis = {};
    selection_.windowsAnalysis = {};
    selection_.imageProblem.clear();
    analysisThread_.requestInterruption();
    emit logMessage(tr("Cancelling..."));
    emit selectionChanged();
}

void SelectionController::acceptAnalysisLog(quint64 request, const QString& message)
{
    if (request == imageRequest_ && selection_.imageState == ImageState::Scanning)
        emit logMessage(message);
}

void SelectionController::acceptAnalysis(quint64 request, const ImageAnalysisResult& result)
{
    if (request != imageRequest_ || selection_.imageState != ImageState::Scanning)
        return;
    selection_.imageAnalysis = {};
    selection_.windowsAnalysis = {};
    selection_.imageProblem = result.problem;
    switch (result.status)
    {
    case ISO_SCAN_OK:
        selection_.imageState = ImageState::Complete;
        selection_.imageAnalysis = result.analysis;
        selection_.windowsAnalysis = result.windows;
        break;
    case ISO_SCAN_PARTIAL:
        selection_.imageState = ImageState::Partial;
        selection_.imageAnalysis = result.analysis;
        selection_.windowsAnalysis = result.windows;
        break;
    case ISO_SCAN_CANCELLED:
        selection_.imageState = ImageState::Cancelled;
        break;
    default:
        selection_.imageState = ImageState::Failed;
        break;
    }
    logImageReport(result.ddRequired);
    emit logMessage(imageStatusText());
    emit selectionChanged();
}

void SelectionController::analysisFinished()
{
    // finished() can precede final thread-local cleanup. Join before reusing
    // this QThread, so a replacement never overlaps the previous RufusJob.
    analysisThread_.wait();
    analysisActive_ = false;
    if (selection_.imageState == ImageState::Scanning && runningRequest_ != imageRequest_)
        startAnalysis(); // Only the most recent selection is pending.
    else if (selection_.imageState == ImageState::Cancelling)
    {
        selection_.imageState = ImageState::Cancelled;
        emit logMessage(imageStatusText());
    }
    else if (selection_.imageState == ImageState::Scanning)
    {
        selection_.imageState = ImageState::Failed;
        selection_.imageProblem = tr("Image analysis ended without a result.");
        emit logMessage(selection_.imageProblem);
    }
    emit selectionChanged();
}

QString SelectionController::pendingChecks() const
{
    const quint32 pending = selection_.imageAnalysis.pending;
    QStringList reasons;
    if (pending & ISO_PENDING_DEEP_DIRECTORIES)
        reasons.append(tr("deep directories"));
    if (pending & ISO_PENDING_CONTENT)
        reasons.append(tr("unreadable bootloader content"));
    if (pending & ISO_PENDING_EMBEDDED_EFI)
        reasons.append(tr("embedded EFI image"));
    if (pending & ISO_PENDING_WINDOWS)
    {
        if (selection_.windowsAnalysis.state == WINDOWS_IMAGE_FAILED)
            reasons.append(tr("Windows metadata (unreadable installation image)"));
        else if (selection_.windowsAnalysis.state == WINDOWS_IMAGE_UNKNOWN)
            reasons.append(tr("Windows metadata (version unknown or incomplete)"));
        else
            reasons.append(tr("Windows metadata"));
    }
    if (pending & ISO_PENDING_SECURE_BOOT)
        reasons.append(tr("Secure Boot checks"));
    if (pending & ISO_PENDING_TRUNCATED)
        reasons.append(tr("image size mismatch"));
    return reasons.join(tr(", "));
}

QString SelectionController::imageStatusText() const
{
    if (selection_.rawImage && selection_.imageState == ImageState::Complete)
        return writer_.isAvailable() ? tr("Raw image selected (DD Image).")
            : tr("Raw image selected; media writing unavailable.");
    switch (selection_.imageState)
    {
    case ImageState::Scanning:
        return tr("Scanning image...");
    case ImageState::Cancelling:
        return tr("Cancelling...");
    case ImageState::Complete:
        if (IS_DD_BOOTABLE(selection_.imageAnalysis.report))
            return tr("ISOHybrid analysis complete; ISO writing unavailable.");
        return selection_.imageAnalysis.filesystem == ISO_FILESYSTEM_UDF
            ? tr("UDF analysis complete; media writing unavailable.")
            : tr("ISO9660 analysis complete; media writing unavailable.");
    case ImageState::Partial:
        return tr("Image analysis incomplete: %1. Media writing unavailable.").arg(pendingChecks());
    case ImageState::Failed:
        return selection_.imageProblem;
    case ImageState::Cancelled:
        return tr("Image analysis cancelled.");
    case ImageState::None:
        return {};
    }
    return {};
}

void SelectionController::logImageReport(bool ddRequired)
{
    const ISO_ANALYSIS& analysis = selection_.imageAnalysis;
    const RUFUS_IMG_REPORT& report = analysis.report;
    if (!analysis.tree_complete)
        return;
    emit logMessage(tr("Volume label: %1").arg(QString::fromUtf8(report.label)));
    logWindowsMetadata();
    emit logMessage(tr("Files: %1; projected size: %2 bytes")
        .arg(analysis.files).arg(report.projected_size));
    if (report.has_syslinux && report.sl_detected_version == 0)
        emit logMessage(tr("Syslinux image version: unknown."));
    if (report.sl_version_inferred)
        emit logMessage(tr("Syslinux fallback: %1 (inferred; native installer compatibility unchecked).")
            .arg(QString::fromUtf8(report.sl_version_str)));
    if (IS_DD_BOOTABLE(report))
    {
        emit logMessage(tr("ISOHybrid detected: Rufus boot marker present (DD candidate)."));
        emit logMessage(ddRequired
            ? tr("Rufus image policy requires DD mode for this image.")
            : tr("Rufus image policy does not require DD mode; this is not an ISO-mode recommendation."));
    }
    else
        emit logMessage(tr("No Rufus boot marker: this image is not a DD candidate."));
    emit logMessage(tr("Native ISO-mode installer compatibility is unchecked. ISO writing remains unavailable."));
}

void SelectionController::logWindowsMetadata()
{
    const WINDOWS_IMAGE_ANALYSIS& windows = selection_.windowsAnalysis;
    if (windows.state == WINDOWS_IMAGE_UNCHECKED)
        return;
    if (windows.state == WINDOWS_IMAGE_FAILED)
    {
        emit logMessage(tr("Windows version: unavailable (image metadata could not be read)"));
        return;
    }
    const RUFUS_WIM_METADATA& metadata = windows.metadata;
    emit logMessage(tr("WIM format: 0x%1; images: %2; part: %3/%4")
        .arg(metadata.archive.version, 0, 16).arg(metadata.archive.image_count)
        .arg(metadata.archive.part_number).arg(metadata.archive.total_parts));
    const winver_t& version = metadata.win_version;
    if (windows.state == WINDOWS_IMAGE_KNOWN)
        emit logMessage(tr("Windows version: %1.%2 (Build %3.%4)")
            .arg(version.major).arg(version.minor).arg(version.build).arg(version.revision));
    else if (version.major != 0 && version.build != 0 && !(metadata.valid_fields & WIM_VERSION_REVISION))
        emit logMessage(tr("Windows version: %1.%2 (Build %3; revision unknown)")
            .arg(version.major).arg(version.minor).arg(version.build));
    else
        emit logMessage(tr("Windows version: unknown (missing, invalid or unsupported version fields)"));
}

} // namespace rufus
