// SPDX-License-Identifier: GPL-3.0-or-later
#include "selection_controller.h"
#include "device_policy.h"
#include "platform/device_selection.h"
#include "platform/device_operations.h"
extern "C" {
#include "../../src/libcdio/cdio/logging.h"
}
#include "platform/secure_boot_cache.h"
#include "device_size.h"
#include "platform/format_options.h"
#include "platform/syslinux_native.h"
#include "platform/prepare_source.h"

#include <QFile>
#include <QFileInfo>
#include <QSet>
#include "application_storage.h"
#include <QtMath>

#include <algorithm>
#include <iterator>
#include <limits>
#include <cstring>

namespace rufus
{
namespace
{
constexpr char enhancedInstallSetting[] = "EnableWindowsDualUefiBiosMode";
constexpr char disableWindowsMbrSetting[] = "DisableRufusMbr";
constexpr char extraHashesSetting[] = "EnableExtraHashes";
constexpr char ignoreBootMarkerSetting[] = "IgnoreBootMarker";
constexpr quint64 imageSizeMarginBytes = 4 * 1024; // Win32 BootCheck VHD/footer allowance.

const Device* findDevice(const QList<Device>& devices, const QString& id)
{
    for (const Device& device : devices)
    {
        if (device.id == id)
            return &device;
    }
    return nullptr;
}

bool smallerDevice(const Device& first, const Device& second)
{
    return first.size < second.size;
}


} // namespace

SelectionController::SelectionController(QObject* parent)
    : QObject(parent)
{
    LoadCachedSecureBootPolicy(ApplicationFilesDirectory(), secureBootPolicy_);
    ApplicationSettings settings;
    usbDebug_ = settings.value(QStringLiteral("EnableUsbDebug"),false).toBool();
    cdio_loglevel_default = usbDebug_ ? CDIO_LOG_INFO : CDIO_LOG_WARN;
    detectFakes_ = !settings.value(QStringLiteral("DisableFakeDrivesCheck"), false).toBool();
    enhancedInstall_ = settings.value(QLatin1String(enhancedInstallSetting), false).toBool();
    windowsRufusMbr_ = !settings.value(QLatin1String(disableWindowsMbrSetting), false).toBool();
    extraHashes_ = settings.value(QLatin1String(extraHashesSetting), false).toBool();
    ignoreBootMarker_ = settings.value(QLatin1String(ignoreBootMarkerSetting), false).toBool();
    if (settings.value(QStringLiteral("DisableVHDs"), false).toBool())
        deviceClasses_ &= ~DeviceClassMask(DeviceClass::Virtual);
    if (settings.value(QStringLiteral("EnableVMDKDetection"), false).toBool())
        deviceClasses_ |= DeviceClassMask(DeviceClass::Vmware);
    fileIndexing_ = settings.value(QStringLiteral("EnableFileIndexing"), false).toBool();
    preserveTimestamps_ = settings.value(QStringLiteral("PreserveTimestamps"), false).toBool();
    const int extVersion = settings.value(QStringLiteral("UseExtVersion"), 3).toInt();
    persistenceExtVersion_ = extVersion >= 2 && extVersion <= 4 ? extVersion : 3;
    const quint32 udf = settings.value(QStringLiteral("UseUdfVersion"), quint32(0)).toUInt();
    udfRevision_ = udf == 0 ? 0x0201 : udf <= UINT16_MAX ? quint16(udf) : UINT16_MAX;
    connect(&hasher_, &ImageHashController::changed, this, &SelectionController::selectionChanged);
    connect(&hasher_, &ImageHashController::logMessage, this, &SelectionController::logMessage);
    connect(&hasher_, &ImageHashController::finished, this, &SelectionController::selectPendingDevice);
    connect(&saver_, &ImageSaveController::changed, this, &SelectionController::selectionChanged);
    connect(&saver_, &ImageSaveController::logMessage, this, &SelectionController::logMessage);
    connect(&saver_, &ImageSaveController::finished, this, &SelectionController::selectPendingDevice);
    connect(&downloader_,&ImageDownload::changed,this,&SelectionController::selectionChanged);
    connect(&downloader_,&ImageDownload::finished,this,&SelectionController::downloadFinished);
    connect(&analysisThread_, &QThread::finished,
            this, &SelectionController::analysisFinished, Qt::QueuedConnection);
    connect(&writer_, &RawWriteController::badBlocksReviewRequired, this, &SelectionController::badBlocksReviewRequired);
    connect(&isoWriter_, &IsoWriteController::badBlocksReviewRequired, this, &SelectionController::badBlocksReviewRequired);
    connect(&writer_, &RawWriteController::changed, this, &SelectionController::selectionChanged);
    connect(&writer_, &RawWriteController::logMessage, this, &SelectionController::logMessage);
    connect(&isoWriter_, &IsoWriteController::changed, this, &SelectionController::selectionChanged);
    connect(&isoWriter_, &IsoWriteController::logMessage, this, &SelectionController::logMessage);
    connect(&isoWriter_, &IsoWriteController::ddRequested, this, &SelectionController::startHybridDd);
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

bool SelectionController::isWritingActive() const
{
    return writer_.isActive() || isoWriter_.isActive() || saver_.isActive() || (deviceOperation_ && deviceOperation_->isActive());
}

bool SelectionController::isBusy() const
{
    return isAnalyzing() || isWritingActive() || hasher_.isActive() || downloader_.isActive() || selection_.imageState == ImageState::ChoosingStore;
}

bool SelectionController::canHashImage() const
{
    const bool accepted = selection_.imageState == ImageState::Complete
        || selection_.imageState == ImageState::Partial;
    return !isBusy() && selection_.bootType == BT_IMAGE && accepted
        && !selection_.imagePath.isEmpty();
}

bool SelectionController::canSaveDevice() const
{
    return !isBusy() && selection_.discoveryAvailable && findDevice(selection_.devices, selection_.deviceId) != nullptr;
}

bool SelectionController::startDeviceOperation(int operation)
{
    if (isBusy() || deviceHelper_.isEmpty() || (operation != ResetUsbPort && operation != ToggleEspType)) return false;
    const Device* device = findDevice(selection_.devices, selection_.deviceId);
    if (!device || device->readOnly) return false;
    PrepareImageRequest request;
    request.deviceOperation = operation;
    request.nonBootable = request.isDevice = true;
    request.path = device->path; request.device = device->number; request.size = device->size;
    request.logicalSectorSize = device->logicalSectorSize;
    request.physicalSectorSize = device->physicalSectorSize;
    request.identity = device->identity; request.objectPath = device->objectPath;
    request.serviceOwner = device->serviceOwner; request.deviceClass = device->deviceClass;
    request.allowFixedUsb = selection_.allowFixedUsb; request.diskSequence = ReadDiskSequence(device->number);
    FreezeDevicePolicy(request);
    ApplicationSettings settings;
    for (int slot = 1; slot <= 8; ++slot) {
        const QString guid = settings.value(QStringLiteral("ToggleEsp%1").arg(slot, 2, 10, QLatin1Char('0'))).toString();
        if (!QUuid(guid).isNull()) request.espRecords.append(QUuid(guid).toString());
    }
    writer_.reset(); isoWriter_.reset(); saver_.reset(); hasher_.reset(); downloader_.reset();
    restoredEsp_.clear();
    deviceOperation_ = std::make_unique<PrepareImageProcess>(deviceHelper_);
    connect(deviceOperation_.get(), &PrepareImageProcess::changed, this, &SelectionController::selectionChanged);
    connect(deviceOperation_.get(), &PrepareImageProcess::logMessage, this, &SelectionController::logMessage);
    connect(deviceOperation_.get(), &PrepareImageProcess::espRecordRequired, this, &SelectionController::saveEspRecord);
    connect(deviceOperation_.get(), &PrepareImageProcess::finished, this, &SelectionController::deviceOperationFinished);
    return deviceOperation_->start(request);
}

void SelectionController::saveEspRecord(const QString& guid, bool restore)
{
    ApplicationSettings settings;
    bool saved = true;
    if (restore) restoredEsp_ = guid;
    else {
        QStringList records;
        for (int slot = 1; slot <= 8; ++slot) {
            const QString value = settings.value(QStringLiteral("ToggleEsp%1").arg(slot, 2, 10, QLatin1Char('0'))).toString();
            if (!value.isEmpty() && value != guid) records.append(value);
        }
        if (records.size() >= 8) records.removeFirst();
        records.append(guid);
        for (int slot = 1; slot <= 8; ++slot)
            settings.setValue(QStringLiteral("ToggleEsp%1").arg(slot, 2, 10, QLatin1Char('0')), records.value(slot - 1));
        settings.sync(); saved = settings.status() == QSettings::NoError;
    }
    if (!saved) emit logMessage(tr("ESP restoration record could not be saved; the device was not changed."));
    deviceOperation_->answerEspRecord(saved);
}

void SelectionController::deviceOperationFinished()
{
    if (deviceOperation_->state() == PrepareImageProcess::State::Complete && !restoredEsp_.isEmpty()) {
        ApplicationSettings settings;
        for (int slot = 1; slot <= 8; ++slot) {
            const QString key = QStringLiteral("ToggleEsp%1").arg(slot, 2, 10, QLatin1Char('0'));
            if (QUuid(settings.value(key).toString()) == QUuid(restoredEsp_)) settings.setValue(key, QString());
        }
    }
    restoredEsp_.clear();
    emit devicesRefreshRequested(); emit selectionChanged();
}

bool SelectionController::startSaveDevice(const QString& path)
{
    if (!canSaveDevice()) return false;
    const Device* device = findDevice(selection_.devices, selection_.deviceId);
    if (deviceOperation_ && !deviceOperation_->isActive()) deviceOperation_.reset();
    writer_.reset();
    downloader_.reset();
    isoWriter_.reset();
    hasher_.reset();
    return saver_.start(*device, path, selection_.allowFixedUsb);
}

void SelectionController::startHashes()
{
    if (!canHashImage())
        return;
    if (deviceOperation_ && !deviceOperation_->isActive()) deviceOperation_.reset();
    writer_.reset();
    downloader_.reset();
    isoWriter_.reset();
    saver_.reset();
    hasher_.start(selection_.imageSource, extraHashes_);
}

void SelectionController::toggleExtraHashes()
{
    if (isBusy())
        return;
    extraHashes_ = !extraHashes_;
    ApplicationSettings settings;
    settings.setValue(QLatin1String(extraHashesSetting), extraHashes_);
    emit selectionChanged();
}

void SelectionController::selectPendingDevice()
{
    if (initialDeviceSelectionPending_ && !selection_.devices.isEmpty())
        selectDevice(selection_.devices.first().id);
}

void SelectionController::setDiscovery(const QList<Device>& devices, const QString& problem)
{
    if (!problem.isEmpty() && problem != selection_.discoveryProblem)
        emit logMessage(problem);
    selection_.discoveryAvailable = problem.isEmpty();
    selection_.discoveryProblem = problem;
    setDevices(problem.isEmpty() ? devices : QList<Device>());
}

void SelectionController::setAllowFixedUsb(bool enabled)
{
    if (isBusy() || selection_.allowFixedUsb == enabled)
        return;
    selection_.allowFixedUsb = enabled;
    setDevices(selection_.devices);
    emit fixedUsbChanged(enabled);
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
        const bool selectedClass = (DeviceClassMask(device.deviceClass) & (deviceClasses() | DeviceClassMask(DeviceClass::Optical))) != 0
            && (device.defaultVisible || selection_.allowFixedUsb);
        const bool supportedSize = device.size > 0 && device.size <= quint64(std::numeric_limits<qint64>::max());
        const bool identified = !device.id.isEmpty() && !device.path.isEmpty() && !ambiguous.contains(device.id);
        if (selectedClass && supportedSize && identified)
            available.append(device);
    }
    std::stable_sort(available.begin(), available.end(), smallerDevice);

    const Device* previous = findDevice(selection_.devices, selection_.deviceId);
    const Device* current = findDevice(available, selection_.deviceId);
    const bool sameMedia = previous && current
        && previous->path == current->path && previous->size == current->size;
    const bool sameIdentity = previous && current
        && previous->identity == current->identity && previous->number == current->number;
    const bool sameService = previous && current && previous->serviceOwner == current->serviceOwner;
    const bool selectionPreserved = sameMedia && sameIdentity && sameService;
    const bool becameReadOnly = selectionPreserved && !previous->readOnly && current->readOnly;
    if (becameReadOnly)
    {
        writer_.invalidate(tr("The selected device is now read-only."));
        isoWriter_.invalidate(tr("The selected device is now read-only."));
    }
    const bool formatChanged = selectionPreserved && (previous->fileSystemType != current->fileSystemType
        || previous->fileSystemVersion != current->fileSystemVersion);
    const bool invalidated = !selection_.deviceId.isEmpty() && !selectionPreserved;
    selection_.devices = available;
    if (invalidated)
    {
        selection_.deviceId.clear();
        selection_.fileSystems = 0;
        selection_.defaultFileSystem = FS_UNKNOWN;
        selection_.fileSystem = FS_UNKNOWN;
        selection_.clusterSizes = {};
        selection_.clusterSize = 0;
        selection_.syslinux = false;
        selection_.partition = PartitionScheme::Gpt;
        writer_.invalidate(tr("The selected device changed or was removed."));
        isoWriter_.invalidate(tr("The selected device changed or was removed."));
        emit logMessage(tr("The selected device changed or was removed. Select a device again."));
    }
    // Match Win32's first target, but never replace an invalidated selection.
    const bool selectInitialDevice = initialDeviceSelectionPending_ && !available.isEmpty();
    if (selectInitialDevice && !isWritingActive() && !hasher_.isActive())
    {
        selectDevice(available.first().id);
        return;
    }
    const bool idleWithoutImage = (selection_.imagePath.isEmpty() || selection_.bootType != BT_IMAGE) && !isBusy();
    if (formatChanged && idleWithoutImage)
        updateFormatOptions(preferredFileSystem_);
    updateVolumeLabel();
    emit selectionChanged();
}

void SelectionController::selectDevice(const QString& id)
{
    if (isWritingActive() || hasher_.isActive() || downloader_.isActive() || id == selection_.deviceId)
        return;
    if (deviceOperation_ && !deviceOperation_->isActive()) deviceOperation_.reset();
    writer_.reset();
    downloader_.reset();
    isoWriter_.reset();
    saver_.reset();
    const Device* device = findDevice(selection_.devices, id);
    selection_.deviceId = device ? id : QString();
    updatePartitionScheme();
    updateFormatOptions(preferredFileSystem_);
    updateVolumeLabel();
    if (device)
    {
        initialDeviceSelectionPending_ = false;
        emit logMessage(tr("Device selected: %1 (%2)").arg(device->name, device->path));
    }
    else if (!id.isEmpty())
        emit logMessage(tr("The selected device is no longer available."));
    emit selectionChanged();
}

bool SelectionController::downloadImage(const QUrl& url,const QString& path)
{
    if (isBusy()) return false;
    if (deviceOperation_ && !deviceOperation_->isActive()) deviceOperation_.reset();
    writer_.reset();
    downloader_.reset();
    isoWriter_.reset(); saver_.reset(); hasher_.reset();
    return downloader_.start(url,path);
}
void SelectionController::downloadFinished()
{
    if (downloader_.state()==ImageDownloadState::Complete) {
        const QString path=downloader_.result().path;
        emit logMessage(tr("Downloaded image: %1\nSHA256: %2").arg(path,QString::fromLatin1(downloader_.result().sha256.toHex())));
        downloader_.reset(); selectImage(path);
    } else emit logMessage(downloader_.result().problem);
    selectPendingDevice();
}

void SelectionController::selectImage(const QString& path)
{
    if (isWritingActive() || hasher_.isActive() || downloader_.isActive() || path.isEmpty())
        return;
    archive_ = {};
    selection_.archivePath.clear();
    if (deviceOperation_ && !deviceOperation_->isActive()) deviceOperation_.reset();
    writer_.reset();
    downloader_.reset();
    isoWriter_.reset();
    saver_.reset();

    hasher_.reset();
    readOptionsRescan_ = false;
    selection_.bootType = BT_IMAGE;
    selection_.partition = PartitionScheme::Gpt;
    ++imageRequest_;
    if (analysisActive_)
        analysisThread_.requestInterruption();
    selection_.imagePath.clear();
    selection_.imageSize = 0;
    selection_.persistenceSize = 0;
    selection_.imageProblem.clear();
    selection_.imageAnalysis = {};
    selection_.windowsAnalysis = {};
    selection_.imageSource = {};
    selection_.ffuStores.clear();
    if (!userChangedLabel_)
        selection_.volumeLabel.clear();
    selection_.clusterSize = selection_.clusterSizes.Default;
    selection_.imageState = ImageState::None;
    selection_.rawImage = false;
    selection_.syslinux = false;
    selection_.windowsToGo = false;
    selection_.windowsToGoTarget = TT_UEFI;
    preferredWindowsTarget_ = TT_MAX;
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
        const QString suffix = info.suffix().toLower();
        if (suffix == QStringLiteral("img") || suffix == QStringLiteral("usb") || suffix == QStringLiteral("wic"))
        {
            selection_.imageProblem = CheckRawImageFile(selection_.imagePath, ignoreBootMarker_);
            selection_.rawImage = selection_.imageProblem.isEmpty();
            if (selection_.rawImage && CapturePrepareSource(selection_.imagePath, selection_.imageSource) != 0)
            {
                selection_.rawImage = false;
                selection_.imageProblem = tr("Cannot retain the image source identity. Select it again.");
            }
            if (selection_.rawImage)
            {
                selection_.imageSource.ignoreBootMarker = ignoreBootMarker_;
                selection_.imageState = ImageState::Complete;
                userChangedLabel_ = false;
                selection_.volumeLabel.clear();
            }
        }
    }

