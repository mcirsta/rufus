// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "platform/raw_write.h"
#include "prepare_image_process.h"
#include "platform/udisks_target.h"
#include <memory>
#include <optional>

namespace rufus
{

enum class WriteState { Idle, Checking, AwaitingConfirmation, Authorizing,
                        Writing, Cancelling, Complete, Failed, Cancelled };

struct RawWriteOutcome
{
    RAW_WRITE_RESULT io = {};
    int error = 0;
    bool started = false;
    QString problem;
};

struct WriteStatus
{
    WriteState state = WriteState::Idle;
    RawWriteOutcome outcome;
    quint64 size = 0;
};

// One concrete raw-write lifecycle. An explicit bus enables acquisition; no
// implicit system-bus fallback. Tests can supply their isolated private bus.
class RawWriteController : public QObject
{
    Q_OBJECT
public:
    explicit RawWriteController(QObject* parent = nullptr);
    ~RawWriteController() override;
    void setConnection(const QDBusConnection& connection);
    void setHelper(const QString& path);
    bool isAvailable() const;
    bool isActive() const;
    bool isWriting() const { return helperActive_ && status_.state != WriteState::Authorizing; }
    const WriteStatus& status() const { return status_; }
    const RawWritePlan* plan() const { return plan_.get(); }
    int partitionCount() const { return access_ ? access_->partitionCount() : 0; }
    QString statusText() const;
    int percent() const;
    void prepare(const QString& path, const Device& device, const PrepareImageRequest* expected = nullptr, bool allowFixedUsb = false);
    void prepareZero(const Device& device, int mode, bool allowFixedUsb, int badBlocksPattern = -1, bool detectFakes = true);
    void confirm();
    void cancel();
    void setJobPriority(int priority) { if (process_) process_->setJobPriority(priority); }
    quint64 totalBytes() const { return progressTotal_; }
    void answerBadBlocks(int choice) { if (process_) process_->answerBadBlocks(choice); }
    void setReportDirectory(const QString& path) { if (!isActive()) reportDirectory_ = path; }
    void invalidate(const QString& problem);
    void reset();

signals:
    void changed();
    void confirmationRequired();
    void logMessage(QString message);
    void badBlocksReviewRequired(rufus::BadBlocksReport report, QString logPath);

private:
    void accessChanged(UDisksTarget::State state);
    bool helperAvailable() const;
    void startHelper();
    void helperChanged();
    void badBlocksProgress(int operation, quint64 processed, quint64 total);
    void helperProgress(int phase, quint64 processed, quint64 total);
    void helperFinished();
    void finishWrite();
    void publish();

    std::optional<QDBusConnection> connection_;
    std::unique_ptr<RawWritePlan> plan_;
    std::unique_ptr<UDisksTarget> access_;
    QString reportDirectory_;
    QString helper_;
    std::unique_ptr<PrepareImageProcess> process_;
    quint64 diskSequence_ = 0;
    quint64 progressTotal_ = 0;
    bool helperActive_ = false;
    int zeroMode_ = ZERO_NONE;
    bool sparseImage_ = false;
    quint64 badBlocksProcessed_ = 0, badBlocksTotal_ = 0;
    WriteStatus status_;
    bool hasResult_ = false;
    QString invalidated_;
};

} // namespace rufus
