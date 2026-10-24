// SPDX-License-Identifier: GPL-3.0-or-later
#include "udisks_target.h"

#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>

namespace rufus
{
namespace
{
const QString service = QStringLiteral("org.freedesktop.UDisks2");
const QString root = QStringLiteral("/org/freedesktop/UDisks2");
constexpr int requestTimeoutMs = 5000;
}

UDisksTarget::UDisksTarget(const QDBusConnection& connection, const RawWritePlan& plan,
    QObject* parent)
    : QObject(parent), connection_(connection), plan_(plan)
{
    qDBusRegisterMetaType<UDisksInterfaces>();
    qDBusRegisterMetaType<UDisksObjects>();
    qDBusRegisterMetaType<QList<QByteArray>>();
    auto* watcher = new QDBusServiceWatcher(service, connection_,
        QDBusServiceWatcher::WatchForOwnerChange, this);
    connect(watcher, &QDBusServiceWatcher::serviceOwnerChanged, this, &UDisksTarget::ownerChanged);
    watchesDisconnect_ = connection_.connect(QString(), QStringLiteral("/org/freedesktop/DBus/Local"),
        QStringLiteral("org.freedesktop.DBus.Local"), QStringLiteral("Disconnected"),
        this, SLOT(busDisconnected()));
}

UDisksTarget::~UDisksTarget()
{
    discardPending();
}

void UDisksTarget::setState(State state)
{
    state_ = state;
    emit stateChanged(state_);
}

void UDisksTarget::discardPending()
{
    delete pending_;
    pending_ = nullptr;
}

void UDisksTarget::fail(const QString& problem)
{
    discardPending();
    problem_ = problem;
    setState(State::Failed);
}

void UDisksTarget::check(bool allowMounted)
{
    if (state_ != State::Idle)
        return;
    if (!connection_.isConnected() || !watchesDisconnect_)
    {
        fail(tr("D-Bus is required to check the selected target."));
        return;
    }
    allowMounted_ = allowMounted;
    snapshot(State::Checking);
}

void UDisksTarget::snapshot(State state)
{
    QDBusMessage request = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.DBus"),
        QStringLiteral("/org/freedesktop/DBus"), QStringLiteral("org.freedesktop.DBus"), QStringLiteral("GetNameOwner"));
    request.setArguments({service});
    state_ = state;
    pending_ = new QDBusPendingCallWatcher(connection_.asyncCall(request, requestTimeoutMs), this);
    connect(pending_, &QDBusPendingCallWatcher::finished, this, &UDisksTarget::ownerResolved);
    emit stateChanged(state_);
}

void UDisksTarget::ownerResolved(QDBusPendingCallWatcher* watcher)
{
    if (watcher != pending_)
        return;
    pending_ = nullptr;
    watcher->deleteLater();
    const QDBusPendingReply<QString> reply = *watcher;
    if (reply.isError() || reply.value() != plan_.target().serviceOwner)
    {
        fail(tr("The UDisks service changed or is unavailable. Prepare a new write plan."));
        return;
    }
    const QDBusMessage request = QDBusMessage::createMethodCall(plan_.target().serviceOwner, root,
        QStringLiteral("org.freedesktop.DBus.ObjectManager"), QStringLiteral("GetManagedObjects"));
    pending_ = new QDBusPendingCallWatcher(connection_.asyncCall(request, requestTimeoutMs), this);
    connect(pending_, &QDBusPendingCallWatcher::finished, this, &UDisksTarget::snapshotFinished);
}

void UDisksTarget::snapshotFinished(QDBusPendingCallWatcher* watcher)
{
    if (watcher != pending_)
        return;
    pending_ = nullptr;
    watcher->deleteLater();
    const QDBusPendingReply<UDisksObjects> reply = *watcher;
    if (reply.isError())
    {
        fail(tr("Cannot revalidate the target: %1").arg(reply.error().message()));
        return;
    }
    // The request was sent to this unique owner after resolving the service;
    // owner-change notifications invalidate the pending check.
    QStringList mounts;
    int partitions = 0;
    QString problem = plan_.checkSnapshot(reply.value(), plan_.target().serviceOwner, allowMounted_ ? &mounts : nullptr, &partitions);
    const bool newWarning = state_ != State::Checking && partitionCount_ <= 1 && partitions > 1;
    if (problem.isEmpty() && newWarning)
        problem = tr("The device now contains multiple partitions. Start again to review the warnings.");
    if (state_ == State::Checking) partitionCount_ = partitions;
    if (!problem.isEmpty())
    {
        fail(problem);
        return;
    }
    if (state_ == State::Checking)
        setState(State::Prepared);
    else if (state_ == State::Rechecking)
        setState(State::Ready);
}

void UDisksTarget::recheck()
{
    if (state_ == State::Prepared)
        snapshot(State::Rechecking);
}

void UDisksTarget::cancel()
{
    if (state_ == State::Cancelled || state_ == State::Failed)
        return;
    discardPending();
    setState(State::Cancelled);
}

void UDisksTarget::ownerChanged(const QString&, const QString&, const QString&)
{
    if (state_ != State::Idle && state_ != State::Cancelled && state_ != State::Failed)
        fail(tr("The UDisks service changed. Prepare a new write plan."));
}

void UDisksTarget::busDisconnected()
{
    if (state_ != State::Cancelled && state_ != State::Failed)
        fail(tr("D-Bus disconnected during target checking."));
}

} // namespace rufus
