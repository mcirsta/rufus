// SPDX-License-Identifier: GPL-3.0-or-later
#include "udisks_target.h"

#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <fcntl.h>

namespace rufus
{
namespace
{
const QString service = QStringLiteral("org.freedesktop.UDisks2");
const QString root = QStringLiteral("/org/freedesktop/UDisks2");
constexpr int requestTimeoutMs = 5000;
}

UDisksTarget::UDisksTarget(const QDBusConnection& connection, const RawWritePlan& plan,
    int authorizationTimeout, QObject* parent)
    : QObject(parent), connection_(connection), plan_(plan),
      authorizationTimeout_(authorizationTimeout > 0 ? authorizationTimeout : DefaultAuthorizationTimeoutMs)
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
    descriptor_ = {};
    problem_ = problem;
    setState(State::Failed);
}

void UDisksTarget::check()
{
    if (state_ != State::Idle)
        return;
    const bool passesDescriptors = connection_.connectionCapabilities() & QDBusConnection::UnixFileDescriptorPassing;
    if (!connection_.isConnected() || !watchesDisconnect_ || !passesDescriptors)
    {
        fail(tr("D-Bus with Unix descriptor passing is required for target access."));
        return;
    }
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
    // owner-change notifications invalidate pending acquisition.
    QString problem = plan_.checkSnapshot(reply.value(), plan_.target().serviceOwner);
    if (problem.isEmpty() && state_ == State::Rechecking)
        problem = CheckRawTargetDescriptor(descriptor_.fileDescriptor(), plan_);
    if (!problem.isEmpty())
    {
        fail(problem);
        return;
    }
    if (state_ == State::Checking)
        setState(State::Prepared);
    else if (state_ == State::BeforeAuthorization)
        open();
    else if (state_ == State::Rechecking)
        setState(State::Ready);
}

void UDisksTarget::authorize()
{
    if (state_ == State::Prepared)
        snapshot(State::BeforeAuthorization);
}

void UDisksTarget::open()
{
    QDBusMessage request = QDBusMessage::createMethodCall(plan_.target().serviceOwner,
        plan_.target().objectPath, QStringLiteral("org.freedesktop.UDisks2.Block"),
        QStringLiteral("OpenDevice"));
    const QVariantMap options = {
        {QStringLiteral("flags"), int(O_EXCL | O_SYNC | O_CLOEXEC)},
        {QStringLiteral("auth.no_user_interaction"), false}
    };
    request.setArguments({QStringLiteral("rw"), options});
    request.setInteractiveAuthorizationAllowed(true);
    state_ = State::Authorizing;
    pending_ = new QDBusPendingCallWatcher(connection_.asyncCall(request, authorizationTimeout_), this);
    connect(pending_, &QDBusPendingCallWatcher::finished, this, &UDisksTarget::openFinished);
    emit stateChanged(state_);
}

void UDisksTarget::openFinished(QDBusPendingCallWatcher* watcher)
{
    if (watcher != pending_)
        return;
    pending_ = nullptr;
    watcher->deleteLater();
    const QDBusPendingReply<QDBusUnixFileDescriptor> reply = *watcher;
    if (reply.isError())
    {
        const QString name = reply.error().name();
        if (name.endsWith(QStringLiteral("NotAuthorizedDismissed")))
            cancel();
        else
            fail(tr("Target authorization/open failed: %1").arg(reply.error().message()));
        return;
    }
    descriptor_ = reply.value();
    if (!descriptor_.isValid())
    {
        fail(tr("UDisks returned no usable target descriptor."));
        return;
    }
    snapshot(State::Rechecking);
}

void UDisksTarget::cancel()
{
    if (state_ == State::Cancelled || state_ == State::Failed)
        return;
    // OpenDevice has no RPC cancellation API. Discarding this call prevents
    // late replies from starting work; the system prompt may remain briefly.
    discardPending();
    descriptor_ = {};
    setState(State::Cancelled);
}

QDBusUnixFileDescriptor UDisksTarget::takeDescriptor()
{
    if (state_ != State::Ready)
        return {};
    QDBusUnixFileDescriptor result = descriptor_;
    descriptor_ = {};
    return result;
}

void UDisksTarget::ownerChanged(const QString&, const QString&, const QString&)
{
    if (state_ != State::Idle && state_ != State::Cancelled && state_ != State::Failed)
        fail(tr("The UDisks service changed. Prepare a new write plan."));
}

void UDisksTarget::busDisconnected()
{
    if (state_ != State::Cancelled && state_ != State::Failed)
        fail(tr("D-Bus disconnected during target acquisition."));
}

} // namespace rufus