    updateFormatOptions();
    if (!selection_.imageProblem.isEmpty())
    {
        selection_.imageState = ImageState::Failed;
        clearRejectedImage();
        emit logMessage(selection_.imageProblem);
    }
    else
    {
        emit logMessage(tr("Image selected: %1").arg(selection_.imagePath));
        if (!analysisActive_ && !selection_.rawImage)
            startAnalysis();
    }
    emit selectionChanged();
    if (!analysisActive_)
        emit imageSelectionFinished();
}

void SelectionController::clearRejectedImage()
{
    // ImageScanThread relinquishes image_path before restoring Boot selection.
    // Keep the failure reason for the log and notification, not a stale image.
    selection_.imagePath.clear();
    selection_.imageSize = 0;
    selection_.persistenceSize = 0;
    selection_.imageSource = {};
    selection_.imageAnalysis = {};
    selection_.windowsAnalysis = {};
    selection_.rawImage = false;
    selection_.syslinux = false;
    selection_.windowsToGo = false;
    updateFormatOptions(preferredFileSystem_);
    updateVolumeLabel();
}

bool SelectionController::canStartWrite() const
{
    const Device* device = findDevice(selection_.devices, selection_.deviceId);
    if (device && device->readOnly) return false;
    if (selection_.bootType != BT_IMAGE)
    {
        const bool formatReady = selection_.fileSystem != FS_UNKNOWN && selection_.clusterSize != 0;
        const bool targetReady = selection_.discoveryAvailable && device && !device->identity.isEmpty();
        return !isBusy() && isoWriter_.isAvailable() && formatReady && targetReady;
    }
    const bool writerReady = !isBusy() && writer_.isAvailable();
    const bool imageReady = selection_.rawImage && selection_.imageState == ImageState::Complete;
    const bool targetReady = device && !device->identity.isEmpty()
        && (!selection_.rawImage || device->size >= (selection_.imageSource.hasCompressedImage()
            ? (selection_.imageSource.ffuMinimumSize != 0 ? selection_.imageSource.ffuMinimumSize
                : selection_.imageSource.decodedImageSize) : selection_.imageSize));
    if (!selection_.discoveryAvailable || !targetReady)
        return false;
    if (selection_.windowsToGo)
        return !isBusy() && isoWriter_.isAvailable() && selection_.fileSystem == FS_NTFS && selection_.clusterSize != 0;
    const IMAGE_WRITE_CHOICES choices = isoWriteChoices(selection_.persistenceSize, selection_.partition == PartitionScheme::Gpt, selection_.fileSystem != FS_NTFS);
    bool isoReady = false;
    for (int i = 0; i < choices.count; ++i)
        isoReady = isoReady || isoWriter_.isAvailable(choices.mode[i]);
    return writerReady && (imageReady || isoReady);
}

