// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "device.h"

#include <QDBusConnection>
#include <QDBusObjectPath>
#include <QMap>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <QVariantMap>

class QDBusMessage;
class QDBusPendingCallWatcher;

namespace rufus
{

// The ObjectManager wire format: a{oa{sa{sv}}}.
using UDisksInterfaces = QMap<QString, QVariantMap>;
using UDisksObjects = QMap<QDBusObjectPath, UDisksInterfaces>;

// Decode the same conservative device eligibility and identity for discovery
// and fresh write-plan snapshots. Does not assign a selection ID/service owner.
bool DescribeUDisksDevice(const UDisksObjects& objects, const QString& path, Device& device);

// Read-only discovery. The caller must explicitly supply the bus connection.
class UDisksClient : public QObject
{
    Q_OBJECT

public:
    explicit UDisksClient(const QDBusConnection& connection, QObject* parent = nullptr);
    void start();

signals:
    void devicesChanged(QList<rufus::Device> devices, QString problem);

private slots:
    void refresh();
    void activationFinished(QDBusPendingCallWatcher* watcher);
    void ownerFinished(QDBusPendingCallWatcher* watcher);
    void requestFinished(QDBusPendingCallWatcher* watcher);
    void ownerChanged(const QString& service, const QString& oldOwner, const QString& newOwner);
    void busDisconnected();
    void interfacesAdded(const QDBusObjectPath& path, const rufus::UDisksInterfaces& interfaces);
    void interfacesRemoved(const QDBusObjectPath& path, const QStringList& interfaces);
    void propertiesChanged(const QString& interface, const QVariantMap& changed,
        const QStringList& invalidated, const QDBusMessage& message);

private:
    struct Record
    {
        Device device;
        QStringList identity;
    };

    void scheduleRefresh();
    void invalidateObject(const QDBusObjectPath& path);
    void publishSnapshot();
    void reportFailure(const QString& problem);

    QDBusConnection connection_;
    QTimer refreshTimer_;
    QDBusPendingCallWatcher* pending_ = nullptr;
    UDisksObjects objects_;
    QString serviceOwner_;
    QMap<QDBusObjectPath, Record> records_;
    quint64 revision_ = 0;
    quint64 requestedRevision_ = 0;
    quint64 nextIdentity_ = 0;
    bool started_ = false;
};

} // namespace rufus

Q_DECLARE_METATYPE(rufus::UDisksInterfaces)
Q_DECLARE_METATYPE(rufus::UDisksObjects)
