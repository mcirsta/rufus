// SPDX-License-Identifier: GPL-3.0-or-later
#include "prepare_image_process.h"

#include <QDir>
#include <QJsonDocument>
#include <cerrno>
#include <cstring>

namespace rufus
{
PrepareImageProcess::PrepareImageProcess(const QString& helper, QObject* parent)
    : QObject(parent), helper_(helper)
{
    connect(&process_, &QProcess::readyReadStandardOutput, this, &PrepareImageProcess::readOutput);
    connect(&process_, &QProcess::readyReadStandardError, this, &PrepareImageProcess::readErrors);
    connect(&process_, &QProcess::finished, this, &PrepareImageProcess::processFinished);
    connect(&process_, &QProcess::errorOccurred, this, &PrepareImageProcess::processError);
}

PrepareImageProcess::~PrepareImageProcess()
{
    // EOF reaches the worker's ordinary cancellation path; let it reap mkfs and
    // flush before destroying QProcess. Do not kill a helper to cancel its job.
    disconnect(&process_, nullptr, this, nullptr);
    process_.closeWriteChannel();
    if (request_.isDevice && !ready_)
        process_.terminate();
    if (process_.state() != QProcess::NotRunning)
        process_.waitForFinished(-1);
}

bool PrepareImageProcess::isActive() const
{
    return state_ == State::Starting || state_ == State::Running || state_ == State::Cancelling;
}

bool PrepareImageProcess::start(const PrepareImageRequest& request)
{
    if (isActive())
        return false;
    const QJsonObject object = EncodePrepareRequest(request);
    PrepareImageRequest checked;
    const bool validRequest = DecodePrepareRequest(object, checked)
        && QJsonDocument(object).toJson(QJsonDocument::Compact).size() < PrepareMessageBytes;
    if (!validRequest || !QDir::isAbsolutePath(helper_))
        return false;
    request_ = request;
    outcome_ = {};
    pending_.clear();
    diagnostics_.clear();
    problem_.clear();
    phase_ = -1;
    ready_ = sent_ = hasResult_ = cancelled_ = false;
    state_ = State::Starting;
    if (request.isDevice)
        process_.start(QStringLiteral("/usr/bin/pkexec"),
            {QStringLiteral("--disable-internal-agent"), helper_, QStringLiteral("--device")});
    else
        process_.start(helper_, {});
    emit changed();
    return true;
}

void PrepareImageProcess::cancel()
{
    if (!isActive() || cancelled_)
        return;
    cancelled_ = true;
    state_ = State::Cancelling;
    process_.closeWriteChannel();
    // Before authorization, pkexec still has our real UID and can be stopped.
    // After elevation, EOF handles cancellation; no job request precedes ready.
    if (request_.isDevice && !ready_)
        process_.terminate();
    emit changed();
}

void PrepareImageProcess::failProtocol()
{
    problem_ = tr("The preparation helper returned an invalid or incomplete response.");
    process_.closeWriteChannel();
}

void PrepareImageProcess::accept(const QJsonObject& object)
{
    if (!problem_.isEmpty())
        return;
    const QString event = object.value("event").toString();
    if (event == QStringLiteral("ready") && !ready_ && object.size() == 1)
    {
        ready_ = true;
        if (cancelled_)
            return;
        const QByteArray message = QJsonDocument(EncodePrepareRequest(request_)).toJson(QJsonDocument::Compact) + '\n';
        if (process_.write(message) != message.size())
        {
            failProtocol();
            return;
        }
        sent_ = true;
        state_ = State::Running;
        emit changed();
        return;
    }
    if (!ready_ || hasResult_)
    {
        failProtocol();
        return;
    }
    if (event == QStringLiteral("result") && DecodePrepareOutcome(object, outcome_))
    {
        const bool extentFits = outcome_.offset <= request_.size && outcome_.size <= request_.size - outcome_.offset;
        const bool complete = outcome_.started && outcome_.offset != 0 && extentFits
            && outcome_.filesystemBytes > 0 && outcome_.filesystemBytes <= outcome_.size;
        if (outcome_.error == 0 && (!complete || outcome_.syncError != 0 || phase_ != 2))
            failProtocol();
        else
            hasResult_ = true;
        return;
    }
    if (event == QStringLiteral("log") && object.size() == 2 && object.value("message").isString())
    {
        emit logMessage(object.value("message").toString());
        return;
    }
    if (event == QStringLiteral("phase") && object.size() == 2 && sent_)
    {
        const QString name = object.value("phase").toString();
        const QStringList phases = {QStringLiteral("checking"), QStringLiteral("partitioning"), QStringLiteral("formatting")};
        const int next = phases.indexOf(name);
        if (next == phase_ + 1 && next >= 0)
        {
            phase_ = next;
            emit phaseChanged(name);
            return;
        }
    }
    failProtocol();
}

void PrepareImageProcess::readOutput()
{
    while (process_.bytesAvailable() > 0)
    {
        pending_ += process_.read(PrepareMessageBytes);
        qsizetype newline;
        while ((newline = pending_.indexOf('\n')) >= 0)
        {
            const QJsonDocument document = QJsonDocument::fromJson(pending_.left(newline));
            if (newline >= PrepareMessageBytes || !document.isObject())
                failProtocol();
            else
                accept(document.object());
            pending_.remove(0, newline + 1);
        }
        if (pending_.size() >= PrepareMessageBytes)
        {
            pending_.clear();
            failProtocol();
        }
    }
}

void PrepareImageProcess::readErrors()
{
    process_.setReadChannel(QProcess::StandardError);
    while (process_.bytesAvailable() > 0)
    {
        const QByteArray bytes = process_.read(PrepareMessageBytes);
        diagnostics_ += bytes.left(PrepareMessageBytes - diagnostics_.size());
    }
    process_.setReadChannel(QProcess::StandardOutput);
}

void PrepareImageProcess::processFinished(int exitCode, QProcess::ExitStatus status)
{
    readOutput();
    readErrors();
    const bool normalExit = status == QProcess::NormalExit;
    const bool cancelledBeforeRequest = request_.isDevice && cancelled_ && !sent_;
    const bool authorizationStopped = request_.isDevice && !ready_ && normalExit
        && (exitCode == 126 || exitCode == 127);
    if (authorizationStopped || cancelledBeforeRequest)
    {
        // 127 also covers absent agents and generic denial. Only an explicit
        // caller cancellation or pkexec's dismissed status is labelled cancelled.
        outcome_.error = cancelled_ || exitCode == 126 ? ECANCELED : EACCES;
        hasResult_ = true;
        problem_ = tr("Device authorization was cancelled, denied or unavailable.");
    }
    const bool expectedExit = hasResult_ && exitCode == (outcome_.error == 0 ? 0 : 1);
    if (!cancelledBeforeRequest && (!pending_.isEmpty() || !normalExit || (!expectedExit && !authorizationStopped)))
        failProtocol();
    if (!problem_.isEmpty() && !authorizationStopped && !cancelledBeforeRequest)
    {
        outcome_.error = EIO;
        // Without a trustworthy result, a sent request may have changed media.
        outcome_.started = outcome_.started || sent_;
    }
    if (outcome_.error == 0 && cancelled_)
        outcome_.error = ECANCELED;
    const bool cancelled = outcome_.error == ECANCELED && outcome_.syncError == 0;
    if (cancelled)
        state_ = State::Cancelled;
    else if (outcome_.error != 0 || outcome_.syncError != 0)
        state_ = State::Failed;
    else
        state_ = State::Complete;
    if (problem_.isEmpty() && outcome_.syncError != 0)
        problem_ = tr("Could not flush the prepared image: %1").arg(QString::fromLocal8Bit(strerror(outcome_.syncError)));
    else if (problem_.isEmpty() && outcome_.error != 0)
        problem_ = QString::fromLocal8Bit(strerror(outcome_.error));
    if (!diagnostics_.isEmpty())
        emit logMessage(QString::fromLocal8Bit(diagnostics_));
    emit changed();
    emit finished();
}

void PrepareImageProcess::processError(QProcess::ProcessError error)
{
    if (error != QProcess::FailedToStart)
        return; // Running-process failures are finalized when the child is reaped.
    outcome_.error = EIO;
    problem_ = process_.errorString();
    state_ = State::Failed;
    emit changed();
    emit finished();
}
}
