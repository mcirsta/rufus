// SPDX-License-Identifier: GPL-3.0-or-later
#include "iso_write_controller.h"
#include "device_policy.h"
#include "platform/zip_extract.h"
#include "platform/media_appearance.h"
#include "platform/device_selection.h"
#include "platform/exfat_format.h"
#include "platform/ext_format.h"
#include "platform/udf_format.h"
#include "platform/format_options.h"
#include "platform/windows_customization.h"
#include "platform/windows_to_go.h"
#include "platform/syslinux_install.h"
#include "platform/syslinux_native.h"
#include "../../res/grub2/grub2_version.h"
#include "../../res/grub/grub_version.h"
#include "rufus_job.h"
#include "download_urls.h"
#include "grub_inspect.h"
#include <QFileInfo>
#include <QCryptographicHash>
#include <QRandomGenerator>
#include "application_storage.h"
#include <QStandardPaths>
#include <QDir>
#include <cerrno>
#include <cstring>
#include <unistd.h>
#include <utility>

namespace rufus
{
class IsoCheckJob final : public RufusJob
{
    Q_OBJECT
public:
    IsoCheckJob(const QDBusConnection& connection, const PrepareImageRequest& request, bool confirmed, bool windowsSelection, const RUFUS_IMG_REPORT& report)
        : connection_(connection), request_(request), report_(report), confirmed_(confirmed), windowsSelection_(windowsSelection) {}
signals:
    void checked(int error, rufus::IsoWriteCheck result);
protected:
    void execute() override
    {
        IsoWriteCheck result;
        const bool sourceFree = request_.isSourceFree();
        // Confirmation rechecks availability against the frozen source plan.
        // The helper repeats full analysis before target access.
        if (confirmed_ && !sourceFree) result.analysis.report = report_;
        int partitions = 0;
        PrepareIsoSource source(request_);
        int error = sourceFree ? 0 : source.open(getuid());
        if (error == 0)
            error = CheckPrepareDeviceSelection(connection_, request_, true, &partitions);
        const bool knownSequence = request_.diskSequence != 0;
        if (error == 0 && (!knownSequence || ReadDiskSequence(request_.device) != request_.diskSequence))
            error = ESTALE;
        if (error == 0 && sourceFree)
            error = checkFormat(result);
        if (error == 0 && request_.windowsToGo)
        {
            WindowsToGoImage image;
            error = image.open(request_, source, !windowsSelection_);
            if (error == 0) error = PlanFormatPartition(request_, result.partition);
            if (error == 0)
            {
                result.analysis.report = image.report;
                result.analysis.tree_complete = true;
                result.windows.metadata = image.plan.metadata;
                result.windows.state = image.plan.metadata.valid_fields == WIM_VERSION_ALL
                    ? WINDOWS_IMAGE_KNOWN : WINDOWS_IMAGE_UNKNOWN;
                result.windowsInstallPath = image.archive;
                result.windowsArchives = image.archives;
                result.windowsInstall = image.editions;
                result.windowsApplyBytes = image.plan.bytes;
                result.choices.count = 1;
                result.choices.mode[0] = IMAGE_WRITE_ISO;
            }
        }
        else if (error == 0 && !sourceFree && !confirmed_)
            error = CheckIsoWriteSource(request_, getuid(), result, nullptr, true);
        if (error == 0 && request_.syslinux) {
            const int provider = request_.syslinuxSelection == BT_IMAGE && SL_MAJOR(SyslinuxSourceReport(result.analysis.report).sl_version) < 5 && request_.fileSystem != FS_NTFS ? BT_SYSLINUX_V4 : request_.syslinuxSelection;
            error = CheckNativeSyslinuxTools(result.problem,provider,request_.fileSystem);
        }
        if (error == 0)
            error = checkFormatter(result);
        if (error == 0 && !sourceFree)
            error = source.check();
        if (error == 0 && request_.archive.present()) {
            const PrepareImageRequest archiveRequest = PrepareArchiveRequest(request_.archive);
            PrepareIsoSource archive(archiveRequest);
            error = archive.open(getuid());
            ZipExtractResult planned;
            GPT_IMAGE_RESULT archivePartition = {};
            if (error == 0) error = PlanFormatPartition(request_, archivePartition);
            const bool fat = request_.fileSystem == FS_FAT16 || request_.fileSystem == FS_FAT32;
            const quint64 maximumFile = fat ? qMin(archivePartition.size, quint64(UINT32_MAX)) : archivePartition.size;
            if (error == 0) error = CheckZipSupplement(archive.retainedSource(), archiveRequest.imagePath,
                maximumFile, quint64(INT64_MAX), planned);
            if (error == 0) error = archive.check();
            const int closed = archive.close();
            if (error == 0) error = closed;
            result.archiveBytes = planned.bytes; result.archiveFiles = planned.files;
            if (error != 0) result.problem = tr("Cannot use the supplementary ZIP archive: %1").arg(QString::fromLocal8Bit(strerror(error)));
        }
        if (error != 0 && result.problem.isEmpty())
            result.problem = (sourceFree ? tr("The selected device or format settings are unavailable: %1") :
                tr("The selected image or device is unavailable: %1")).arg(QString::fromLocal8Bit(strerror(error)));
        result.existingPartitions = partitions;
        emit checked(error, result);
    }
private:
    int checkFormat(IsoWriteCheck& result)
    {
        const quint32 sectorBytes = request_.logicalSectorSize;
        if (EncodePrepareRequest(request_).isEmpty())
            return EINVAL;
        int error = 0;
        if (request_.msDos) { QByteArray bytes; error = ReadMsDosImage(request_.dosImagePath,getuid(),bytes); }
        if (error == 0) error = PlanFormatPartition(request_, result.partition);
        if (error == 0)
            error = CheckFormatSettings(request_, result.partition);
        const bool syslinuxFat = request_.isStandaloneSyslinux() && request_.fileSystem != FS_NTFS;
        if (error == 0 && syslinuxFat)
            error = CheckNativeSyslinuxGeometry(request_.fileSystem, sectorBytes, result.partition.offset, result.problem);
        return error;
    }