IMAGE_WRITE_CHOICES SelectionController::isoWriteChoices(quint64 persistenceSize, bool gpt, bool fat) const
{
    if (selection_.bootType != BT_IMAGE)
        return {};
    if (selection_.windowsToGo)
    {
        IMAGE_WRITE_CHOICES choices = {};
        if (!analysisActive_ && canSelectWindowsToGo())
        {
            choices.count = 1;
            choices.mode[0] = IMAGE_WRITE_ISO;
        }
        return choices;
    }
    // Unknown Windows metadata does not prevent DD. ISO admission below still
    // requires the saved Alt+E choice for a mixed WinPE/Syslinux image.
    const bool legacyCandidate = IsLegacyWinPeImage(selection_.imageAnalysis, true, true, true)
        || HAS_BOOTMGR(selection_.imageAnalysis.report);
    const quint32 allowedPending = ISO_PENDING_SECURE_BOOT | ISO_PENDING_EMBEDDED_EFI
        | (legacyCandidate ? ISO_PENDING_WINDOWS : 0);
    const bool usablePartial = selection_.imageState == ImageState::Partial
        && selection_.imageAnalysis.tree_complete && (selection_.imageAnalysis.pending & ~allowedPending) == 0;
    const bool reportReady = selection_.imageState == ImageState::Complete || usablePartial;
    if (analysisActive_ || !reportReady)
        return {};
    const RUFUS_IMG_REPORT& report = selection_.imageAnalysis.report;
    const IMAGE_WRITE_CHOICES choices = GetFinalImageWriteChoices(&report, persistenceSize, gpt, fat);
    const bool legacy = report.winpe != 0 || report.uses_minint;
    const bool windows = HAS_WINDOWS(report) || HAS_WININST(report) || HAS_WINPE(report) || report.uses_minint;
    const bool grub4dosBios = !windows && !IS_EFI_BOOTABLE(report)
        && GetSecondaryBootType(BT_IMAGE, &report, false) == BT_GRUB4DOS;
    const bool legacyRoute = canSelectWindowsBios() || canSelectSyslinux() || canSelectGrub() || canSelectKolibri();
    const bool unavailableBios = (legacy && !IS_EFI_BOOTABLE(report) && !legacyRoute)
        || (grub4dosBios && !canSelectGrub());
    if (unavailableBios)
    {
        IMAGE_WRITE_CHOICES supported = {};
        for (int i = 0; i < choices.count; ++i)
            if (choices.mode[i] == IMAGE_WRITE_DD)
                supported.mode[supported.count++] = IMAGE_WRITE_DD;
        return supported; // An unavailable ISO lane must not hide a valid DD image.
    }
    const bool windowsBiosOnly = !IS_EFI_BOOTABLE(report)
        && IsWindowsBiosImage(selection_.imageAnalysis, selection_.windowsAnalysis, true, true, enhancedInstall(), true, true);
    if (windowsBiosOnly && !canSelectWindowsBios())
        return {};
    return choices;
}

PrepareImageRequest SelectionController::selectedImageRequest() const
{
    PrepareImageRequest source = selection_.imageSource;
    source.rockRidge = rockRidge_; source.joliet = joliet_;
    source.preserveTimestamps = preserveTimestamps_;
    const bool ntfs = selection_.fileSystem == FS_NTFS && !selection_.rawImage;
    source.ntfsCompression = ntfs && ntfsCompression_; source.fileIndexing = ntfs && fileIndexing_;
    const Device* device = findDevice(selection_.devices, selection_.deviceId);
    source.deviceClass = device ? device->deviceClass : DeviceClass::Usb;
    source.allowFixedUsb = selection_.allowFixedUsb;
    source.extendedLabel = extendedLabel_ && canExtendedLabel();
    source.runtimeValidation = runtimeValidationEnabled();
    source.oldBiosFixes = oldBiosFixes_ && canOldBiosFixes();
    source.quickFormat = quickFormat();
    source.badBlocksPattern = badBlocksEnabled_ ? badBlocksPattern_ : -1;
    source.detectFakes = detectFakes_;
    if (selection_.windowsToGo)
    {
        source.windowsToGo = true;
        source.windowsToGoTarget = selection_.windowsToGoTarget;
        source.mbr = selection_.partition == PartitionScheme::Mbr;
        source.uefiNtfs = source.mbr && biosImageUefiNtfs(FS_NTFS);
        source.windowsRufusMbr = !source.mbr || windowsRufusMbr_;
        source.windowsEdition = 1;
        source.archive = archive_;
        return source;
    }
    source.syslinux = selection_.syslinux;
    source.windowsBios = selection_.partition == PartitionScheme::Mbr && canSelectWindowsBios();
    source.kolibriOs = selection_.partition == PartitionScheme::Mbr && !source.syslinux
        && !source.windowsBios && canSelectKolibri();
    const bool grubImage = selection_.partition == PartitionScheme::Mbr && !source.syslinux
        && !source.windowsBios && !source.kolibriOs;
    source.grub4dos = grubImage && GetSecondaryBootType(BT_IMAGE, &selection_.imageAnalysis.report, false) == BT_GRUB4DOS;
    source.grub2 = grubImage && !source.grub4dos;
    source.mbr = grubImage || source.windowsBios || source.kolibriOs;
    source.windowsRufusMbr = !(source.mbr || source.syslinux) || windowsRufusMbr_;
    source.uefiNtfs = (source.syslinux || source.mbr) && biosImageUefiNtfs(selection_.fileSystem);
    source.enhancedInstall = enhancedInstall() && !source.syslinux && !grubImage && !source.kolibriOs;
    source.persistenceSize = selection_.persistenceSize;
    source.persistenceExtVersion = source.persistenceSize != 0 ? persistenceExtVersion_ : 3;
    if (selection_.fileSystem >= FS_FAT16 && selection_.fileSystem < FS_EXT2) source.archive = archive_;
    return source;
}

void SelectionController::selectArchive(const QString& path)
{
    if (isBusy()) return;
    archive_ = {};
    selection_.archivePath.clear();
    if (deviceOperation_ && !deviceOperation_->isActive()) deviceOperation_.reset();
    writer_.reset();
    downloader_.reset();
    isoWriter_.reset(); saver_.reset();
    if (!path.isEmpty()) {
        const QString absolute = QFileInfo(path).absoluteFilePath();
        const int error = CapturePrepareArchive(absolute, archive_);
        if (error == 0) {
            selection_.archivePath = archive_.path;
            emit logMessage(tr("Using supplementary ZIP archive: %1").arg(archive_.path));
        } else {
            // Keep the requested attachment visible and make START's preflight
            // refuse it, rather than silently creating media without the files.
            archive_.path = absolute; selection_.archivePath = absolute;
            emit logMessage(tr("Cannot read the supplementary ZIP archive: %1").arg(QString::fromLocal8Bit(strerror(error))));
        }
    }
    emit selectionChanged();
}

bool SelectionController::canFullFormat() const
{
    const Device* device = findDevice(selection_.devices, selection_.deviceId);
    return device != nullptr && !device->readOnly && SupportsFullFormat(selection_.fileSystem, device->size);
}

bool SelectionController::quickFormat() const
{
    return !canFullFormat() || quickFormat_;
}

bool SelectionController::canExtendedLabel() const
{
    const int filesystem = selection_.fileSystem;
    const bool supported = filesystem == FS_FAT16 || filesystem == FS_FAT32 || filesystem == FS_NTFS || filesystem == FS_EXFAT || filesystem == FS_UDF;
    return supported && !(selection_.bootType == BT_IMAGE && selection_.rawImage);
}

bool SelectionController::canOldBiosFixes() const
{
    if (selection_.windowsToGo && selection_.windowsToGoTarget != TT_BIOS) return false;
    if (selection_.partition != PartitionScheme::Mbr || selection_.bootType == BT_NON_BOOTABLE
        || selection_.bootType == BT_UEFI_NTFS) return false;
    if (selection_.bootType != BT_IMAGE) return true;
    const RUFUS_IMG_REPORT& report = selection_.imageAnalysis.report;
    return !selection_.rawImage && IS_BIOS_BOOTABLE(report) && !IS_DD_ONLY(report);
}

void SelectionController::setOldBiosFixes(bool enabled)
{
    if (isBusy() || !canOldBiosFixes() || enabled == oldBiosFixes_) return;
    oldBiosFixes_ = enabled;
    const int filesystem = selection_.fileSystem;
    const quint32 cluster = selection_.clusterSize;
    updateFormatOptions(filesystem);
    if (cluster != 0 && (selection_.clusterSizes.Allowed & cluster)) selection_.clusterSize = cluster;
    clampPersistenceSize();
    isoWriter_.reset(); writer_.reset(); saver_.reset();
    emit selectionChanged();
}

void SelectionController::setExtendedLabel(bool enabled)
{
    if (isBusy() || enabled == extendedLabel_) return;
    extendedLabel_ = enabled;
    isoWriter_.reset(); writer_.reset(); saver_.reset();
    emit selectionChanged();
}

