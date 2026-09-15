// SPDX-License-Identifier: GPL-3.0-or-later
#include "udisks_client.h"

#include <QDBusArgument>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QFile>

#include <limits>

namespace rufus
{
namespace
{

const QString service = QStringLiteral("org.freedesktop.UDisks2");
const QString rootPath = QStringLiteral("/org/freedesktop/UDisks2");
const QString managerInterface = QStringLiteral("org.freedesktop.DBus.ObjectManager");
const QString blockInterface = QStringLiteral("org.freedesktop.UDisks2.Block");
const QString driveInterface = QStringLiteral("org.freedesktop.UDisks2.Drive");
const QString partitionInterface = QStringLiteral("org.freedesktop.UDisks2.Partition");

QString objectPath(const QVariant& value)
{
    return qdbus_cast<QDBusObjectPath>(value).path();
}

QString devicePath(const QVariant& value)
{
    QByteArray bytes = qdbus_cast<QByteArray>(value);
    if (!bytes.endsWith('\0'))
        return {};
    bytes.chop(1);
    if (!bytes.startsWith("/dev/") || bytes.contains('\0') || bytes.contains("/../"))
        return {};
    return QFile::decodeName(bytes);
}

bool eligible(const UDisksInterfaces& interfaces, const QVariantMap& block,
    const QVariantMap& drive)
{
    // A conservative initial filter, not a guarantee that a target is safe to erase.
    const quint64 size = block.value(QStringLiteral("Size")).toULongLong();
    return !interfaces.contains(partitionInterface)
        && drive.value(QStringLiteral("ConnectionBus")).toString() == QStringLiteral("usb")
        && drive.value(QStringLiteral("Removable")).toBool()
        && drive.value(QStringLiteral("MediaRemovable")).toBool()
        && drive.value(QStringLiteral("MediaAvailable")).toBool()
        && !drive.value(QStringLiteral("Optical"), true).toBool()
        && !block.value(QStringLiteral("ReadOnly"), true).toBool()
        && !block.value(QStringLiteral("HintSystem"), true).toBool()
        && !block.value(QStringLiteral("HintIgnore"), true).toBool()
        && size > 0 && size <= quint64(std::numeric_limits<qint64>::max());
}

QStringList deviceIdentity(const QVariantMap& block, const QVariantMap& drive,
    const Device& device)
{
    return {device.path, QString::number(device.size),
        objectPath(block.value(QStringLiteral("Drive"))),
        block.value(QStringLiteral("Id")).toString(),
        block.value(QStringLiteral("DeviceNumber")).toString(),
        drive.value(QStringLiteral("Serial")).toString(),
        drive.value(QStringLiteral("WWN")).toString(),
        drive.value(QStringLiteral("TimeDetected")).toString(),
        drive.value(QStringLiteral("TimeMediaDetected")).toString()};
}

} // namespace

bool DescribeUDisksDevice(const UDisksObjects& objects, const QString& path, Device& device)
{
    device = {};
    const UDisksInterfaces interfaces = objects.value(QDBusObjectPath(path));
    const QVariantMap block = interfaces.value(blockInterface);
    const QString drivePath = objectPath(block.value(QStringLiteral("Drive")));
    if (drivePath.isEmpty() || drivePath == QStringLiteral("/"))
        return false;
    const QVariantMap drive = objects.value(QDBusObjectPath(drivePath)).value(driveInterface);
    if (!eligible(interfaces, block, drive))
        return false;
    device.path = devicePath(block.value(QStringLiteral("Device")));
    if (device.path.isEmpty())
        return false;
    device.name = (drive.value(QStringLiteral("Vendor")).toString() + QLatin1Char(' ')
        + drive.value(QStringLiteral("Model")).toString()).simplified();
    if (device.name.isEmpty())
        device.name = UDisksClient::tr("USB device");
    device.size = block.value(QStringLiteral("Size")).toULongLong();
    device.removable = true;
    device.objectPath = path;
    device.drivePath = drivePath;
    device.number = block.value(QStringLiteral("DeviceNumber")).toULongLong();
    device.identity = deviceIdentity(block, drive, device);
    return true;
}

UDisksClient::UDisksClient(const QDBusConnection& connection, QObject* parent)
    : QObject(parent), connection_(connection)
{
    qDBusRegisterMetaType<UDisksInterfaces>();
    qDBusRegisterMetaType<UDisksObjects>();
    refreshTimer_.setSingleShot(true);
    connect(&refreshTimer_, &QTimer::timeout, this, &UDisksClient::refresh);
}

void UDisksClient::start()
{
    if (started_)
        return;
    started_ = true;
    if (!connection_.isConnected())
    {
        reportFailure(tr("Cannot connect to D-Bus. Device discovery unavailable."));
        return;
    }

    auto* watcher = new QDBusServiceWatcher(service, connection_,
        QDBusServiceWatcher::WatchForOwnerChange, this);
    connect(watcher, &QDBusServiceWatcher::serviceOwnerChanged,
        this, &UDisksClient::ownerChanged);
    const bool added = connection_.connect(service, rootPath, managerInterface,
        QStringLiteral("InterfacesAdded"), this,
        SLOT(interfacesAdded(QDBusObjectPath,rufus::UDisksInterfaces)));
    const bool removed = connection_.connect(service, rootPath, managerInterface,
        QStringLiteral("InterfacesRemoved"), this,
        SLOT(interfacesRemoved(QDBusObjectPath,QStringList)));
    const bool changed = connection_.connect(service, QString(),
        QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("PropertiesChanged"),
        this, SLOT(propertiesChanged(QString,QVariantMap,QStringList,QDBusMessage)));
    const bool disconnected = connection_.connect(QString(),
        QStringLiteral("/org/freedesktop/DBus/Local"), QStringLiteral("org.freedesktop.DBus.Local"),
        QStringLiteral("Disconnected"), this, SLOT(busDisconnected()));
    if (!added || !removed || !changed || !disconnected)
    {
        reportFailure(tr("Cannot watch UDisks2 device changes. Device discovery unavailable."));
        return;
    }
    refresh();
}

void UDisksClient::refresh()
{
    // Dropping the watcher disconnects obsolete replies; it does not cancel the service call.
    delete pending_;
    QDBusMessage request = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.DBus"),
        QStringLiteral("/org/freedesktop/DBus"), QStringLiteral("org.freedesktop.DBus"), QStringLiteral("StartServiceByName"));
    request.setArguments({service, uint(0)});
    pending_ = new QDBusPendingCallWatcher(connection_.asyncCall(request, 5000), this);
    requestedRevision_ = revision_;
    connect(pending_, &QDBusPendingCallWatcher::finished, this, &UDisksClient::activationFinished);
}

