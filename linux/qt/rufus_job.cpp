// SPDX-License-Identifier: GPL-3.0-or-later
#include "rufus_job.h"
#include "platform/rufus_reporting.h"

#include <QDebug>
#include <QThread>

#include <cmath>
#include <cstdarg>
#include <exception>
#include <mutex>
#include <utility>

static_assert(sizeof(int) == 4, "Rufus reporting requires a 32-bit C int");

namespace rufus
{
namespace
{

// Rufus still has process-global state. Refuse overlapping operations rather
// than suggesting that thread-local report routing makes its core reentrant.
std::mutex operationMutex;
thread_local RufusJob* activeJob = nullptr;
constexpr qint64 refreshIntervalMs = 50;

class ActiveJobScope
{
public:
    explicit ActiveJobScope(RufusJob& job)
    {
        activeJob = &job;
    }

    ~ActiveJobScope()
    {
        activeJob = nullptr;
    }
    ActiveJobScope(const ActiveJobScope&) = delete;
    ActiveJobScope& operator=(const ActiveJobScope&) = delete;
};

} // namespace

struct ReportingAdapter
{
    static void log(QString message)
    {
        if (activeJob)
            emit activeJob->logMessage(std::move(message));
        else
            qWarning().noquote() << message;
    }

    static void percentage(int operation, float percent)
    {
        if (activeJob)
            activeJob->reportPercentage({operation, percent});
    }

    static void counts(int operation, int messageId, quint64 processed,
                       quint64 total, bool forced)
    {
        if (activeJob)
            activeJob->reportCounts({operation, messageId, processed, total, forced});
    }
};

RufusJob::RufusJob(QObject* parent)
    : QObject(parent)
{
}

void RufusJob::run()
{
    if (QThread::currentThread() != thread())
    {
        qWarning("RufusJob::run must execute on the job's owning thread");
        return;
    }
    if (started_)
        return;
    started_ = true;

    // Nested execution on this thread must not attempt to lock its own mutex.
    std::unique_lock<std::mutex> lock(operationMutex, std::defer_lock);
    if (activeJob || !lock.try_lock())
    {
        emit failed(tr("Another Rufus operation is already running."));
        emit finished();
        return;
    }

    std::optional<QString> failure;
    {
        ActiveJobScope scope(*this);
        try
        {
            execute();
        }
        catch (const std::exception& error)
        {
            failure = QString::fromUtf8(error.what());
        }
        catch (...)
        {
            failure = tr("The Rufus operation failed with an unknown exception.");
        }
        flushProgress();
    }
    lock.unlock();
    if (failure)
        emit failed(*failure);
    emit finished();
}

void RufusJob::reportPercentage(Percentage update)
{
    // Negative percentages are discrete steps: never discard them. Starts,
    // completion, operation changes and resets must also pass through.
    const bool operationChanged = lastPercentage_ && update.operation != lastPercentage_->operation;
    const bool reset = lastPercentage_ && update.percent < lastPercentage_->percent;
    const bool endpoint = update.percent <= 0 || update.percent >= 100 || !std::isfinite(update.percent);
    const bool continuous = lastPercentage_ && !operationChanged && !reset;
    const bool boundary = !continuous || endpoint;
    if (boundary)
        flushProgress();
    lastPercentage_ = update;
    pendingPercentage_ = update;
    percentageSequence_ = ++reportSequence_;
    if (boundary || !percentageTimer_.isValid()
        || percentageTimer_.elapsed() >= refreshIntervalMs)
        flushProgress();
}

void RufusJob::reportCounts(Counts update)
{
    const bool sameOperation = lastCounts_ && update.operation == lastCounts_->operation
        && update.messageId == lastCounts_->messageId;
    const bool sameTotal = lastCounts_ && update.total == lastCounts_->total;
    const bool reset = lastCounts_ && update.processed < lastCounts_->processed;
    const bool continuous = sameOperation && sameTotal && !reset;
    const bool endpoint = update.processed == 0 || (update.total != 0 && update.processed >= update.total);
    const bool explicitUpdate = update.operation < 0 || update.forced;
    const bool boundary = !continuous || endpoint || explicitUpdate;
    if (boundary)
        flushProgress();
    lastCounts_ = update;
    pendingCounts_ = update;
    countsSequence_ = ++reportSequence_;
    if (boundary || !countsTimer_.isValid()
        || countsTimer_.elapsed() >= refreshIntervalMs)
        flushProgress();
}

void RufusJob::flushProgress()
{
    // Keep retained samples in source order across both progress entry points.
    // In particular, an old percentage must not arrive after a new OP_INIT.
    if (pendingCounts_ && pendingPercentage_ && countsSequence_ < percentageSequence_)
    {
        flushCounts();
        flushPercentage();
    }
    else
    {
        flushPercentage();
        flushCounts();
    }
}

void RufusJob::flushPercentage()
{
    if (!pendingPercentage_)
        return;
    const Percentage update = *pendingPercentage_;
    pendingPercentage_.reset();
    percentageTimer_.start();
    emit progressChanged(update.operation, update.percent);
}

void RufusJob::flushCounts()
{
    if (!pendingCounts_)
        return;
    const Counts update = *pendingCounts_;
    pendingCounts_.reset();
    countsTimer_.start();
    emit progressWithInfo(update.operation, update.messageId,
                          update.processed, update.total, update.forced);
}

} // namespace rufus

extern "C" void uprintf(const char* format, ...)
{
    if (!format)
        return;
    va_list arguments;
    va_start(arguments, format);
    const QString message = QString::vasprintf(format, arguments);
    va_end(arguments);
    rufus::ReportingAdapter::log(message);
}

extern "C" void uprintfs(const char* message)
{
    if (message)
        rufus::ReportingAdapter::log(QString::fromUtf8(message));
}

extern "C" void UpdateProgress(int operation, float percent)
{
    rufus::ReportingAdapter::percentage(operation, percent);
}

extern "C" void _UpdateProgressWithInfo(int operation, int messageId,
                                       uint64_t processed, uint64_t total, int force)
{
    rufus::ReportingAdapter::counts(operation, messageId, processed, total, force != 0);
}

extern "C" int IsRufusJobCancelled(void)
{
    return QThread::currentThread()->isInterruptionRequested();
}

extern "C" int IsIsoScanCancelled(void)
{
    return IsRufusJobCancelled();
}
