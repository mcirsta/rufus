// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "raw_write_job.h"
#include "platform/udisks_target.h"
#include <QThread>
#include <memory>
#include <optional>

namespace rufus
{

enum class WriteState { Idle, Checking, AwaitingConfirmation, Authorizing,
                        Writing, Cancelling, Complete, Failed, Cancelled };

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
    bool isAvailable() const;
    bool isActive() const;
    bool isWriting() const { return workerActive_; }
    const WriteStatus& status() const { return status_; }
    const RawWritePlan* plan() const { return plan_.get(); }
    QString statusText() const;
    int percent() const;
    void prepare(const QString& path, const Device& device);
    void confirm();
    void cancel();
    void invalidate(const QString& problem);
    void reset();

signals:
    void changed();
    void confirmationRequired();
    void logMessage(QString message);

private:
    void accessChanged(UDisksTarget::State state);
    void startWorker();
    void acceptProgress(quint64 request, int phase, quint64 processed);
    void acceptResult(quint64 request, const rufus::RawWriteOutcome& outcome);
    void workerFinished();
    void publish();

    std::optional<QDBusConnection> connection_;
    std::unique_ptr<RawWritePlan> plan_;
    std::unique_ptr<UDisksTarget> access_;
    QThread thread_;
    WriteStatus status_;
    quint64 request_ = 0;
    bool workerActive_ = false;
    bool hasResult_ = false;
    QString invalidated_;
};

} // namespace rufus