void UDisksClient::activationFinished(QDBusPendingCallWatcher* watcher)
{
    if (watcher != pending_)
        return;
    pending_ = nullptr;
    watcher->deleteLater();
    if (requestedRevision_ != revision_)
        return;
    // Preserve D-Bus activation, then pin the snapshot to the resolved owner.
    // An already running service need not have an activation file on this bus;
    // GetNameOwner is the final availability check even if activation failed.
    QDBusMessage request = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.DBus"),
        QStringLiteral("/org/freedesktop/DBus"), QStringLiteral("org.freedesktop.DBus"), QStringLiteral("GetNameOwner"));
    request.setArguments({service});
    pending_ = new QDBusPendingCallWatcher(connection_.asyncCall(request, 5000), this);
    connect(pending_, &QDBusPendingCallWatcher::finished, this, &UDisksClient::ownerFinished);
}

void UDisksClient::ownerFinished(QDBusPendingCallWatcher* watcher)
{
    if (watcher != pending_)
        return;
    pending_ = nullptr;
    watcher->deleteLater();
    if (requestedRevision_ != revision_)
        return;
    const QDBusPendingReply<QString> reply = *watcher;
    if (reply.isError() || !reply.value().startsWith(QLatin1Char(':')))
    {
        reportFailure(tr("Cannot identify the UDisks service owner. Device discovery unavailable."));
        refreshTimer_.start(2000);
        return;
    }
    serviceOwner_ = reply.value();
    const QDBusMessage request = QDBusMessage::createMethodCall(serviceOwner_, rootPath,
        managerInterface, QStringLiteral("GetManagedObjects"));
    pending_ = new QDBusPendingCallWatcher(connection_.asyncCall(request, 5000), this);
    connect(pending_, &QDBusPendingCallWatcher::finished, this, &UDisksClient::requestFinished);
}

