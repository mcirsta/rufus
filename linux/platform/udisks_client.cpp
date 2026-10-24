// SPDX-License-Identifier: GPL-3.0-or-later
#include "udisks_client.h"
#include "target_mounts.h"
#include "native_disk_facts.h"
#include "device_exclusions.h"
#include "application_devices.h"

#include <QDBusArgument>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QFile>
#include <QSet>
#include <QtConcurrentRun>

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
const QString partitionTableInterface = QStringLiteral("org.freedesktop.UDisks2.PartitionTable");
const QString filesystemInterface = QStringLiteral("org.freedesktop.UDisks2.Filesystem");
constexpr int requestTimeoutMs = 5000;
constexpr int retryIntervalMs = 2000;
constexpr int refreshDelayMs = 25;

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
    const QVariantMap& drive, DeviceAccess access)
{
    // A conservative initial filter, not a guarantee that a target is safe to erase.
    if (interfaces.contains(partitionInterface))
        return false;
    const quint64 size = block.value(QStringLiteral("Size")).toULongLong();
    const QVariant removable = drive.value(QStringLiteral("Removable"));
    const QVariant mediaRemovable = drive.value(QStringLiteral("MediaRemovable"));
    const bool knownFlags = removable.metaType().id() == QMetaType::Bool
        && mediaRemovable.metaType().id() == QMetaType::Bool;
    const bool supportedSize = size > 0 && size <= quint64(std::numeric_limits<qint64>::max());
    const bool knownSystemHint = block.value(QStringLiteral("HintSystem")).metaType().id() == QMetaType::Bool;
    if (!knownFlags || !supportedSize || !knownSystemHint)
        return false;
    const QVariant readOnly = block.value(QStringLiteral("ReadOnly"));
    const bool accessAllowed = readOnly.metaType() == QMetaType::fromType<bool>()
        && (access == DeviceAccess::Read || !readOnly.toBool());
    const bool availableMedia = drive.value(QStringLiteral("MediaAvailable")).toBool()
        && (!drive.value(QStringLiteral("Optical"), true).toBool() || access == DeviceAccess::Read) && accessAllowed;
    const bool excluded = block.value(QStringLiteral("HintIgnore"), true).toBool();
    return availableMedia && !excluded;
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

static QString checkTargetMounts(const QVariantMap& filesystem, const QVariantMap& block,
    bool& mounted, DeviceAccess access, bool nativeSystemCheck)
{
    mounted = false;
    const QVariant value = filesystem.value(QStringLiteral("MountPoints"));
    bool valid = value.metaType() == QMetaType::fromType<QList<QByteArray>>();
    if (value.metaType() == QMetaType::fromType<QDBusArgument>())
        valid = value.value<QDBusArgument>().currentSignature() == QStringLiteral("aay");
    if (!valid)
        return QStringLiteral("Cannot determine the target's mount state.");
    const QList<QByteArray> paths = qdbus_cast<QList<QByteArray>>(value);
    if (paths.isEmpty())
        return {};
    // A mounted volume needs explicit eligibility, including child partitions.
    for (const char* property : {"HintSystem", "HintIgnore", "ReadOnly"})
    {
        const QVariant flag = block.value(QLatin1String(property));
        const bool readFlag = QLatin1String(property) == QLatin1String("ReadOnly");
        const bool systemHint = QLatin1String(property) == QLatin1String("HintSystem");
        const bool enforcedFlag = (!systemHint || !nativeSystemCheck) && (!readFlag || access == DeviceAccess::Write);
        const bool protectedFlag = flag.toBool() && enforcedFlag;
        if (flag.metaType() != QMetaType::fromType<bool>() || protectedFlag)
            return QStringLiteral("The mounted target volume is protected or its state is unknown.");
    }
    bool validNumber = false;
    const quint64 number = block.value(QStringLiteral("DeviceNumber")).toULongLong(&validNumber);
    if (!validNumber || number == 0)
        return QStringLiteral("Cannot identify the mounted target volume.");
    for (QByteArray path : paths)
    {
        if (!path.endsWith('\0'))
            return QStringLiteral("The target has an invalid mountpoint.");
        path.chop(1);
        const bool absolutePath = path.startsWith('/') && !path.contains('\0');
        const QList<QByteArray> components = path.split('/');
        const bool cleanPath = !components.contains(".") && !components.contains("..") && !path.contains("//");
        if (!absolutePath || !cleanPath)
            return QStringLiteral("The target has an invalid mountpoint.");
        // Other system/source ancestry needs the kernel mount graph, not a
        // blacklist of directory names. This is only the metadata precheck.
        if (path == "/")
            return QStringLiteral("The target contains a system mount.");
    }
    mounted = true;
    return {};
}

QString ListUDisksTargetMounts(const UDisksObjects& objects, const Device& target,
    quint64 sourceDevice, QStringList& mountedFilesystems, bool allowFixedUsb, DeviceAccess access)
{
    mountedFilesystems.clear();
    Device current;
    if (!DescribeUDisksDevice(objects, target.objectPath, current, allowFixedUsb, access, DeviceClassMask(target.deviceClass)))
        return QStringLiteral("The selected target is no longer eligible.");
    const bool sameDevice = current.number != 0 && current.number == target.number && current.path == target.path;
    const bool sameMedia = current.size == target.size && current.identity == target.identity;
    if (!sameDevice || !sameMedia)
        return QStringLiteral("The selected target changed. Prepare a new write plan.");
    if (current.deviceClass == DeviceClass::Optical && access == DeviceAccess::Read) return {};
    QStringList mounts;
    QSet<QString> members{target.objectPath};
    for (auto object = objects.cbegin(); object != objects.cend(); ++object)
    {
        const QVariantMap block = object->value(blockInterface);
        const QVariantMap partition = object->value(partitionInterface);
        if ((!current.drivePath.isEmpty() && current.drivePath != QStringLiteral("/")
                && objectPath(block.value(QStringLiteral("Drive"))) == current.drivePath)
            || objectPath(partition.value(QStringLiteral("Table"))) == target.objectPath)
            members.insert(object.key().path());
    }
    for (auto object = objects.cbegin(); object != objects.cend(); ++object)
    {
        const UDisksInterfaces& interfaces = object.value();
        const QVariantMap block = interfaces.value(blockInterface);
        if (members.contains(objectPath(block.value(QStringLiteral("CryptoBackingDevice")))))
            return QStringLiteral("The target has active layered storage.");
        if (!members.contains(object.key().path()))
            continue;
        if (sourceDevice != 0 && block.value(QStringLiteral("DeviceNumber")).toULongLong() == sourceDevice)
            return QStringLiteral("The source image is stored on the selected target.");
        const bool nativeSystemCheck = current.deviceClass != DeviceClass::Usb;
        const bool protectedMember = (!nativeSystemCheck && block.value(QStringLiteral("HintSystem")).toBool())
            || block.value(QStringLiteral("HintIgnore")).toBool();
        if (protectedMember)
            return QStringLiteral("The target contains a protected volume.");
        if (interfaces.contains(QStringLiteral("org.freedesktop.UDisks2.Swapspace")))
            return QStringLiteral("Swap targets are not supported by this first raw-writing path.");
        const QString usage = block.value(QStringLiteral("IdUsage")).toString();
        const bool encrypted = interfaces.contains(QStringLiteral("org.freedesktop.UDisks2.Encrypted"))
            || usage == QStringLiteral("crypto");
        const bool layered = interfaces.contains(QStringLiteral("org.freedesktop.UDisks2.PhysicalVolume"))
            || usage == QStringLiteral("raid");
        if (encrypted || layered)
            return QStringLiteral("Encrypted and layered targets are not supported by this raw-writing path.");
        for (const char* property : {"MDRaid", "MDRaidMember", "CryptoBackingDevice"})
        {
            const QString relation = objectPath(block.value(QLatin1String(property)));
            if (!relation.isEmpty() && relation != QStringLiteral("/"))
                return QStringLiteral("Layered targets are not supported by this raw-writing path.");
        }
        if (!interfaces.contains(filesystemInterface))
            continue;
        bool mounted = false;
        const QString problem = checkTargetMounts(interfaces.value(filesystemInterface), block, mounted, access, nativeSystemCheck);
        if (!problem.isEmpty())
            return problem;
        if (!mounted)
            continue;
        const bool wholeDisk = object.key().path() == target.objectPath;
        const bool targetPartition = objectPath(interfaces.value(partitionInterface)
            .value(QStringLiteral("Table"))) == target.objectPath;
        if (!wholeDisk && !targetPartition)
            return QStringLiteral("A mounted volume cannot be associated with the selected disk.");
        mounts.append(object.key().path());
    }
    mountedFilesystems = mounts;
    return {};
}

QString MatchUDisksTargetMounts(const UDisksObjects& objects, const Device& target,
    quint64 sourceDevice, quint64 diskSequence, const NativeTargetMounts& native, bool allowFixedUsb, DeviceAccess access)
{
    QStringList candidates;
    const QString problem = ListUDisksTargetMounts(objects, target, sourceDevice, candidates, allowFixedUsb, access);
    if (!problem.isEmpty())
        return problem;
    const bool sameDisk = native.device == target.number;
    const bool sameSequence = diskSequence != 0 && native.diskSequence == diskSequence;
    if (!sameDisk || !sameSequence)
        return QStringLiteral("The selected target changed. Prepare a new write plan.");
    if (candidates.size() != native.volumes.size())
        return QStringLiteral("The target's mount state differs between UDisks and the kernel.");
    QSet<quint64> devices;
    QSet<int> mountIds;
    QSet<QByteArray> paths;
    for (const NativeTargetMount& volume : native.volumes)
    {
        const quint64 storage = volume.backing != 0 ? volume.backing : volume.device;
        const bool duplicate = devices.contains(storage) || mountIds.contains(volume.id)
            || paths.contains(volume.path);
        if (volume.device == 0 || volume.id <= 0 || duplicate)
            return QStringLiteral("The native target mount inventory is ambiguous.");
        devices.insert(storage);
        mountIds.insert(volume.id);
        paths.insert(volume.path);
    }
    for (const QString& candidate : candidates)
    {
        const UDisksInterfaces interfaces = objects.value(QDBusObjectPath(candidate));
        const quint64 device = interfaces.value(blockInterface).value(QStringLiteral("DeviceNumber")).toULongLong();
        const QList<QByteArray> mounts = qdbus_cast<QList<QByteArray>>(
            interfaces.value(filesystemInterface).value(QStringLiteral("MountPoints")));
        // The native adapter rejects bind aliases and multiple mounts of one
        // filesystem. Do not silently select one of several service mountpoints.
        if (mounts.size() != 1 || !devices.remove(device))
            return QStringLiteral("The target's mounted volumes cannot be matched to the kernel.");
        QByteArray path = mounts.first();
        path.chop(1); // ListUDisksTargetMounts validated the terminating NUL.
        bool matched = false;
        for (const NativeTargetMount& volume : native.volumes)
            if ((volume.backing != 0 ? volume.backing : volume.device) == device && volume.path == path)
                matched = true;
        if (!matched)
            return QStringLiteral("The target's mountpoints differ between UDisks and the kernel.");
    }
    return {};
}

QString CheckUDisksTargetUse(const UDisksObjects& objects, const Device& target, quint64 sourceDevice, bool allowFixedUsb, DeviceAccess access)
{
    QStringList mounts;
    const QString problem = ListUDisksTargetMounts(objects, target, sourceDevice, mounts, allowFixedUsb, access);
    if (!problem.isEmpty())
        return problem;
    if (!mounts.isEmpty())
        return QStringLiteral("Unmount the target's filesystems before preparing the write.");
    return {};
}

static QVariantMap existingFileSystem(const UDisksObjects& objects, const QString& path)
{
    const QVariantMap wholeDisk = objects.value(QDBusObjectPath(path)).value(blockInterface);
    if (wholeDisk.value(QStringLiteral("IdUsage")).toString() == QStringLiteral("filesystem"))
        return wholeDisk;

    quint64 firstOffset = std::numeric_limits<quint64>::max();
    QVariantMap firstFileSystem;
    for (const UDisksInterfaces& interfaces : objects)
    {
        const QVariantMap partition = interfaces.value(partitionInterface);
        if (objectPath(partition.value(QStringLiteral("Table"))) != path)
            continue;
        const QVariantMap block = interfaces.value(blockInterface);
        const bool filesystem = block.value(QStringLiteral("IdUsage")).toString() == QStringLiteral("filesystem");
        if (!filesystem || partition.value(QStringLiteral("IsContainer")).toBool())
            continue;
        bool validOffset = false;
        const quint64 offset = partition.value(QStringLiteral("Offset")).toULongLong(&validOffset);
        if (!validOffset || offset >= firstOffset)
            continue;
        firstOffset = offset;
        firstFileSystem = block;
    }
    return firstFileSystem;
}

// Match GetDrivePartitionData's warning count, including extended containers.
// Unknown metadata counts as a partition rather than suppressing a warning.
static int warningPartitionCount(const UDisksObjects& objects, const QString& path, const QString& table)
{
    constexpr uint unusedMbrType = 0x00;
    constexpr uint rufusExtraMbrType = 0xea;
    constexpr uint efiMbrType = 0xef;
    const QString efiGptType = QStringLiteral("c12a7328-f81f-11d2-ba4b-00a0c93ec93b");
    const QString reservedGptType = QStringLiteral("e3c9e316-0b5c-4db8-817d-f92df00215ae");
    int count = 0;
    for (const UDisksInterfaces& interfaces : objects)
    {
        const QVariantMap partition = interfaces.value(partitionInterface);
        if (objectPath(partition.value(QStringLiteral("Table"))) != path)
            continue;
        const QString type = partition.value(QStringLiteral("Type")).toString();
        if (table == QStringLiteral("gpt"))
        {
            const bool auxiliaryType = type.compare(efiGptType, Qt::CaseInsensitive) == 0
                || type.compare(reservedGptType, Qt::CaseInsensitive) == 0;
            const bool uefiNtfs = partition.value(QStringLiteral("Name")).toString() == QStringLiteral("UEFI:NTFS");
            if (auxiliaryType || uefiNtfs)
                continue;
        }
        else if (table == QStringLiteral("dos"))
        {
            bool valid = false;
            const uint mbrType = type.toUInt(&valid, 16);
            if (valid && (mbrType == unusedMbrType || mbrType == rufusExtraMbrType))
                continue;
            const QVariantMap block = interfaces.value(blockInterface);
            const QString label = block.value(QStringLiteral("IdLabel")).toString();
            const bool rufusLabel = label.startsWith(QStringLiteral("UEFI_NTFS"))
                || label.startsWith(QStringLiteral("RUFUS_BOOT"));
            const bool fat = block.value(QStringLiteral("IdType")).toString() == QStringLiteral("vfat");
            if (valid && mbrType == efiMbrType && fat && rufusLabel)
                continue;
        }
        ++count;
    }
    return count;
}

bool DescribeUDisksDevice(const UDisksObjects& objects, const QString& path, Device& device, bool allowFixedUsb, DeviceAccess access, quint32 classes)
{
    device = {};
    const UDisksInterfaces interfaces = objects.value(QDBusObjectPath(path));
    const QVariantMap block = interfaces.value(blockInterface);
    const QString drivePath = objectPath(block.value(QStringLiteral("Drive")));
    NativeDiskFacts facts;
    const quint64 number = block.value(QStringLiteral("DeviceNumber")).toULongLong();
    const quint64 size = block.value(QStringLiteral("Size")).toULongLong();
    if (ReadNativeDiskFacts(number, size, facts) != 0) return false;
    const bool driveLess = drivePath.isEmpty() || drivePath == QStringLiteral("/");
    QVariantMap drive = objects.value(QDBusObjectPath(drivePath)).value(driveInterface);
    if (driveLess) {
        if (facts.deviceClass != DeviceClass::Virtual || facts.backingIdentity.isEmpty()) return false;
        // Positive native file-backed identity replaces only absent Drive facts.
        drive.insert(QStringLiteral("Removable"), false);
        drive.insert(QStringLiteral("MediaRemovable"), false);
        drive.insert(QStringLiteral("MediaAvailable"), true);
        drive.insert(QStringLiteral("Optical"), false);
    }
    const QVariant optical = drive.value(QStringLiteral("Optical"));
    if (optical.metaType().id() != QMetaType::Bool || optical.toBool() != (facts.deviceClass == DeviceClass::Optical)) return false;
    if (!eligible(interfaces, block, drive, access)) return false;
    if (facts.deviceClass == DeviceClass::Optical && size <= 4096) return false;
    if ((classes & DeviceClassMask(facts.deviceClass)) == 0)
        return false;
    const bool usb = drive.value(QStringLiteral("ConnectionBus")).toString() == QStringLiteral("usb");
    if (usb != facts.usbTransport)
        return false;
    if (facts.deviceClass == DeviceClass::Usb && block.value(QStringLiteral("HintSystem"), true).toBool())
        return false;
    const bool nativeRemovable = facts.deviceClass == DeviceClass::NonUsbRemovable;
    if (nativeRemovable && !drive.value(QStringLiteral("Removable")).toBool())
        return false;
    const QString media = drive.value(QStringLiteral("Media")).toString();
    const QStringList compatibility = qdbus_cast<QStringList>(drive.value(QStringLiteral("MediaCompatibility")));
    HDD_SCORE score;
    const bool defaultVisible = EvaluateNativeDiskFilter(facts, media, compatibility, score) == NativeDiskFilter::Include;
    if (!defaultVisible && !allowFixedUsb)
        return false;
    device.path = devicePath(block.value(QStringLiteral("Device")));
    if (device.path.isEmpty())
        return false;
    device.deviceClass = facts.deviceClass;
    device.name = (drive.value(QStringLiteral("Vendor")).toString() + QLatin1Char(' ')
        + drive.value(QStringLiteral("Model")).toString()).simplified();
    if (device.name.isEmpty())
        device.name = facts.usbTransport ? UDisksClient::tr("USB device") : UDisksClient::tr("Disk device");
    device.logicalSectorSize = facts.logicalSectorSize;
    device.physicalSectorSize = facts.physicalSectorSize;
    device.size = block.value(QStringLiteral("Size")).toULongLong();
    device.readOnly = facts.deviceClass == DeviceClass::Optical || block.value(QStringLiteral("ReadOnly")).toBool();
    device.removable = drive.value(QStringLiteral("Removable")).toBool();
    device.mediaRemovable = drive.value(QStringLiteral("MediaRemovable")).toBool();
    device.defaultVisible = defaultVisible;
    device.requiresNativeProtection = facts.deviceClass != DeviceClass::Usb || !device.removable || !device.mediaRemovable
        || facts.mediaRemoval != MediaRemoval::Removable;
    device.objectPath = path;
    device.drivePath = drivePath;
    device.number = block.value(QStringLiteral("DeviceNumber")).toULongLong();
    device.identity = deviceIdentity(block, drive, device);
    // Classification inputs are selection facts, not content metadata. Keep
    // native byte strings lossless; display-name conversion must not hide a change.
    device.identity.append({facts.sysfsPath, QString::number(facts.diskSequence),
        QString::number(facts.logicalSectorSize), QString::number(facts.physicalSectorSize),
        QString::number(int(facts.mediaRemoval)), QString::number(facts.usbIdsKnown),
        QString::number(facts.usbVendor), QString::number(facts.usbProduct),
        QString::fromLatin1(facts.vendor.toHex()), QString::fromLatin1(facts.model.toHex()),
        QString::number(device.removable), QString::number(device.mediaRemovable), media,
        QString::number(DeviceClassMask(device.deviceClass)), QString::number(facts.usbTransport),
        drive.value(QStringLiteral("ConnectionBus")).toString()});
    device.identity.append(compatibility);
    device.identity.append(facts.backingIdentity);
    device.partitionTableType = interfaces.value(partitionTableInterface).value(QStringLiteral("Type")).toString();
    device.partitionCount = warningPartitionCount(objects, path, device.partitionTableType);
    const QVariantMap filesystem = existingFileSystem(objects, path);
    device.volumeLabel = filesystem.value(QStringLiteral("IdLabel")).toString();
    device.fileSystemType = filesystem.value(QStringLiteral("IdType")).toString();
    device.fileSystemVersion = filesystem.value(QStringLiteral("IdVersion")).toString();
    const bool virtualDisk = device.deviceClass == DeviceClass::Virtual || device.deviceClass == DeviceClass::Vmware;
    const bool sandbox = device.volumeLabel == QStringLiteral("PortableBaseLayer");
    const bool antivirus = device.volumeLabel.startsWith(QStringLiteral("Bitdefender"), Qt::CaseInsensitive)
        && device.size <= 32 * 1024 * 1024;
    return !sandbox && !(virtualDisk && antivirus);
}

UDisksClient::UDisksClient(const QDBusConnection& connection, QObject* parent)
    : QObject(parent), connection_(connection), applicationDevices_(NativeApplicationDevices())
{
    qDBusRegisterMetaType<UDisksInterfaces>();
    qDBusRegisterMetaType<UDisksObjects>();
    qDBusRegisterMetaType<QList<QByteArray>>();
    refreshTimer_.setSingleShot(true);
    connect(&refreshTimer_, &QTimer::timeout, this, &UDisksClient::refresh);
    connect(&contentWatcher_, &QFutureWatcher<QList<Device>>::finished, this, &UDisksClient::contentFinished);
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

void UDisksClient::setUsbDebug(bool enabled)
{
    usbDebug_ = enabled;
    if (started_) { refreshTimer_.stop(); refresh(); }
}

void UDisksClient::setAllowFixedUsb(bool enabled)
{
    if (allowFixedUsb_ == enabled)
        return;
    allowFixedUsb_ = enabled;
    ++revision_;
    ++contentRevision_;
    if (!started_)
        return;
    // Remove newly ineligible entries immediately; additions need a fresh snapshot.
    if (!enabled && !serviceOwner_.isEmpty())
        publishSnapshot();
    refreshTimer_.stop();
    refresh();
}

void UDisksClient::refresh()
{
    // Dropping the watcher disconnects obsolete replies; it does not cancel the service call.
    delete pending_;
    QDBusMessage request = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.DBus"),
        QStringLiteral("/org/freedesktop/DBus"), QStringLiteral("org.freedesktop.DBus"), QStringLiteral("StartServiceByName"));
    request.setArguments({service, uint(0)});
    pending_ = new QDBusPendingCallWatcher(connection_.asyncCall(request, requestTimeoutMs), this);
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
    pending_ = new QDBusPendingCallWatcher(connection_.asyncCall(request, requestTimeoutMs), this);
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
        refreshTimer_.start(retryIntervalMs);
        return;
    }
    serviceOwner_ = reply.value();
    const QDBusMessage request = QDBusMessage::createMethodCall(serviceOwner_, rootPath,
        managerInterface, QStringLiteral("GetManagedObjects"));
    pending_ = new QDBusPendingCallWatcher(connection_.asyncCall(request, requestTimeoutMs), this);
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
        refreshTimer_.start(retryIntervalMs);
        return;
    }
    objects_ = reply.value();
    publishSnapshot();
}

