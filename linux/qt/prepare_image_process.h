// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "prepare_image_protocol.h"
#include <QObject>
#include <QProcess>

namespace rufus
{
// One helper process per image preparation. The caller supplies a trusted
// application helper path; requests cannot choose commands or formatter paths.
// A device request launches pkexec once; its helper validates the target anew.
// File requests remain unprivileged. Application START integration is separate.
class PrepareImageProcess : public QObject
{
    Q_OBJECT
public:
    enum class State { Idle, Starting, Running, Cancelling, Complete, Cancelled, Failed };
    Q_ENUM(State)
    explicit PrepareImageProcess(const QString& helper, QObject* parent = nullptr);
    ~PrepareImageProcess() override;
    bool start(const PrepareImageRequest& request);
    void cancel();
    bool isActive() const;
    State state() const { return state_; }
    const PrepareImageOutcome& outcome() const { return outcome_; }
    const QString& problem() const { return problem_; }

signals:
    void changed();
    void phaseChanged(QString phase);
    void logMessage(QString message);
    // Emitted only after the helper exits and releases its descriptors/children.
    void finished();

private:
    void readOutput();
    void readErrors();
    void accept(const QJsonObject& object);
    void processFinished(int exitCode, QProcess::ExitStatus status);
    void processError(QProcess::ProcessError error);
    void failProtocol();

    const QString helper_;
    QProcess process_;
    PrepareImageRequest request_;
    PrepareImageOutcome outcome_;
    QByteArray pending_;
    QByteArray diagnostics_;
    QString problem_;
    State state_ = State::Idle;
    int phase_ = -1;
    bool ready_ = false;
    bool sent_ = false;
    bool hasResult_ = false;
    bool cancelled_ = false;
};
}
