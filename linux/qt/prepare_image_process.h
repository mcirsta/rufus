// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "prepare_image_protocol.h"
#include <QObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QHash>
#include <memory>

namespace rufus
{
// One helper process per image preparation. The caller supplies a trusted
// application helper path; requests cannot choose commands or formatter paths.
// A device request launches pkexec once; its helper validates the target anew.
// File requests remain unprivileged. IsoWriteController owns the START lifecycle.
class PrepareImageProcess : public QObject
{
    Q_OBJECT
public:
    enum class State { Idle, Starting, Running, Cancelling, Complete, Cancelled, Failed };
    Q_ENUM(State)
    explicit PrepareImageProcess(const QString& helper, QObject* parent = nullptr);
    ~PrepareImageProcess() override;
    // Optional owned selected core; stage privately before the existing launch.
    // Metadata stays below the JSON frame limit, and files live until helper exit.
    bool start(const PrepareImageRequest& request, const QByteArray& grub2Core = {}, const QByteArray& syslinuxResources = {});
    void cancel();
    void answerEspRecord(bool accepted);
    void setReportDirectory(const QString& path) { if (!isActive()) reportDirectory_ = path; }
    void answerBadBlocks(int choice);
    void setJobPriority(int priority);
    bool isActive() const;
    State state() const { return state_; }
    const PrepareImageOutcome& outcome() const { return outcome_; }
    const QString& problem() const { return problem_; }

signals:
    void espRecordRequired(QString guid, bool restore);
    void changed();
    void phaseChanged(QString phase);
    void progressChanged(int operation, quint64 processed, quint64 total);
    void stepChanged(int operation); // Original UpdateProgress(op, -1) for discrete boot-file copy steps.
    void logMessage(QString message);
    void rawProgressChanged(int phase, quint64 processed, quint64 total);
    // Emitted only after the helper exits and releases its descriptors/children.
    void finished();
    void badBlocksReviewRequired(rufus::BadBlocksReport report, QString logPath);

private:
    void readOutput();
    void readErrors();
    void accept(const QJsonObject& object);
    bool acceptRaw(const QJsonObject& object);
    void processFinished(int exitCode, QProcess::ExitStatus status);
    void processError(QProcess::ProcessError error);
    void failProtocol();
    bool acceptBadBlocks(const QJsonObject& object);
    bool openBadBlocksLog();
    bool writeBadBlocksLog(const QByteArray& bytes);
    void closeBadBlocksLog(bool retain);

    QString reportDirectory_;
    std::unique_ptr<QTemporaryFile> badBlocksLog_;
    QHash<quint64, int> badBlocksErrors_;
    BadBlocksReport badBlocksReport_;
    int badBlocksRound_ = 0;
    bool badBlocksPassed_ = false, badBlocksWaiting_ = false, badBlocksRetry_ = false;
    bool badBlocksReported_ = false;
    const QString helper_;
    QProcess process_;
    PrepareImageRequest request_;
    std::unique_ptr<QTemporaryDir> coreDirectory_;
    PrepareImageOutcome outcome_;
    RAW_WRITE_RESULT rawProgress_ = {};
    QByteArray pending_;
    QByteArray diagnostics_;
    QString problem_;
    State state_ = State::Idle;
    int phase_ = -1;
    QString phaseName_;
    QStringList phases_;
    quint64 processed_ = 0, total_ = 0;
    int copyStepsRemaining_ = 0;
    bool hasProgress_ = false;
    bool espRecordWaiting_ = false, espRecordSeen_ = false, espRecordApproved_ = false;
    bool ready_ = false;
    bool sent_ = false;
    bool hasResult_ = false;
    bool cancelled_ = false;
    bool skippedPhase_ = false;
};
}