void SelectionController::setQuickFormat(bool quick)
{
    if (isBusy() || (!quick && !canFullFormat()) || quick == quickFormat_) return;
    quickFormat_ = quick;
    isoWriter_.reset(); writer_.reset(); saver_.reset();
    emit selectionChanged();
}

void SelectionController::setBadBlocksEnabled(bool enabled)
{
    if (isBusy() || enabled == badBlocksEnabled_) return;
    badBlocksEnabled_ = enabled;
    isoWriter_.reset(); writer_.reset(); saver_.reset();
    emit selectionChanged();
}

void SelectionController::setBadBlocksPattern(int pattern)
{
    if (isBusy() || pattern < 0 || pattern >= BADLOCKS_PATTERN_TYPES || pattern == badBlocksPattern_) return;
    badBlocksPattern_ = pattern;
    isoWriter_.reset(); writer_.reset(); saver_.reset();
    emit selectionChanged();
}

void SelectionController::toggleFakeDetection()
{
    if (isBusy()) return;
    detectFakes_ = !detectFakes_;
    ApplicationSettings settings;
    settings.setValue(QStringLiteral("DisableFakeDrivesCheck"), !detectFakes_);
    isoWriter_.reset(); writer_.reset(); saver_.reset();
    emit selectionChanged();
}

void SelectionController::answerBadBlocks(int choice)
{
    if (writer_.isActive()) writer_.answerBadBlocks(choice);
    else if (isoWriter_.isActive()) isoWriter_.answerBadBlocks(choice);
}

bool SelectionController::canSetPersistence() const
{
    const bool image = selection_.bootType == BT_IMAGE && !selection_.rawImage
        && selection_.imageAnalysis.tree_complete && HAS_PERSISTENCE(selection_.imageAnalysis.report);
    if (!image)
        return false;
    const auto choices = isoWriteChoices(0, selection_.partition == PartitionScheme::Gpt,
        selection_.fileSystem != FS_NTFS);
    for (int i = 0; i < choices.count; ++i)
        if (choices.mode[i] == IMAGE_WRITE_ISO)
            return true;
    return false;
}

bool SelectionController::canRuntimeValidate() const
{
    const bool imageReady = selection_.imageState == ImageState::Complete || selection_.imageState == ImageState::Partial;
    if (!imageReady || selection_.rawImage || !selection_.imageAnalysis.tree_complete) return false;
    return CanUseRuntimeUefiValidation(&selection_.imageAnalysis.report, selection_.bootType,
        selection_.windowsToGo, selection_.partition == PartitionScheme::Mbr, enhancedInstall_);
}

void SelectionController::setRuntimeValidation(bool enabled)
{
    if (isBusy() || !canRuntimeValidate() || runtimeValidation_ == enabled) return;
    runtimeValidation_ = enabled;
    isoWriter_.reset();
    emit selectionChanged();
}

quint64 SelectionController::maximumPersistenceSize() const
{
    const Device* device = findDevice(selection_.devices, selection_.deviceId);
    if (!device || !canSetPersistence() || selection_.clusterSize == 0)
        return 0;
    PrepareImageRequest request = selectedImageRequest();
    request.size = device->size;
    request.logicalSectorSize = device->logicalSectorSize;
    request.physicalSectorSize = device->physicalSectorSize;
    request.fileSystem = selection_.fileSystem;
    request.clusterSize = selection_.clusterSize;
    request.label = QStringLiteral("RUFUS");
    request.persistenceExtVersion = persistenceExtVersion_;
    const quint64 projected = selection_.imageAnalysis.report.projected_size;
    const quint64 reserve = projected / 10;
    if (projected > device->size || reserve > device->size - projected)
        return 0;
    const quint64 required = projected + reserve;
    quint64 lower = MinimumPersistenceBytes / PersistenceUnitBytes;
    quint64 upper = device->size / PersistenceUnitBytes;
    quint64 accepted = 0;
    // Whole MiB choices and the production planner keep the slider inside both
    // the source-space allowance and the selected filesystem's geometry.
    while (lower <= upper)
    {
        const quint64 middle = lower + (upper - lower) / 2;
        request.persistenceSize = middle * PersistenceUnitBytes;
        GPT_IMAGE_RESULT partition = {};
        const int layoutError = PlanFormatPartition(request, partition);
        const bool fits = layoutError == 0 && partition.size >= required;
        if (fits && CheckFormatSettings(request, partition) == 0)
        {
            accepted = middle;
            lower = middle + 1;
        }
        else
            upper = middle - 1;
    }
    return accepted * PersistenceUnitBytes;
}

void SelectionController::clampPersistenceSize()
{
    if (selection_.persistenceSize != 0)
        selection_.persistenceSize = qMin(selection_.persistenceSize, maximumPersistenceSize());
}

void SelectionController::setPersistenceSize(quint64 bytes)
{
    if (isBusy() || !canSetPersistence())
        return;
    bytes = qMin(bytes, maximumPersistenceSize());
    bytes -= bytes % PersistenceUnitBytes;
    if (bytes != 0 && bytes < MinimumPersistenceBytes)
        bytes = qMin(MinimumPersistenceBytes, maximumPersistenceSize());
    if (bytes == selection_.persistenceSize)
        return;
    selection_.persistenceSize = bytes;
    isoWriter_.reset();
    if (deviceOperation_ && !deviceOperation_->isActive()) deviceOperation_.reset();
    writer_.reset();
    downloader_.reset();
    saver_.reset();
    emit selectionChanged();
}

void SelectionController::startZero(bool fast)
{
    if (isBusy()) return;
    downloader_.reset();
    const Device* device = findDevice(selection_.devices, selection_.deviceId);
    if (!device || device->readOnly || !selection_.discoveryAvailable) return;
    isoWriter_.reset();
    saver_.reset();
    writer_.prepareZero(*device, fast ? ZERO_FAST : ZERO_FULL, selection_.allowFixedUsb,
        badBlocksEnabled_ ? badBlocksPattern_ : -1, detectFakes_);
}

void SelectionController::answerRevocationWarning(bool proceed)
{
    if (!revocationWarningPending_) return;
    revocationWarningPending_ = false;
    if (!proceed || revocationWarningRequest_ != imageRequest_) return;
    revocationWarningAccepted_ = true;
    startWrite();
    revocationWarningAccepted_ = false;
}

void SelectionController::startWrite()
{
    if (!canStartWrite())
        return;
    const Device* device = findDevice(selection_.devices, selection_.deviceId);
    const quint64 projected = selection_.imageAnalysis.report.projected_size;
    const bool oversized = projected > device->size && projected - device->size > imageSizeMarginBytes;
    if (sizeChecks_ && selection_.bootType == BT_IMAGE && !selection_.rawImage && oversized) {
        const QString message = tr("The image is too big for the selected target.");
        emit logMessage(message); emit imageOptionRejected(message); return;
    }
    downloader_.reset();
    const int revoked = selection_.imageAnalysis.report.has_secureboot_bootloader & 0xfe;
    const bool warn = selection_.bootType == BT_IMAGE && revoked != 0 && !revocationWarningAccepted_;
    if (warn) {
        if (!revocationWarningPending_) {
            revocationWarningPending_ = true; revocationWarningRequest_ = imageRequest_;
            emit revocationWarningRequired(revoked);
        }
        return;
    }
    hasher_.reset();
    saver_.reset();
    if (selection_.bootType != BT_IMAGE)
    {
        PrepareImageRequest source;
        const bool ntfs = selection_.fileSystem == FS_NTFS;
        const bool udf = selection_.fileSystem == FS_UDF && selection_.bootType == BT_NON_BOOTABLE;
        source.ntfsCompression = ntfs && ntfsCompression_;
        source.fileIndexing = (ntfs || udf) && fileIndexing_;
        source.sizeChecks = selection_.bootType == BT_MSDOS ? sizeChecks_ : true;
        source.extendedLabel = extendedLabel_ && canExtendedLabel();
        source.oldBiosFixes = oldBiosFixes_ && canOldBiosFixes();
        source.quickFormat = quickFormat();
        source.badBlocksPattern = badBlocksEnabled_ ? badBlocksPattern_ : -1;
        source.detectFakes = detectFakes_;
        source.nonBootable = selection_.bootType == BT_NON_BOOTABLE;
        if (selection_.fileSystem == FS_UDF) source.udfRevision = udfRevision_;
        source.uefiNtfs = selection_.bootType == BT_UEFI_NTFS;
        source.freeDos = selection_.bootType == BT_FREEDOS;
        source.msDos = selection_.bootType == BT_MSDOS;
        source.reactOs = selection_.bootType == BT_REACTOS;
        source.grub2 = selection_.bootType == BT_GRUB2;
        source.grub4dos = selection_.bootType == BT_GRUB4DOS;
        source.syslinux = selection_.syslinux;
        source.syslinuxSelection = source.syslinux ? selection_.bootType : BT_IMAGE;
        source.deviceClass = device->deviceClass;
    source.allowFixedUsb = selection_.allowFixedUsb;
        source.mbr = selection_.partition == PartitionScheme::Mbr && !source.syslinux;
        source.sfd = selection_.partition == PartitionScheme::Sfd;
        if (selection_.fileSystem >= FS_FAT16 && selection_.fileSystem < FS_EXT2) source.archive = archive_;
        isoWriter_.prepare(source, *device, selection_.volumeLabel, selection_.clusterSize, {}, selection_.fileSystem);
    }
    else if (selection_.rawImage)
    {
        PrepareImageRequest source = selection_.imageSource;
        source.badBlocksPattern = badBlocksEnabled_ ? badBlocksPattern_ : -1;
        source.detectFakes = detectFakes_;
        writer_.prepare(selection_.imagePath, *device, &source, selection_.allowFixedUsb);
    }
    else
    {
        const PrepareImageRequest source = selectedImageRequest();
        isoWriter_.prepare(source, *device, selection_.volumeLabel,
            selection_.clusterSize, isoWriteChoices(selection_.persistenceSize, selection_.partition == PartitionScheme::Gpt, selection_.fileSystem != FS_NTFS),
            selection_.fileSystem);
    }
}

