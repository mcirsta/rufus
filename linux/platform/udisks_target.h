// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "raw_write_plan.h"
#include <QDBusConnection>
#include <QObject>

class QDBusPendingCallWatcher;

namespace rufus
{

// Read-only preflight using an explicitly supplied bus; no implicit fallback. Keep the prepared
// plan alive and unchanged through this object and any subsequent writer.
class UDisksTarget : public QObject
{
    Q_OBJECT
public:
    enum class State { Idle, Checking, Prepared, Rechecking, Ready, Failed, Cancelled };
    Q_ENUM(State)
    UDisksTarget(const QDBusConnection& connection, const RawWritePlan& plan,
        QObject* parent = nullptr);
    ~UDisksTarget() override;
    State state() const { return state_; }
    const QString& problem() const { return problem_; }
    void check(bool allowMounted = false);
    int partitionCount() const { return partitionCount_; }
    // Fresh read-only preflight after confirmation; Ready permits launching the
    // helper, which alone authorizes and acquires the device.
    void recheck();
    void cancel();

signals:
    void stateChanged(rufus::UDisksTarget::State state);

private slots:
    void ownerResolved(QDBusPendingCallWatcher* watcher);
    void snapshotFinished(QDBusPendingCallWatcher* watcher);
    void ownerChanged(const QString& service, const QString& oldOwner, const QString& newOwner);
    void busDisconnected();

private:
    void snapshot(State state);
    void fail(const QString& problem);
    void setState(State state);
    void discardPending();

    QDBusConnection connection_;
    const RawWritePlan& plan_;
    QDBusPendingCallWatcher* pending_ = nullptr;
    State state_ = State::Idle;
    QString problem_;
    bool watchesDisconnect_ = false;
    bool allowMounted_ = false;
    int partitionCount_ = 0;
};

} // namespace rufus
