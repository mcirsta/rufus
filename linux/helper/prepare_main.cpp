#ifdef RUFUS_SAVE_SOURCE_HELPER
#include "platform/save_source.h"
#endif
// SPDX-License-Identifier: GPL-3.0-or-later
#include "qt/prepare_image_protocol.h"
#include "qt/rufus_job.h"
#include "qt/job_priority.h"
#include "rufus_reporting.h"
#include "platform/fat_format.h"
#include "platform/fat_boot.h"
#include "platform/freedos_copy.h"
#include "platform/partition_image.h"
#include "platform/prepare_device.h"
#include "platform/prepare_badblocks.h"
#include "platform/media_appearance.h"
#include "platform/media_integrity.h"
#include "platform/zip_extract.h"
#include <QMutex>
#include <QPointer>
#include <QWaitCondition>
#include <QElapsedTimer>
#include "platform/device_operations.h"
#include "platform/format_options.h"
#include "platform/raw_device.h"
#include "platform/prepare_source.h"
#include "platform/grub2_cache.h"
#include "platform/target_mounts.h"
#include "platform/iso_extract.h"
#include "platform/uefi_ntfs.h"
#include "platform/iso_write_check.h"
#include "platform/windows_customization.h"
#include "platform/windows_bootloaders.h"
#include "platform/windows_to_go.h"
#include "platform/syslinux_install.h"
#include "platform/syslinux_native.h"
#include "platform/syslinux_ntfs.h"
#include "platform/ntfs_boot.h"
#include "platform/winpe_prepare.h"
#include "platform/legacy_iso.h"
#include "winpe_setup.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QSocketNotifier>
#include <QScopedPointer>
#include <QTemporaryFile>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QRandomGenerator>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace
{
constexpr int sectorBytes = 512;
constexpr int sectorsPerTrack = 63;
constexpr int requestTimeoutMs = 120000;
constexpr int inputReadsPerEvent = 16;
constexpr int logCharacters = 256;

struct IsoView
{
    ISO_ANALYSIS_SOURCE source = {};
    ~IsoView() { CloseIsoAnalysisSource(&source); }
};

int checkFile(int fd, const rufus::PrepareImageRequest& request)
{
    struct stat file, path;
    if (fstat(fd, &file) != 0 || lstat(QFile::encodeName(request.path).constData(), &path) != 0)
        return errno;
    const bool ordinaryFile = S_ISREG(file.st_mode) && S_ISREG(path.st_mode);
    const bool ownedFile = file.st_uid == getuid() && file.st_nlink == 1;
    if (!ordinaryFile || !ownedFile)
        return ENOTSUP;
    const bool sameIdentity = quint64(file.st_dev) == request.device && quint64(file.st_ino) == request.inode;
    const bool samePath = path.st_dev == file.st_dev && path.st_ino == file.st_ino;
    const bool sameSize = file.st_size >= 0 && quint64(file.st_size) == request.size;
    if (!sameIdentity || !samePath || !sameSize)
        return ESTALE;
    return QThread::currentThread()->isInterruptionRequested() ? ECANCELED : 0;
}

// Native partition functions revalidate the target before destructive work.
int prepareFormatPartition(int fd, const rufus::PrepareImageRequest& request, GPT_IMAGE_RESULT& partition, quint32 diskSignature = 0)
{
    partition = {};
    const quint64 device = request.isDevice ? request.device : 0;
    if (request.sfd)
    {
        const SFD_IMAGE_OPTIONS layout = {request.size, request.logicalSectorSize, device, request.physicalSectorSize};
        SFD_IMAGE_RESULT result = {};
        const int error = PrepareSfdTarget(fd, &layout, &result);
        partition.offset = result.offset;
        partition.size = result.size;
        partition.started = result.started;
        partition.sync_error = result.sync_error;
        return error;
    }
    const quint64 bootBytes = request.usesUefiNtfs() ? quint64(RUFUS_UEFI_NTFS_BYTES) : 0;
    if (request.mbr || request.syslinux)
    {
        const MBR_IMAGE_OPTIONS layout = {request.size, request.logicalSectorSize, sectorsPerTrack,
            request.clusterSize, diskSignature, device, request.fileSystem,
            rufus::FormatMbrBootCode(request), bootBytes, request.persistenceSize, request.oldBiosFixes, request.windowsToGo, false, request.physicalSectorSize};
        MBR_IMAGE_RESULT result = {};
        const int error = PrepareMbrTarget(fd, &layout, &result);
        partition.offset = result.offset;
        partition.size = result.size;
        partition.uefi_ntfs_offset = result.uefi_ntfs_offset;
        partition.uefi_ntfs_size = result.uefi_ntfs_size;
        partition.persistence_offset = result.persistence_offset;
        partition.persistence_size = result.persistence_size;
        partition.started = result.started;
        partition.sync_error = result.sync_error;
        return error;
    }
    const GPT_IMAGE_OPTIONS layout = {request.size, request.logicalSectorSize, sectorsPerTrack,
        rufus::FormatLayoutClusterSize(request), device, bootBytes, request.persistenceSize, request.isoEsp, request.windowsToGo, request.physicalSectorSize};
    return CreateGptImagePartition(fd, &layout, &partition);
}

class PrepareWorker final : public rufus::RufusJob
{
    Q_OBJECT
public:
    explicit PrepareWorker(const rufus::PrepareImageRequest& request) : request_(request) {}
    bool answerEspRecord(bool accepted)
    {
        QMutexLocker lock(&espMutex_);
        if (!espWaiting_) return false;
        espAccepted_ = accepted; espWaiting_ = false; espCondition_.wakeAll(); return true;
    }
    bool answerBadBlocks(int round, int choice)
    {
        QMutexLocker lock(&badBlocksMutex_);
        if (!badBlocksWaiting_ || round != badBlocksRound_ || choice < rufus::BadBlocksAbort || choice > rufus::BadBlocksIgnore) return false;
        badBlocksChoice_ = choice; badBlocksWaiting_ = false; badBlocksCondition_.wakeAll(); return true;
    }

signals:
    void espRecord(QString guid, bool restore);
    void phase(QString phase);
    void result(rufus::PrepareImageOutcome outcome);
    void badBlocksError(quint64 block, int type, int round);
    void badBlocksReport(rufus::BadBlocksReport report);

protected:
    void execute() override
    {
        const rufus::BadBlocksCallbacks callbacks{this, nativeBadBlocksPhase, nativeBadBlocksError, nativeBadBlocksReview, nativeBadBlocksComplete};
        rufus::BadBlocksSession badBlocks(callbacks);
        rufus::PrepareImageOutcome outcome;
        emit phase(QStringLiteral("checking"));
        if (request_.deviceOperation != 0) {
            const uid_t caller = uid_t(qEnvironmentVariable("PKEXEC_UID").toUInt());
            rufus::RunDeviceOperation(request_, caller, saveEspRecord, this, outcome);
            emit result(outcome); return;
        }
        if (request_.extendedLabel)
        {
            QByteArray icon;
            outcome.error = rufus::ReadMediaIcon(icon);
            if (outcome.error != 0) { emit result(outcome); return; }
        }
        if (request_.rawWrite)
        {
            outcome.rawWrite = true;
            const uid_t caller = uid_t(qEnvironmentVariable("PKEXEC_UID").toUInt());
            outcome.error = rufus::WriteRawDevice(request_, caller, outcome.raw);
            outcome.syncError = outcome.raw.sync_error;
            outcome.started = outcome.raw.bytes_written != 0 || badBlocks.started();
            if (badBlocks.syncError() != 0) outcome.syncError = outcome.raw.sync_error = badBlocks.syncError();
            emit result(outcome);
            return;
        }
        outcome.error = rufus::CheckWindowsCustomizationSettings(request_);
        if (outcome.error != 0)
        {
            emit result(outcome);
            return;
        }
        // Retain selected bytes before opening or changing the target. The
        // writer uses this snapshot and never reopens a host cache/staging path.
        if (request_.msDos) {
            const uid_t caller = uid_t(qEnvironmentVariable("PKEXEC_UID").toUInt());
            QByteArray bytes;
            outcome.error = rufus::ReadMsDosImage(request_.dosImagePath,caller,bytes);
            if (outcome.error == 0) outcome.error = rufus::ReadMsDosFiles(bytes,selectedDosFiles_);
            if (outcome.error != 0) { emit result(outcome); return; }
        }
        if (request_.hasSyslinuxResources()) {
            const uid_t caller = uid_t(qEnvironmentVariable("PKEXEC_UID").toUInt());
            QByteArray bytes;
            outcome.error = rufus::LoadPrepareSyslinuxResources(request_,caller,bytes);
            if (!outcome.error) outcome.error = rufus::DecodeSyslinuxResources(bytes,selectedSyslinux_);
            if (outcome.error) { emit result(outcome); return; }
        }
        if (request_.hasGrub2Core())
        {
            GPT_IMAGE_RESULT partition = {};
            outcome.error = rufus::PlanFormatPartition(request_, partition);
            const uid_t caller = uid_t(qEnvironmentVariable("PKEXEC_UID").toUInt());
            if (outcome.error == 0 && request_.grub4dos)
            {
                outcome.error = rufus::LoadPrepareGrub4dosLoader(request_, caller, selectedLoader_);
                if (outcome.error == 0)
                    outcome.error = rufus::ReadEmbeddedGrub4dosSbr(partition.offset, selectedCore_);
                grub4dosSbr_ = true;
            }
            else if (outcome.error == 0)
                outcome.error = rufus::LoadPrepareGrub2Core(request_, caller, partition.offset, selectedCore_);
            if (outcome.error != 0)
            {
                emit result(outcome);
                return;
            }
        }
        if (request_.isDevice)
        {
            rufus::PrepareIsoSource source(request_);
            rufus::WindowsToGoImage windowsToGo;
            const rufus::PrepareImageRequest archiveRequest = rufus::PrepareArchiveRequest(request_.archive);
            rufus::PrepareIsoSource archive(archiveRequest);
            const auto table = request_.sfd ? rufus::PrepareDeviceTarget::Table::Sfd
                : (request_.syslinux || request_.mbr) ? rufus::PrepareDeviceTarget::Table::Mbr
                : rufus::PrepareDeviceTarget::Table::Gpt;
            rufus::PrepareDeviceTarget target(request_, table);
            const uid_t caller = uid_t(qEnvironmentVariable("PKEXEC_UID").toUInt());
            if (!request_.imagePath.isEmpty())
                outcome.error = source.open(caller);
            if (outcome.error == 0 && request_.windowsToGo)
                outcome.error = windowsToGo.open(request_, source);
            if (outcome.error == 0 && request_.archive.present()) {
                outcome.error = archive.open(caller);
                rufus::ZipExtractResult planned;
                GPT_IMAGE_RESULT archivePartition = {};
                if (outcome.error == 0) outcome.error = rufus::PlanFormatPartition(request_, archivePartition);
                const bool fat = request_.fileSystem == FS_FAT16 || request_.fileSystem == FS_FAT32;
                const quint64 maximumFile = fat ? qMin(archivePartition.size, quint64(UINT32_MAX)) : archivePartition.size;
                if (outcome.error == 0) outcome.error = rufus::CheckZipSupplement(archive.retainedSource(),
                    archiveRequest.imagePath, maximumFile, quint64(INT64_MAX), planned);
                if (outcome.error == 0) outcome.error = archive.check();
                if (outcome.error != 0) emit logMessage(QStringLiteral("Supplementary ZIP validation failed: %1").arg(planned.path));
            }
            const bool grub4dosImage = request_.grub4dos && !request_.imagePath.isEmpty();
            if (outcome.error == 0 && !request_.imagePath.isEmpty() && !request_.windowsToGo)
            {
                // Reject unsupported sources before target access or unmounts.
                outcome.error = rufus::CheckIsoWriteImage(request_, caller, checkedImage_, &selectedView_.source, true);
                if (outcome.error != 0)
                    emit logMessage(checkedImage_.problem);
            }
            if (outcome.error == 0 && (request_.windowsBios || request_.grub4dos || request_.syslinux))
            {
                grub4dosSbr_ = GetSecondaryBootType(BT_IMAGE, &checkedImage_.analysis.report, false) == BT_GRUB4DOS;
                if (grub4dosSbr_)
                {
                    GPT_IMAGE_RESULT partition = {};
                    outcome.error = rufus::PlanFormatPartition(request_, partition);
                    if (outcome.error == 0)
                        outcome.error = rufus::ReadEmbeddedGrub4dosSbr(partition.offset, selectedCore_);
                }
            }
            if (outcome.error == 0 && (request_.extendedLabel || request_.archive.present() || request_.windowsToGo))
                outcome.error = target.checkMountSupport();
            if (outcome.error == 0 && request_.persistenceSize != 0 && access(RUFUS_MKE2FS_PATH, X_OK) != 0)
                outcome.error = errno;
            if (outcome.error == 0)
                outcome.error = request_.archive.present()
                    ? target.open(caller, request_.imagePath.isEmpty() ? nullptr : &source, &archive)
                    : target.open(caller, request_.imagePath.isEmpty() ? nullptr : &source);
            if (outcome.error == 0)
                outcome.error = rufus::CreatePrivateMountNamespace();
            if (outcome.error == 0)
            {
                if (request_.windowsToGo)
                    prepareWindowsToGo(target, source, windowsToGo, outcome);
                else if (request_.fileSystem == FS_NTFS)
                    prepareNtfs(target, source, outcome);
                else if (request_.fileSystem == FS_EXFAT)
                    prepareExfat(target, outcome);
                else if (request_.fileSystem == FS_EXT2 || request_.fileSystem == FS_EXT3 || request_.fileSystem == FS_UDF)
                    prepareNativeFormat(target, outcome);
                else
                    prepare(target.descriptor(), outcome, &target, &source);
            }
            if (outcome.error == 0 && request_.archive.present()) {
                emit phase(QStringLiteral("archive"));
                outcome.error = archive.check();
                const bool mountAttempted = outcome.error == 0;
                if (mountAttempted) outcome.error = target.mount(outcome.offset, outcome.size);
                rufus::ZipExtractResult copied;
                const bool fat = request_.fileSystem == FS_FAT16 || request_.fileSystem == FS_FAT32;
                const quint64 maximumFile = fat ? qMin(outcome.size, quint64(UINT32_MAX)) : outcome.size;
                if (outcome.error == 0) outcome.error = rufus::ExtractZipSupplement(archive.retainedSource(),
                    archiveRequest.imagePath, target.mountDescriptor(), maximumFile, quint64(INT64_MAX), copied);
                outcome.archiveBytes = copied.bytes;
                if (copied.syncError != 0) outcome.syncError = copied.syncError;
                if (outcome.error == 0) outcome.error = archive.check();
                if (outcome.error != 0) emit logMessage(QStringLiteral("Supplementary ZIP extraction failed at %1.").arg(copied.path));
                emit phase(QStringLiteral("archive-finalizing"));
                int syncError = 0;
                if (mountAttempted) outcome.cleanupError = target.finishMount(syncError);
                if (outcome.syncError == 0) outcome.syncError = syncError;
                if (outcome.error == 0) outcome.error = outcome.syncError != 0 ? outcome.syncError : outcome.cleanupError;
                if (outcome.error == 0) outcome.error = target.check();
                outcome.archiveDone = outcome.error == 0;
            }
            const bool isoMbr = !request_.imagePath.isEmpty() && !request_.windowsToGo
                && (request_.mbr || request_.syslinux);
            if (outcome.error == 0 && isoMbr)
            {
                outcome.error = source.check();
                if (outcome.error == 0) finishIsoBoot(target, outcome);
                if (outcome.error == 0) outcome.error = source.check();
            }
            const int archiveClosed = archive.close();
            if (outcome.error == 0) outcome.error = archiveClosed;
            if (request_.windowsToGo)
            {
                if (outcome.error == 0) outcome.error = source.check();
                windowsToGo.close();
                const int sourceClosed = source.close();
                const int targetClosed = target.finish();
                if (outcome.cleanupError == 0) outcome.cleanupError = sourceClosed != 0 ? sourceClosed : targetClosed;
                if (outcome.error == 0) outcome.error = outcome.syncError != 0 ? outcome.syncError : outcome.cleanupError;
                outcome.windowsToGoDone = outcome.error == 0;
            }
        }
        else
        {
            const QByteArray path = QFile::encodeName(request_.path);
            const int fd = open(path.constData(), O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
            if (fd < 0)
                outcome.error = errno;
            else
            {
                outcome.error = checkFile(fd, request_);
                if (outcome.error == 0 && flock(fd, LOCK_EX | LOCK_NB) != 0)
                    outcome.error = errno;
                if (outcome.error == 0)
                    prepare(fd, outcome);
                if (close(fd) != 0 && outcome.error == 0)
                    outcome.error = errno;
            }
        }
        outcome.started = outcome.started || badBlocks.started();
        if (badBlocks.syncError() != 0) outcome.syncError = badBlocks.syncError();
        emit result(outcome);
    }

private:
    void preparePersistence(rufus::PrepareDeviceTarget& device, const GPT_IMAGE_RESULT& layout,
        rufus::PrepareImageOutcome& outcome)
    {
        if (request_.persistenceSize == 0) return;
        FORMAT_RESULT formatted = {};
        emit logMessage(QStringLiteral("Creating the Linux persistence volume."));
        outcome.error = device.formatPersistence(layout, checkedImage_.analysis.report.uses_casper,
            QStringLiteral(RUFUS_MKE2FS_PATH), formatted);
        outcome.syncError = formatted.sync_error;
    }

    int loadSecondaryBoot(quint64 partitionOffset, QByteArray& core)
    {
        if (selectedCore_.isEmpty())
            return rufus::ReadEmbeddedGrub2Core(partitionOffset, core);
        const quint64 prefixBytes = grub4dosSbr_ ? sectorBytes : 0;
        const bool hasPayload = quint64(selectedCore_.size()) > prefixBytes;
        if (!hasPayload)
            return EBADMSG;
        const quint64 payloadBytes = quint64(selectedCore_.size()) - prefixBytes;
        const bool hasGap = partitionOffset >= sectorBytes;
        const bool fitsGap = hasGap && payloadBytes <= partitionOffset - sectorBytes;
        if (!fitsGap)
            return ENOSPC;
        core = selectedCore_;
        return 0;
    }

    void prepareNativeFormat(rufus::PrepareDeviceTarget& device, rufus::PrepareImageOutcome& outcome)
    {
        GPT_IMAGE_RESULT partition = {};
        outcome.error = rufus::PlanFormatPartition(request_, partition);
        if (outcome.error != 0)
            return;
        const bool udf = request_.fileSystem == FS_UDF;
        const rufus::UdfFormatOptions udfFormat = {partition.size, request_.label, request_.udfRevision, request_.quickFormat, request_.logicalSectorSize, request_.physicalSectorSize};
        const rufus::ExtFormatOptions format = {partition.size, request_.fileSystem, request_.label, false, request_.quickFormat, request_.logicalSectorSize, request_.physicalSectorSize};
        outcome.error = rufus::CheckFormatSettings(request_, partition);
        if (outcome.error == 0 && udf) {
            QString problem;
            outcome.error = rufus::CheckUdfFormatTools(QStringLiteral(RUFUS_MKUDFFS_PATH), udfFormat, problem);
            if (outcome.error != 0) emit logMessage(problem);
        }
        if (outcome.error == 0 && !udf && access(RUFUS_MKE2FS_PATH, X_OK) != 0)
            outcome.error = errno;
        QByteArray grubCore;
        if (outcome.error == 0 && request_.grub2)
            outcome.error = loadSecondaryBoot(partition.offset, grubCore);
        if (outcome.error == 0)
            outcome.error = device.check();
        if (outcome.error != 0)
            return;
        outcome.error = rufus::RunPrepareBadBlocks(request_, device.descriptor());
        if (outcome.error == 0) outcome.error = device.check();
        if (outcome.error != 0) return;
        emit phase(QStringLiteral("partitioning"));
        const quint32 signature = QRandomGenerator::system()->generate();
        outcome.error = prepareFormatPartition(device.descriptor(), request_, partition, signature);
        outcome.started = partition.started;
        outcome.syncError = partition.sync_error;
        outcome.offset = partition.offset;
        outcome.size = partition.size;
        if (outcome.error != 0)
            return;
        emit phase(QStringLiteral("formatting"));
        FORMAT_RESULT formatted = {};
        outcome.error = udf ? device.formatUdf(partition, udfFormat, QStringLiteral(RUFUS_MKUDFFS_PATH), formatted)
            : device.formatExt(partition, format, QStringLiteral(RUFUS_MKE2FS_PATH), formatted);
        outcome.syncError = formatted.sync_error;
        outcome.filesystemBytes = formatted.filesystem_bytes;
        if (outcome.error == 0)
            outcome.error = device.check();
        if (outcome.error != 0)
            return;
        // Windows treats the root indexing hint as best effort. The native
        // UDF provider has no corresponding attribute; report that limitation
        // without fabricating metadata or changing the completed format.
        if (udf && !request_.fileIndexing) {
            emit logMessage(QStringLiteral("Disabling file indexing..."));
            emit logMessage(QStringLiteral("Could not disable UDF file content indexing: the Linux UDF provider does not expose the Windows NOT_CONTENT_INDEXED attribute."));
        }
        const MBR_IMAGE_OPTIONS mbr = {request_.size, request_.logicalSectorSize, sectorsPerTrack,
            request_.clusterSize, signature, request_.device, request_.fileSystem,
            rufus::FormatMbrBootCode(request_), 0, request_.persistenceSize, request_.oldBiosFixes, false, false, request_.physicalSectorSize};
        if (request_.grub2) installSecondaryBoot(device, mbr, grubCore, outcome);
        if (outcome.error == 0)
            finalizeDevice(device, outcome);
    }

    int checkSyslinux(rufus::IsoWriteCheck& checked, IsoView& view, quint64 offset)
    {
        const uid_t caller = uid_t(qEnvironmentVariable("PKEXEC_UID").toUInt());
        int error = request_.syslinuxSelection == BT_IMAGE
            ? rufus::CheckIsoWriteImage(request_, caller, checked, &view.source, true)
            : rufus::CheckNativeSyslinuxGeometry(request_.fileSystem, request_.logicalSectorSize, offset, checked.problem);
        if (error != 0)
        {
            emit logMessage(checked.problem);
            return error;
        }
        if (request_.syslinuxSelection == BT_IMAGE) {
            checkedImage_ = checked;
            windowsEfiRepair_ = checked.windowsEfiRepair;
            const auto report = rufus::SyslinuxSourceReport(checked.analysis.report);
            if (!request_.hasSyslinuxResources() && SL_MAJOR(report.sl_version) < 5) selectedSyslinux_ = checked.syslinuxResources;
            useSelectedSyslinux_ = request_.hasSyslinuxResources() || SL_MAJOR(report.sl_version) < 5;
            if (useSelectedSyslinux_) {
                error = rufus::ValidateSyslinuxResources(report,selectedSyslinux_);
                if (!error && selectedSyslinux_.module != checked.syslinuxResources.module) error = ESTALE;
                quint64 extra = quint64(selectedSyslinux_.loader.size()+selectedSyslinux_.oldModules[0].size()+selectedSyslinux_.oldModules[1].size());
                if (!error && (extra > checked.partition.size || checked.tree.required_bytes > checked.partition.size-extra)) error = ENOSPC;
            } else if (report.sl_version != rufus::nativeSyslinuxVersion || (!report.sl_version_inferred && strcmp(report.sl_version_ext,"/pre1") != 0)) error = ENOENT;
            if (error) return error;
        }
        const int provider = request_.syslinuxSelection == BT_IMAGE && SL_MAJOR(rufus::SyslinuxSourceReport(checked.analysis.report).sl_version) < 5 && request_.fileSystem != FS_NTFS ? BT_SYSLINUX_V4 : request_.syslinuxSelection;
        error = rufus::CheckNativeSyslinuxTools(checked.problem, provider, request_.fileSystem);
        if (error != 0)
            emit logMessage(checked.problem);
        return error;
    }

    void installSyslinux(rufus::PrepareDeviceTarget& device, rufus::PrepareIsoSource& source,
        const MBR_IMAGE_OPTIONS& layout, const FAT_FORMAT_OPTIONS& format,
        const rufus::IsoWriteCheck& checked, const IsoView& view, rufus::PrepareImageOutcome& outcome)
    {
        const bool standalone = request_.isStandaloneSyslinux();
        if (standalone)
            emit phase(QStringLiteral("bootloader"));
        emit logMessage(QStringLiteral("Installing Syslinux using the qualified native payload."));
        const QByteArray imagePath = QFile::encodeName(request_.imagePath);
        rufus::NativeSyslinuxRequest install;
        install.target = device.descriptor();
        install.format = format;
        install.fileSystem = request_.fileSystem;
        install.bootType = request_.syslinuxSelection;
        if (!standalone)
        {
            install.image = &source.retainedSource();
            install.imagePath = imagePath.constData();
            install.source = &view.source;
            install.analysis = &checked.analysis;
        }
        install.rockRidge = request_.rockRidge;
        const int provider = !standalone && SL_MAJOR(rufus::SyslinuxSourceReport(checked.analysis.report).sl_version) < 5 ? BT_SYSLINUX_V4 : request_.syslinuxSelection;
        install.installer = QString::fromLocal8Bit(rufus::NativeSyslinuxInstaller(provider));
        install.resources = !standalone && useSelectedSyslinux_ ? &selectedSyslinux_ : nullptr;
        install.toolsDirectory = QStringLiteral(RUFUS_MTOOLS_DIRECTORY);
        install.temporaryRoot = QStringLiteral("/run");
        install.deviceLayout = &layout;
        install.diskSequence = request_.diskSequence;
        FORMAT_RESULT installed;
        QString problem;
        outcome.error = rufus::InstallNativeSyslinux(install, installed, problem);
        outcome.syncError = installed.sync_error;
        if (!problem.isEmpty())
            emit logMessage(problem);
        if (outcome.error == 0)
            outcome.error = device.check();
        if (outcome.error == 0 && !standalone)
            outcome.error = source.check();
        if (outcome.error == 0 && standalone)
        {
            // The original InstallSyslinux advances twice, despite three
            // allocated copy slots. Publish only after native readback succeeds.
            UpdateProgress(OP_FILE_COPY, -1.0f);
            UpdateProgress(OP_FILE_COPY, -1.0f);
        }
    }

    int checkExtraction(rufus::PrepareDeviceTarget& device, rufus::PrepareIsoSource& source,
        quint64 partitionBytes, ISO_EXTRACT_OPTIONS& options)
    {
        if (request_.imagePath.isEmpty())
            return 0;
        options.windows_links = request_.fileSystem == FS_NTFS;
        ISO_EXTRACT_RESULT planned = {};
        const uint64_t fileLimit = request_.fileSystem == FS_NTFS ? 0 : UINT32_MAX;
        int error = 0;
        // Syslinux has already checked the same image in checkSyslinux. Every
        // other ISO route needs fresh policy, including plain UEFI repair.
        const bool needsImagePolicy = !request_.syslinux;
        if (needsImagePolicy)
        {
            // Use trusted image facts checked before any partition writes.
            // The client supplies choices, never XML or its destination path.
            rufus::IsoWriteCheck fresh;
            const uid_t caller = uid_t(qEnvironmentVariable("PKEXEC_UID").toUInt());
            const bool checkedBeforeTargetAccess = checkedImage_.analysis.tree_complete;
            if (!checkedBeforeTargetAccess)
                error = rufus::CheckIsoWriteImage(request_, caller, fresh);
            const rufus::IsoWriteCheck& checked = checkedBeforeTargetAccess ? checkedImage_ : fresh;
            if (error != 0)
                emit logMessage(checked.problem);
            planned = checked.tree;
            checkedImage_ = checked;
            device.requireWindowsLinks(request_.fileSystem == FS_NTFS && checked.analysis.report.has_symlinks == SYMLINKS_RR);
            options.split_install_wim = error == 0 && checked.splitInstallWim;
            // Match Rufus's highest reported EFI architecture selection.
            for (unsigned bits = checked.analysis.report.has_efi >> 1; bits > 1; bits >>= 1)
                ++windowsArch_;
            windowsBuild_ = checked.analysis.report.win_version.build;
            windowsSetupLanguage_ = checked.windowsInstall.uiLanguage;
            windowsEfiRepair_ = checked.windowsEfiRepair;
            if (!windowsEfiRepair_.isEmpty())
                windowsArch_ = ARCH_X86_64;
        }
        else
            error = CheckIsoTree(source.retainedPath(), &options, fileLimit, &planned);
        if (error == 0 && planned.required_bytes > partitionBytes)
            error = ENOSPC;
        if (error == 0)
            error = source.check();
        if (error == 0)
            error = device.checkMountSupport();
        return error;
    }

    int openBootPayload(QScopedPointer<QTemporaryFile>& payload)
    {
        if (!request_.usesUefiNtfs())
            return 0;
        // Materialize only the embedded Rufus resource, before partition writes.
        // The caller owns the temporary file through every early return.
        payload.reset(QTemporaryFile::createNativeFile(QStringLiteral(":/rufus/uefi/uefi-ntfs.img")));
        if (!payload || !payload->flush())
            return EIO;
        return payload->size() == RUFUS_UEFI_NTFS_BYTES ? 0 : EBADMSG;
    }

    void installUefiNtfs(rufus::PrepareDeviceTarget& device, QTemporaryFile& payload,
        rufus::PrepareImageOutcome& outcome, quint32 diskSignature = 0)
    {
        // Qt can link an unnamed temporary inode when fileName() is first used.
        // Capture the source identity afterward, including that ctime change.
        const QByteArray path = QFile::encodeName(payload.fileName());
        IMAGE_FILE_SOURCE source = {};
        source.fd = payload.handle(); // Borrowed; the QTemporaryFile closes it.
        if (fstat(source.fd, &source.info) != 0)
        {
            outcome.error = errno;
            return;
        }
        RAW_WRITE_RESULT written;
        if (request_.mbr || request_.syslinux)
        {
            const MBR_IMAGE_OPTIONS layout = {request_.size, request_.logicalSectorSize, sectorsPerTrack,
                request_.clusterSize, diskSignature, request_.device, request_.fileSystem,
                rufus::FormatMbrBootCode(request_), RUFUS_UEFI_NTFS_BYTES, request_.persistenceSize, request_.oldBiosFixes, request_.windowsToGo, false, request_.physicalSectorSize};
            outcome.error = WriteMbrUefiNtfsImage(&source, path.constData(), device.descriptor(), &layout, &written);
        }
        else
        {
            const GPT_IMAGE_OPTIONS layout = {request_.size, request_.logicalSectorSize, sectorsPerTrack,
                request_.clusterSize, request_.device, RUFUS_UEFI_NTFS_BYTES, request_.persistenceSize, false, false, request_.physicalSectorSize};
            outcome.error = WriteUefiNtfsImage(&source, path.constData(), device.descriptor(), &layout, &written);
        }
        outcome.syncError = written.sync_error;
        if (outcome.error == 0)
            outcome.error = device.check();
    }

    void prepareWindowsToGo(rufus::PrepareDeviceTarget& device, rufus::PrepareIsoSource& source,
        rufus::WindowsToGoImage& image, rufus::PrepareImageOutcome& outcome)
    {
        GPT_IMAGE_RESULT layout = {};
        outcome.error = rufus::PlanFormatPartition(request_, layout);
        const rufus::NtfsFormatOptions options = {layout.size, layout.offset, request_.clusterSize, request_.label, request_.quickFormat, request_.ntfsCompression, request_.fileIndexing, request_.logicalSectorSize, request_.physicalSectorSize};
        if (outcome.error == 0) outcome.error = rufus::CheckNtfsFormatOptions(options);
        if (outcome.error == 0 && image.plan.bytes > layout.size) outcome.error = ENOSPC;
        if (outcome.error == 0 && access(RUFUS_MKNTFS_PATH, X_OK) != 0) outcome.error = errno;
        if (outcome.error == 0 && !request_.quickFormat)
            outcome.error = rufus::CheckNtfsFullFormatTools(QStringLiteral(RUFUS_MKNTFS_PATH));
        if (outcome.error == 0 && !request_.mbr && access(RUFUS_MKFS_FAT_PATH, X_OK) != 0) outcome.error = errno;
        QScopedPointer<QTemporaryFile> payload;
        if (outcome.error == 0) outcome.error = openBootPayload(payload);
        if (outcome.error == 0) outcome.error = source.check();
        if (outcome.error == 0) outcome.error = device.check();
        if (outcome.error == 0) outcome.error = rufus::RunPrepareBadBlocks(request_, device.descriptor());
        if (outcome.error != 0) return;
        quint32 signature = request_.mbr ? QRandomGenerator::global()->generate() : 0;
        if (request_.mbr && signature == 0) signature = 1;
        emit phase(QStringLiteral("partitioning"));
        outcome.error = prepareFormatPartition(device.descriptor(), request_, layout, signature);
        outcome.started = layout.started;
        outcome.syncError = layout.sync_error;
        outcome.offset = layout.offset;
        outcome.size = layout.size;
        rufus::WindowsToGoBootOptions bootOptions;
        if (outcome.error == 0)
            outcome.error = rufus::ReadWindowsToGoBootIdentity(device.descriptor(), request_, signature, bootOptions.bcd);
        bootOptions.architecture = image.architecture;
        bootOptions.bios = image.bios;
        bootOptions.uefi = image.uefi;
        bootOptions.ca2023 = request_.windowsFlags & UNATTEND_USE_MS2023_BOOTLOADERS;
        if (outcome.error != 0) return;
        emit phase(QStringLiteral("formatting"));
        rufus::NtfsFormatResult formatted;
        outcome.error = device.formatNtfs(layout, options, QStringLiteral(RUFUS_MKNTFS_PATH), formatted);
        outcome.syncError = formatted.format.sync_error;
        outcome.filesystemBytes = formatted.format.filesystem_bytes;
        if (outcome.error == 0 && !request_.mbr)
        {
            FAT32_FORMAT_RESULT esp = {};
            outcome.error = device.formatWindowsEsp(layout, RUFUS_MKFS_FAT_PATH, esp);
            if (esp.sync_error != 0) outcome.syncError = esp.sync_error;
        }
        if (outcome.error != 0) return;
        emit phase(QStringLiteral("extracting"));
        rufus::WindowsApplyResult applied;
        outcome.error = device.applyWindowsImage(image.handle(), request_.windowsEdition, source, layout, applied);
        if (applied.syncError != 0) outcome.syncError = applied.syncError;
        outcome.copiedBytes = outcome.error == 0 ? applied.plan.bytes : applied.completedBytes;
        if (outcome.error != 0) return;
        emit phase(QStringLiteral("bootloader"));
        if (request_.mbr && request_.windowsToGoTarget == TT_BIOS)
        {
            rufus::NtfsBootResult record;
            outcome.error = device.installNtfsBoot(options, record);
            if (record.syncError != 0) outcome.syncError = record.syncError;
        }
        if (outcome.error == 0 && payload) installUefiNtfs(device, *payload, outcome, signature);
        QTemporaryDir stage(QStringLiteral("/tmp/rufus-togo-boot-XXXXXX"));
        if (outcome.error == 0 && !stage.isValid()) outcome.error = EIO;
        const int stageDirectory = outcome.error == 0 ? ::open(QFile::encodeName(stage.path()).constData(),
            O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC) : -1;
        if (outcome.error == 0 && stageDirectory < 0) outcome.error = errno;
        const bool mainAttempted = outcome.error == 0;
        if (mainAttempted) outcome.error = device.mount(layout.offset, layout.size);
        rufus::WindowsBootFilesResult files;
        if (outcome.error == 0)
            outcome.error = rufus::StageWindowsToGoBootFiles(device.mountDescriptor(), stageDirectory, bootOptions, files);
        if (outcome.error != 0 && !files.path.isEmpty())
            emit logMessage(QStringLiteral("Windows boot-file staging failed at %1.").arg(files.path));
        if (outcome.error == 0 && request_.mbr)
            outcome.error = rufus::InstallWindowsToGoBootFiles(stageDirectory, device.mountDescriptor(), files);
        if (outcome.error == 0 && request_.mbr)
            outcome.error = rufus::SetWindowsToGoBootAttributes(device.mountDescriptor(), image.bios, image.uefi);
        if (outcome.error == 0 && request_.windowsFlags != 0)
        {
            emit phase(QStringLiteral("customizing"));
            outcome.error = rufus::ApplyWindowsToGoCustomization(device.mountPath(), image.architecture,
                request_.windowsFlags, request_.windowsUsername);
        }
        if (outcome.error == 0) outcome.error = source.check();
        if (mainAttempted) finishCopy(device, outcome);
        if (outcome.error == 0 && !request_.mbr)
        {
            outcome.error = device.mountWindowsEsp(layout);
            if (outcome.error == 0)
                outcome.error = rufus::InstallWindowsToGoBootFiles(stageDirectory, device.mountDescriptor(), files);
            if (outcome.error == 0)
                outcome.error = rufus::SetWindowsToGoBootAttributes(device.mountDescriptor(), image.bios, image.uefi);
            finishCopy(device, outcome, false, false);
        }
        if (stageDirectory >= 0 && ::close(stageDirectory) != 0)
        {
            if (outcome.cleanupError == 0) outcome.cleanupError = errno;
            if (outcome.error == 0) outcome.error = outcome.cleanupError;
        }
        if (stage.isValid() && !stage.remove())
        {
            if (outcome.cleanupError == 0) outcome.cleanupError = EIO;
            if (outcome.error == 0) outcome.error = outcome.cleanupError;
        }
        if (outcome.error == 0) outcome.error = source.check();
    }

    void prepareNtfs(rufus::PrepareDeviceTarget& device, rufus::PrepareIsoSource& image,
        rufus::PrepareImageOutcome& outcome)
    {
        QScopedPointer<QTemporaryFile> payload;
        outcome.error = openBootPayload(payload);
        if (outcome.error != 0)
            return;
        GPT_IMAGE_RESULT partition = {};
        outcome.error = rufus::PlanFormatPartition(request_, partition);
        if (outcome.error != 0)
            return;
        QByteArray grubCore;
        // Windows preflight has independently checked the SBR selection before
        // target access. Reuse the same retained payload and bounded writer.
        const bool hasSbr = request_.grub2 || request_.hasGrub2Core() || grub4dosSbr_;
        if (hasSbr)
        {
            outcome.error = loadSecondaryBoot(partition.offset, grubCore);
            if (outcome.error != 0)
                return;
        }
        const rufus::NtfsFormatOptions options = {partition.size, partition.offset, request_.clusterSize, request_.label, request_.quickFormat, request_.ntfsCompression, request_.fileIndexing, request_.logicalSectorSize, request_.physicalSectorSize};
        const QByteArray label = request_.label.toUtf8();
        const QByteArray imagePath = QFile::encodeName(request_.imagePath);
        ISO_EXTRACT_OPTIONS extraction = {{request_.rockRidge, request_.joliet, {0, 0}},
            label.constData(), imagePath.constData(), false, request_.persistenceSize, nullptr, request_.preserveTimestamps, request_.fileSystem == FS_NTFS && request_.ntfsCompression && request_.clusterSize <= rufus::NtfsCompressionClusterBytes};
        rufus::IsoWriteCheck checked;
        IsoView view;
        if (request_.syslinux) {
            extraction.analysis.embedded_sl_version[0] = 0x407;
            extraction.analysis.embedded_sl_version[1] = rufus::nativeSyslinuxVersion;
        }
        outcome.error = rufus::CheckNtfsFormatOptions(options);
        if (outcome.error == 0 && !request_.quickFormat)
            outcome.error = rufus::CheckNtfsFullFormatTools(QStringLiteral(RUFUS_MKNTFS_PATH));
        if (outcome.error == 0 && access(RUFUS_MKNTFS_PATH, X_OK) != 0)
            outcome.error = errno;
        if (outcome.error == 0 && request_.syslinux)
        {
            outcome.error = checkSyslinux(checked, view, partition.offset);
            device.requireWindowsLinks(checked.analysis.report.has_symlinks == SYMLINKS_RR);
            if (outcome.error == 0)
                outcome.error = device.checkMountSupport();
        }
        else if (outcome.error == 0 && !request_.imagePath.isEmpty())
            outcome.error = checkExtraction(device, image, partition.size, extraction);
        else if (outcome.error == 0 && request_.grub4dos)
            outcome.error = device.checkMountSupport();
        if (outcome.error == 0)
            outcome.error = device.check();
        if (outcome.error == 0 && !request_.imagePath.isEmpty())
            outcome.error = image.check();
        if (outcome.error != 0)
            return;
        outcome.error = rufus::RunPrepareBadBlocks(request_, device.descriptor());
        if (outcome.error == 0) outcome.error = device.check();
        if (outcome.error != 0) return;
        emit phase(QStringLiteral("partitioning"));
        const quint32 signature = request_.syslinux || request_.grub2 || request_.grub4dos || request_.windowsBios || request_.kolibriOs
            ? QRandomGenerator::global()->generate() : 0;
        windowsDiskSignature_ = signature;
        outcome.error = prepareFormatPartition(device.descriptor(), request_, partition, signature);
        outcome.started = partition.started;
        outcome.syncError = partition.sync_error;
        outcome.offset = partition.offset;
        outcome.size = partition.size;
        if (outcome.error != 0)
            return;
        emit phase(QStringLiteral("formatting"));
        preparePersistence(device, partition, outcome);
        if (outcome.error != 0) return;
        rufus::NtfsFormatResult formatted;
        outcome.error = device.formatNtfs(partition, options, QStringLiteral(RUFUS_MKNTFS_PATH), formatted);
        outcome.syncError = formatted.format.sync_error;
        outcome.filesystemBytes = formatted.format.filesystem_bytes;
        if (outcome.error == 0)
            outcome.error = device.check();
        if (outcome.error != 0)
            return;
        if (request_.nonBootable)
        {
            finalizeDevice(device, outcome);
            return;
        }
        if (request_.syslinux)
        {
            if (hasSbr)
            {
                const quint64 bootBytes = request_.usesUefiNtfs() ? quint64(RUFUS_UEFI_NTFS_BYTES) : 0;
                const MBR_IMAGE_OPTIONS mbr = {request_.size, request_.logicalSectorSize, sectorsPerTrack,
                    request_.clusterSize, signature, request_.device, request_.fileSystem,
                    rufus::FormatMbrBootCode(request_), bootBytes, request_.persistenceSize, request_.oldBiosFixes, false, false, request_.physicalSectorSize};
                installSecondaryBoot(device, mbr, grubCore, outcome);
            }
            if (outcome.error != 0)
                return;
            if (request_.usesUefiNtfs())
            {
                emit phase(QStringLiteral("bootloader"));
                installUefiNtfs(device, *payload, outcome, signature);
            }
            if (outcome.error != 0)
                return;
            installSyslinuxNtfs(device, image, options, formatted.volumeId, extraction, checked, view, outcome);
            return;
        }
        if (request_.grub2 || request_.grub4dos || request_.windowsBios || request_.kolibriOs || request_.reactOs)
        {
            if (hasSbr)
            {
                const quint64 bootBytes = request_.usesUefiNtfs() ? quint64(RUFUS_UEFI_NTFS_BYTES) : 0;
                const MBR_IMAGE_OPTIONS mbr = {request_.size, request_.logicalSectorSize, sectorsPerTrack,
                    request_.clusterSize, signature, request_.device, request_.fileSystem,
                    rufus::FormatMbrBootCode(request_), bootBytes, request_.persistenceSize, request_.oldBiosFixes, false, false, request_.physicalSectorSize};
                installSecondaryBoot(device, mbr, grubCore, outcome);
            }
            else
                emit phase(QStringLiteral("bootloader"));
            if (outcome.error == 0)
            {
                rufus::NtfsBootResult boot;
                outcome.error = device.installNtfsBoot(options, boot);
                outcome.syncError = boot.syncError;
            }
            if (outcome.error == 0 && payload)
                installUefiNtfs(device, *payload, outcome, signature);
            if (outcome.error == 0 && request_.grub4dos && request_.isSourceFree())
                copyGrub4dos(device, partition, outcome);
            else if (outcome.error == 0 && !request_.imagePath.isEmpty())
                extract(device, image, partition, extraction, outcome);
            else if (outcome.error == 0)
                finalizeDevice(device, outcome);
            return;
        }
        emit phase(QStringLiteral("bootloader"));
        installUefiNtfs(device, *payload, outcome);
        if (outcome.error == 0 && !request_.imagePath.isEmpty())
            extract(device, image, partition, extraction, outcome);
        else if (outcome.error == 0)
            finalizeDevice(device, outcome);
    }

    void installSyslinuxNtfs(rufus::PrepareDeviceTarget& device, rufus::PrepareIsoSource& image,
        const rufus::NtfsFormatOptions& format, quint64 volumeId, const ISO_EXTRACT_OPTIONS& extraction,
        const rufus::IsoWriteCheck& checked, const IsoView& view, rufus::PrepareImageOutcome& outcome)
    {
        const bool standalone = request_.isStandaloneSyslinux();
        emit phase(QStringLiteral("mounting"));
        outcome.error = device.mount(format.partitionOffset, format.size);
        if (outcome.error == 0 && !standalone)
            copyImage(device, image, extraction, outcome);
        struct stat partition;
        if (outcome.error == 0 && fstat(device.partitionDescriptor(), &partition) != 0)
            outcome.error = errno;
        if (outcome.error == 0)
        {
            if (standalone)
                emit phase(QStringLiteral("bootloader"));
            emit logMessage(QStringLiteral("Installing Syslinux using the qualified native NTFS payload."));
            rufus::NativeSyslinuxNtfsRequest install;
            install.partition = device.partitionDescriptor();
            install.directory = device.mountDescriptor();
            install.device = partition.st_rdev;
            install.diskSequence = request_.diskSequence;
            install.format = format;
            install.volumeId = volumeId;
            install.bootType = request_.syslinuxSelection;
            const QByteArray imagePath = QFile::encodeName(request_.imagePath);
            if (!standalone)
            {
                install.image = &image.retainedSource();
                install.imagePath = imagePath.constData();
                install.source = &view.source;
                install.analysis = &checked.analysis;
            }
            install.rockRidge = request_.rockRidge;
            install.resources = !standalone && useSelectedSyslinux_ ? &selectedSyslinux_ : nullptr;
            install.mountPath = QString::fromLocal8Bit(device.mountPath());
            install.installer = QString::fromLocal8Bit(rufus::NativeSyslinuxInstaller(request_.syslinuxSelection, FS_NTFS));
            FORMAT_RESULT installed;
            QString problem;
            outcome.error = rufus::InstallNativeSyslinuxNtfs(install, installed, problem);
            outcome.syncError = installed.sync_error;
            if (!problem.isEmpty())
                emit logMessage(problem);
            if (outcome.error == 0 && standalone)
            {
                // Match the same two original copy events as the FAT route.
                UpdateProgress(OP_FILE_COPY, -1.0f);
                UpdateProgress(OP_FILE_COPY, -1.0f);
            }
        }
        finishCopy(device, outcome);
        if (outcome.error == 0 && !standalone)
            outcome.error = image.check();
    }

    void prepareExfat(rufus::PrepareDeviceTarget& device, rufus::PrepareImageOutcome& outcome)
    {
        QScopedPointer<QTemporaryFile> payload;
        outcome.error = openBootPayload(payload);
        if (outcome.error != 0)
            return;
        GPT_IMAGE_RESULT partition = {};
        outcome.error = rufus::PlanFormatPartition(request_, partition);
        if (outcome.error != 0)
            return;
        const rufus::ExfatFormatOptions options = {partition.size, request_.clusterSize,
            request_.label, partition.offset, request_.quickFormat, request_.logicalSectorSize, request_.physicalSectorSize};
        const QString mkfs = QStringLiteral(RUFUS_MKFS_EXFAT_PATH);
        QString problem;
        outcome.error = rufus::CheckExfatFormatOptions(options);
        if (outcome.error == 0)
            outcome.error = rufus::CheckExfatFormatTools(mkfs, problem);
        if (!problem.isEmpty())
            emit logMessage(problem);
        if (outcome.error == 0)
            outcome.error = device.check();
        if (outcome.error != 0)
            return;
        outcome.error = rufus::RunPrepareBadBlocks(request_, device.descriptor());
        if (outcome.error == 0) outcome.error = device.check();
        if (outcome.error != 0) return;
        emit phase(QStringLiteral("partitioning"));
        outcome.error = prepareFormatPartition(device.descriptor(), request_, partition);
        outcome.started = partition.started;
        outcome.syncError = partition.sync_error;
        outcome.offset = partition.offset;
        outcome.size = partition.size;
        if (outcome.error != 0)
            return;
        emit phase(QStringLiteral("formatting"));
        FORMAT_RESULT formatted = {};
        outcome.error = device.formatExfat(options, mkfs, formatted);
        outcome.syncError = formatted.sync_error;
        outcome.filesystemBytes = formatted.filesystem_bytes;
        if (outcome.error != 0)
            return;
        if (payload)
        {
            emit phase(QStringLiteral("bootloader"));
            installUefiNtfs(device, *payload, outcome);
        }
        if (outcome.error == 0)
            finalizeDevice(device, outcome);
    }

    void finalizeDevice(rufus::PrepareDeviceTarget& device, rufus::PrepareImageOutcome& outcome)
    {
        if (request_.extendedLabel)
        {
            emit phase(QStringLiteral("mounting"));
            outcome.error = device.mount(outcome.offset, outcome.size);
            finishCopy(device, outcome);
            return;
        }
        emit phase(QStringLiteral("finalizing"));
        // The formatter and optional boot writer synced their data. Flush the whole disk
        // too before publishing success; cancellation must still wait for sync.
        while (fsync(device.descriptor()) != 0)
        {
            if (errno == EINTR)
                continue;
            outcome.syncError = errno;
            outcome.error = errno;
            return;
        }
        outcome.error = device.check();
    }

    void prepare(int fd, rufus::PrepareImageOutcome& outcome, rufus::PrepareDeviceTarget* device = nullptr,
        rufus::PrepareIsoSource* source = nullptr)
    {
        GPT_IMAGE_RESULT partition = {};
        FAT_FORMAT_OPTIONS format = {};
        FORMAT_RESULT formatted = {};
        const bool fat16 = request_.fileSystem == FS_FAT16;
        const QByteArray label = request_.label.toUtf8();
        const QByteArray imagePath = QFile::encodeName(request_.imagePath);
        ISO_EXTRACT_OPTIONS options = {{request_.rockRidge, request_.joliet, {0, 0}},
            label.constData(), imagePath.constData(), false, request_.persistenceSize, nullptr, request_.preserveTimestamps, request_.fileSystem == FS_NTFS && request_.ntfsCompression && request_.clusterSize <= rufus::NtfsCompressionClusterBytes};
        rufus::IsoWriteCheck checked;
        IsoView view;
        // Check all operations before partition writes, using the adapters' rules.
        if (label.size() > FAT32_LABEL_LENGTH)
        {
            outcome.error = EINVAL;
            return;
        }
        const bool biosBoot = request_.syslinux || request_.isDos() || request_.reactOs || request_.grub2 || request_.grub4dos || request_.windowsBios || request_.kolibriOs;
        const quint32 diskSignature = biosBoot ? QRandomGenerator::system()->generate() : 0;
        windowsDiskSignature_ = diskSignature;
        const MBR_IMAGE_OPTIONS mbr = {request_.size, request_.logicalSectorSize, sectorsPerTrack, request_.clusterSize,
            diskSignature, request_.device, request_.fileSystem, rufus::FormatMbrBootCode(request_), 0, request_.persistenceSize, request_.oldBiosFixes, false, false, request_.physicalSectorSize};
        if (request_.syslinux) {
            options.analysis.embedded_sl_version[0] = 0x407;
            options.analysis.embedded_sl_version[1] = rufus::nativeSyslinuxVersion;
        }
        outcome.error = rufus::PlanFormatPartition(request_, partition);
        if (outcome.error != 0)
            return;
        format.offset = partition.offset;
        format.size = partition.size;
        format.cluster_size = rufus::FormatFatClusterSize(request_, partition);
        format.volume_id = request_.volumeId;
        format.full_format = !request_.quickFormat;
        format.sector_size = request_.logicalSectorSize;
        format.physical_sector_size = request_.physicalSectorSize;
        memcpy(format.label, label.constData(), size_t(label.size()));
        if (device != nullptr)
            outcome.error = fat16 ? CheckFat16FormatOptions(&format) : CheckFat32FormatOptions(&format);
        else if (fat16)
            outcome.error = CheckFat16ImageRequest(fd, RUFUS_MKFS_FAT_PATH, &format);
        else
            outcome.error = CheckFat32ImageRequest(fd, RUFUS_MKFS_FAT_PATH, &format);
        if (outcome.error == 0 && access(RUFUS_MKFS_FAT_PATH, X_OK) != 0)
            outcome.error = errno;
        if (outcome.error == 0 && (request_.isDos() || request_.grub4dos))
            outcome.error = device->checkMountSupport();
        if (outcome.error != 0)
            return;
        QByteArray grubCore;
        const bool hasSbr = request_.grub2 || request_.hasGrub2Core() || grub4dosSbr_;
        if (hasSbr)
            outcome.error = loadSecondaryBoot(partition.offset, grubCore);
        if (outcome.error != 0)
            return;
        if (request_.syslinux)
            outcome.error = checkSyslinux(checked, view, partition.offset);
        else if (!request_.imagePath.isEmpty())
            outcome.error = checkExtraction(*device, *source, partition.size, options);
        if (outcome.error != 0)
            return;
        outcome.error = device != nullptr ? device->check() : checkFile(fd, request_);
        if (outcome.error == 0 && !request_.imagePath.isEmpty())
            outcome.error = source->check();
        if (outcome.error != 0)
            return;
        if (device != nullptr) outcome.error = rufus::RunPrepareBadBlocks(request_, device->descriptor());
        if (outcome.error == 0 && source != nullptr && !request_.imagePath.isEmpty()) outcome.error = source->check();
        if (outcome.error != 0) return;
        emit phase(QStringLiteral("partitioning"));
        outcome.error = prepareFormatPartition(fd, request_, partition, diskSignature);
        outcome.started = partition.started;
        outcome.syncError = partition.sync_error;
        outcome.offset = partition.offset;
        outcome.size = partition.size;
        if (outcome.error == 0)
            outcome.error = device != nullptr ? device->check() : checkFile(fd, request_);
        if (outcome.error != 0)
            return;
        emit phase(QStringLiteral("formatting"));
        if (device != nullptr) preparePersistence(*device, partition, outcome);
        if (outcome.error != 0) return;
        if (device != nullptr)
            outcome.error = device->format(format, RUFUS_MKFS_FAT_PATH, formatted);
        else if (fat16)
            outcome.error = FormatFat16Image(fd, RUFUS_MKFS_FAT_PATH, &format, &formatted);
        else
            outcome.error = FormatFat32Image(fd, RUFUS_MKFS_FAT_PATH, &format, &formatted);
        outcome.syncError = formatted.sync_error;
        outcome.filesystemBytes = formatted.filesystem_bytes;
        if (outcome.error == 0)
            outcome.error = device != nullptr ? device->check() : checkFile(fd, request_);
        if (outcome.error == 0 && (request_.grub2 || request_.grub4dos || request_.windowsBios || request_.kolibriOs || request_.reactOs))
            installFatBoot(*device, mbr, format, grubCore, outcome);
        if (outcome.error == 0 && request_.syslinux && hasSbr)
            installSecondaryBoot(*device, mbr, grubCore, outcome);
        if (outcome.error == 0 && !request_.imagePath.isEmpty())
            extract(*device, *source, partition, options, outcome);
        if (outcome.error == 0 && request_.syslinux)
            installSyslinux(*device, *source, mbr, format, checked, view, outcome);
        if (outcome.error == 0 && request_.isDos())
            installFreeDos(*device, mbr, format, outcome);
        if (outcome.error == 0 && request_.grub4dos && request_.isSourceFree())
            copyGrub4dos(*device, partition, outcome);
        const bool standaloneSyslinux = request_.isStandaloneSyslinux();
        const bool formatOnly = request_.imagePath.isEmpty() && (request_.nonBootable || request_.grub2 || request_.reactOs || standaloneSyslinux);
        if (outcome.error == 0 && formatOnly)
            finalizeDevice(*device, outcome);
    }

    void installFatBoot(rufus::PrepareDeviceTarget& device, const MBR_IMAGE_OPTIONS& mbr,
        const FAT_FORMAT_OPTIONS& format, const QByteArray& core, rufus::PrepareImageOutcome& outcome)
    {
        if (request_.grub2 || request_.hasGrub2Core() || grub4dosSbr_)
            installSecondaryBoot(device, mbr, core, outcome);
        else
            emit phase(QStringLiteral("bootloader"));
        MBR_IMAGE_RESULT layout = {};
        if (outcome.error == 0)
            outcome.error = CheckMbrImage(device.descriptor(), &mbr, &layout);
        if (outcome.error != 0)
            return;
        // Keep the normal bootloader phase and verify the partition layout.
        // Legacy FAT32 records need final allocation hints after copy/unmount.
        if (checkedImage_.legacyWinPe && request_.fileSystem == FS_FAT32)
        {
            outcome.error = device.check();
            return;
        }
        FAT_BOOT_RESULT boot = {};
        const auto& report = checkedImage_.analysis.report;
        const FAT_BOOT_CODE bootCode = request_.reactOs ? FAT_BOOT_REACTOS : HAS_KOLIBRIOS(report) ? FAT_BOOT_KOLIBRI
            : HAS_BOOTMGR(report) ? FAT_BOOT_WINDOWS : HAS_WINPE(report) ? FAT_BOOT_WINPE : FAT_BOOT_STANDARD;
        outcome.error = InstallFatDeviceBootRecords(device.descriptor(), request_.device,
            request_.size, request_.fileSystem, bootCode, &format, &boot);
        outcome.syncError = boot.sync_error;
        if (outcome.error == 0)
            outcome.error = device.check();
        if (outcome.error == 0)
            outcome.error = CheckMbrImage(device.descriptor(), &mbr, &layout);
        // Standalone GRUB copies no modules/config/OS. Image jobs extract those
        // through the ordinary image path after the boot records are installed.
    }

    void installSecondaryBoot(rufus::PrepareDeviceTarget& device, const MBR_IMAGE_OPTIONS& mbr,
        const QByteArray& core, rufus::PrepareImageOutcome& outcome)
    {
        if (!request_.syslinux)
            emit phase(QStringLiteral("bootloader"));
        emit logMessage(grub4dosSbr_ ? QStringLiteral("Installing GRUB4DOS boot records.")
            : QStringLiteral("Installing GRUB2 boot records."));
        MBR_CORE_RESULT written = {};
        const unsigned char* bytes = reinterpret_cast<const unsigned char*>(core.constData());
        outcome.error = grub4dosSbr_ ? WriteGrub4dosSbrImage(device.descriptor(), &mbr, bytes, quint64(core.size()), &written)
            : WriteGrub2CoreImage(device.descriptor(), &mbr, bytes, quint64(core.size()), &written);
        outcome.syncError = written.sync_error;
        if (outcome.error == 0)
            outcome.error = device.check();
    }

    void installFreeDos(rufus::PrepareDeviceTarget& device, const MBR_IMAGE_OPTIONS& mbr,
        const FAT_FORMAT_OPTIONS& format, rufus::PrepareImageOutcome& outcome)
    {
        emit phase(QStringLiteral("bootloader"));
        MBR_IMAGE_RESULT layout = {};
        outcome.error = CheckMbrImage(device.descriptor(), &mbr, &layout);
        FAT_BOOT_RESULT boot = {};
        if (outcome.error == 0)
            outcome.error = InstallFatDeviceBootRecords(device.descriptor(), request_.device,
                request_.size, request_.fileSystem, request_.msDos ? FAT_BOOT_STANDARD : FAT_BOOT_FREEDOS, &format, &boot);
        outcome.syncError = boot.sync_error;
        if (outcome.error == 0)
            outcome.error = device.check();
        if (outcome.error != 0)
            return;
        emit phase(QStringLiteral("mounting"));
        outcome.error = device.mount(format.offset, format.size);
        if (outcome.error == 0)
        {
            emit phase(QStringLiteral("extracting"));
            // Windows advances once before ExtractDOS, then three/five times
            // while copying the MS-DOS/FreeDOS payload. Startup files add no progress slot.
            UpdateProgress(OP_FILE_COPY, -1.0f);
            rufus::FreeDosCopyResult copied;
            outcome.error = request_.msDos ? rufus::CopyMsDosFiles(device.mountDescriptor(),selectedDosFiles_,copied)
                : rufus::CopyFreeDosFiles(device.mountDescriptor(), copied);
            outcome.copiedBytes = copied.bytesWritten;
            outcome.syncError = copied.syncError;
            if (outcome.error == 0)
            {
                const QByteArray keyboard = request_.dosKeyboard.toLatin1();
                outcome.error = rufus::WriteDosStartupFiles(device.mountDescriptor(),
                    keyboard.constData(), request_.dosCodepage, !request_.msDos, copied);
                outcome.copiedBytes += copied.bytesWritten;
                outcome.syncError = copied.syncError;
            }
            if (outcome.error != 0 && !copied.path.isEmpty())
                emit logMessage(QStringLiteral("DOS copying stopped at %1").arg(copied.path));
        }
        finishCopy(device, outcome);
    }

    void copyGrub4dos(rufus::PrepareDeviceTarget& device, const GPT_IMAGE_RESULT& partition,
        rufus::PrepareImageOutcome& outcome)
    {
        emit phase(QStringLiteral("mounting"));
        outcome.error = device.mount(partition.offset, partition.size);
        if (outcome.error == 0)
        {
            emit phase(QStringLiteral("extracting"));
            rufus::FreeDosCopyResult copied;
            outcome.error = rufus::CopyGrub4dosLoader(device.mountDescriptor(), selectedLoader_, copied,
                request_.fileSystem == FS_NTFS && request_.ntfsCompression && request_.clusterSize <= rufus::NtfsCompressionClusterBytes);
            outcome.copiedBytes = copied.bytesWritten;
            outcome.syncError = copied.syncError;
            if (outcome.error != 0)
                emit logMessage(QStringLiteral("GRUB4DOS loader copying failed."));
        }
        finishCopy(device, outcome);
    }

    void extract(rufus::PrepareDeviceTarget& device, rufus::PrepareIsoSource& source,
        const GPT_IMAGE_RESULT& partition, const ISO_EXTRACT_OPTIONS& options, rufus::PrepareImageOutcome& outcome)
    {
        outcome.error = source.check();
        if (outcome.error != 0)
            return;
        emit phase(QStringLiteral("mounting"));
        outcome.error = device.mount(partition.offset, partition.size);
        if (outcome.error == 0)
            copyImage(device, source, options, outcome);
        finishCopy(device, outcome);
    }

    void copyImage(rufus::PrepareDeviceTarget& device, rufus::PrepareIsoSource& source,
        const ISO_EXTRACT_OPTIONS& options, rufus::PrepareImageOutcome& outcome)
    {
        emit phase(QStringLiteral("extracting"));
        outcome.error = source.check();
        const ISO_EXTRACT_EVENTS events = {this, request_.runtimeValidation, beginIntegrity,
            copiedIntegrity, modifiedIntegrity, splitIntegrity, relocatedIntegrity, linkedIntegrity};
        ISO_EXTRACT_OPTIONS observed = options;
        observed.events = &events;
        observed.windows_links = request_.fileSystem == FS_NTFS;
        const ISO_BOOT_RESOURCES resources = {{reinterpret_cast<const uint8_t*>(selectedSyslinux_.oldModules[0].constData()),reinterpret_cast<const uint8_t*>(selectedSyslinux_.oldModules[1].constData())},
            {size_t(selectedSyslinux_.oldModules[0].size()),size_t(selectedSyslinux_.oldModules[1].size())}};
        observed.boot_resources = useSelectedSyslinux_ ? &resources : nullptr;
        QByteArray reactosMboot, reactosConfig;
        ISO_PREPARED_REACTOS preparedReactos = {};
        const auto& sourceReport = checkedImage_.analysis.report;
        if (outcome.error == 0 && request_.syslinux && HAS_REACTOS(sourceReport))
        {
            outcome.error = rufus::ReadReactOsIsoFiles(sourceReport, reactosMboot, reactosConfig);
            rufus::LegacyIsoResult installed;
            if (outcome.error == 0)
                outcome.error = rufus::InstallReactOsIsoFiles(sourceReport, device.mountDescriptor(), installed);
            if (installed.syncError != 0) outcome.syncError = installed.syncError;
            preparedReactos = {reinterpret_cast<const uint8_t*>(reactosMboot.constData()), size_t(reactosMboot.size()),
                reinterpret_cast<const uint8_t*>(reactosConfig.constData()), size_t(reactosConfig.size())};
            observed.prepared_reactos = &preparedReactos;
        }
        ISO_EXTRACT_RESULT copied = {};
        if (outcome.error == 0)
            outcome.error = ExtractIsoTree(source.retainedPath(), device.mountPath(), &observed, &copied);
        outcome.copiedBytes = copied.bytes_written;
        if (outcome.error != 0 && copied.path[0] != 0)
            emit logMessage(QStringLiteral("Extraction stopped at %1").arg(QString::fromUtf8(copied.path)));
        if (outcome.error == 0)
            outcome.error = source.check();
        if (outcome.error == 0 && HAS_KOLIBRIOS(sourceReport))
        {
            rufus::LegacyIsoResult installed;
            outcome.error = rufus::InstallKolibriIsoLoader(selectedView_.source, device.mountDescriptor(), installed);
            if (installed.syncError != 0) outcome.syncError = installed.syncError;
            if (outcome.error == 0) outcome.error = modifiedIntegrity(this, "MTLD_F32");
            if (outcome.error == 0) outcome.error = source.check();
        }
        if (outcome.error == 0 && checkedImage_.legacyWinPe)
        {
            emit logMessage(QStringLiteral("Setting up legacy WinPE boot files."));
            const RUFUS_IMG_REPORT& report = checkedImage_.analysis.report;
            outcome.error = rufus::PrepareWinPeRootFiles(device.mountDescriptor(), report.winpe,
                report.uses_minint, report.winpe_options_known);
            if (outcome.error == 0)
                outcome.error = source.check();
            else
                emit logMessage(QStringLiteral("Legacy WinPE preparation failed: %1 (%2).")
                    .arg(QString::fromLocal8Bit(strerror(outcome.error))).arg(outcome.error));
        }
        if (outcome.error == 0 && !windowsEfiRepair_.isEmpty())
        {
            emit logMessage(QStringLiteral("Setting up the Windows EFI fallback loader."));
            outcome.error = rufus::InstallWindowsEfiRepair(windowsEfiRepair_, device.mountPath());
            if (outcome.error == 0) outcome.error = modifiedIntegrity(this, "EFI/BOOT/BOOTX64.EFI");
        }
        if (outcome.error == 0 && request_.windowsFlags != 0)
        {
            emit phase(QStringLiteral("customizing"));
            QStringList modified;
            outcome.error = rufus::ApplyWindowsCustomization(device.mountPath(), windowsArch_, windowsBuild_,
                request_, windowsSetupLanguage_, &modified);
            for (const QString& path : modified) {
                if (outcome.error != 0) break;
                const QByteArray encoded = path.toUtf8();
                outcome.error = modifiedIntegrity(this, encoded.constData());
            }
        }
    }

    void finishCopy(rufus::PrepareDeviceTarget& device, rufus::PrepareImageOutcome& outcome, bool appearance = true, bool finalizingPhase = true)
    {
        if (finalizingPhase) emit phase(QStringLiteral("finalizing"));
        if (integrity_.active()) {
            if (outcome.error == 0) {
                rufus::MediaIntegrityResult integrity;
                outcome.error = integrity_.finish(integrity);
                if (integrity.syncError != 0) outcome.syncError = integrity.syncError;
                if (outcome.error == 0) emit logMessage(QStringLiteral("Updated %1 checksum entries; installed %2 runtime EFI validators.")
                    .arg(integrity.updated).arg(integrity.validators));
            } else {
                const int closed = integrity_.close();
                if (closed != 0 && outcome.cleanupError == 0) outcome.cleanupError = closed;
            }
        }
        if (outcome.error == 0 && request_.extendedLabel && appearance)
        {
            rufus::MediaAppearanceResult appearance;
            outcome.error = rufus::WriteMediaAppearance(device.mountDescriptor(), request_.displayLabel, appearance);
            if (appearance.syncError != 0) outcome.syncError = appearance.syncError;
            outcome.appearanceDone = outcome.error == 0;
            if (outcome.error == 0)
                emit logMessage(appearance.preserved ? QStringLiteral("Keeping the existing autorun.inf.")
                    : QStringLiteral("Created autorun.inf and the Rufus icon."));
        }
        int syncError = 0;
        outcome.cleanupError = device.finishMount(syncError);
        if (outcome.syncError == 0)
            outcome.syncError = syncError;
        if (outcome.error == 0)
            outcome.error = outcome.syncError != 0 ? outcome.syncError : outcome.cleanupError;
        if (outcome.error == 0)
            outcome.error = device.check();
    }

    void finishIsoBoot(rufus::PrepareDeviceTarget& device, rufus::PrepareImageOutcome& outcome)
    {
        const RUFUS_IMG_REPORT& report = checkedImage_.analysis.report;
        const WINPE_SETUP_PLAN plan = GetWinPeSetupPlan(report.winpe, report.uses_minint);
        const bool bios = request_.syslinux || request_.windowsBios || request_.grub2
            || request_.grub4dos || request_.kolibriOs;
        const bool masquerade = checkedImage_.legacyWinPe && plan.masquerade_disk;
        const RUFUS_BOOT_OPTIONS dispatch = {BT_IMAGE, request_.fileSystem, bios ? TT_BIOS : TT_UEFI,
            false, request_.enhancedInstall, request_.windowsRufusMbr, masquerade};
        const bool fat32 = checkedImage_.legacyWinPe && request_.fileSystem == FS_FAT32
            && GetBootInstallType(&report, &dispatch) == RUFUS_BOOT_INSTALL_PBR;
        MBR_BOOT_CODE finalCode;
        switch (GetMasterBootRecord(&report, &dispatch))
        {
        case RUFUS_MBR_ZERO: finalCode = MBR_BOOT_ZERO; break;
        case RUFUS_MBR_RUFUS: finalCode = MBR_BOOT_RUFUS; break;
        case RUFUS_MBR_WIN7: finalCode = MBR_BOOT_WIN7; break;
        case RUFUS_MBR_SYSLINUX: finalCode = MBR_BOOT_SYSLINUX; break;
        case RUFUS_MBR_GRUB2: finalCode = MBR_BOOT_GRUB2; break;
        case RUFUS_MBR_GRUB4DOS: finalCode = MBR_BOOT_GRUB4DOS; break;
        case RUFUS_MBR_REACTOS: finalCode = MBR_BOOT_REACTOS; break;
        case RUFUS_MBR_KOLIBRIOS: finalCode = MBR_BOOT_KOLIBRIOS; break;
        default: outcome.error = ENOTSUP; return;
        }
        const MBR_BOOT_CODE initialCode = rufus::FormatMbrBootCode(request_);
        const bool writeMbr = masquerade || finalCode != initialCode;
        if (!fat32 && !writeMbr)
            return;
        // Linux's DOS parser rejects 0x81. Keep 0x80 through partition refresh,
        // formatting and private mounting; install the final original MBR and
        // partition flag after successful copying and unmount, without a rescan.
        const quint64 bootBytes = request_.usesUefiNtfs() ? quint64(RUFUS_UEFI_NTFS_BYTES) : 0;
        MBR_IMAGE_OPTIONS options = {request_.size, request_.logicalSectorSize, sectorsPerTrack,
            request_.clusterSize, windowsDiskSignature_, request_.device, request_.fileSystem,
            initialCode, bootBytes, request_.persistenceSize, request_.oldBiosFixes, false, false, request_.physicalSectorSize};
        MBR_IMAGE_RESULT written = {};
        outcome.error = CheckMbrImage(device.descriptor(), &options, &written);
        if (outcome.error == 0 && fat32)
        {
            emit logMessage(QStringLiteral("Installing legacy WinPE FAT32 boot records."));
            FAT_FORMAT_OPTIONS format = {};
            format.offset = outcome.offset;
            format.size = outcome.size;
            format.cluster_size = request_.clusterSize;
            format.volume_id = request_.volumeId;
            format.full_format = !request_.quickFormat;
            format.sector_size = request_.logicalSectorSize;
            format.physical_sector_size = request_.physicalSectorSize;
            const QByteArray label = request_.label.toUtf8();
            memcpy(format.label, label.constData(), size_t(label.size())); // Preflight bounded the label.
            FAT_BOOT_RESULT boot = {};
            const FAT_BOOT_CODE code = HAS_BOOTMGR(report) ? FAT_BOOT_WINDOWS : FAT_BOOT_WINPE;
            outcome.error = HAS_KOLIBRIOS(report)
                ? InstallFatDeviceBootRecords(device.descriptor(), request_.device, request_.size, FS_FAT32,
                    FAT_BOOT_KOLIBRI, &format, &boot)
                : InstallLegacyFatDeviceBootRecords(device.descriptor(), request_.device,
                    request_.size, code, masquerade, &format, &boot);
            outcome.syncError = boot.sync_error;
        }
        if (outcome.error == 0 && writeMbr)
        {
            options.boot_code = finalCode;
            options.masquerade_disk = masquerade;
            outcome.error = CreateMbrImage(device.descriptor(), &options, &written);
            outcome.syncError = written.sync_error;
        }
        if (outcome.error == 0)
            outcome.error = device.check();
    }

    static void nativeBadBlocksComplete(void* context)
    {
        auto* worker = static_cast<PrepareWorker*>(context);
        if (worker->request_.rawWrite) emit worker->phase(QStringLiteral("writing"));
    }
    static void nativeBadBlocksPhase(void* context, int round)
    {
        auto* worker = static_cast<PrepareWorker*>(context);
        worker->badBlocksRound_ = round;
        emit worker->phase(QStringLiteral("badblocks"));
    }
    static void nativeBadBlocksError(void* context, uint64_t block, unsigned type)
    {
        auto* worker = static_cast<PrepareWorker*>(context);
        emit worker->badBlocksError(block, int(type), worker->badBlocksRound_);
    }
    static int nativeBadBlocksReview(void* context, const BADBLOCK_RESULT& result, int error)
    {
        auto* worker = static_cast<PrepareWorker*>(context);
        rufus::BadBlocksReport report;
        report.round = worker->badBlocksRound_; report.error = error;
        report.count = result.count; report.readErrors = result.read_errors; report.writeErrors = result.write_errors;
        report.corruptionErrors = result.corruption_errors; report.written = result.bytes_written;
        report.read = result.bytes_read; report.threshold = result.threshold_reached;
        QMutexLocker lock(&worker->badBlocksMutex_);
        const bool review = error == 0 && result.count != 0;
        worker->badBlocksWaiting_ = review;
        worker->badBlocksChoice_ = rufus::BadBlocksAbort;
        emit worker->badBlocksReport(report);
        while (worker->badBlocksWaiting_ && !QThread::currentThread()->isInterruptionRequested())
            worker->badBlocksCondition_.wait(&worker->badBlocksMutex_, 100);
        worker->badBlocksWaiting_ = false;
        return QThread::currentThread()->isInterruptionRequested() ? rufus::BadBlocksAbort : worker->badBlocksChoice_;
    }
    static int beginIntegrity(void* context, int root, unsigned kind)
    {
        auto* worker = static_cast<PrepareWorker*>(context);
        return kind == 0 && !worker->request_.runtimeValidation ? 0 : worker->integrity_.begin(root, kind, worker->request_.runtimeValidation);
    }
    static int copiedIntegrity(void* context, const char* path, uint64_t size, const uint8_t* digest)
    {
        auto* worker = static_cast<PrepareWorker*>(context);
        return worker->integrity_.active() ? worker->integrity_.copied(path, size, digest) : 0;
    }
    static int modifiedIntegrity(void* context, const char* path)
    {
        auto* worker = static_cast<PrepareWorker*>(context);
        return worker->integrity_.active() ? worker->integrity_.modified(path) : 0;
    }
    static int splitIntegrity(void* context, const char* original, int stage, const char* name)
    {
        auto* worker = static_cast<PrepareWorker*>(context);
        return worker->integrity_.active() ? worker->integrity_.splitPart(original, stage, name) : 0;
    }
    static int relocatedIntegrity(void* context, const char* original, const char* destination)
    {
        auto* worker = static_cast<PrepareWorker*>(context);
        return worker->integrity_.active() ? worker->integrity_.relocated(original,destination) : 0;
    }
    static int linkedIntegrity(void* context, const char* path, const char* target)
    {
        auto* worker = static_cast<PrepareWorker*>(context);
        return worker->integrity_.active() ? worker->integrity_.linked(path,target) : 0;
    }
    static int saveEspRecord(void* context, const QString& guid, bool restore)
    {
        auto* worker = static_cast<PrepareWorker*>(context);
        QMutexLocker lock(&worker->espMutex_);
        worker->espWaiting_ = true; worker->espAccepted_ = false;
        emit worker->espRecord(guid, restore);
        QElapsedTimer timer; timer.start();
        while (worker->espWaiting_ && !QThread::currentThread()->isInterruptionRequested()) {
            if (timer.elapsed() > 120000) { worker->espWaiting_ = false; return ETIMEDOUT; }
            worker->espCondition_.wait(&worker->espMutex_, 100);
        }
        if (QThread::currentThread()->isInterruptionRequested()) return ECANCELED;
        return worker->espAccepted_ ? 0 : EIO;
    }
    QMutex espMutex_;
    QWaitCondition espCondition_;
    bool espWaiting_ = false, espAccepted_ = false;
    rufus::MediaIntegrity integrity_;
    QMutex badBlocksMutex_;
    QWaitCondition badBlocksCondition_;
    bool badBlocksWaiting_ = false;
    int badBlocksChoice_ = rufus::BadBlocksAbort, badBlocksRound_ = 0;
    rufus::PrepareImageRequest request_;
    IsoView selectedView_;
    rufus::SyslinuxResources selectedSyslinux_;
    bool useSelectedSyslinux_ = false;
    QByteArray selectedCore_;
    QByteArray selectedLoader_;
    QVector<rufus::MsDosFile> selectedDosFiles_;
    bool grub4dosSbr_ = false;
    int windowsArch_ = ARCH_UNKNOWN;
    uint32_t windowsBuild_ = 0;
    QString windowsSetupLanguage_;
    QByteArray windowsEfiRepair_;
    rufus::IsoWriteCheck checkedImage_;
    quint32 windowsDiskSignature_ = 0;
};

class PrepareServer : public QObject
{
    Q_OBJECT
public:
    explicit PrepareServer(bool deviceMode) : input_(STDIN_FILENO, QSocketNotifier::Read), deviceMode_(deviceMode)
    {
        connect(&input_, &QSocketNotifier::activated, this, &PrepareServer::readInput);
        connect(&thread_, &QThread::finished, this, &PrepareServer::finish);
        qRegisterMetaType<rufus::BadBlocksReport>();
        timer_.setSingleShot(true);
        connect(&timer_, &QTimer::timeout, this, &PrepareServer::requestExpired);
        timer_.start(requestTimeoutMs);
        send({{"event", "ready"}});
    }

    ~PrepareServer() override
    {
        thread_.requestInterruption();
        thread_.quit();
        thread_.wait();
    }

private:
    void send(const QJsonObject& object)
    {
        if (outputClosed_)
            return;
        const QByteArray message = QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
        // Every frame fits PIPE_BUF: a nonblocking pipe write is atomic or fails.
        ssize_t count = -1;
        if (message.size() <= rufus::PrepareMessageBytes)
        {
            do { count = write(STDOUT_FILENO, message.constData(), size_t(message.size())); }
            while (count < 0 && errno == EINTR);
        }
        if (count != message.size())
        {
            outputClosed_ = true;
            stop(ECANCELED);
        }
    }

    void stop(int error)
    {
        input_.setEnabled(false);
        timer_.stop();
        if (stopError_ == 0)
            stopError_ = error;
        if (started_)
            thread_.requestInterruption();
        else
            QTimer::singleShot(0, this, &PrepareServer::finish);
    }

    void requestExpired() { stop(ETIMEDOUT); }
    void phase(const QString& name)
    {
        phase_ = name;
        send({{"event", "phase"}, {"phase", name}});
    }

    void progress(int operation, int, quint64 processed, quint64 total, bool)
    {
        if ((phase_ == QStringLiteral("badblocks") && operation == OP_BADBLOCKS)
            || (phase_ == QStringLiteral("formatting") && operation == OP_FORMAT))
        {
            send(rufus::EncodePrepareProgress(operation, processed, total));
            return;
        }
        if (rawMode_)
        {
            // Compressed preflight remains in the existing checking phase.
            if (operation >= RAW_WRITE_COPY && operation <= RAW_WRITE_VERIFY)
                send(rufus::EncodeRawProgress(operation, processed, total));
            return;
        }
        // Raw UEFI:NTFS payload reports use their own operation IDs. Only
        // forward the operation belonging to the active ISO creation phase.
        const bool copying = bootSelection_ == BT_IMAGE && phase_ == QStringLiteral("extracting") && operation == OP_FILE_COPY;
        const bool patching = phase_ == QStringLiteral("customizing") && operation == OP_PATCH;
        const bool archive = phase_ == QStringLiteral("archive") && operation == OP_EXTRACT_ZIP;
        if (copying || patching || archive)
            send(rufus::EncodePrepareProgress(operation, processed, total));
    }
    void step(int operation, float percent)
    {
        const bool dosCopy = (bootSelection_ == BT_FREEDOS || bootSelection_ == BT_MSDOS) && phase_ == QStringLiteral("extracting");
        const bool standaloneSyslinux = bootSelection_ == BT_SYSLINUX_V4 || bootSelection_ == BT_SYSLINUX_V6;
        const bool syslinuxCopy = standaloneSyslinux && phase_ == QStringLiteral("bootloader");
        const bool copyStep = operation == OP_FILE_COPY && percent < 0;
        if ((dosCopy || syslinuxCopy) && copyStep)
            send({{"event", "step"}, {"operation", operation}});
    }
    void log(const QString& message) { send({{"event", "log"}, {"message", message.left(logCharacters)}}); }
    void result(const rufus::PrepareImageOutcome& outcome) { outcome_ = outcome; hasResult_ = true; }

    void badBlocksError(quint64 block, int type, int round)
    {
        send({{"event", "badblocks-error"}, {"round", round}, {"block", QString::number(block)}, {"type", type}});
    }
    void badBlocksReport(const rufus::BadBlocksReport& report)
    {
        awaitingBadBlocks_ = report.error == 0 && report.count != 0;
        badBlocksRound_ = report.round;
        send(rufus::EncodeBadBlocksReport(report));
    }

    void espRecord(const QString& guid, bool restore)
    {
        send({{"event", "esp-record"}, {"guid", guid}, {"restore", restore}});
    }

    bool consumeCommands()
    {
        qsizetype newline = -1;
        while ((newline = buffer_.indexOf('\n')) >= 0) {
            if (newline >= rufus::PrepareMessageBytes) { stop(EMSGSIZE); return false; }
            const QByteArray frame = buffer_.left(newline);
            buffer_.remove(0, newline + 1);
            const QJsonObject command = QJsonDocument::fromJson(frame).object();
            if (command.size() == 1 && command.contains("priority")) {
                const QJsonValue priority = command.value("priority");
                const bool validPriority = priority.isDouble() && priority.toDouble() >= rufus::MinimumJobPriority
                    && priority.toDouble() <= rufus::MaximumJobPriority && priority.toDouble() == double(priority.toInt());
                if (!validPriority) { stop(EPROTO); return false; }
                const int error = rufus::SetJobPriority(priority.toInt());
                if (error != 0) log(QStringLiteral("Cannot apply all native job scheduling preferences: %1").arg(error));
                continue;
            }
            if (command.size() == 1 && command.value("espRecord").isBool()) {
                if (!worker_ || !worker_->answerEspRecord(command.value("espRecord").toBool())) { stop(EPROTO); return false; }
                continue;
            }
            const QString choice = command.value("badBlocks").toString();
            const int value = choice == QStringLiteral("retry") ? rufus::BadBlocksRetry
                : choice == QStringLiteral("ignore") ? rufus::BadBlocksIgnore
                : choice == QStringLiteral("abort") ? rufus::BadBlocksAbort : -1;
            const bool exact = command.size() == 2
                && command.value("round").toInt(0) == badBlocksRound_;
            if (!awaitingBadBlocks_ || !exact || value < 0 || !worker_
                || !worker_->answerBadBlocks(badBlocksRound_, value)) { stop(EPROTO); return false; }
            awaitingBadBlocks_ = false;
        }
        return true;
    }

    void readInput()
    {
        char bytes[512];
        for (int attempt = 0; attempt < inputReadsPerEvent; ++attempt)
        {
            const ssize_t count = read(STDIN_FILENO, bytes, sizeof(bytes));
            if (count < 0 && errno == EINTR)
                continue;
            if (count < 0 && errno == EAGAIN)
                return;
            if (count <= 0)
            {
                stop(count == 0 ? ECANCELED : errno);
                return;
            }
            buffer_.append(bytes, qsizetype(count));
            if (buffer_.size() > rufus::PrepareMessageBytes
                && (buffer_.indexOf('\n') < 0 || buffer_.indexOf('\n') >= rufus::PrepareMessageBytes))
            {
                stop(EMSGSIZE);
                return;
            }
            qsizetype newline = buffer_.indexOf('\n');
            if (newline < 0)
                continue;
            if (started_) {
                if (!consumeCommands()) return;
                continue;
            }
            rufus::PrepareImageRequest request;
            const QJsonDocument document = QJsonDocument::fromJson(buffer_.left(newline));
            const bool oneRequest = document.isObject();
            if (!oneRequest || !rufus::DecodePrepareRequest(document.object(), request))
            {
                stop(EPROTO);
                return;
            }
            if (request.isDevice != deviceMode_)
            {
                stop(EPERM);
                return;
            }
            rufus::SetJobPriority(request.jobPriority);
            rawMode_ = request.rawWrite;
            const bool standaloneGrub2 = request.grub2 && request.imagePath.isEmpty();
            const bool standaloneGrub4dos = request.grub4dos && request.isSourceFree();
            bootSelection_ = request.isDos() ? request.dosBootType() : request.reactOs ? BT_REACTOS : standaloneGrub2 ? BT_GRUB2
                : standaloneGrub4dos ? BT_GRUB4DOS : request.syslinuxSelection;
            buffer_.remove(0,newline+1);
            if (!consumeCommands()) return; // Validate coalesced controls before starting any device work.
            timer_.stop();
            PrepareWorker* worker = new PrepareWorker(request);
            worker_ = worker;
            worker->moveToThread(&thread_);
            connect(worker, &PrepareWorker::espRecord, this, &PrepareServer::espRecord);
            connect(worker, &PrepareWorker::badBlocksError, this, &PrepareServer::badBlocksError);
            connect(worker, &PrepareWorker::badBlocksReport, this, &PrepareServer::badBlocksReport);
            connect(&thread_, &QThread::started, worker, &rufus::RufusJob::run);
            connect(worker, &PrepareWorker::phase, this, &PrepareServer::phase);
            connect(worker, &rufus::RufusJob::progressWithInfo, this, &PrepareServer::progress);
            connect(worker, &rufus::RufusJob::progressChanged, this, &PrepareServer::step);
            connect(worker, &rufus::RufusJob::logMessage, this, &PrepareServer::log);
            connect(worker, &rufus::RufusJob::failed, this, &PrepareServer::log);
            connect(worker, &PrepareWorker::result, this, &PrepareServer::result);
            connect(worker, &rufus::RufusJob::finished, &thread_, &QThread::quit, Qt::DirectConnection);
            connect(&thread_, &QThread::finished, worker, &QObject::deleteLater);
            started_ = true;
            thread_.start();
        }
    }

    void finish()
    {
        thread_.wait();
        if (!hasResult_)
        {
            outcome_.error = EIO;
            outcome_.started = started_; // No result: conservatively report possible writes, even on cancellation.
        }
        const bool preserveFailure = stopError_ == ECANCELED && hasResult_
            && outcome_.error != 0 && outcome_.error != ECANCELED;
        if (stopError_ != 0 && !preserveFailure)
            outcome_.error = stopError_;
        outcome_.rawWrite = rawMode_;
        send(rufus::EncodePrepareOutcome(outcome_));
        QCoreApplication::exit(outcome_.error == 0 ? 0 : 1);
    }

    QPointer<PrepareWorker> worker_;
    bool awaitingBadBlocks_ = false;
    int badBlocksRound_ = 0;
    QSocketNotifier input_;
    QTimer timer_;
    QThread thread_;
    QByteArray buffer_;
    QString phase_;
    rufus::PrepareImageOutcome outcome_;
    int stopError_ = 0;
    bool started_ = false;
    bool rawMode_ = false;
    int bootSelection_ = BT_IMAGE;
    bool hasResult_ = false;
    bool outputClosed_ = false;
    const bool deviceMode_;
};
}

int main(int argc, char** argv)
{
#ifdef RUFUS_SAVE_SOURCE_HELPER
    if (argc == 2 && strcmp(argv[1], "--save-device") == 0) return rufus::RunSaveSourceHelper(argc, argv);
#endif
    const bool deviceMode = argc == 2 && strcmp(argv[1], "--device") == 0;
    const bool ordinaryUser = getuid() != 0 && getuid() == geteuid() && getgid() == getegid();
    bool callerValid = false;
    const quint64 caller = qEnvironmentVariable("PKEXEC_UID").toULongLong(&callerValid);
    const bool authorizedRoot = deviceMode && getuid() == 0 && geteuid() == 0
        && callerValid && caller > 0 && caller < UINT32_MAX;
    const bool fileMode = argc == 1 && ordinaryUser;
    if (!fileMode && !authorizedRoot)
        return 77;
    if (deviceMode)
    {
        // Native validation must query the real system service, never a bus
        // address supplied by the invoking user's environment.
        qunsetenv("DBUS_SYSTEM_BUS_ADDRESS");
        qunsetenv("DBUS_SESSION_BUS_ADDRESS");
        // The worker isolates its mounts after exclusive acquisition. Creating
        // a private copy here would retain any desktop mounts of the target.
        if (prctl(PR_SET_CHILD_SUBREAPER, 1) != 0)
            return 77;
    }
    struct stat input, output;
    if (fstat(STDIN_FILENO, &input) != 0 || fstat(STDOUT_FILENO, &output) != 0)
        return 77;
    if (!S_ISFIFO(input.st_mode) || !S_ISFIFO(output.st_mode))
        return 77;
    signal(SIGPIPE, SIG_IGN);
    if (fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK) != 0 || fcntl(STDOUT_FILENO, F_SETFL, O_NONBLOCK) != 0)
        return 77;
    QCoreApplication application(argc, argv);
    const int wimError = wimlib_global_init(WIMLIB_INIT_FLAG_ASSUME_UTF8 |
        WIMLIB_INIT_FLAG_DEFAULT_CASE_INSENSITIVE);
    if (wimError != 0)
        return 77;
    int result;
    {
        PrepareServer server(deviceMode);
        result = application.exec();
    }
    wimlib_global_cleanup();
    return result;
}

#include "prepare_main.moc"