void SelectionController::startHybridDd(PrepareImageRequest source, Device target)
{
    source.runtimeValidation = false;
    writer_.prepare(source.imagePath, target, &source, source.allowFixedUsb);
}

bool SelectionController::biosImageUefiNtfs(int filesystem) const
{
    if (selection_.bootType != BT_IMAGE)
        return false;
    const RUFUS_PARTITION_OPTIONS options = {BT_IMAGE, filesystem,
        selection_.windowsToGo ? selection_.windowsToGoTarget : TT_BIOS,
        false, selection_.windowsToGo, false, enhancedInstall(), false, 0};
    return (GetExtraPartitions(&selection_.imageAnalysis.report, &options) & XP_UEFI_NTFS) != 0;
}

bool SelectionController::canSelectSyslinux() const
{
    const Device* device = findDevice(selection_.devices, selection_.deviceId);
    const RUFUS_IMG_REPORT& report = selection_.imageAnalysis.report;
    const RUFUS_BOOT_OPTIONS dispatch = {BT_IMAGE, FS_FAT32, TT_BIOS, false, enhancedInstall(), false, false};
    if (!device || selection_.bootType != BT_IMAGE || selection_.rawImage || !selection_.imageAnalysis.tree_complete)
        return false;
    if (report.disable_iso || GetBootInstallType(&report, &dispatch) != RUFUS_BOOT_INSTALL_SYSLINUX)
        return false;
    const quint32 sectorBytes = device->logicalSectorSize;
    const RUFUS_FS_OPTIONS options = {BT_IMAGE, TT_BIOS, true, false, enhancedInstall()};
    bool forcedFat = false;
    const quint32 allowed = GetAllowedFileSystems(&report, &options, &forcedFat);
    for (int filesystem : {FS_FAT32, FS_FAT16, FS_NTFS})
    {
        if ((allowed & FS_MASK(filesystem)) == 0)
            continue;
        const bool uefiNtfs = biosImageUefiNtfs(filesystem);
        const RUFUS_CLUSTER_SIZES clusters = NativeFileSystemClusterSizes(device->size, filesystem,
            true, BT_IMAGE, PartitionScheme::Gpt, uefiNtfs, MBR_BOOT_ZERO, oldBiosFixes_ && canOldBiosFixes(), false, TT_UEFI, true, device->logicalSectorSize, device->physicalSectorSize);
        if (clusters.Allowed == 0)
            continue;
        PrepareImageRequest request;
        request.size = device->size;
        request.logicalSectorSize = device->logicalSectorSize;
        request.physicalSectorSize = device->physicalSectorSize;
        request.clusterSize = clusters.Default;
        request.fileSystem = filesystem;
        request.syslinux = true;
        request.uefiNtfs = uefiNtfs;
        GPT_IMAGE_RESULT partition = {};
        if (PlanFormatPartition(request, partition) != 0)
            continue;
        QString problem;
        // Preliminary version/geometry eligibility only. START checks the exact
        // source layout/pair, including any inferred version, before authorization.
        if (CheckNativeSyslinuxSelection(report, filesystem, sectorBytes, partition.offset, problem) == 0)
            return true;
    }
    return false;
}

bool SelectionController::canSelectGrub() const
{
    const Device* device = findDevice(selection_.devices, selection_.deviceId);
    const RUFUS_IMG_REPORT& report = selection_.imageAnalysis.report;
    const bool imageReady = selection_.bootType == BT_IMAGE && !selection_.rawImage
        && selection_.imageAnalysis.tree_complete && !report.disable_iso;
    if (!device || !imageReady || canSelectWindowsBios())
        return false;
    const RUFUS_BOOT_OPTIONS dispatch = {BT_IMAGE, FS_FAT32, TT_BIOS, false, enhancedInstall(), false, false};
    if (GetBootInstallType(&report, &dispatch) == RUFUS_BOOT_INSTALL_SYSLINUX)
        return false;
    // Use Rufus's SBR precedence. START rechecks the source and module support.
    const int secondaryBoot = GetSecondaryBootType(BT_IMAGE, &report, false);
    const bool grub4dos = secondaryBoot == BT_GRUB4DOS;
    if (secondaryBoot != BT_GRUB2 && !grub4dos)
        return false;
    const RUFUS_FS_OPTIONS options = {BT_IMAGE, TT_BIOS, true, false, enhancedInstall()};
    bool forcedFat = false;
    const quint32 allowed = GetAllowedFileSystems(&report, &options, &forcedFat);
    for (int filesystem : {FS_FAT16, FS_FAT32, FS_NTFS})
    {
        if ((allowed & FS_MASK(filesystem)) == 0)
            continue;
        const RUFUS_CLUSTER_SIZES clusters = NativeFileSystemClusterSizes(device->size,
            filesystem, false, BT_IMAGE, PartitionScheme::Mbr, biosImageUefiNtfs(filesystem),
            MBR_BOOT_ZERO, oldBiosFixes_ && canOldBiosFixes(), false, TT_UEFI, true, device->logicalSectorSize, device->physicalSectorSize);
        if (clusters.Allowed != 0)
            return true;
    }
    return false;
}

MBR_BOOT_CODE SelectionController::windowsImageBoot(int filesystem) const
{
    const bool windowsFilesystem = filesystem == FS_NTFS || filesystem == FS_FAT16 || filesystem == FS_FAT32;
    const bool windowsMbr = selection_.bootType == BT_IMAGE && selection_.partition == PartitionScheme::Mbr
        && IsWindowsBiosImage(selection_.imageAnalysis, selection_.windowsAnalysis, true, true, enhancedInstall(), true, true) && windowsFilesystem;
    if (!windowsMbr)
        return MBR_BOOT_ZERO;
    return windowsRufusMbr_ ? MBR_BOOT_RUFUS : MBR_BOOT_WIN7;
}

bool SelectionController::canSelectWindowsToGo() const
{
    const bool accepted = selection_.imageState == ImageState::Complete || selection_.imageState == ImageState::Partial;
    return selection_.bootType == BT_IMAGE && accepted && !selection_.rawImage
        && HAS_WINTOGO(selection_.imageAnalysis.report);
}

bool SelectionController::canSelectWindowsToGoMbr() const
{
    const Device* device = findDevice(selection_.devices, selection_.deviceId);
    if (!device || !selection_.windowsToGo || !canSelectWindowsToGo()) return false;
    const int target = HAS_BOOTMGR_BIOS(selection_.imageAnalysis.report) ? TT_BIOS : TT_UEFI;
    return NativeFileSystemClusterSizes(device->size, FS_NTFS, false, BT_IMAGE, PartitionScheme::Mbr,
        true, MBR_BOOT_ZERO, false, true, target, true, device->logicalSectorSize, device->physicalSectorSize).Allowed != 0;
}

void SelectionController::setWindowsToGo(bool enabled)
{
    if (isBusy() || enabled == selection_.windowsToGo) return;
    if (enabled && !canSelectWindowsToGo()) return;
    if (!enabled && selection_.imageAnalysis.report.is_windows_img) return;
    selection_.windowsToGo = enabled;
    selection_.persistenceSize = 0;
    isoWriter_.reset(); writer_.reset(); saver_.reset();
    updatePartitionScheme();
    updateFormatOptions(preferredFileSystem_);
    emit selectionChanged();
}

void SelectionController::setWindowsToGoTarget(int target)
{
    if (isBusy() || !selection_.windowsToGo || target == selection_.windowsToGoTarget) return;
    const bool bios = target == TT_BIOS && selection_.partition == PartitionScheme::Mbr
        && HAS_BOOTMGR_BIOS(selection_.imageAnalysis.report);
    if (target != TT_UEFI && !bios) return;
    selection_.windowsToGoTarget = target;
    preferredWindowsTarget_ = target;
    isoWriter_.reset(); writer_.reset(); saver_.reset();
    updateFormatOptions(FS_NTFS);
    emit selectionChanged();
}

bool SelectionController::canSelectWindowsBios() const
{
    if (selection_.windowsToGo) return false;
    const Device* device = findDevice(selection_.devices, selection_.deviceId);
    if (!device || selection_.bootType != BT_IMAGE || selection_.rawImage)
        return false;
    if (!IsWindowsBiosImage(selection_.imageAnalysis, selection_.windowsAnalysis, true, true, enhancedInstall(), true, true))
        return false;
    const auto clusters = NativeFileSystemClusterSizes(device->size, FS_NTFS, false, BT_IMAGE,
        PartitionScheme::Mbr, biosImageUefiNtfs(FS_NTFS), windowsRufusMbr_ ? MBR_BOOT_RUFUS : MBR_BOOT_WIN7,
        oldBiosFixes_ && canOldBiosFixes(), false, TT_UEFI, true, device->logicalSectorSize, device->physicalSectorSize);
    return clusters.Allowed != 0;
}

bool SelectionController::canSelectKolibri() const
{
    const Device* device = findDevice(selection_.devices, selection_.deviceId);
    const auto& report = selection_.imageAnalysis.report;
    if (!device || device->logicalSectorSize != 512 || selection_.bootType != BT_IMAGE || selection_.rawImage
        || !selection_.imageAnalysis.tree_complete || report.disable_iso || !HAS_KOLIBRIOS(report))
        return false;
    const RUFUS_BOOT_OPTIONS options = {BT_IMAGE, FS_FAT32, TT_BIOS, false, enhancedInstall(), false, false};
    return GetBootInstallType(&report, &options) == RUFUS_BOOT_INSTALL_PBR
        && GetSecondaryBootType(BT_IMAGE, &report, false) == BT_IMAGE;
}