void UDisksClient::scheduleRefresh()
{
    ++revision_;
    ++contentRevision_;
    contentPending_ = false;
    contentObjects_.clear();
    contentDevices_.clear();
    // Coalesce a service's burst of property and interface notifications.
    if (!refreshTimer_.isActive() || refreshTimer_.remainingTime() > refreshDelayMs)
        refreshTimer_.start(refreshDelayMs);
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

void UDisksClient::interfacesAdded(const QDBusObjectPath& path, const UDisksInterfaces& interfaces)
{
    const bool identityOrEligibility = interfaces.contains(blockInterface) || interfaces.contains(driveInterface)
        || interfaces.contains(partitionInterface) || interfaces.contains(QStringLiteral("org.freedesktop.UDisks2.Loop"));
    if (identityOrEligibility)
        invalidateObject(path);
    scheduleRefresh();
}

void UDisksClient::interfacesRemoved(const QDBusObjectPath& path, const QStringList& interfaces)
{
    // Replacing existing content can remove these interfaces without unplugging
    // the disk. Block/Drive removal still invalidates the selection immediately.
    QStringList removed = interfaces;
    removed.removeAll(filesystemInterface);
    removed.removeAll(partitionTableInterface);
    if (!removed.isEmpty())
        invalidateObject(path);
    scheduleRefresh();
}

void UDisksClient::propertiesChanged(const QString& interface, const QVariantMap& changed,
    const QStringList& invalidated, const QDBusMessage& message)
{
    if (interface == partitionTableInterface || interface == partitionInterface || interface == filesystemInterface)
    {
        scheduleRefresh();
        return;
    }
    if (interface != blockInterface && interface != driveInterface)
        return;

    QStringList properties = changed.keys();
    properties.append(invalidated);
    const QString readOnlyProperty = QStringLiteral("ReadOnly");
    const QVariant readOnly = changed.value(readOnlyProperty);
    const bool readOnlyChanged = interface == blockInterface
        && readOnly.metaType() == QMetaType::fromType<bool>() && !invalidated.contains(readOnlyProperty);
    if (readOnlyChanged)
    {
        // Access can change without replacing the physical source. Publish a
        // known capability change immediately so pending writes are cancelled,
        // while saves retain the same selection. Unknown state still invalidates.
        objects_[QDBusObjectPath(message.path())][blockInterface][readOnlyProperty] = readOnly;
        properties.removeAll(readOnlyProperty);
    }
    // Display-only updates may keep the user's selection. Other updates require
    // a new snapshot and selection, including media changes on a card reader.
    properties.removeAll(QStringLiteral("Vendor"));
    properties.removeAll(QStringLiteral("Model"));
    properties.removeAll(QStringLiteral("HintName"));
    properties.removeAll(QStringLiteral("HintIconName"));
    properties.removeAll(QStringLiteral("HintSymbolicIconName"));
    // Formatting changes these content facts without replacing the device.
    // Keep its selection identity; write-time use checks query fresh snapshots.
    if (interface == blockInterface)
    {
        for (const char* property : {"IdUsage", "IdType", "IdVersion", "IdLabel", "IdUUID"})
            properties.removeAll(QLatin1String(property));
    }
    if (!properties.isEmpty())
        invalidateObject(QDBusObjectPath(message.path()));
    else if (readOnlyChanged)
        publishSnapshot();
    scheduleRefresh();
}

void UDisksClient::setDeviceClasses(quint32 classes)
{
    if ((classes & ~AllDeviceClasses) != 0 || classes == deviceClasses_)
        return;
    deviceClasses_ = classes;
    publishSnapshot();
}

void UDisksClient::publishSnapshot()
{
    if (exclusionsReader_) exclusions_ = exclusionsReader_();
    QMap<QDBusObjectPath, Record> current;
    QList<Device> devices;
    for (auto object = objects_.cbegin(); object != objects_.cend(); ++object)
    {
        Record record;
        const bool candidate = DescribeUDisksDevice(objects_, object.key().path(), record.device, allowFixedUsb_, DeviceAccess::Read, deviceClasses_ | DeviceClassMask(DeviceClass::Optical));
        const int excluded = candidate ? CheckDeviceExclusions(record.device.path, record.device.number, record.device.size, exclusions_) : 0;
        const bool accepted = candidate && excluded == 0 && !applicationDevices_.contains(record.device.number);
        if (candidate && excluded != 0) emit logMessage(QStringLiteral("Device excluded by IgnoreUsb/IgnoreDisk policy: %1 (%2)").arg(record.device.path).arg(excluded));
        if (usbDebug_ && object.value().contains(blockInterface)) {
            const auto block = object.value().value(blockInterface);
            emit logMessage(QStringLiteral("UDisks enumeration: object=%1 drive=%2 device=%3 eligible=%4 classes=0x%5 fixed=%6")
                .arg(object.key().path(), objectPath(block.value(QStringLiteral("Drive"))), QString::fromLocal8Bit(block.value(QStringLiteral("Device")).toByteArray().split('\0').first()))
                .arg(accepted).arg(deviceClasses_,0,16).arg(allowFixedUsb_));
        }
        if (!accepted) continue;
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
    ++contentRevision_;
    contentObjects_ = objects_;
    contentDevices_ = devices;
    contentPending_ = true;
    if (!contentReading_)
        startContentRead();
}

void UDisksClient::startContentRead()
{
    contentPending_ = false;
    contentReading_ = true;
    readingRevision_ = contentRevision_;
    contentWatcher_.setFuture(QtConcurrent::run(ReadUDisksDeviceContent,contentObjects_,contentDevices_));
    contentObjects_.clear();
    contentDevices_.clear();
}

void UDisksClient::contentFinished()
{
    contentReading_ = false;
    bool changed = false;
    if (readingRevision_ == contentRevision_)
    {
        for (const Device& device : contentWatcher_.result())
        {
            auto record = records_.find(QDBusObjectPath(device.objectPath));
            if (record == records_.end() || record->device.id != device.id)
                continue;
            if (record->device.volumeLabel == device.volumeLabel && record->device.mountedVolumes == device.mountedVolumes)
                continue;
            record->device.volumeLabel = device.volumeLabel;
            record->device.mountedVolumes = device.mountedVolumes;
            changed = true;
        }
    }
    if (changed)
    {
        QList<Device> devices;
        for (const Record& record : records_) devices.append(record.device);
        emit devicesChanged(devices,QString());
    }
    if (contentPending_)
        startContentRead();
}

void UDisksClient::reportFailure(const QString& problem)
{
    ++contentRevision_;
    contentPending_ = false;
    contentObjects_.clear();
    contentDevices_.clear();
    objects_.clear();
    serviceOwner_.clear();
    records_.clear();
    emit devicesChanged({}, problem);
}

} // namespace rufus
