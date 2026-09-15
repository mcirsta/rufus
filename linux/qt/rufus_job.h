// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QString>

#include <optional>

namespace rufus
{

// Move this object to a worker thread, then invoke run() there. The operation
// and all context-free C reporting calls must execute on that same thread.
// Keep the job alive until finished(); connect GUI consumers using queued
// connections. The adapter has no widget pointers or GUI dependencies.
class RufusJob : public QObject
{
    Q_OBJECT

public:
    explicit RufusJob(QObject* parent = nullptr);

public slots:
    void run();

signals:
    void logMessage(QString message);
    // Upstream operation-local percentage. A negative value advances a step.
    void progressChanged(int operation, float percent);
    // Preserve OP_INIT / OP_NOOP and message IDs, including the original
    // initialization payload; these fields are not always byte counts.
    void progressWithInfo(int operation, int messageId,
                          quint64 processed, quint64 total, bool forced);
    void failed(QString message);
    // Emitted once, after all reports, whether the operation returned or threw.
    // This denotes job termination, not media validation or bootability.
    void finished();

protected:
    // A concrete worker implements one operation; run() owns its lifecycle.
    virtual void execute() = 0;

private:
    friend struct ReportingAdapter;

    struct Percentage
    {
        int operation;
        float percent;
    };
    struct Counts
    {
        int operation;
        int messageId;
        quint64 processed;
        quint64 total;
        bool forced;
    };

    void reportPercentage(Percentage update);
    void reportCounts(Counts update);
    void flushProgress();
    void flushPercentage();
    void flushCounts();

    bool started_ = false;
    QElapsedTimer percentageTimer_;
    QElapsedTimer countsTimer_;
    std::optional<Percentage> lastPercentage_;
    std::optional<Counts> lastCounts_;
    std::optional<Percentage> pendingPercentage_;
    std::optional<Counts> pendingCounts_;
    quint64 reportSequence_ = 0;
    quint64 percentageSequence_ = 0;
    quint64 countsSequence_ = 0;
};

} // namespace rufus