bool SelectionController::canSelectFormatMbr() const
{
    const Device* device = findDevice(selection_.devices, selection_.deviceId);
    return selection_.bootType != BT_IMAGE && device && device->logicalSectorSize >= 512 && device->logicalSectorSize <= 4096
        && device->size <= (quint64(1) << 32) * device->logicalSectorSize;
}

bool SelectionController::canSelectSfd() const
{
    const Device* device = findDevice(selection_.devices, selection_.deviceId);
    if (selection_.bootType != BT_NON_BOOTABLE || !device)
        return false;
    for (int filesystem : {FS_FAT16, FS_FAT32, FS_NTFS, FS_UDF, FS_EXFAT, FS_EXT2, FS_EXT3})
    {
        const auto clusters = NativeFileSystemClusterSizes(device->size, filesystem, false, BT_NON_BOOTABLE, PartitionScheme::Sfd, false, MBR_BOOT_ZERO, false, false, TT_UEFI, true, device->logicalSectorSize, device->physicalSectorSize);
        if (clusters.Allowed != 0)
            return true;
    }
    return false;
}

void SelectionController::updatePartitionScheme()
{
    selection_.partition = PartitionScheme::Gpt;
    if (selection_.windowsToGo && selection_.bootType == BT_IMAGE)
    {
        selection_.syslinux = false;
        if (preferredPartition_ == PartitionScheme::Mbr && canSelectWindowsToGoMbr())
            selection_.partition = PartitionScheme::Mbr;
        const bool bios = selection_.partition == PartitionScheme::Mbr && HAS_BOOTMGR_BIOS(selection_.imageAnalysis.report)
            && preferredWindowsTarget_ != TT_UEFI;
        selection_.windowsToGoTarget = bios ? TT_BIOS : TT_UEFI;
        return;
    }
    if (selection_.bootType != BT_IMAGE)
    {
        selection_.syslinux = selection_.bootType == BT_SYSLINUX_V4 || selection_.bootType == BT_SYSLINUX_V6;
        const bool biosOnly = selection_.bootType == BT_FREEDOS || selection_.bootType == BT_MSDOS || selection_.bootType == BT_REACTOS || selection_.bootType == BT_GRUB2
            || selection_.bootType == BT_GRUB4DOS || selection_.syslinux;
        if (biosOnly)
        {
            selection_.partition = PartitionScheme::Mbr;
            return; // Keep the previous preference for modes that offer GPT/SFD.
        }
        if (selection_.deviceId.isEmpty())
            return;
        // Win32 remembers explicit choices, defaults UEFI:NTFS to GPT and
        // non-bootable to MBR, and forces GPT above 2 TiB.
        if (!canSelectFormatMbr())
            preferredPartition_ = PartitionScheme::Gpt;
        const bool defaultGpt = selection_.bootType == BT_UEFI_NTFS
            && preferredPartition_ == PartitionScheme::ImageDefault;
        if (preferredPartition_ == PartitionScheme::Sfd && canSelectSfd())
            selection_.partition = PartitionScheme::Sfd;
        else
            selection_.partition = defaultGpt || preferredPartition_ == PartitionScheme::Gpt
                ? PartitionScheme::Gpt : PartitionScheme::Mbr;
        return;
    }
    const bool biosOnly = !IS_EFI_BOOTABLE(selection_.imageAnalysis.report);
    if (canSelectWindowsBios())
    {
        const bool windowsDefaultMbr = biosOnly || enhancedInstall();
        const bool preferredMbr = preferredPartition_ == PartitionScheme::Mbr
            || preferredPartition_ == PartitionScheme::Sfd;
        selection_.syslinux = false;
        selection_.partition = windowsDefaultMbr || preferredMbr ? PartitionScheme::Mbr : PartitionScheme::Gpt;
        return;
    }
    // Win32 defaults hybrids to MBR and keeps an explicit partition choice.
    const bool imageDefault = preferredPartition_ == PartitionScheme::ImageDefault;
    const bool preferMbr = preferredPartition_ == PartitionScheme::Mbr
        || preferredPartition_ == PartitionScheme::Sfd // Win32 falls back to the first offered entry.
        || (imageDefault && IS_DD_BOOTABLE(selection_.imageAnalysis.report));
    selection_.syslinux = canSelectSyslinux() && (biosOnly || preferMbr);
    const bool grub = canSelectGrub() && (biosOnly || preferMbr);
    if (selection_.syslinux || grub || (canSelectKolibri() && (biosOnly || preferMbr)))
        selection_.partition = PartitionScheme::Mbr;
}

void SelectionController::setPartitionScheme(PartitionScheme scheme)
{
    if (isBusy())
        return;
    const bool gpt = scheme == PartitionScheme::Gpt;
    const bool mbr = scheme == PartitionScheme::Mbr;
    const bool sfd = scheme == PartitionScheme::Sfd;
    if (!gpt && !mbr && !sfd)
        return;
    if (selection_.bootType != BT_IMAGE)
    {
        const bool biosOnly = selection_.bootType == BT_FREEDOS || selection_.bootType == BT_MSDOS || selection_.bootType == BT_REACTOS || selection_.bootType == BT_GRUB2
            || selection_.bootType == BT_GRUB4DOS || selection_.syslinux;
        if (biosOnly && !mbr)
            return;
        if (mbr && !canSelectFormatMbr())
            return;
        if (sfd && !canSelectSfd())
            return;
    }
    else
    {
        if (sfd || (mbr && !canSelectSyslinux() && !canSelectGrub() && !canSelectWindowsBios() && !canSelectKolibri() && !canSelectWindowsToGoMbr()))
            return;
        if (gpt && !IS_EFI_BOOTABLE(selection_.imageAnalysis.report))
            return;
    }
    const auto current = selection_.partition;
    preferredPartition_ = scheme;
    if (scheme == current)
        return; // Selecting the current entry still records an explicit preference.
    // An explicit Win32 selection updates only the target, without applying
    // the device-refresh GPT default again on media larger than 2 TiB.
    if (selection_.bootType != BT_IMAGE || canSelectWindowsBios() || selection_.windowsToGo)
        selection_.partition = scheme;
    else
        updatePartitionScheme();
    if (selection_.windowsToGo)
        selection_.windowsToGoTarget = mbr && HAS_BOOTMGR_BIOS(selection_.imageAnalysis.report)
            && preferredWindowsTarget_ != TT_UEFI ? TT_BIOS : TT_UEFI;
    isoWriter_.reset();
    if (deviceOperation_ && !deviceOperation_->isActive()) deviceOperation_.reset();
    writer_.reset();
    downloader_.reset();
    saver_.reset();
    updateFormatOptions(preferredFileSystem_);
    emit selectionChanged();
}

void SelectionController::setBootType(int type)
{
    const bool supported = type == BT_IMAGE || type == BT_NON_BOOTABLE || type == BT_UEFI_NTFS
        || type == BT_FREEDOS || type == BT_MSDOS || type == BT_REACTOS || type == BT_GRUB2 || type == BT_GRUB4DOS
        || type == BT_SYSLINUX_V4 || type == BT_SYSLINUX_V6;
    if (!supported || isBusy() || type == selection_.bootType)
        return;
    selection_.bootType = type;
    selection_.windowsToGo = type == BT_IMAGE && selection_.imageAnalysis.report.is_windows_img;
    selection_.persistenceSize = 0;
    hasher_.reset();
    if (deviceOperation_ && !deviceOperation_->isActive()) deviceOperation_.reset();
    writer_.reset();
    downloader_.reset();
    isoWriter_.reset();
    saver_.reset();
    updatePartitionScheme();
    updateFormatOptions(preferredFileSystem_);
    updateVolumeLabel();
    emit selectionChanged();
}

void SelectionController::setVolumeLabel(const QString& label)
{
    if (isBusy())
        return;
    selection_.volumeLabel = label;
    userChangedLabel_ = true;
    isoWriter_.reset();
    if (deviceOperation_ && !deviceOperation_->isActive()) deviceOperation_.reset();
    writer_.reset();
    downloader_.reset();
    saver_.reset();
    emit selectionChanged();
}

void SelectionController::updateVolumeLabel()
{
    if (userChangedLabel_ || (selection_.rawImage && selection_.bootType == BT_IMAGE) || isBusy())
        return;
    const RUFUS_IMG_REPORT& report = selection_.imageAnalysis.report;
    if (selection_.bootType == BT_IMAGE && selection_.imageAnalysis.tree_complete && report.label[0] != '\0')
    {
        selection_.volumeLabel = QString::fromUtf8(report.label);
        return;
    }
    const Device* device = findDevice(selection_.devices, selection_.deviceId);
    selection_.volumeLabel.clear();
    if (device)
        selection_.volumeLabel = device->volumeLabel.isEmpty() ? DeviceSizeLabel(device->size) : device->volumeLabel;
}

void SelectionController::setClusterSize(quint32 bytes)
{
    if (isBusy())
        return;
    const bool singleSize = bytes != 0 && (bytes & (bytes - 1)) == 0;
    if (!singleSize || (selection_.clusterSizes.Allowed & bytes) == 0)
        return;
    selection_.clusterSize = bytes;
    clampPersistenceSize();
    isoWriter_.reset();
    if (deviceOperation_ && !deviceOperation_->isActive()) deviceOperation_.reset();
    writer_.reset();
    downloader_.reset();
    saver_.reset();
    emit selectionChanged();
}