void UDisksClient::requestFinished(QDBusPendingCallWatcher* watcher)
{
    if (watcher != pending_)
        return;
    pending_ = nullptr;
    watcher->deleteLater();
    if (requestedRevision_ != revision_)
        return;

    const QDBusPendingReply<UDisksObjects> reply = *watcher;
    if (reply.isError())
    {
        reportFailure(tr("UDisks2 discovery unavailable: %1").arg(reply.error().message()));
        refreshTimer_.start(2000);
        return;
    }
    objects_ = reply.value();
    publishSnapshot();
}

void UDisksClient::scheduleRefresh()
{
    ++revision_;
    // Coalesce a service's burst of property and interface notifications.
    if (!refreshTimer_.isActive() || refreshTimer_.remainingTime() > 25)
        refreshTimer_.start(25);
}

void UDisksClient::ownerChanged(const QString&, const QString& oldOwner, const QString& newOwner)
{
    ++revision_;
    delete pending_;
    pending_ = nullptr;
    refreshTimer_.stop();
    if (!oldOwner.isEmpty())
        reportFailure(tr("UDisks2 service changed or stopped. Device discovery unavailable."));
    if (!newOwner.isEmpty())
        refresh();
}

void UDisksClient::busDisconnected()
{
    ++revision_;
    delete pending_;
    pending_ = nullptr;
    refreshTimer_.stop();
    reportFailure(tr("D-Bus connection lost. Restart Rufus to resume device discovery."));
}

void UDisksClient::invalidateObject(const QDBusObjectPath& path)
{
    if (objects_.remove(path) > 0)
        publishSnapshot();
}

void UDisksClient::interfacesAdded(const QDBusObjectPath& path, const UDisksInterfaces&)
{
    invalidateObject(path);
    scheduleRefresh();
}

void UDisksClient::interfacesRemoved(const QDBusObjectPath& path, const QStringList&)
{
    invalidateObject(path);
    scheduleRefresh();
}

void UDisksClient::propertiesChanged(const QString& interface, const QVariantMap& changed,
    const QStringList& invalidated, const QDBusMessage& message)
{
    if (interface != blockInterface && interface != driveInterface)
        return;

    QStringList properties = changed.keys();
    properties.append(invalidated);
    // Display-only updates may keep the user's selection. Other updates require
    // a new snapshot and selection, including media changes on a card reader.
    properties.removeAll(QStringLiteral("Vendor"));
    properties.removeAll(QStringLiteral("Model"));
    properties.removeAll(QStringLiteral("HintName"));
    properties.removeAll(QStringLiteral("HintIconName"));
    properties.removeAll(QStringLiteral("HintSymbolicIconName"));
    if (!properties.isEmpty())
        invalidateObject(QDBusObjectPath(message.path()));
    scheduleRefresh();
}

void UDisksClient::publishSnapshot()
{
    QMap<QDBusObjectPath, Record> current;
    QList<Device> devices;
    for (auto object = objects_.cbegin(); object != objects_.cend(); ++object)
    {
        Record record;
        if (!DescribeUDisksDevice(objects_, object.key().path(), record.device))
            continue;
        record.device.serviceOwner = serviceOwner_;
        record.identity = record.device.identity;
        const auto previous = records_.constFind(object.key());
        if (previous != records_.cend() && previous->identity == record.identity)
            record.device.id = previous->device.id;
        else
            record.device.id = QStringLiteral("udisks-%1").arg(++nextIdentity_);
        devices.append(record.device);
        current.insert(object.key(), record);
    }
    records_ = current;
    emit devicesChanged(devices, QString());
}

void UDisksClient::reportFailure(const QString& problem)
{
    objects_.clear();
    serviceOwner_.clear();
    records_.clear();
    emit devicesChanged({}, problem);
}

} // namespace rufus
