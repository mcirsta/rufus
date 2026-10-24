// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "device.h"
#include "device_policy.h"

#include <QDBusConnection>
#include <QDBusObjectPath>
#include <QMap>
#include <QFutureWatcher>
#include <QObject>
#include <QSet>
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

// Display metadata from already mounted filesystems. No mount, authorization or
// device-node access. Call in a worker; failed/unmounted labels use IdLabel.
QList<Device> ReadUDisksDeviceContent(const UDisksObjects& objects, QList<Device> devices);

// Decode eligibility and identity with a fresh native whole-disk snapshot for discovery
// and access checks. Does not assign a selection ID/service owner. The shared
// HDD/card filter controls the default list; opt-in bypasses only that filter.
bool DescribeUDisksDevice(const UDisksObjects& objects, const QString& path, Device& device,
    bool allowFixedUsb = false, DeviceAccess access = DeviceAccess::Write, quint32 classes = DefaultDeviceClasses);
// Shared current-use checks for raw writing and preparation. Zero sourceDevice
// means this operation has no source image; mounted/layered targets still fail.
QString CheckUDisksTargetUse(const UDisksObjects& objects, const Device& target, quint64 sourceDevice, bool allowFixedUsb = false, DeviceAccess access = DeviceAccess::Write);
// Read-only candidate list for a future authorized unmount step. On any failure,
// mountedFilesystems is empty. This metadata check is not permission to unmount:
// the caller still needs fresh identity, kernel mount/source/holder checks and
// exclusive acquisition after unmounting. Paths are UDisks objects, not mountpoints.
QString ListUDisksTargetMounts(const UDisksObjects& objects, const Device& target,
    quint64 sourceDevice, QStringList& mountedFilesystems, bool allowFixedUsb = false, DeviceAccess access = DeviceAccess::Write);
struct NativeTargetMounts;
// Match a freshly checked native inventory to the same UDisks snapshot used for
// eligibility. The caller supplies the original disk sequence and validates the
// service owner/source descriptor. No unmount or permission request is made.
QString MatchUDisksTargetMounts(const UDisksObjects& objects, const Device& target,
    quint64 sourceDevice, quint64 diskSequence, const NativeTargetMounts& native, bool allowFixedUsb = false, DeviceAccess access = DeviceAccess::Write);

// Read-only discovery. The caller must explicitly supply the bus connection.
class UDisksClient : public QObject
{
    Q_OBJECT

public:
    explicit UDisksClient(const QDBusConnection& connection, QObject* parent = nullptr);
    void start();
    using ExclusionsReader = DeviceExclusions (*)();
    void setExclusions(const DeviceExclusions& exclusions) { exclusions_ = exclusions; exclusionsReader_ = nullptr; }
    void setExclusionsReader(ExclusionsReader reader) { exclusionsReader_ = reader; }

public slots:
    void setAllowFixedUsb(bool enabled);
    void setDeviceClasses(quint32 classes);
    void setUsbDebug(bool enabled);
    void rescan() { refreshTimer_.stop(); refresh(); }

signals:
    void logMessage(QString message);
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
    void contentFinished();

private:
    DeviceExclusions exclusions_;
    ExclusionsReader exclusionsReader_ = nullptr;
    struct Record
    {
        Device device;
        QStringList identity;
    };

    void scheduleRefresh();
    void invalidateObject(const QDBusObjectPath& path);
    void publishSnapshot();
    void startContentRead();
    void reportFailure(const QString& problem);

    QDBusConnection connection_;
    QTimer refreshTimer_;
    QDBusPendingCallWatcher* pending_ = nullptr;
    UDisksObjects objects_;
    QString serviceOwner_;
    QMap<QDBusObjectPath, Record> records_;
    const QSet<quint64> applicationDevices_;
    QFutureWatcher<QList<Device>> contentWatcher_;
    UDisksObjects contentObjects_;
    QList<Device> contentDevices_;
    quint64 contentRevision_ = 0;
    quint64 readingRevision_ = 0;
    bool contentPending_ = false;
    bool contentReading_ = false;
    quint64 revision_ = 0;
    quint64 requestedRevision_ = 0;
    quint64 nextIdentity_ = 0;
    bool started_ = false;
    bool usbDebug_ = false;
    bool allowFixedUsb_ = false;
    quint32 deviceClasses_ = DefaultDeviceClasses;
};

} // namespace rufus

Q_DECLARE_METATYPE(rufus::UDisksInterfaces)
Q_DECLARE_METATYPE(rufus::UDisksObjects)