void SelectionController::updateFormatOptions(int preselected)
{
    if (preselected == FS_UNKNOWN) preselected = startupFileSystem_;
    selection_.fileSystems = 0;
    selection_.defaultFileSystem = FS_UNKNOWN;
    selection_.fileSystem = FS_UNKNOWN;
    selection_.clusterSizes = {};
    selection_.clusterSize = 0;
    const Device* device = findDevice(selection_.devices, selection_.deviceId);
    if (!device || (selection_.rawImage && selection_.bootType == BT_IMAGE))
        return;
    // PopulateProperties uses the existing filesystem only until an image or
    // an explicit user choice takes precedence. Unknown FAT versions stay unknown.
    if ((selection_.imagePath.isEmpty() || selection_.bootType != BT_IMAGE) && preselected == FS_UNKNOWN)
    {
        if (device->fileSystemType == QStringLiteral("exfat"))
            preselected = FS_EXFAT;
        else if (device->fileSystemType == QStringLiteral("ntfs"))
            preselected = FS_NTFS;
        else if (device->fileSystemType == QStringLiteral("udf"))
            preselected = FS_UDF;
        else if (device->fileSystemType == QStringLiteral("ext2"))
            preselected = FS_EXT2;
        else if (device->fileSystemType == QStringLiteral("ext3"))
            preselected = FS_EXT3;
        else if (device->fileSystemType == QStringLiteral("vfat")
            && device->fileSystemVersion == QStringLiteral("FAT32"))
            preselected = FS_FAT32;
        else if (device->fileSystemType == QStringLiteral("vfat")
            && device->fileSystemVersion == QStringLiteral("FAT16"))
            preselected = FS_FAT16;
    }
    RUFUS_CLUSTER_SIZES clusters[FS_MAX] = {};
    quint32 available = 0;
    const bool windowsImage = windowsImageBoot(FS_NTFS) != MBR_BOOT_ZERO;
    const bool grubImage = selection_.bootType == BT_IMAGE && !selection_.windowsToGo && !selection_.syslinux
        && selection_.partition == PartitionScheme::Mbr && !windowsImage;
    const bool biosImage = selection_.syslinux || grubImage || windowsImage || selection_.windowsToGo;
    // Like Win32, mark the first format the target supports, before image
    // filtering or user preferences. Native cluster policy also gates MBR.
    for (int filesystem : {FS_FAT16, FS_FAT32, FS_NTFS, FS_UDF, FS_EXFAT, FS_EXT2, FS_EXT3})
    {
        const bool ext = filesystem == FS_EXT2 || filesystem == FS_EXT3;
        const bool unavailable = (selection_.bootType == BT_IMAGE && filesystem == FS_EXFAT) ||
            (ext && !advancedFormat_);
        if (unavailable)
            continue;
        clusters[filesystem] = NativeFileSystemClusterSizes(device->size, filesystem, selection_.syslinux,
            selection_.bootType, selection_.partition, biosImage && biosImageUefiNtfs(filesystem), windowsImageBoot(filesystem), oldBiosFixes_ && canOldBiosFixes(),
            selection_.windowsToGo, selection_.windowsToGoTarget, sizeChecks_, device->logicalSectorSize, device->physicalSectorSize);
        if (clusters[filesystem].Allowed == 0)
            continue;
        available |= FS_MASK(filesystem);
        if (selection_.defaultFileSystem == FS_UNKNOWN)
            selection_.defaultFileSystem = filesystem;
    }
    const RUFUS_FS_OPTIONS options = {selection_.bootType,
        selection_.windowsToGo ? selection_.windowsToGoTarget :
            (selection_.partition == PartitionScheme::Mbr || selection_.bootType == BT_FREEDOS || selection_.bootType == BT_MSDOS || selection_.bootType == BT_REACTOS || selection_.bootType == BT_GRUB2) ? TT_BIOS : TT_UEFI,
        selection_.bootType == BT_IMAGE && bool(selection_.imageAnalysis.tree_complete), selection_.windowsToGo,
        selection_.bootType == BT_IMAGE && enhancedInstall() && !selection_.syslinux && !grubImage};
    const RUFUS_IMG_REPORT& report = selection_.imageAnalysis.report;
    bool forcedFat = false;
    selection_.fileSystems = available & GetAllowedFileSystems(&report, &options, &forcedFat);
    if (grubImage)
        selection_.fileSystems &= FS_MASK(FS_FAT16) | FS_MASK(FS_FAT32) | FS_MASK(FS_NTFS);
    if (forcedFat)
        emit logMessage(tr("WARNING: FAT32 has been forcefully enabled, but this image may not work with FAT32."));
    int preferred = GetPreferredFileSystem(&report, &options, selection_.fileSystems, preselected);
    if (preferred == FS_UNKNOWN)
    {
        // Win32 keeps the manual choice when the image has no filesystem
        // preference, including a rejected scan. Image defaults still win.
        if (preselected == FS_UNKNOWN)
            preselected = preferredFileSystem_;
        const bool validPreselection = preselected == FS_FAT16 || preselected == FS_FAT32 ||
            preselected == FS_NTFS || preselected == FS_UDF || preselected == FS_EXFAT || preselected == FS_EXT2 || preselected == FS_EXT3;
        if (validPreselection && (selection_.fileSystems & FS_MASK(preselected)))
            preferred = preselected;
        else
            for (int filesystem : {FS_FAT16, FS_FAT32, FS_NTFS, FS_UDF, FS_EXFAT, FS_EXT2, FS_EXT3})
                if (selection_.fileSystems & FS_MASK(filesystem))
                {
                    preferred = filesystem;
                    break;
                }
    }
    selection_.fileSystem = preferred;
    if (!SupportsFullFormat(preferred, device->size)) quickFormat_ = true;
    if (preferred != FS_UNKNOWN)
        selection_.clusterSizes = clusters[preferred];
    selection_.clusterSize = selection_.clusterSizes.Default;
    clampPersistenceSize();
}

void SelectionController::setStartupFileSystem(int filesystem)
{
    if (isBusy() || filesystem < FS_UNKNOWN || filesystem >= FS_MAX) return;
    startupFileSystem_ = filesystem;
    updateFormatOptions();
    emit selectionChanged();
}

void SelectionController::setFileSystem(int filesystem)
{
    if (isBusy())
        return;
    const bool supported = filesystem == FS_FAT16 || filesystem == FS_FAT32 ||
        filesystem == FS_NTFS || filesystem == FS_UDF || filesystem == FS_EXFAT || filesystem == FS_EXT2 || filesystem == FS_EXT3;
    if (!supported || (selection_.fileSystems & FS_MASK(filesystem)) == 0)
        return;
    const Device* device = findDevice(selection_.devices, selection_.deviceId);
    if (!device)
        return;
    preferredFileSystem_ = filesystem;
    if (filesystem == selection_.fileSystem)
        return; // An explicit choice of the current default must still be remembered.
    selection_.fileSystem = filesystem;
    if (!SupportsFullFormat(filesystem, device->size)) quickFormat_ = true;
    const bool biosImage = selection_.syslinux || (selection_.bootType == BT_IMAGE
        && selection_.partition == PartitionScheme::Mbr);
    selection_.clusterSizes = NativeFileSystemClusterSizes(device->size, filesystem, selection_.syslinux,
        selection_.bootType, selection_.partition, biosImage && biosImageUefiNtfs(filesystem), windowsImageBoot(filesystem), oldBiosFixes_ && canOldBiosFixes(),
        selection_.windowsToGo, selection_.windowsToGoTarget, true, device->logicalSectorSize, device->physicalSectorSize);
    selection_.clusterSize = selection_.clusterSizes.Default;
    clampPersistenceSize();
    isoWriter_.reset();
    if (deviceOperation_ && !deviceOperation_->isActive()) deviceOperation_.reset();
    writer_.reset();
    downloader_.reset();
    saver_.reset();
    emit selectionChanged();
}

void SelectionController::setAdvancedFormat(bool enabled)
{
    if (isBusy() || advancedFormat_ == enabled)
        return;
    advancedFormat_ = enabled;
    // Win32 keeps selected_fs across the temporary fallback when ext is hidden.
    if (preferredFileSystem_ == FS_UNKNOWN)
        preferredFileSystem_ = selection_.fileSystem;
    updateFormatOptions(preferredFileSystem_);
    isoWriter_.reset();
    if (deviceOperation_ && !deviceOperation_->isActive()) deviceOperation_.reset();
    writer_.reset();
    downloader_.reset();
    saver_.reset();
    emit selectionChanged();
}

void SelectionController::toggleEnhancedInstall()
{
    if (isBusy())
        return;
    enhancedInstall_ = !enhancedInstall_;
    ApplicationSettings settings;
    settings.setValue(QLatin1String(enhancedInstallSetting), enhancedInstall_);
    isoWriter_.reset();
    if (deviceOperation_ && !deviceOperation_->isActive()) deviceOperation_.reset();
    writer_.reset();
    downloader_.reset();
    saver_.reset();
    // Win32 Alt+E keeps the selected filesystem when it is still allowed.
    if (selection_.bootType == BT_IMAGE)
    {
        const int filesystem = selection_.fileSystem;
        updatePartitionScheme();
        updateFormatOptions(filesystem);
    }
    emit selectionChanged();
}

void SelectionController::toggleWindowsMbr()
{
    if (isBusy())
        return;
    windowsRufusMbr_ = !windowsRufusMbr_;
    ApplicationSettings settings;
    settings.setValue(QLatin1String(disableWindowsMbrSetting), !windowsRufusMbr_);
    isoWriter_.reset();
    if (deviceOperation_ && !deviceOperation_->isActive()) deviceOperation_.reset();
    writer_.reset();
    downloader_.reset();
    saver_.reset();
    emit selectionChanged();
}

void SelectionController::toggleBootMarker()
{
    if (isBusy()) return;
    ignoreBootMarker_ = !ignoreBootMarker_;
    ApplicationSettings settings;
    settings.setValue(QLatin1String(ignoreBootMarkerSetting), ignoreBootMarker_);
    emit logMessage(ignoreBootMarker_ ? tr("Boot marker check disabled for the next image selection.")
        : tr("Boot marker check enabled for the next image selection."));
    emit selectionChanged();
}

