// SPDX-License-Identifier: GPL-3.0-or-later
#include "qt/rufus_job.h"
#include "platform/rufus_reporting.h"

#include <QCoreApplication>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QSemaphore>
#include <QSignalSpy>
#include <QTest>
#include <QTextDocument>
#include <QThread>

#include <memory>
#include <stdexcept>

extern "C" void report_from_c(void);
extern "C" void report_progress_from_c(void);
extern "C" void report_burst_from_c(void);

namespace
{

enum class TestOperation
{
    Messages, Progress, BurstThenFail, Blocking, RecordExecution,
    PhaseReset, Nested, SingleMessage, UnknownFailure
};

struct JobState
{
    bool executed = false;
    QSemaphore entered;
    QSemaphore release;
    rufus::RufusJob* nestedJob = nullptr;
};

class TestJob : public rufus::RufusJob
{
public:
    TestJob(TestOperation operation, JobState& state)
        : operation_(operation), state_(state)
    {
    }

protected:
    void execute() override
    {
        state_.executed = true;
        switch (operation_)
        {
        case TestOperation::Messages:
            report_from_c();
            break;
        case TestOperation::Progress:
            report_progress_from_c();
            break;
        case TestOperation::BurstThenFail:
            report_burst_from_c();
            throw std::runtime_error("operation failed");
        case TestOperation::Blocking:
            state_.entered.release();
            if (!state_.release.tryAcquire(1, 5000))
                throw std::runtime_error("test release timed out");
            break;
        case TestOperation::RecordExecution:
            break;
        case TestOperation::PhaseReset:
            UpdateProgress(3, 0);
            UpdateProgress(3, 20);
            _UpdateProgressWithInfo(3, 231, 20, 100, 0);
            UpdateProgress(3, 30);
            _UpdateProgressWithInfo(3, 231, 30, 100, 0);
            _UpdateProgressWithInfo(-1, 1, 0, 0, 0);
            break;
        case TestOperation::Nested:
            uprintfs("before");
            state_.nestedJob->run();
            uprintfs("after");
            break;
        case TestOperation::SingleMessage:
            uprintfs("queued message");
            break;
        case TestOperation::UnknownFailure:
            throw 7;
        }
    }

private:
    TestOperation operation_;
    JobState& state_;
};

class RunningJob
{
public:
    explicit RunningJob(TestOperation operation)
        : job(new TestJob(operation, state))
    {
        job->moveToThread(&thread);
        QObject::connect(&thread, &QThread::started, job, &rufus::RufusJob::run);
        QObject::connect(job, &rufus::RufusJob::finished,
                         &thread, &QThread::quit, Qt::DirectConnection);
        QObject::connect(&thread, &QThread::finished, job, &QObject::deleteLater);
    }

    ~RunningJob()
    {
        thread.quit();
        thread.wait();
    }

    bool execute()
    {
        thread.start();
        return thread.wait(5000);
    }

    // Read state only after joining, except for the thread-safe semaphores.
    JobState state;
    QThread thread;
    TestJob* job;
};

struct Counts
{
    int operation;
    int messageId;
    quint64 processed;
    quint64 total;
    bool forced;
};

void deliverQueuedReports()
{
    // Tests join workers first: queued arguments must survive sender deletion.
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
}

} // namespace

class TestRufusReporting : public QObject
{
    Q_OBJECT

private:
    void observe(rufus::RufusJob* job)
    {
        connect(job, &rufus::RufusJob::logMessage,
                &log_, &QPlainTextEdit::appendPlainText, Qt::QueuedConnection);
        connect(job, &rufus::RufusJob::logMessage,
                this, &TestRufusReporting::recordMessage, Qt::QueuedConnection);
        connect(job, &rufus::RufusJob::progressChanged,
                this, &TestRufusReporting::recordPercentage, Qt::QueuedConnection);
        connect(job, &rufus::RufusJob::progressWithInfo,
                this, &TestRufusReporting::recordCounts, Qt::QueuedConnection);
        connect(job, &rufus::RufusJob::failed,
                this, &TestRufusReporting::recordFailure, Qt::QueuedConnection);
        connect(job, &rufus::RufusJob::finished,
                this, &TestRufusReporting::recordCompletion, Qt::QueuedConnection);
    }