    int checkFormatter(IsoWriteCheck& result)
    {
        if (request_.fileSystem == FS_NTFS && result.analysis.report.has_symlinks == SYMLINKS_RR
            && access(RUFUS_NTFS_3G_PATH,X_OK) != 0) {
            result.problem = tr("Windows-compatible NTFS symbolic links require ntfs-3g."); return errno;
        }
        if (request_.fileSystem == FS_UDF) {
            const UdfFormatOptions options = {result.partition.size, request_.label, request_.udfRevision, request_.quickFormat, request_.logicalSectorSize, request_.physicalSectorSize};
            return CheckUdfFormatTools(QStringLiteral(RUFUS_MKUDFFS_PATH), options, result.problem);
        }
        if (request_.fileSystem == FS_EXFAT)
            return CheckExfatFormatTools(QStringLiteral(RUFUS_MKFS_EXFAT_PATH), result.problem);
        const bool ext = request_.fileSystem == FS_EXT2 || request_.fileSystem == FS_EXT3;
        const char* program = ext ? RUFUS_MKE2FS_PATH
            : request_.fileSystem == FS_NTFS ? RUFUS_MKNTFS_PATH : RUFUS_MKFS_FAT_PATH;
        if (access(program, X_OK) == 0)
            return 0;
        const int error = errno;
        result.problem = tr("Required formatter is unavailable: %1").arg(QString::fromUtf8(program));
        return error;
    }

