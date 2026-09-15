// SPDX-License-Identifier: GPL-3.0-or-later
#include "raw_write_job.h"

#include <QFile>
#include <cerrno>
#include <cstring>

namespace rufus
{

RawWriteJob::RawWriteJob(quint64 request, const RawWritePlan& plan, QDBusUnixFileDescriptor descriptor)
    : request_(request), plan_(plan), descriptor_(descriptor)
{
    qRegisterMetaType<RawWriteOutcome>();
    connect(this, &RufusJob::progressWithInfo, this, &RawWriteJob::forwardProgress, Qt::DirectConnection);
    connect(this, &RufusJob::failed, this, &RawWriteJob::reportFailure, Qt::DirectConnection);
}

void RawWriteJob::forwardProgress(int operation, int, quint64 processed, quint64, bool)
{
    emit writeProgress(request_, operation, processed);
}

void RawWriteJob::reportFailure(const QString& problem)
{
    outcome_.error = EIO;
    outcome_.problem = problem;
    emit writeResult(request_, outcome_);
}

void RawWriteJob::execute()
{
    const QByteArray path = QFile::encodeName(plan_.sourcePath());
    outcome_.error = WriteRawImageToDevice(&plan_.source(), path.constData(),
        descriptor_.fileDescriptor(), plan_.target().number, plan_.target().size, &outcome_.io);
    if (outcome_.error != 0 && outcome_.error != ECANCELED)
        outcome_.problem = tr("Image writing failed: %1").arg(QString::fromLocal8Bit(strerror(outcome_.error)));
    if (outcome_.io.sync_error != 0)
    {
        if (!outcome_.problem.isEmpty())
            outcome_.problem += QLatin1Char(' ');
        outcome_.problem += tr("Flushing failed: %1.")
            .arg(QString::fromLocal8Bit(strerror(outcome_.io.sync_error)));
    }
    emit writeResult(request_, outcome_);
}

} // namespace rufus