    void recordMessage(QString message)
    {
        onGuiThread_ &= QThread::currentThread() == thread();
        messages_.append(message);
    }

    void recordPercentage(int operation, float percent)
    {
        onGuiThread_ &= QThread::currentThread() == thread();
        percentages_.append({operation, percent});
        events_.append(QStringLiteral("percent:%1").arg(percent));
        if (percent >= 0)
            progress_.setValue(static_cast<int>(percent));
    }

    void recordCounts(int operation, int messageId, quint64 done, quint64 total, bool force)
    {
        counts_.append({operation, messageId, done, total, force});
        events_.append(operation == -1 ? QStringLiteral("init")
                                      : QStringLiteral("count:%1").arg(done));
    }

    void recordFailure(QString message)
    {
        failures_.append(message);
        events_.append(message);
    }

    void recordCompletion()
    {
        events_.append(QStringLiteral("finished"));
    }

    QPlainTextEdit log_;
    QProgressBar progress_;
    QStringList messages_;
    QStringList failures_;
    QStringList events_;
    QList<QPair<int, float>> percentages_;
    QList<Counts> counts_;
    bool onGuiThread_ = true;

private slots:
    void init()
    {
        log_.clear();
        progress_.reset();
        messages_.clear();
        failures_.clear();
        events_.clear();
        percentages_.clear();
        counts_.clear();
        onGuiThread_ = true;
    }

    void cMessagesReachWidgetsAsOwnedValues()
    {
        RunningJob worker(TestOperation::Messages);
        observe(worker.job);
        QVERIFY(worker.execute());
        QVERIFY(log_.toPlainText().isEmpty());
        deliverQueuedReports();
        QVERIFY(onGuiThread_);
        QCOMPARE(messages_.size(), 3);
        QCOMPARE(messages_[0], QString::fromUtf8("Image: Mediu și fișiere.iso (7)"));
        QCOMPARE(messages_[1], QStringLiteral("Literal 100% text"));
        QCOMPARE(messages_[2], QString(8192, QLatin1Char('a')));
        QCOMPARE(log_.toPlainText(), messages_.join(QLatin1Char('\n')));
    }

    void preservesProgressControlEventsAndIntegers()
    {
        RunningJob worker(TestOperation::Progress);
        observe(worker.job);
        QVERIFY(worker.execute());
        deliverQueuedReports();
        const QList<QPair<int, float>> expected = {
            {3, 0}, {3, 25}, {3, -1}, {3, -1}, {4, 0}, {4, 100}
        };
        QCOMPARE(percentages_, expected);
        QVERIFY(onGuiThread_);
        QCOMPARE(progress_.value(), 100);
        QCOMPARE(counts_.size(), 3);
        QCOMPARE(counts_[0].operation, -1);
        QCOMPARE(counts_[0].processed, quint64(0x1234));
        QCOMPARE(counts_[1].messageId, 231);
        QCOMPARE(counts_[1].processed, quint64(9007199254740993ULL));
        QCOMPARE(counts_[1].total, quint64(18014398509481991ULL));
        QVERIFY(counts_[1].forced);
        QCOMPARE(counts_[2].operation, -2);
        QCOMPARE(counts_[2].total, quint64(0));
    }

    void flushesLatestProgressBeforeFailureAndCompletion()
    {
        RunningJob worker(TestOperation::BurstThenFail);
        observe(worker.job);
        QVERIFY(worker.execute());
        deliverQueuedReports();
        QVERIFY(!percentages_.isEmpty());
        QVERIFY(!counts_.isEmpty());
        QCOMPARE(percentages_.last().second, 50.0f);
        QCOMPARE(counts_.last().processed, quint64(10000));
        QVERIFY(percentages_.size() < 10000);
        QVERIFY(counts_.size() < 10000);
        QCOMPARE(failures_, QStringList({QStringLiteral("operation failed")}));
        QVERIFY(events_.size() >= 2);
        QCOMPARE(events_[events_.size() - 2], QStringLiteral("operation failed"));
        QCOMPARE(events_.last(), QStringLiteral("finished"));
    }

