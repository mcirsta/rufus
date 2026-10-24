// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "platform/device.h"
#include "platform/format_options.h"
#include "image_analysis_job.h"
#include "image_hash_controller.h"
#include "image_download.h"
#include "image_save_controller.h"
#include "raw_write_controller.h"
#include "iso_write_controller.h"
#include "image_policy.h"
#include "format_policy.h"

#include <QObject>
#include <QString>
#include <QThread>

namespace rufus
{

enum class ImageState
{
    None, Scanning, Cancelling, Complete, Partial, Failed, Cancelled, Unsupported, ChoosingStore
};

struct Selection
{
    QList<Device> devices;
    QString deviceId;
    QString imagePath;
    QString archivePath;
    quint64 imageSize = 0;
    quint64 persistenceSize = 0;
    QString imageProblem;
    ImageState imageState = ImageState::None;
    ISO_ANALYSIS imageAnalysis = {};
    WINDOWS_IMAGE_ANALYSIS windowsAnalysis = {};
    PrepareImageRequest imageSource;
    QStringList ffuStores;
    QString volumeLabel;
    quint32 fileSystems = 0;
    int defaultFileSystem = FS_UNKNOWN;
    int fileSystem = FS_UNKNOWN;
    RUFUS_CLUSTER_SIZES clusterSizes = {};
    quint32 clusterSize = 0;
    bool discoveryAvailable = false;
    QString discoveryProblem;
    bool rawImage = false;
    bool syslinux = false;
    bool windowsToGo = false;
    int windowsToGoTarget = TT_UEFI;
    int bootType = BT_IMAGE;
    bool allowFixedUsb = false;
    PartitionScheme partition = PartitionScheme::Gpt; // Selected layout, including BIOS ISO jobs.
};

// Selection serializes analysis and raw writing; each operation owns its job.
class SelectionController : public QObject
{
    Q_OBJECT

public:
    explicit SelectionController(QObject* parent = nullptr);
    ~SelectionController() override;
    const Selection& selection() const;
    bool isAnalyzing() const;
    const SecureBootPolicy& secureBootPolicy() const { return secureBootPolicy_; }
    void setSecureBootPolicy(const SecureBootPolicy& policy);
    QString imageStatusText() const;
    void setDevices(const QList<Device>& devices);
    RawWriteController& writer() { return writer_; }
    IsoWriteController& isoWriter() { return isoWriter_; }
    bool isWritingActive() const;
    bool isBusy() const;
    bool canHashImage() const;
    ImageHashController& hasher() { return hasher_; }
    ImageSaveController& saver() { return saver_; }
    ImageDownload& downloader() { return downloader_; }
    bool downloadImage(const QUrl& url,const QString& path);
    bool canSaveDevice() const;
    bool startSaveDevice(const QString& path);
    void configureDeviceOperations(const QString& helper) { deviceHelper_ = helper; }
    bool startDeviceOperation(int operation);
    PrepareImageProcess* deviceOperation() const { return deviceOperation_.get(); }
    bool isoSupportEnabled() const { return isoSupport_; }
    bool jolietEnabled() const { return joliet_; }
    bool rockRidgeEnabled() const { return rockRidge_; }
    quint32 deviceClasses() const;
    bool ntfsCompression() const { return ntfsCompression_; }
    bool fileIndexing() const { return fileIndexing_; }
    bool preserveTimestamps() const { return preserveTimestamps_; }
    bool sizeChecksEnabled() const { return sizeChecks_; }
    bool usbDebug() const { return usbDebug_; }
    void toggleUsbDebug();
    bool extraHashes() const { return extraHashes_; }
    bool ignoreBootMarker() const { return ignoreBootMarker_; }
    bool canStartWrite() const;
    bool canSetPersistence() const;
    bool canFullFormat() const;
    bool quickFormat() const;
    bool extendedLabelEnabled() const { return extendedLabel_; }
    bool canExtendedLabel() const;
    bool canRuntimeValidate() const;
    bool runtimeValidationEnabled() const { return runtimeValidation_ && canRuntimeValidate(); }
    bool oldBiosFixesEnabled() const { return oldBiosFixes_; }
    bool canOldBiosFixes() const;
    bool badBlocksEnabled() const { return badBlocksEnabled_; }
    int badBlocksPattern() const { return badBlocksPattern_; }
    bool detectFakes() const { return detectFakes_; }
    quint64 maximumPersistenceSize() const;
    bool canSelectSyslinux() const;
    bool canSelectGrub() const;
    bool canSelectWindowsBios() const;
    bool canSelectKolibri() const;
    bool canSelectWindowsToGo() const;
    bool canSelectWindowsToGoMbr() const;
    bool windowsRufusMbr() const { return windowsRufusMbr_; }
    bool canSelectFormatMbr() const;
    bool canSelectSfd() const;
    // Win32 Alt+E is a saved preference; shared image policy decides its effect.
    bool enhancedInstall() const { return enhancedInstall_; }
    IMAGE_WRITE_CHOICES isoWriteChoices(quint64 persistenceSize, bool gpt, bool fat) const;

public slots:
    void setRuntimeValidation(bool enabled);
    void setDiscovery(const QList<rufus::Device>& devices, const QString& problem);
    void selectDevice(const QString& id);
    void selectImage(const QString& path);
    void selectArchive(const QString& path);
    void chooseFfuStore(unsigned index);
    void cancelAnalysis();
    void startHashes();
    void toggleExtraHashes();
    void toggleBootMarker();
    void toggleIsoSupport();
    void toggleJoliet();
    void toggleRockRidge();
    void togglePreserveTimestamps();
    void toggleSizeChecks();
    void toggleVirtualDisks();
    void toggleVmwareDisks();
    void toggleNonUsbRemovable();
    void toggleNtfsCompression();
    void toggleFileIndexing();
    void startWrite();
    void answerRevocationWarning(bool proceed);
    void startZero(bool fast);
    void setQuickFormat(bool quick);
    void setExtendedLabel(bool enabled);
    void setOldBiosFixes(bool enabled);
    void setBadBlocksEnabled(bool enabled);
    void setBadBlocksPattern(int pattern);
    void toggleFakeDetection();
    void answerBadBlocks(int choice);
    void setVolumeLabel(const QString& label);
    void setPersistenceSize(quint64 bytes);
    void setClusterSize(quint32 bytes);
    void setFileSystem(int filesystem);
    void setStartupFileSystem(int filesystem);
    void setPartitionScheme(rufus::PartitionScheme scheme);
    void setBootType(int type);
    void setWindowsToGo(bool enabled);
    void setWindowsToGoTarget(int target);
    void toggleEnhancedInstall();
    void toggleWindowsMbr();
    void setAllowFixedUsb(bool enabled);
    void setAdvancedFormat(bool enabled);

signals:
    void devicesRefreshRequested();
    void selectionChanged();
    void fixedUsbChanged(bool enabled);
    void deviceClassesChanged(quint32 classes);
    void usbDebugChanged(bool enabled);
    void imageSelectionFinished();
    void revocationWarningRequired(int mask);
    void logMessage(QString message);
    void optionStatusChanged(QString message);
    void imageOptionRejected(QString message);
    void badBlocksReviewRequired(rufus::BadBlocksReport report, QString logPath);

private:
    void saveEspRecord(const QString& guid, bool restore);
    void deviceOperationFinished();
    QString deviceHelper_;
    QString restoredEsp_;
    std::unique_ptr<PrepareImageProcess> deviceOperation_;
    void clearRejectedImage();
    void rescanReadOptions(bool force);
    void reportReadOption(const QString& name, bool enabled);
    void startAnalysis();
    void downloadFinished();
    void selectPendingDevice();
    void updateFormatOptions(int preselected = FS_UNKNOWN);
    void clampPersistenceSize();
    PrepareImageRequest selectedImageRequest() const;
    void updatePartitionScheme();
    bool biosImageUefiNtfs(int filesystem) const;
    MBR_BOOT_CODE windowsImageBoot(int filesystem) const;
    void acceptAnalysis(quint64 request, const rufus::ImageAnalysisResult& result);
    void acceptAnalysisLog(quint64 request, const QString& message);
    void analysisFinished();
    void logImageReport(bool ddRequired);
    void logWindowsMetadata();
    QString pendingChecks() const;
    void updateVolumeLabel();
    void startHybridDd(rufus::PrepareImageRequest source, rufus::Device target);

