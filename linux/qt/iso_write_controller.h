// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "platform/device.h"
#include "platform/iso_write_check.h"
#include "platform/prepare_source.h"
#include "prepare_image_process.h"
#include "grub2_core_selection.h"
#include "syslinux_selection.h"
#include "msdos_selection.h"
#include <QDBusConnection>
#include <QThread>
#include <memory>
#include <optional>

namespace rufus
{
enum class IsoWriteState { Idle, Checking, ChoosingMode, SelectingGrubCore, ChoosingWindowsArchive, ChoosingWindowsEdition, ChoosingWindowsOptions, ChoosingDosOptions, AwaitingConfirmation,
    Authorizing, Writing, Cancelling, Complete, Failed, Cancelled };

class IsoWriteController : public QObject
{
    Q_OBJECT
public:
    explicit IsoWriteController(QObject* parent = nullptr);
    ~IsoWriteController() override;
    void configure(const QDBusConnection& connection, const QString& helper,
        const QString& filesDirectory = {}, const QUrl& filesServer = {});
    bool isAvailable(IMAGE_WRITE_MODE mode = IMAGE_WRITE_ISO) const;
    bool isActive() const;
    bool isWriting() const;
    IsoWriteState state() const { return state_; }
    const PrepareImageRequest& request() const { return request_; }
    const Device& target() const { return target_; }
    const IsoWriteCheck& check() const { return check_; }
    int windowsOptions() const { return windowsOptions_; }
    bool expertMode() const { return expertMode_; }
    const QString& grubFilesDirectory() const { return filesDirectory_; }
    bool hasGrubDownloadProgress() const { return grubDownloadTotal_ != 0; }
    const Grub2CoreSelectionResult& grubCoreResult() const { return selectedCore_; }
    void toggleExpertMode();
    PrepareImageOutcome outcome() const { return process_ ? process_->outcome() : PrepareImageOutcome(); }
    QString statusText() const;
    int progressPosition() const
    {
        if (state_ == IsoWriteState::Complete)
            return MAX_PROGRESS;
        if (state_ == IsoWriteState::SelectingGrubCore)
            return progressPosition_;
        return qMin(progressPosition_, MAX_PROGRESS - 1);
    }
    void prepare(const PrepareImageRequest& source, const Device& target, const QString& label, quint32 cluster,
        const IMAGE_WRITE_CHOICES& choices, int fileSystem = FS_FAT32);
    void chooseMode(IMAGE_WRITE_MODE mode);
    void chooseWindowsOptions(const PrepareImageRequest& options);
    void chooseWindowsArchive(const QString& path);
    void chooseWindowsEdition(int image);
    void chooseDosOptions(const QString& keyboard, quint32 codepage);
    void chooseMsDosDownload(bool accept) { msDos_.choose(accept); }
    void chooseGrubDownload(Grub2DownloadChoice choice);
    bool isSelectingSyslinux() const { return syslinuxResources_.isActive(); }
    void confirm();
    void cancel();
    void setJobPriority(int priority) { if (process_) process_->setJobPriority(priority); }
    void discardCachedBootResources();
    QString progressPhase() const { return phase_; }
    quint64 processedBytes() const { return processedBytes_; }
    quint64 totalBytes() const { return totalBytes_; }
    void answerBadBlocks(int choice) { if (process_) process_->answerBadBlocks(choice); }
    void setReportDirectory(const QString& path) { if (!isActive()) reportDirectory_ = path; }
    void invalidate(const QString& problem);
    void reset();

signals:
    void changed();
    void modeRequired();
    void windowsOptionsRequired();
    void windowsArchiveRequired();
    void windowsEditionRequired();
    void dosOptionsRequired();
    void grubDownloadRequired(QString detectedVersion);
    void msDosDownloadRequired();
    void confirmationRequired();
    void ddRequested(rufus::PrepareImageRequest source, rufus::Device target);
    void logMessage(QString message);
    void badBlocksReviewRequired(rufus::BadBlocksReport report, QString logPath);

private:
    void startCheck(bool confirmed);
    void acceptCheck(int error, const rufus::IsoWriteCheck& result);
    void checkFailed(const QString& problem);
    void checkFinished();
    void requestImageOptions();
    void requestWindowsImage();
    void msDosFinished(rufus::MsDosSelectionResult result);
    void syslinuxFinished(int error, QByteArray resources);
    void grubCoreFinished(rufus::Grub2CoreSelectionResult result);
    void grubDownloadProgress(quint64 received, quint64 total);
    void processChanged();
    void processFinished();
    void phaseChanged(const QString& phase);
    void acceptProgress(int operation, quint64 processed, quint64 total);
    void acceptStep(int operation);
    void publish();

    std::optional<QDBusConnection> connection_;
    QString reportDirectory_;
    QString helper_;
    QString filesDirectory_;
    QUrl filesServer_;
    MsDosSelection msDos_;
    QByteArray selectedDosImage_;
    SyslinuxSelection syslinuxResources_;
    QByteArray selectedSyslinux_;
    Grub2CoreSelection grubCore_;
    Grub2CoreSelectionResult selectedCore_;
    quint64 grubDownloadTotal_ = 0;
    PrepareImageRequest request_;
    Device target_;
    std::unique_ptr<PrepareIsoSource> source_;
    std::unique_ptr<PrepareImageProcess> process_;
    QThread thread_;
    IsoWriteCheck check_;
    IsoWriteState state_ = IsoWriteState::Idle;
    QString problem_, phase_, invalidated_;
    int checkError_ = 0;
    bool workerActive_ = false;
    bool confirmed_ = false;
    bool hasResult_ = false;
    bool windowsArchiveChosen_ = false, windowsImageChosen_ = false;
    int windowsOptions_;
    bool expertMode_ = false;
    RUFUS_PROGRESS progress_ = {};
    int progressPosition_ = 0;
    quint64 processedBytes_ = 0, totalBytes_ = 0;
    float operationPercent_ = 0.0f;
};
}
Q_DECLARE_METATYPE(rufus::IsoWriteCheck)
