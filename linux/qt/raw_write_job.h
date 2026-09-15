// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "rufus_job.h"
#include "platform/raw_write_plan.h"
#include "platform/raw_write.h"
#include <QDBusUnixFileDescriptor>

namespace rufus
{

struct RawWriteOutcome
{
    RAW_WRITE_RESULT io = {};
    int error = 0;
    QString problem;
};

// The controller retains the immutable plan until the worker has been joined.
// This job owns the authorized descriptor. It never opens a device pathname.
class RawWriteJob final : public RufusJob
{
    Q_OBJECT
public:
    RawWriteJob(quint64 request, const RawWritePlan& plan, QDBusUnixFileDescriptor descriptor);

signals:
    void writeProgress(quint64 request, int phase, quint64 processed);
    void writeResult(quint64 request, rufus::RawWriteOutcome outcome);

protected:
    void execute() override;

private:
    void forwardProgress(int operation, int message, quint64 processed, quint64 total, bool forced);
    void reportFailure(const QString& problem);

    quint64 request_;
    const RawWritePlan& plan_;
    QDBusUnixFileDescriptor descriptor_;
    RawWriteOutcome outcome_;
};

} // namespace rufus

Q_DECLARE_METATYPE(rufus::RawWriteOutcome)