void SelectionController::reportReadOption(const QString& name, bool enabled)
{
    const QString message = (enabled ? tr("%1 enabled") : tr("%1 disabled")).arg(name);
    emit logMessage(message); emit optionStatusChanged(message); emit selectionChanged();
}
void SelectionController::rescanReadOptions(bool force)
{
    const bool iso = selection_.imageAnalysis.filesystem == ISO_FILESYSTEM_ISO9660;
    const QString suffix = QFileInfo(selection_.imagePath).suffix().toLower();
    const bool explicitRaw = suffix == QLatin1String("img") || suffix == QLatin1String("usb") || suffix == QLatin1String("wic");
    if (selection_.imageSource.hasCompressedImage() || explicitRaw) return;
    if (selection_.bootType != BT_IMAGE || selection_.imagePath.isEmpty() || (!force && (!isoSupport_ || !iso))) return;
    if (deviceOperation_ && !deviceOperation_->isActive()) deviceOperation_.reset();
    writer_.reset(); isoWriter_.reset();
    readOptionsRescan_ = true; revocationWarningAccepted_ = revocationWarningPending_ = false;
    ++imageRequest_; selection_.imageState = ImageState::Scanning;
    startAnalysis();
}
void SelectionController::toggleIsoSupport()
{
    if (isBusy()) return;
    isoSupport_ = !isoSupport_; rescanReadOptions(true); reportReadOption(tr("ISO Support"), isoSupport_);
}
void SelectionController::toggleJoliet()
{
    if (isBusy()) return;
    joliet_ = !joliet_; rescanReadOptions(false); reportReadOption(tr("Joliet support"), joliet_);
}
void SelectionController::toggleRockRidge()
{
    if (isBusy()) return;
    rockRidge_ = !rockRidge_; rescanReadOptions(false); reportReadOption(tr("Rock Ridge support"), rockRidge_);
}
void SelectionController::togglePreserveTimestamps()
{
    if (isBusy()) return;
    preserveTimestamps_ = !preserveTimestamps_;
    ApplicationSettings settings; settings.setValue(QStringLiteral("PreserveTimestamps"), preserveTimestamps_);
    reportReadOption(tr("Preserve timestamps"), preserveTimestamps_);
}
void SelectionController::toggleSizeChecks()
{
    if (isBusy()) return;
    sizeChecks_ = !sizeChecks_;
    if (selection_.bootType == BT_MSDOS) updateFormatOptions(selection_.fileSystem);
    reportReadOption(tr("Size checks"), sizeChecks_);
}

quint32 SelectionController::deviceClasses() const
{
    return (deviceClasses_ & DeviceClassMask(DeviceClass::Virtual)) != 0 ? deviceClasses_
        : deviceClasses_ & ~DeviceClassMask(DeviceClass::Vmware);
}

void SelectionController::toggleVirtualDisks()
{
    if (isBusy()) return;
    deviceClasses_ ^= DeviceClassMask(DeviceClass::Virtual);
    const bool enabled = (deviceClasses_ & DeviceClassMask(DeviceClass::Virtual)) != 0;
    ApplicationSettings settings; settings.setValue(QStringLiteral("DisableVHDs"), !enabled);
    setDevices(selection_.devices);
    emit deviceClassesChanged(deviceClasses());
    reportReadOption(tr("Virtual disk detection"), enabled);
}

void SelectionController::toggleVmwareDisks()
{
    if (isBusy()) return;
    deviceClasses_ ^= DeviceClassMask(DeviceClass::Vmware);
    const bool enabled = (deviceClasses_ & DeviceClassMask(DeviceClass::Vmware)) != 0;
    ApplicationSettings settings; settings.setValue(QStringLiteral("EnableVMDKDetection"), enabled);
    setDevices(selection_.devices);
    emit deviceClassesChanged(deviceClasses());
    reportReadOption(tr("VMware disk detection"), enabled);
}

void SelectionController::toggleNonUsbRemovable()
{
    if (isBusy()) return;
    deviceClasses_ ^= DeviceClassMask(DeviceClass::NonUsbRemovable);
    const bool enabled = (deviceClasses_ & DeviceClassMask(DeviceClass::NonUsbRemovable)) != 0;
    if (enabled) previousFixedUsb_ = selection_.allowFixedUsb;
    setAllowFixedUsb(enabled ? true : previousFixedUsb_);
    setDevices(selection_.devices);
    emit deviceClassesChanged(deviceClasses());
    reportReadOption(tr("Listing of non-USB removable drives"), enabled);
}

void SelectionController::toggleNtfsCompression()
{
    if (isBusy()) return;
    ntfsCompression_ = !ntfsCompression_; reportReadOption(tr("NTFS compression"), ntfsCompression_);
}
void SelectionController::toggleFileIndexing()
{
    if (isBusy()) return;
    fileIndexing_ = !fileIndexing_;
    ApplicationSettings settings; settings.setValue(QStringLiteral("EnableFileIndexing"), fileIndexing_);
    reportReadOption(tr("File indexing"), fileIndexing_);
}

void SelectionController::setSecureBootPolicy(const SecureBootPolicy& policy)
{
    // Existing analysis/jobs retain their immutable snapshot, as on Windows.
    secureBootPolicy_ = policy;
}

void SelectionController::startAnalysis()
{
    runningRequest_ = imageRequest_;
    analysisActive_ = true;
    PrepareImageRequest options = selection_.imageSource;
    options.rockRidge = rockRidge_; options.joliet = joliet_;
    ImageAnalysisJob* job = new ImageAnalysisJob(runningRequest_, selection_.imagePath,
        selection_.imageSize, imageModified_, ignoreBootMarker_, options, isoWriter_.expertMode(), secureBootPolicy_, isoSupport_);
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
    selection_.imageSource = {};
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
    selection_.imageSource = result.source;
    selection_.ffuStores = result.ffuStores;
    if (result.status == ISO_SCAN_OK && result.ffuStores.size() > 1)
    {
        selection_.imageState = ImageState::ChoosingStore;
        selection_.rawImage = false;
        emit selectionChanged();
        return;
    }
    selection_.rawImage = result.rawImage && result.status == ISO_SCAN_OK;
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
    const RUFUS_IMG_REPORT& report = selection_.imageAnalysis.report;
    const bool bootable = IS_DD_BOOTABLE(report) || IS_BIOS_BOOTABLE(report) || IS_EFI_BOOTABLE(report);
    if (selection_.imageState == ImageState::Complete && !bootable)
    {
        selection_.imageState = ImageState::Unsupported;
        selection_.imageProblem = tr("The image has no supported boot method.");
    }
    const bool accepted = selection_.imageState == ImageState::Complete || selection_.imageState == ImageState::Partial;
    selection_.windowsToGo = accepted && result.analysis.report.is_windows_img;
    if (accepted && !readOptionsRescan_)
        userChangedLabel_ = false;
    const int selectedFileSystem = readOptionsRescan_ ? selection_.fileSystem : FS_UNKNOWN;
    readOptionsRescan_ = false;
    updatePartitionScheme();
    updateFormatOptions(selectedFileSystem);
    logImageReport(result.ddRequired);
    emit logMessage(imageStatusText());
    emit selectionChanged();
}

void SelectionController::chooseFfuStore(unsigned index)
{
    if (analysisActive_ || selection_.imageState != ImageState::ChoosingStore) return;
    if (index == 0)
    {
        selection_.imageState = ImageState::Cancelled;
        selection_.ffuStores.clear();
        clearRejectedImage();
        emit selectionChanged();
        return;
    }
    if (index > unsigned(selection_.ffuStores.size())) return;
    selection_.imageSource.ffuStore = index;
    selection_.imageState = ImageState::Scanning;
    ++imageRequest_;
    startAnalysis();
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
    const bool rejected = selection_.imageState == ImageState::Failed
        || selection_.imageState == ImageState::Cancelled || selection_.imageState == ImageState::Unsupported;
    if (rejected)
        clearRejectedImage();
    else
        updateVolumeLabel();
    emit selectionChanged();
    if (!analysisActive_)
        emit imageSelectionFinished();
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
    {
        if (selection_.imageSource.hasCompressedImage())
        {
            if (selection_.imageSource.ffuMinimumSize != 0 && writer_.isAvailable())
                return tr("Optimized FFU selected: minimum target size %1 bytes.").arg(selection_.imageSource.ffuMinimumSize);
            return writer_.isAvailable()
                ? tr("Compressed image selected (DD Image): %1 decoded bytes.").arg(selection_.imageSource.decodedImageSize)
                : tr("Compressed image selected; media writing unavailable.");
        }
        return writer_.isAvailable() ? tr("Raw image selected (DD Image).")
            : tr("Raw image selected; media writing unavailable.");
    }
    if (isoWriter_.isAvailable() && isoWriteChoices(selection_.persistenceSize, selection_.partition == PartitionScheme::Gpt, selection_.fileSystem != FS_NTFS).count > 0)
    {
        const QString pending = pendingChecks();
        return pending.isEmpty() ? tr("Image selected. START checks the image, settings and device.")
            : tr("Image selected. Pending checks: %1. START checks ISO preparation separately.").arg(pending);
    }
    switch (selection_.imageState)
    {
    case ImageState::ChoosingStore:
        return tr("Select the FFU store to write.");
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
    case ImageState::Unsupported:
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
    emit logMessage(isoWriter_.isAvailable()
        ? tr("ISO preparation and native installer compatibility will be checked at START.")
        : tr("Native ISO-mode installer compatibility is unchecked. ISO writing remains unavailable."));
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

void SelectionController::toggleUsbDebug()
{
    if (isBusy()) return;
    usbDebug_ = !usbDebug_;
    cdio_loglevel_default = usbDebug_ ? CDIO_LOG_INFO : CDIO_LOG_WARN;
    ApplicationSettings settings; settings.setValue(QStringLiteral("EnableUsbDebug"),usbDebug_);
    emit usbDebugChanged(usbDebug_);
    emit optionStatusChanged((usbDebug_ ? tr("%1 enabled") : tr("%1 disabled")).arg(tr("USB debug")));
}

} // namespace rufus