    void rejectsOverlappingLegacyJobs()
    {
        RunningJob first(TestOperation::Blocking);
        RunningJob second(TestOperation::RecordExecution);
        observe(second.job);
        first.thread.start();
        QVERIFY(first.state.entered.tryAcquire(1, 3000));
        const bool secondFinished = second.execute();
        first.state.release.release();
        QVERIFY(first.thread.wait(5000));
        QVERIFY(secondFinished);
        deliverQueuedReports();
        QVERIFY(!second.state.executed);
        QCOMPARE(failures_.size(), 1);

        // Releasing a job must also release the process-wide operation guard.
        RunningJob next(TestOperation::RecordExecution);
        QVERIFY(next.execute());
        QVERIFY(next.state.executed);
    }

    void phaseInitializationFollowsPendingProgress()
    {
        RunningJob worker(TestOperation::PhaseReset);
        observe(worker.job);
        QVERIFY(worker.execute());
        deliverQueuedReports();
        QVERIFY(events_.contains(QStringLiteral("percent:30")));
        QVERIFY(events_.contains(QStringLiteral("count:30")));
        QVERIFY(events_.size() >= 2);
        QCOMPARE(events_[events_.size() - 2], QStringLiteral("init"));
        QCOMPARE(events_.last(), QStringLiteral("finished"));
    }

    void nestedCallsKeepTheOuterJobAndJobsRunOnce()
    {
        JobState nestedState;
        TestJob nested(TestOperation::RecordExecution, nestedState);
        QSignalSpy rejected(&nested, &rufus::RufusJob::failed);
        JobState outerState;
        outerState.nestedJob = &nested;
        TestJob outer(TestOperation::Nested, outerState);
        QSignalSpy completed(&outer, &rufus::RufusJob::finished);
        observe(&outer);
        outer.run();
        outer.run();
        deliverQueuedReports();
        QVERIFY(!nestedState.executed);
        QCOMPARE(rejected.size(), 1);
        QCOMPARE(completed.size(), 1);
        QCOMPARE(messages_, QStringList({QStringLiteral("before"), QStringLiteral("after")}));
    }

    void exceptionReleasesRoutingAndOperationGuard()
    {
        JobState failedState;
        TestJob failed(TestOperation::UnknownFailure, failedState);
        QSignalSpy failures(&failed, &rufus::RufusJob::failed);
        QSignalSpy completed(&failed, &rufus::RufusJob::finished);
        QSignalSpy oldMessages(&failed, &rufus::RufusJob::logMessage);
        failed.run();
        QCOMPARE(failures.size(), 1);
        QCOMPARE(completed.size(), 1);

        // Reuse the same thread to detect a stale active-job pointer as well
        // as a locked process guard after an exception.
        JobState nextState;
        TestJob next(TestOperation::SingleMessage, nextState);
        observe(&next);
        next.run();
        deliverQueuedReports();
        QVERIFY(nextState.executed);
        QVERIFY(failures_.isEmpty());
        QCOMPARE(oldMessages.size(), 0);
        QCOMPARE(messages_, QStringList({QStringLiteral("queued message")}));
    }

    void deletingTheWidgetBeforeDeliveryIsSafe()
    {
        std::unique_ptr<QPlainTextEdit> log = std::make_unique<QPlainTextEdit>();
        QSignalSpy changes(log->document(), &QTextDocument::contentsChanged);
        RunningJob worker(TestOperation::SingleMessage);
        connect(worker.job, &rufus::RufusJob::logMessage, log.get(),
                &QPlainTextEdit::appendPlainText, Qt::QueuedConnection);
        QVERIFY(worker.execute());
        QCOMPARE(changes.size(), 0);
        log.reset();
        deliverQueuedReports();
        QCOMPARE(changes.size(), 0);
    }
};

QTEST_MAIN(TestRufusReporting)
#include "test_rufus_reporting.moc"