    bool revocationWarningPending_ = false, revocationWarningAccepted_ = false;
    quint64 revocationWarningRequest_ = 0;
    SecureBootPolicy secureBootPolicy_;
    bool usbDebug_ = false;
    Selection selection_;
    PrepareArchiveSource archive_;
    RawWriteController writer_;
    IsoWriteController isoWriter_;
    ImageHashController hasher_;
    ImageSaveController saver_;
    ImageDownload downloader_;
    QDateTime imageModified_;
    QThread analysisThread_;
    bool initialDeviceSelectionPending_ = true;
    bool userChangedLabel_ = false;
    bool analysisActive_ = false;
    bool enhancedInstall_ = false;
    bool windowsRufusMbr_ = true;
    bool advancedFormat_ = false;
    bool quickFormat_ = true;
    bool extendedLabel_ = true;
    bool runtimeValidation_ = false; // Original session preference, not a saved setting.
    bool oldBiosFixes_ = false;
    bool badBlocksEnabled_ = false, detectFakes_ = true;
    int badBlocksPattern_ = 0;
    bool extraHashes_ = false;
    bool ignoreBootMarker_ = false;
    quint32 deviceClasses_ = DefaultDeviceClasses;
    bool previousFixedUsb_ = false;
    bool ntfsCompression_ = false, fileIndexing_ = false;
    bool isoSupport_ = true, joliet_ = true, rockRidge_ = true, sizeChecks_ = true;
    bool preserveTimestamps_ = false;
    bool readOptionsRescan_ = false;
    int persistenceExtVersion_ = 3;
    quint16 udfRevision_ = 0x0201;
    PartitionScheme preferredPartition_ = PartitionScheme::ImageDefault;
    int preferredFileSystem_ = FS_UNKNOWN;
    int startupFileSystem_ = FS_UNKNOWN;
    int preferredWindowsTarget_ = TT_MAX;
    quint64 imageRequest_ = 0;
    quint64 runningRequest_ = 0;
};

} // namespace rufus