    QDBusConnection connection_;
    PrepareImageRequest request_;
    RUFUS_IMG_REPORT report_;
    bool confirmed_;
    bool windowsSelection_;
};

IsoWriteController::IsoWriteController(QObject* parent) : QObject(parent)
{
    windowsOptions_ = UNATTEND_DEFAULT_SELECTION_MASK;
    ApplicationSettings settings;
    expertMode_ = settings.value(QStringLiteral("ExpertMode"), false).toBool();
    bool valid = false;
    const quint32 stored = settings.value(QStringLiteral("WindowsUserExperience"), QVariant::fromValue<quint32>(0)).toUInt(&valid);
    if (valid)
    {
        const int mask = int(stored >> 16) & UNATTEND_DEFAULT_MASK;
        windowsOptions_ = (windowsOptions_ & ~mask) | (int(stored) & mask);
    }
    qRegisterMetaType<IsoWriteCheck>();
    connect(&msDos_,&MsDosSelection::downloadRequired,this,&IsoWriteController::msDosDownloadRequired);
    connect(&msDos_,&MsDosSelection::progress,this,&IsoWriteController::grubDownloadProgress);
    connect(&msDos_,&MsDosSelection::completed,this,&IsoWriteController::msDosFinished);
    connect(&syslinuxResources_,&SyslinuxSelection::downloadRequired,this,&IsoWriteController::grubDownloadRequired);
    connect(&syslinuxResources_,&SyslinuxSelection::progress,this,&IsoWriteController::grubDownloadProgress);
    connect(&syslinuxResources_,&SyslinuxSelection::completed,this,&IsoWriteController::syslinuxFinished);
    connect(&grubCore_, &Grub2CoreSelection::downloadRequired, this, &IsoWriteController::grubDownloadRequired);
    connect(&grubCore_, &Grub2CoreSelection::completed, this, &IsoWriteController::grubCoreFinished);
    connect(&grubCore_, &Grub2CoreSelection::progress, this, &IsoWriteController::grubDownloadProgress);
    connect(&thread_, &QThread::finished, this, &IsoWriteController::checkFinished, Qt::QueuedConnection);
}

void IsoWriteController::toggleExpertMode()
{
    expertMode_ = !expertMode_;
    ApplicationSettings settings;
    settings.setValue(QStringLiteral("ExpertMode"), expertMode_);
}

IsoWriteController::~IsoWriteController()
{
    thread_.requestInterruption();
    thread_.quit();
    thread_.wait();
    // PrepareImageProcess closes stdin and waits for the helper's cleanup.
    process_.reset();
}

void IsoWriteController::configure(const QDBusConnection& connection, const QString& helper,
    const QString& filesDirectory, const QUrl& filesServer)
{
    if (isActive())
        return;
    reset();
    connection_ = connection;
    helper_ = helper;
    filesDirectory_ = filesDirectory.isEmpty()
        ? ApplicationFilesDirectory()
        : filesDirectory;
    filesServer_ = filesServer.isEmpty() ? QUrl(QStringLiteral(FILES_URL)) : filesServer;
    emit changed();
}

bool IsoWriteController::isAvailable(IMAGE_WRITE_MODE mode) const
{
    if (!connection_ || !connection_->isConnected())
        return false;
    if (mode != IMAGE_WRITE_ISO && mode != IMAGE_WRITE_DD && mode != IMAGE_WRITE_ESP)
        return false;
    const QFileInfo helper(helper_);
    return helper.isAbsolute() && helper.isFile() && helper.isExecutable();
}

bool IsoWriteController::isActive() const
{
    switch (state_)
    {
    case IsoWriteState::Checking:
    case IsoWriteState::ChoosingMode:
    case IsoWriteState::ChoosingWindowsArchive:
    case IsoWriteState::ChoosingWindowsEdition:
    case IsoWriteState::SelectingGrubCore:
    case IsoWriteState::ChoosingWindowsOptions:
    case IsoWriteState::ChoosingDosOptions:
    case IsoWriteState::AwaitingConfirmation:
    case IsoWriteState::Authorizing:
    case IsoWriteState::Writing:
    case IsoWriteState::Cancelling:
        return true;
    default:
        return false;
    }
}

bool IsoWriteController::isWriting() const
{
    return process_ && process_->isActive() && process_->state() != PrepareImageProcess::State::Starting;
}

void IsoWriteController::reset()
{
    if (isActive())
        return;
    process_.reset();
    source_.reset();
    selectedSyslinux_.clear();
    selectedCore_ = {};
    selectedDosImage_.clear();
    grubDownloadTotal_ = 0;
    request_ = {};
    target_ = {};
    check_ = {};
    problem_.clear();
    phase_.clear();
    progressPosition_ = 0;
    operationPercent_ = 0.0f;
    invalidated_.clear();
    state_ = IsoWriteState::Idle;
    windowsArchiveChosen_ = windowsImageChosen_ = false;
}

void IsoWriteController::publish()
{
    emit logMessage(statusText());
    emit changed();
}

void IsoWriteController::prepare(const PrepareImageRequest& source, const Device& target,
    const QString& label, quint32 cluster, const IMAGE_WRITE_CHOICES& choices, int fileSystem)
{
    if (isActive() || !isAvailable(IMAGE_WRITE_DD))
        return;
    reset();
    target_ = target;
    request_ = source;
    request_.windowsFlags = 0;
    request_.windowsUsername.clear();
    if (!request_.windowsToGo) request_.windowsInstallPath.clear();
    request_.windowsEdition = request_.windowsToGo ? 1 : 0;
    request_.windowsRegion = {};
    request_.windowsSilentConfirmed = false;
    request_.path = target.path;
    request_.device = target.number;
    request_.size = target.size;
    request_.logicalSectorSize = target.logicalSectorSize;
    request_.physicalSectorSize = target.physicalSectorSize;
    request_.identity = target.identity;
    request_.objectPath = target.objectPath;
    request_.serviceOwner = target.serviceOwner;
    request_.isDevice = true;
    FreezeDevicePolicy(request_);
    request_.label = label;
    request_.displayLabel = request_.extendedLabel ? ExtendedMediaLabel(label) : QString();
    request_.clusterSize = cluster;
    request_.fileSystem = fileSystem;
    request_.udfRevision = source.udfRevision;
    const bool ext = fileSystem == FS_EXT2 || fileSystem == FS_EXT3;
    const bool nativeSerial = fileSystem == FS_NTFS || fileSystem == FS_EXFAT || fileSystem == FS_UDF || ext;
    request_.volumeId = nativeSerial ? 0 : QRandomGenerator::global()->generate();
    request_.nonBootable = source.nonBootable;
    const bool standaloneSyslinux = request_.isStandaloneSyslinux();
    if (request_.isSourceFree())
    {
        const bool supported = fileSystem == FS_FAT16 || fileSystem == FS_FAT32 ||
            fileSystem == FS_NTFS || fileSystem == FS_EXFAT
            || (fileSystem == FS_UDF && source.nonBootable) || (ext && (source.grub2 || source.nonBootable));
        const bool sourceFree = source.imagePath.isEmpty() && (!source.syslinux || standaloneSyslinux) && !source.enhancedInstall;
        const bool bootFormat = !request_.isStandaloneUefiNtfs() || (!request_.nonBootable && nativeSerial);
        if (!supported || !sourceFree || !bootFormat)
        {
            state_ = IsoWriteState::Failed;
            problem_ = tr("The selected boot type requires compatible format settings and no image.");
            publish();
            return;
        }
        // Win32 includes exFAT in ToValidLabel's FAT-style normalization.
        request_.label = ext ? NormalizeExtFormatLabel(label)
            : NormalizeFormatLabel(label, target.size, fileSystem == FS_NTFS || fileSystem == FS_UDF);
        request_.diskSequence = ReadDiskSequence(target.number);
        if (request_.isDos())
        {
            if (request_.msDos) request_.dosImagePath = QDir(filesDirectory_).filePath(QStringLiteral("diskcopy.dll"));
            // The user chooses DOS settings before checks, confirmation and
            // authorization. The device/format selection is already frozen.
            request_.dosKeyboard.clear();
            request_.dosCodepage = 0;
            state_ = IsoWriteState::ChoosingDosOptions;
            publish();
            emit dosOptionsRequired();
        }
        else if (request_.grub4dos)
        {
            // This request requires a frozen loader before strict preflight.
            // Acquisition and device checks both precede authorization.
            state_ = IsoWriteState::SelectingGrubCore;
            publish();
            if (state_ != IsoWriteState::SelectingGrubCore)
                return;
            const int error = grubCore_.startGrub4dos(filesDirectory_, filesServer_);
            if (error != 0)
            {
                Grub2CoreSelectionResult failure;
                failure.error = error;
                grubCoreFinished(failure);
            }
        }
        else
            startCheck(false);
        return;
    }
    source_ = std::make_unique<PrepareIsoSource>(request_);
    const int error = source_->open(getuid());
    if (error != 0)
    {
        state_ = IsoWriteState::Failed;
        problem_ = tr("The selected image or device changed. Select it again.");
        source_.reset();
        publish();
        return;
    }
    check_.choices = choices;
    if (choices.count < 1 || choices.count > 3)
    {
        state_ = IsoWriteState::Failed;
        problem_ = tr("No supported image mode is available.");
        source_.reset();
        publish();
        return;
    }
    state_ = IsoWriteState::ChoosingMode;
    publish();
    if (choices.count == 1)
        chooseMode(choices.mode[0]);
    else
        emit modeRequired();
}

void IsoWriteController::startCheck(bool confirmed)
{
    if (!isAvailable())
    {
        state_ = IsoWriteState::Failed;
        problem_ = connection_ && connection_->isConnected()
            ? tr("The Rufus preparation helper is unavailable. Install the matching helper and try again.")
            : tr("The device service connection is unavailable. Reconnect and try again.");
        source_.reset();
        publish();
        return;
    }
    confirmed_ = confirmed;
    hasResult_ = false;
    workerActive_ = true;
    state_ = IsoWriteState::Checking;
    IsoCheckJob* job = new IsoCheckJob(*connection_, request_, confirmed, request_.windowsToGo && !windowsImageChosen_, check_.analysis.report);
    job->moveToThread(&thread_);
    connect(&thread_, &QThread::started, job, &RufusJob::run);
    connect(job, &IsoCheckJob::checked, this, &IsoWriteController::acceptCheck, Qt::QueuedConnection);
    connect(job, &RufusJob::logMessage, this, &IsoWriteController::logMessage, Qt::QueuedConnection);
    connect(job, &RufusJob::failed, this, &IsoWriteController::checkFailed, Qt::QueuedConnection);
    connect(job, &RufusJob::finished, &thread_, &QThread::quit, Qt::DirectConnection);
    connect(&thread_, &QThread::finished, job, &QObject::deleteLater);
    thread_.start();
    publish();
}

void IsoWriteController::acceptCheck(int error, const IsoWriteCheck& result)
{
    checkError_ = error;
    hasResult_ = true;
    if (!confirmed_)
        check_ = result;
    problem_ = result.problem;
    const bool newWarning = confirmed_ && check_.existingPartitions <= 1 && result.existingPartitions > 1;
    const bool changedWindowsPlan = confirmed_ && request_.windowsToGo
        && (result.windowsApplyBytes != check_.windowsApplyBytes || result.partition.offset != check_.partition.offset
            || result.partition.size != check_.partition.size);
    if (checkError_ == 0 && changedWindowsPlan)
    {
        checkError_ = ESTALE;
        problem_ = tr("The Windows To Go deployment plan changed. Start again.");
    }
    if (checkError_ == 0 && newWarning)
    {
        checkError_ = ESTALE;
        problem_ = tr("The device now contains multiple partitions. Start again to review the warnings.");
    }
}

void IsoWriteController::checkFailed(const QString& problem)
{
    checkError_ = EIO;
    hasResult_ = true;
    problem_ = problem;
}

void IsoWriteController::checkFinished()
{
    thread_.wait();
    workerActive_ = false;
    if (state_ == IsoWriteState::Cancelling)
    {
        state_ = invalidated_.isEmpty() ? IsoWriteState::Cancelled : IsoWriteState::Failed;
        problem_ = invalidated_;
        source_.reset();
        publish();
        return;
    }
    const bool failedCheck = !hasResult_ || checkError_ != 0;
    if (failedCheck)
    {
        state_ = IsoWriteState::Failed;
        if (problem_.isEmpty())
            problem_ = tr("Preparation checks ended without a result.");
        source_.reset();
        publish();
        return;
    }
    if (!confirmed_)
    {
        if (request_.windowsToGo && !windowsImageChosen_)
        {
            requestWindowsImage();
            return;
        }
        if (request_.syslinux && !request_.isStandaloneSyslinux() && selectedSyslinux_.isEmpty()) {
            state_ = IsoWriteState::SelectingGrubCore; publish();
            if (state_ != IsoWriteState::SelectingGrubCore) return;
            const int error = syslinuxResources_.start(request_,check_.analysis.report,check_.syslinuxResources,filesDirectory_,filesServer_);
            if (error) syslinuxFinished(error,{});
            return;
        }
        const bool biosMbr = request_.mbr || request_.syslinux;
        if (!request_.windowsToGo && NeedsGrub2CoreLookup(&check_.analysis.report, biosMbr, biosMbr, GRUB2_PACKAGE_VERSION))
        {
            // Win32 performs this lookup even when Syslinux later wins SBR
            // precedence. Do not turn a lookup into a GRUB installer selection.
            state_ = IsoWriteState::SelectingGrubCore;
            publish();
            if (state_ != IsoWriteState::SelectingGrubCore)
                return;
            const int error = grubCore_.start(request_, check_.analysis.report, filesDirectory_,
                biosMbr, biosMbr, check_.partition.offset, filesServer_);
            if (error != 0)
            {
                Grub2CoreSelectionResult failure;
                failure.error = error;
                grubCoreFinished(failure);
            }
            return;
        }
        requestImageOptions();
        return;
    }
    RUFUS_PROGRESS_OPTIONS progressOptions = {};
    const bool sourceFree = request_.isSourceFree();
    progressOptions.bootable = !request_.nonBootable;
    progressOptions.selection = request_.syslinuxSelection;
    if (request_.isDos()) progressOptions.selection = request_.dosBootType();
    else if (request_.reactOs) progressOptions.selection = BT_REACTOS;
    else if (request_.grub2 && sourceFree) progressOptions.selection = BT_GRUB2;
    else if (request_.grub4dos && sourceFree) progressOptions.selection = BT_GRUB4DOS;
    else if (request_.isStandaloneUefiNtfs()) progressOptions.selection = BT_UEFI_NTFS;
    else if (request_.nonBootable) progressOptions.selection = BT_NON_BOOTABLE;
    progressOptions.image = !sourceFree;
    progressOptions.windows_customization = request_.windowsFlags != 0;
    progressOptions.filesystem_steps = nb_steps[request_.fileSystem];
    // Same large-FAT32 weighting as Win32; the native formatter exposes no
    // intermediate callbacks, so its range completes at the following stage.
    constexpr quint64 largeFat32Bytes = 32ULL * 1024 * 1024 * 1024;
    progressOptions.bad_blocks = request_.badBlocksPattern >= 0;
    progressOptions.format_progress = !request_.quickFormat || request_.persistenceSize != 0
        || (request_.fileSystem == FS_FAT32 && (request_.isoEsp || request_.size >= largeFat32Bytes));
    progressOptions.ntfs_image = request_.fileSystem == FS_NTFS && !sourceFree;
    progressOptions.extra_archive = request_.archive.present();
    InitProgressState(&progress_, &progressOptions);
    progressPosition_ = 0;
    operationPercent_ = 0.0f;
    process_ = std::make_unique<PrepareImageProcess>(helper_);
    process_->setReportDirectory(reportDirectory_);
    connect(process_.get(), &PrepareImageProcess::badBlocksReviewRequired, this, &IsoWriteController::badBlocksReviewRequired);
    connect(process_.get(), &PrepareImageProcess::changed, this, &IsoWriteController::processChanged);
    connect(process_.get(), &PrepareImageProcess::progressChanged, this, &IsoWriteController::acceptProgress);
    connect(process_.get(), &PrepareImageProcess::stepChanged, this, &IsoWriteController::acceptStep);
    connect(process_.get(), &PrepareImageProcess::phaseChanged, this, &IsoWriteController::phaseChanged);
    connect(process_.get(), &PrepareImageProcess::logMessage, this, &IsoWriteController::logMessage);
    connect(process_.get(), &PrepareImageProcess::finished, this, &IsoWriteController::processFinished);
    state_ = IsoWriteState::Authorizing;
    const bool sourceGrub2Sbr = (request_.windowsBios || request_.syslinux)
        && GetSecondaryBootType(BT_IMAGE, &check_.analysis.report, false) == BT_GRUB2;
    const bool standaloneGrub4dos = request_.grub4dos && sourceFree;
    const QByteArray core = request_.msDos ? selectedDosImage_ : request_.grub2 || standaloneGrub4dos || sourceGrub2Sbr ? selectedCore_.bytes : QByteArray();
    PrepareImageRequest launch = request_;
    if (standaloneGrub4dos)
    {
        // Preflight validates the selected metadata. The process creates its
        // private staged file from retained bytes, never from the cache path.
        launch.grub2CorePath.clear();
        launch.grub2CoreSize = 0;
        launch.grub2CoreSha256.clear();
    }
    if (!process_->start(launch, core, selectedSyslinux_))
    {
        state_ = IsoWriteState::Failed;
        problem_ = tr("Cannot start the preparation helper.");
        source_.reset();
    }
    publish();
}

void IsoWriteController::requestImageOptions()
{
    const bool sourceGrub2Sbr = (request_.windowsBios || request_.syslinux)
        && GetSecondaryBootType(BT_IMAGE, &check_.analysis.report, false) == BT_GRUB2;
    if (sourceGrub2Sbr && selectedCore_.bytes.isEmpty())
    {
        const int error = ReadEmbeddedGrub2Core(check_.partition.offset, selectedCore_.bytes);
        if (error != 0)
        {
            state_ = IsoWriteState::Failed;
            problem_ = tr("Cannot read the embedded Grub installation file: %1").arg(QString::fromLocal8Bit(strerror(error)));
            source_.reset();
            publish();
            return;
        }
        selectedCore_.sha256 = QCryptographicHash::hash(selectedCore_.bytes, QCryptographicHash::Sha256);
    }
    const int available = request_.windowsToGo ? GetWindowsToGoUnattendFlags(&check_.analysis.report, false)
        : GetWindowsUnattendFlags(&check_.analysis.report, false);
    if (available != 0)
    {
        state_ = IsoWriteState::ChoosingWindowsOptions;
        publish();
        emit windowsOptionsRequired();
    }
    else
    {
        state_ = IsoWriteState::AwaitingConfirmation;
        publish();
        emit confirmationRequired();
    }
}

void IsoWriteController::requestWindowsImage()
{
    request_.windowsInstallPath = check_.windowsInstallPath;
    if (!windowsArchiveChosen_ && check_.windowsArchives.size() > 1)
    {
        state_ = IsoWriteState::ChoosingWindowsArchive;
        publish();
        emit windowsArchiveRequired();
        return;
    }
    windowsArchiveChosen_ = true;
    state_ = IsoWriteState::ChoosingWindowsEdition;
    publish();
    if (check_.windowsInstall.editions.size() == 1)
        chooseWindowsEdition(check_.windowsInstall.editions.first().image);
    else
        emit windowsEditionRequired();
}

void IsoWriteController::chooseWindowsArchive(const QString& path)
{
    if (state_ != IsoWriteState::ChoosingWindowsArchive || !check_.windowsArchives.contains(path)) return;
    windowsArchiveChosen_ = true;
    request_.windowsInstallPath = path;
    request_.windowsEdition = 1;
    startCheck(false);
}

void IsoWriteController::chooseWindowsEdition(int image)
{
    if (state_ != IsoWriteState::ChoosingWindowsEdition) return;
    bool available = false;
    for (const WindowsEdition& edition : check_.windowsInstall.editions)
        if (edition.image == image) available = true;
    if (!available) return;
    request_.windowsEdition = image;
    windowsImageChosen_ = true;
    startCheck(false);
}

void IsoWriteController::msDosFinished(MsDosSelectionResult result)
{
    if (state_ != IsoWriteState::SelectingGrubCore && state_ != IsoWriteState::Cancelling) return;
    progressPosition_ = 0; operationPercent_ = 0; grubDownloadTotal_ = 0;
    if (result.error != 0) {
        const bool cancelled = result.error == ECANCELED && invalidated_.isEmpty();
        state_ = cancelled ? IsoWriteState::Cancelled : IsoWriteState::Failed;
        problem_ = invalidated_.isEmpty() ? tr("Cannot select the MS-DOS files: %1").arg(QString::fromLocal8Bit(strerror(result.error))) : invalidated_;
        selectedDosImage_.clear(); publish(); return;
    }
    request_.dosImagePath = result.path;
    selectedDosImage_ = std::move(result.bytes);
    startCheck(false);
}

void IsoWriteController::syslinuxFinished(int error, QByteArray resources)
{
    if (state_ != IsoWriteState::SelectingGrubCore && state_ != IsoWriteState::Cancelling) return;
    if (error) {
        state_ = error == ECANCELED && invalidated_.isEmpty() ? IsoWriteState::Cancelled : IsoWriteState::Failed;
        problem_ = !invalidated_.isEmpty() ? invalidated_ : error == ECANCELED ? QString() : tr("Cannot select matching Syslinux resources: %1").arg(QString::fromLocal8Bit(strerror(error)));
        source_.reset(); publish(); return;
    }
    SyslinuxResources selected;
    error = DecodeSyslinuxResources(resources,selected);
    const quint64 extra = quint64(selected.loader.size()+selected.oldModules[0].size()+selected.oldModules[1].size());
    if (!error && (extra > check_.partition.size || check_.tree.required_bytes > check_.partition.size-extra)) error = ENOSPC;
    if (error) { state_ = IsoWriteState::Failed; problem_ = tr("The selected Syslinux resources do not fit this target."); source_.reset(); publish(); return; }
    selectedSyslinux_ = std::move(resources); state_ = IsoWriteState::Checking; checkFinished();
}

void IsoWriteController::grubCoreFinished(Grub2CoreSelectionResult result)
{
    if (state_ != IsoWriteState::SelectingGrubCore && state_ != IsoWriteState::Cancelling)
        return;
    progressPosition_ = 0;
    operationPercent_ = 0.0f;
    grubDownloadTotal_ = 0;
    if (result.error != 0)
    {
        const bool cancelled = result.error == ECANCELED && invalidated_.isEmpty();
        state_ = cancelled ? IsoWriteState::Cancelled : IsoWriteState::Failed;
        problem_ = invalidated_.isEmpty()
            ? tr("Cannot select the Grub installation file: %1").arg(QString::fromLocal8Bit(strerror(result.error)))
            : invalidated_;
        selectedCore_ = {};
        source_.reset();
        publish();
        return;
    }
    selectedCore_ = std::move(result);
    if (request_.grub4dos)
    {
        request_.grub2CorePath = selectedCore_.cachePath;
        request_.grub2CoreSize = quint64(selectedCore_.bytes.size());
        request_.grub2CoreSha256 = selectedCore_.sha256;
        startCheck(false);
    }
    else
        requestImageOptions();
}

void IsoWriteController::grubDownloadProgress(quint64 received, quint64 total)
{
    if (state_ != IsoWriteState::SelectingGrubCore)
        return;
    grubDownloadTotal_ = total;
    operationPercent_ = ProgressPercent(received, total);
    constexpr float fullPercent = 100.0f;
    progressPosition_ = int(operationPercent_ * MAX_PROGRESS / fullPercent);
    emit changed();
}

void IsoWriteController::chooseGrubDownload(Grub2DownloadChoice choice)
{
    if (state_ == IsoWriteState::SelectingGrubCore)
        if (syslinuxResources_.isActive()) syslinuxResources_.choose(choice);
        else grubCore_.choose(choice);
}

void IsoWriteController::chooseMode(IMAGE_WRITE_MODE mode)
{
    if (state_ != IsoWriteState::ChoosingMode)
        return;
    if (mode == IMAGE_WRITE_NONE)
    {
        cancel();
        return;
    }
    bool offered = false;
    for (int i = 0; i < check_.choices.count; ++i)
        offered = offered || check_.choices.mode[i] == mode;
    if (!offered)
        return;
    if (mode == IMAGE_WRITE_DD)
    {
        PrepareImageRequest source = request_;
        source.archive = {}; // Windows bypasses supplemental extraction for DD.
        source.runtimeValidation = false;
        const Device target = target_;
        state_ = IsoWriteState::Idle;
        source_.reset();
        emit ddRequested(source, target);
        emit changed();
        return;
    }
    request_.isoEsp = mode == IMAGE_WRITE_ESP;
    // Win32's ESP path uses formatter defaults and always runs its quick
    // large-FAT32 formatter. Keep the user's allocation unit for layout.
    if (request_.isoEsp && request_.fileSystem == FS_FAT32) request_.quickFormat = true;
    const bool grubFat = request_.fileSystem == FS_FAT16 || request_.fileSystem == FS_FAT32;
    const bool grubNtfs = request_.fileSystem == FS_NTFS;
    if (request_.grub2 && !grubFat && !grubNtfs)
    {
        state_ = IsoWriteState::Failed;
        problem_ = tr("GRUB ISO creation requires FAT16, FAT32 or NTFS.");
        source_.reset();
        publish();
        return;
    }
    // Normalize only the accepted ISO plan. DD neither formats nor changes
    // the text in the form; confirmation shows the actual filesystem label.
    request_.label = NormalizeFormatLabel(request_.label, request_.size, request_.fileSystem == FS_NTFS);
    request_.diskSequence = ReadDiskSequence(request_.device);
    if (request_.diskSequence == 0)
    {
        state_ = IsoWriteState::Failed;
        problem_ = tr("The selected device's identity is unavailable. Select it again.");
        source_.reset();
        publish();
        return;
    }
    startCheck(false);
}

void IsoWriteController::chooseWindowsOptions(const PrepareImageRequest& options)
{
    if (state_ != IsoWriteState::ChoosingWindowsOptions)
        return;
    // Copy choices only: the dialog must not replace retained source/device facts.
    PrepareImageRequest selected = request_;
    selected.windowsFlags = options.windowsFlags;
    selected.windowsUsername = options.windowsUsername;
    if (!request_.windowsToGo)
    {
        selected.windowsInstallPath = options.windowsInstallPath;
        selected.windowsEdition = options.windowsEdition;
        selected.windowsRegion = options.windowsRegion;
        selected.windowsSilentConfirmed = options.windowsSilentConfirmed;
    }
    const bool validSettings = CheckWindowsCustomizationSettings(selected) == 0;
    const bool imageAllows = request_.windowsToGo
        || CheckWindowsCustomizationImage(check_.analysis.report, selected.windowsFlags) == 0;
    const int offered = request_.windowsToGo ? GetWindowsToGoUnattendFlags(&check_.analysis.report, expertMode_)
        : GetWindowsUnattendFlags(&check_.analysis.report, expertMode_);
    const bool offeredChoices = (selected.windowsFlags & ~offered) == 0;
    if (!validSettings || !imageAllows || !offeredChoices)
    {
        state_ = IsoWriteState::Failed;
        problem_ = tr("The selected Windows customization is not available for this image.");
        source_.reset();
        publish();
        return;
    }
    if (!request_.windowsToGo && CheckWindowsInstallSelection(selected, check_, problem_) != 0)
    {
        state_ = IsoWriteState::Failed;
        source_.reset();
        publish();
        return;
    }
    request_ = selected;
    const int editable = offered & (request_.windowsToGo ? WindowsToGoCustomizationFlags : WindowsCustomizationFlags);
    windowsOptions_ = (windowsOptions_ & ~editable) | (selected.windowsFlags & editable);
    ApplicationSettings settings;
    settings.setValue(QStringLiteral("WindowsUserExperience"), quint32((UNATTEND_DEFAULT_MASK << 16) | windowsOptions_));
    state_ = IsoWriteState::AwaitingConfirmation;
    publish();
    emit confirmationRequired();
}

void IsoWriteController::chooseDosOptions(const QString& keyboard, quint32 codepage)
{
    if (state_ != IsoWriteState::ChoosingDosOptions)
        return;
    request_.dosKeyboard = keyboard;
    request_.dosCodepage = codepage;
    // Reuse the strict request boundary; never silently substitute a fallback.
    if (EncodePrepareRequest(request_).isEmpty())
    {
        state_ = IsoWriteState::Failed;
        problem_ = tr("The selected DOS settings are unavailable.");
        publish();
        return;
    }
    if (request_.msDos) {
        state_ = IsoWriteState::SelectingGrubCore;
        publish();
        if (state_ != IsoWriteState::SelectingGrubCore) return;
        const int error = msDos_.start(filesDirectory_);
        if (error != 0) { MsDosSelectionResult failure; failure.error = error; msDosFinished(failure); }
    } else startCheck(false);
}

void IsoWriteController::confirm()
{
    if (state_ == IsoWriteState::AwaitingConfirmation)
        startCheck(true);
}

void IsoWriteController::cancel()
{
    if (!isActive() || state_ == IsoWriteState::Cancelling)
        return;
    const bool running = workerActive_ || syslinuxResources_.isActive() || grubCore_.isActive() || msDos_.isActive() || (process_ && process_->isActive());
    state_ = running ? IsoWriteState::Cancelling : IsoWriteState::Cancelled;
    if (syslinuxResources_.isActive() || grubCore_.isActive() || msDos_.isActive())
    {
        if (request_.msDos) msDos_.cancel();
        else if (syslinuxResources_.isActive()) syslinuxResources_.cancel();
        else grubCore_.cancel();
        return; // The synchronous result publishes the terminal state.
    }
    selectedCore_ = {};
    if (workerActive_)
        thread_.requestInterruption();
    else if (process_ && process_->isActive())
        process_->cancel();
    else
        source_.reset();
    publish();
}

void IsoWriteController::invalidate(const QString& problem)
{
    if (!isActive())
        return;
    invalidated_ = problem;
    cancel();
    if (!isActive())
    {
        state_ = IsoWriteState::Failed;
        problem_ = problem;
        publish();
    }
}

void IsoWriteController::processChanged()
{
    if (state_ == IsoWriteState::Cancelling)
        return;
    if (process_->state() == PrepareImageProcess::State::Running)
    {
        state_ = IsoWriteState::Writing;
        publish();
    }
}

void IsoWriteController::phaseChanged(const QString& phase)
{
    processedBytes_ = totalBytes_ = 0;
    phase_ = phase;
    operationPercent_ = 0.0f;
    const bool windowsBootWork = request_.windowsToGo && (phase == QStringLiteral("bootloader")
        || phase == QStringLiteral("mounting") || phase == QStringLiteral("customizing"));
    if (windowsBootWork) { publish(); return; }
    int operation = OP_ANALYZE_MBR;
    float percent = 0.0f;
    if (phase == QStringLiteral("badblocks")) operation = OP_BADBLOCKS;
    else if (phase == QStringLiteral("partitioning")) operation = OP_PARTITION;
    else if (phase == QStringLiteral("formatting"))
        operation = progress_.nb_slots[OP_FORMAT] != 0 ? OP_FORMAT : OP_CREATE_FS;
    else if (phase == QStringLiteral("bootloader") && request_.isStandaloneSyslinux())
        operation = OP_FILE_COPY;
    else if (phase == QStringLiteral("bootloader") || phase == QStringLiteral("mounting"))
        operation = OP_FIX_MBR;
    else if (phase == QStringLiteral("extracting")) operation = OP_FILE_COPY;
    else if (phase == QStringLiteral("customizing")) operation = OP_PATCH;
    else if (phase == QStringLiteral("archive")) operation = OP_EXTRACT_ZIP;
    else if (phase == QStringLiteral("archive-finalizing")) { operation = OP_EXTRACT_ZIP; percent = 100.0f; }
    else if (phase == QStringLiteral("finalizing"))
    {
        operation = OP_FINALIZE;
        percent = -1.0f; // Original first finalization step, before cleanup.
    }
    const int position = UpdateProgressState(&progress_, operation, percent);
    if (position >= 0)
        progressPosition_ = position;
    publish();
}

void IsoWriteController::acceptProgress(int operation, quint64 processed, quint64 total)
{
    processedBytes_ = processed; totalBytes_ = total;
    operationPercent_ = ProgressPercent(processed, total);
    const int position = UpdateProgressState(&progress_, operation, operationPercent_);
    if (position >= 0)
        progressPosition_ = position;
    emit changed(); // Progress refreshes must not flood the log.
}

void IsoWriteController::acceptStep(int operation)
{
    const int position = UpdateProgressState(&progress_, operation, -1.0f);
    if (position >= 0)
        progressPosition_ = position;
    emit changed();
}

void IsoWriteController::processFinished()
{
    problem_ = process_->problem();
    if (!invalidated_.isEmpty())
    {
        state_ = IsoWriteState::Failed;
        problem_ = invalidated_ + QLatin1Char(' ') + problem_;
    }
    else if (process_->state() == PrepareImageProcess::State::Complete)
    {
        const PrepareImageOutcome& outcome = process_->outcome();
        const bool standaloneGrub4dos = request_.grub4dos && request_.isSourceFree();
        const bool completeCopy = request_.windowsToGo ? outcome.windowsToGoDone && outcome.copiedBytes == check_.windowsApplyBytes
            : request_.isDos() ? outcome.copiedBytes != 0
            : standaloneGrub4dos ? outcome.copiedBytes == request_.grub2CoreSize
            : outcome.copiedBytes == check_.tree.bytes_written;
        const bool samePartition = outcome.offset == check_.partition.offset
            && outcome.size == check_.partition.size;
        const bool formatOnly = request_.nonBootable || request_.isStandaloneUefiNtfs() || request_.isStandaloneSyslinux() || request_.reactOs || (request_.grub2 && request_.isSourceFree());
        const bool nativeFormat = request_.fileSystem == FS_NTFS || request_.fileSystem == FS_UDF || request_.fileSystem == FS_EXT2 || request_.fileSystem == FS_EXT3;
        const bool exactFormatSize = request_.fileSystem == FS_EXFAT ||
            (nativeFormat && (formatOnly || request_.grub2 || request_.grub4dos || request_.windowsToGo));
        const bool completeFormat = !exactFormatSize || outcome.filesystemBytes == outcome.size;
        const bool sameOperation = outcome.windowsToGoDone == request_.windowsToGo;
        state_ = completeCopy && samePartition && completeFormat && sameOperation ? IsoWriteState::Complete : IsoWriteState::Failed;
        if (!sameOperation)
            problem_ = tr("The helper reported completing a different operation.");
        else if (!completeCopy)
            problem_ = request_.windowsToGo ? tr("The helper did not complete Windows image deployment and boot setup.")
                : tr("The helper did not report copying the complete image tree.");
        else if (!samePartition)
            problem_ = tr("The helper reported a partition layout that differs from the confirmed plan.");
        else if (!completeFormat)
            problem_ = tr("The helper did not report formatting the complete partition.");
    }
    else if (process_->state() == PrepareImageProcess::State::Cancelled)
        state_ = IsoWriteState::Cancelled;
    else
        state_ = IsoWriteState::Failed;
    source_.reset();
    publish();
}

QString IsoWriteController::statusText() const
{
    const bool standaloneSyslinux = request_.isStandaloneSyslinux();
    const QLatin1String syslinuxName(request_.syslinuxSelection == BT_SYSLINUX_V4 ? nativeSyslinux4Name : nativeSyslinuxName);
    const bool sourceFree = request_.isSourceFree();
    switch (state_)
    {
    case IsoWriteState::Idle: return {};
    case IsoWriteState::Checking: return sourceFree ? tr("Checking the format settings and device...")
        : tr("Checking the selected image, settings and device...");
    case IsoWriteState::ChoosingMode: return tr("Waiting for image mode selection...");
    case IsoWriteState::SelectingGrubCore:
        if (request_.msDos) return msDos_.isActive() ? tr("Selecting MS-DOS files...") : tr("Checking MS-DOS files...");
        return grubDownloadTotal_ == 0 ? tr("Checking bootloader...")
            : tr("Downloading: %1").arg(QString::asprintf("%0.1f%%", double(operationPercent_)));
    case IsoWriteState::ChoosingWindowsOptions: return tr("Waiting for Windows customization choices...");
    case IsoWriteState::ChoosingWindowsArchive: return tr("Waiting for a Windows installation archive...");
    case IsoWriteState::ChoosingWindowsEdition: return tr("Waiting for a Windows edition...");
    case IsoWriteState::ChoosingDosOptions: return tr("Waiting for FreeDOS settings...");
    case IsoWriteState::AwaitingConfirmation: return tr("Waiting for confirmation...");
    case IsoWriteState::Authorizing: return tr("Waiting for device authorization...");
    case IsoWriteState::Cancelling: return tr("Cancelling... Waiting for device cleanup.");
    case IsoWriteState::Complete:
        if (request_.windowsToGo) return tr("Windows To Go creation completed.");
        if (standaloneSyslinux) return tr("%1 creation completed.").arg(syslinuxName);
        if (request_.grub2 && sourceFree) return tr("Grub " GRUB2_PACKAGE_VERSION " creation completed.");
        if (request_.grub4dos && sourceFree) return tr("Grub4DOS " GRUB4DOS_VERSION " creation completed.");
        if (request_.isStandaloneUefiNtfs()) return tr("UEFI:NTFS creation completed.");
        if (request_.isDos()) return request_.msDos ? tr("MS-DOS creation completed.") : tr("FreeDOS creation completed.");
        if (request_.reactOs) return tr("ReactOS boot records created.");
        return sourceFree ? tr("Formatting completed.") : tr("ISO image creation completed.");
    case IsoWriteState::Cancelled:
        if (standaloneSyslinux && !process_) return tr("%1 creation cancelled.").arg(syslinuxName);
        if (request_.grub2 && sourceFree && !process_) return tr("Grub " GRUB2_PACKAGE_VERSION " creation cancelled.");
        if (request_.grub4dos && sourceFree && !process_) return tr("Grub4DOS " GRUB4DOS_VERSION " creation cancelled.");
        if (request_.isDos() && !process_) return request_.msDos ? tr("MS-DOS creation cancelled.") : tr("FreeDOS creation cancelled.");
        return process_ && process_->outcome().started
            ? tr("Creation cancelled. The device contains incomplete media.")
            : (sourceFree ? tr("Formatting cancelled.") : tr("ISO image creation cancelled."));
    case IsoWriteState::Failed:
        return process_ && process_->outcome().started
            ? problem_ + tr(" The device may contain incomplete media.") : problem_;
    case IsoWriteState::Writing:
        if (phase_ == QStringLiteral("badblocks")) return tr("Checking for bad blocks and fake capacity...");
        if (phase_ == QStringLiteral("partitioning")) return tr("Partitioning...");
        if (phase_ == QStringLiteral("formatting") && (request_.fileSystem == FS_EXT2 || request_.fileSystem == FS_EXT3))
            return tr("Formatting ext%1...").arg(request_.fileSystem == FS_EXT2 ? 2 : 3);
        if (phase_ == QStringLiteral("formatting") && request_.fileSystem == FS_UDF) return tr("Formatting UDF...");
        if (phase_ == QStringLiteral("formatting") && request_.fileSystem == FS_EXFAT) return tr("Formatting exFAT...");
        if (phase_ == QStringLiteral("formatting"))
            return request_.fileSystem == FS_NTFS ? tr("Formatting NTFS...") :
                tr("Formatting FAT%1...").arg(request_.fileSystem == FS_FAT16 ? 16 : 32);
        if (phase_ == QStringLiteral("bootloader") && standaloneSyslinux)
            return tr("Installing %1...").arg(syslinuxName);
        if (phase_ == QStringLiteral("bootloader") && request_.grub4dos)
            return sourceFree ? tr("Installing Grub4DOS " GRUB4DOS_VERSION "...") : tr("Writing master boot record...");
        if (phase_ == QStringLiteral("bootloader") && request_.grub2)
            return sourceFree ? tr("Installing Grub " GRUB2_PACKAGE_VERSION "...") : tr("Installing Grub2...");
        if (phase_ == QStringLiteral("bootloader") && request_.windowsToGo) return tr("Setting up Windows boot files...");
        if (phase_ == QStringLiteral("bootloader"))
            return request_.isDos() || request_.reactOs || request_.windowsBios || request_.kolibriOs ? tr("Writing partition boot record...") : tr("Installing UEFI:NTFS...");
        if (phase_ == QStringLiteral("customizing")) return tr("Applying Windows customization...");
        if (phase_ == QStringLiteral("archive")) return tr("Copying supplementary ZIP files: %1")
            .arg(QString::asprintf("%0.1f%%", double(operationPercent_)));
        if (phase_ == QStringLiteral("archive-finalizing")) return tr("Finalizing supplementary files, please wait...");
        if (phase_ == QStringLiteral("mounting") && standaloneSyslinux)
            return tr("Installing %1...").arg(syslinuxName);
        if (phase_ == QStringLiteral("mounting")) return tr("Preparing file extraction...");
        if (phase_ == QStringLiteral("extracting") && request_.grub4dos && sourceFree) return tr("Copying: grldr");
        if (phase_ == QStringLiteral("extracting") && request_.isDos()) return tr("Copying DOS files...");
        if (phase_ == QStringLiteral("extracting") && request_.windowsToGo)
            return tr("Applying Windows image: %1").arg(QString::asprintf("%0.1f%%", double(operationPercent_)));
        if (phase_ == QStringLiteral("extracting"))
            return tr("Copying ISO files: %1").arg(QString::asprintf("%0.1f%%", double(operationPercent_)));
        if (phase_ == QStringLiteral("finalizing")) return tr("Finalizing, please wait...");
        return sourceFree ? tr("Checking the selected device...") : tr("Checking the selected image and device...");
    }
    return {};
}
void IsoWriteController::discardCachedBootResources()
{
    if (isActive()) return;
    selectedCore_ = {}; selectedDosImage_.clear();
}

}
#include "iso_write_controller.moc"
