// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "raw_write_plan.h"
#include <QDBusConnection>
#include <QDBusUnixFileDescriptor>
#include <QObject>

class QDBusPendingCallWatcher;

namespace rufus
{

// Explicitly supplied bus; no implicit system-bus fallback. Keep the prepared
// plan alive and unchanged through this object and any subsequent writer.
class UDisksTarget : public QObject
{
    Q_OBJECT
public:
    static constexpr int DefaultAuthorizationTimeoutMs = 120000;
    enum class State { Idle, Checking, Prepared, BeforeAuthorization, Authorizing,
                       Rechecking, Ready, Failed, Cancelled };
    Q_ENUM(State)
    UDisksTarget(const QDBusConnection& connection, const RawWritePlan& plan,
        int authorizationTimeout = DefaultAuthorizationTimeoutMs, QObject* parent = nullptr);
    ~UDisksTarget() override;
    State state() const { return state_; }
    const QString& problem() const { return problem_; }
    void check();
    // Call only after the user confirms the Prepared plan. No password handling.
    void authorize();
    void cancel();
    // Valid only once in Ready. Transfers ownership to the caller; further
    // device/source revalidation during writing belongs to that caller.
    QDBusUnixFileDescriptor takeDescriptor();

signals:
    void stateChanged(rufus::UDisksTarget::State state);

private slots:
    void ownerResolved(QDBusPendingCallWatcher* watcher);
    void snapshotFinished(QDBusPendingCallWatcher* watcher);
    void openFinished(QDBusPendingCallWatcher* watcher);
    void ownerChanged(const QString& service, const QString& oldOwner, const QString& newOwner);
    void busDisconnected();

private:
    void snapshot(State state);
    void open();
    void fail(const QString& problem);
    void setState(State state);
    void discardPending();

    QDBusConnection connection_;
    const RawWritePlan& plan_;
    int authorizationTimeout_;
    QDBusPendingCallWatcher* pending_ = nullptr;
    QDBusUnixFileDescriptor descriptor_;
    State state_ = State::Idle;
    QString problem_;
    bool watchesDisconnect_ = false;
};

} // namespace rufus
