// SPDX-License-Identifier: GPL-3.0-or-later
#include "native_disk_fixture.h"
#include "platform/udisks_client.h"
#include "platform/target_mounts.h"

#include <QTest>
#include <sys/sysmacros.h>
#include <utility>

namespace
{
const QString blockInterface = QStringLiteral("org.freedesktop.UDisks2.Block");
const QString driveInterface = QStringLiteral("org.freedesktop.UDisks2.Drive");
const QString partitionInterface = QStringLiteral("org.freedesktop.UDisks2.Partition");
const QString filesystemInterface = QStringLiteral("org.freedesktop.UDisks2.Filesystem");
const QString root = QStringLiteral("/org/freedesktop/UDisks2/");
const QDBusObjectPath disk(root + QStringLiteral("block_devices/test"));
const QDBusObjectPath first(root + QStringLiteral("block_devices/test1"));
const QDBusObjectPath second(root + QStringLiteral("block_devices/test2"));
const QDBusObjectPath drive(root + QStringLiteral("drives/test"));
constexpr quint64 diskNumber = 2048;
constexpr quint64 firstNumber = diskNumber + 1;
constexpr quint64 secondNumber = diskNumber + 2;

QVariantMap block(quint64 number)
{
    return {{QStringLiteral("Drive"), QVariant::fromValue(drive)},
        {QStringLiteral("DeviceNumber"), number},
        {QStringLiteral("HintSystem"), false}, {QStringLiteral("HintIgnore"), false},
        {QStringLiteral("ReadOnly"), false}};
}

void addFilesystem(rufus::UDisksObjects& objects, const QDBusObjectPath& path,
    const QList<QByteArray>& mounts)
{
    objects[path][filesystemInterface][QStringLiteral("MountPoints")] = QVariant::fromValue(mounts);
}

void addPartition(rufus::UDisksObjects& objects, const QDBusObjectPath& path, quint64 number)
{
    objects[path][blockInterface] = block(number);
    objects[path][partitionInterface][QStringLiteral("Table")] = QVariant::fromValue(disk);
    addFilesystem(objects, path, {});
}
}

class TestUDisksMounts : public QObject
{
    Q_OBJECT
    rufus::UDisksObjects objects_;
    rufus::Device target_;

private slots:
    void init()
    {
        ResetTestNativeDisks();
        objects_.clear();
        objects_[disk][blockInterface] = block(diskNumber);
        QVariantMap& diskBlock = objects_[disk][blockInterface];
        diskBlock[QStringLiteral("Device")] = QByteArray("/dev/test") + '\0';
        diskBlock[QStringLiteral("Size")] = quint64(64 * 1024 * 1024);
        objects_[drive][driveInterface] = {
            {QStringLiteral("ConnectionBus"), QStringLiteral("usb")},
            {QStringLiteral("Removable"), true}, {QStringLiteral("MediaRemovable"), true},
            {QStringLiteral("MediaAvailable"), true}, {QStringLiteral("Optical"), false}};
        QVERIFY(rufus::DescribeUDisksDevice(objects_, disk.path(), target_));
    }

    void ordinaryMounts_data()
    {
        QTest::addColumn<QString>("kind");
        for (const char* kind : {"none", "whole-disk", "partition", "two-partitions", "two-mountpoints",
                 "partition-table-only", "unrelated-device"})
            QTest::newRow(kind) << QString::fromLatin1(kind);
    }

    void ordinaryMounts()
    {
        QFETCH(QString, kind);
        QStringList expected;
        const QByteArray desktopPath = QByteArray("/run/media/user/USB stick") + '\0';
        if (kind == QStringLiteral("whole-disk"))
        {
            addFilesystem(objects_, disk, {desktopPath});
            expected << disk.path();
        }
        else if (kind != QStringLiteral("none"))
        {
            addPartition(objects_, first, firstNumber);
            addPartition(objects_, second, secondNumber);
            addFilesystem(objects_, first, {desktopPath});
            expected << first.path();
            if (kind == QStringLiteral("two-partitions"))
            {
                addFilesystem(objects_, second, {QByteArray("/mnt/second") + '\0'});
                expected << second.path();
            }
            if (kind == QStringLiteral("two-mountpoints"))
                addFilesystem(objects_, first, {desktopPath, QByteArray("/mnt/bind") + '\0'});
            if (kind == QStringLiteral("partition-table-only"))
                objects_[first][blockInterface][QStringLiteral("Drive")] = QVariant::fromValue(QDBusObjectPath("/"));
            if (kind == QStringLiteral("unrelated-device"))
            {
                objects_[second][blockInterface][QStringLiteral("Drive")] = QVariant::fromValue(QDBusObjectPath("/"));
                objects_[second].remove(partitionInterface);
                objects_[second][QStringLiteral("org.freedesktop.UDisks2.Swapspace")] = {};
                addFilesystem(objects_, second, {QByteArray("/") + '\0'});
            }
        }
        QStringList mounts{QStringLiteral("stale")};
        const QString problem = rufus::ListUDisksTargetMounts(objects_, target_, 0, mounts);
        QVERIFY2(problem.isEmpty(), qPrintable(problem));
        QCOMPARE(mounts, expected);
        // Creation still rejects all mounted cases until native unmount is qualified.
        QCOMPARE(rufus::CheckUDisksTargetUse(objects_, target_, 0).isEmpty(), expected.isEmpty());
    }

    void unsafeMember_data()
    {
        QTest::addColumn<QString>("kind");
        QTest::addColumn<bool>("earlier");
        for (const char* kind : {"source", "swap", "encrypted", "crypto-usage", "physical-volume",
                 "raid-usage", "MDRaid", "MDRaidMember", "CryptoBackingDevice", "active-crypto",
                 "system", "ignored", "unknown-mounts", "malformed-mounts"})
        {
            QTest::newRow(qPrintable(QString::fromLatin1(kind) + QStringLiteral("-before")))
                << QString::fromLatin1(kind) << true;
            QTest::newRow(qPrintable(QString::fromLatin1(kind) + QStringLiteral("-after")))
                << QString::fromLatin1(kind) << false;
        }
    }

    void unsafeMember()
    {
        QFETCH(QString, kind);
        QFETCH(bool, earlier);
        addPartition(objects_, first, firstNumber);
        addPartition(objects_, second, secondNumber);
        const QDBusObjectPath unsafe = earlier ? first : second;
        const QDBusObjectPath mounted = earlier ? second : first;
        const quint64 unsafeNumber = earlier ? firstNumber : secondNumber;
        addFilesystem(objects_, mounted, {QByteArray("/mnt/usb") + '\0'});
        QVariantMap& member = objects_[unsafe][blockInterface];
        quint64 sourceDevice = 0;
        if (kind == QStringLiteral("source"))
            sourceDevice = unsafeNumber;
        else if (kind == QStringLiteral("swap"))
            objects_[unsafe][QStringLiteral("org.freedesktop.UDisks2.Swapspace")] = {};
        else if (kind == QStringLiteral("encrypted"))
            objects_[unsafe][QStringLiteral("org.freedesktop.UDisks2.Encrypted")] = {};
        else if (kind == QStringLiteral("crypto-usage"))
            member[QStringLiteral("IdUsage")] = QStringLiteral("crypto");
        else if (kind == QStringLiteral("physical-volume"))
            objects_[unsafe][QStringLiteral("org.freedesktop.UDisks2.PhysicalVolume")] = {};
        else if (kind == QStringLiteral("raid-usage"))
            member[QStringLiteral("IdUsage")] = QStringLiteral("raid");
        else if (kind == QStringLiteral("active-crypto"))
            objects_[QDBusObjectPath(root + QStringLiteral("block_devices/cleartext"))]
                [blockInterface][QStringLiteral("CryptoBackingDevice")] = QVariant::fromValue(unsafe);
        else if (kind == QStringLiteral("system"))
            member[QStringLiteral("HintSystem")] = true;
        else if (kind == QStringLiteral("ignored"))
            member[QStringLiteral("HintIgnore")] = true;
        else if (kind == QStringLiteral("unknown-mounts"))
            objects_[unsafe][filesystemInterface].clear();
        else if (kind == QStringLiteral("malformed-mounts"))
            objects_[unsafe][filesystemInterface][QStringLiteral("MountPoints")] = QStringLiteral("unmounted");
        else
            member[kind] = QVariant::fromValue(QDBusObjectPath(root + QStringLiteral("layer")));
        QStringList mounts{QStringLiteral("stale")};
        QVERIFY(!rufus::ListUDisksTargetMounts(objects_, target_, sourceDevice, mounts).isEmpty());
        QVERIFY(mounts.isEmpty()); // Never return a partial list, regardless of object order.
        QVERIFY(!rufus::CheckUDisksTargetUse(objects_, target_, sourceDevice).isEmpty());
        QVERIFY(!rufus::ListUDisksTargetMounts(objects_, target_, sourceDevice, mounts, false, rufus::DeviceAccess::Read).isEmpty());
        QVERIFY(mounts.isEmpty());
    }

    void readOnlySourceMounts()
    {
        objects_[disk][blockInterface][QStringLiteral("ReadOnly")] = true;
        QVERIFY(rufus::DescribeUDisksDevice(objects_, disk.path(), target_, false, rufus::DeviceAccess::Read));
        QVERIFY(target_.readOnly);
        addPartition(objects_, first, firstNumber);
        objects_[first][blockInterface][QStringLiteral("ReadOnly")] = true;
        addFilesystem(objects_, first, {QByteArray("/mnt/usb") + '\0'});
        QStringList mounts;
        QVERIFY(!rufus::ListUDisksTargetMounts(objects_, target_, 0, mounts).isEmpty());
        QVERIFY(rufus::ListUDisksTargetMounts(objects_, target_, 0, mounts, false, rufus::DeviceAccess::Read).isEmpty());
        QCOMPARE(mounts, QStringList{first.path()});
        rufus::NativeTargetMounts native;
        native.device = diskNumber; native.diskSequence = 10;
        native.volumes.append({QByteArray("/mnt/usb"), firstNumber, 20});
        QVERIFY(rufus::MatchUDisksTargetMounts(objects_, target_, 0, 10, native, false, rufus::DeviceAccess::Read).isEmpty());
        QVERIFY(!rufus::ListUDisksTargetMounts(objects_, target_, firstNumber, mounts, false, rufus::DeviceAccess::Read).isEmpty());
        objects_[first][blockInterface][QStringLiteral("HintSystem")] = true;
        QVERIFY(!rufus::ListUDisksTargetMounts(objects_, target_, 0, mounts, false, rufus::DeviceAccess::Read).isEmpty());
        QVERIFY(mounts.isEmpty());
    }

    void invalidMountedVolume_data()
    {
        QTest::addColumn<QString>("kind");
        for (const char* kind : {"missing-system", "missing-ignore", "missing-readonly", "readonly",
                 "malformed-flag", "zero-number", "missing-number", "invalid-number", "no-table",
                 "wrong-table", "relative-path", "empty-path", "unterminated-path", "embedded-nul",
                 "dot-path", "parent-path", "root-path", "duplicate-slash"})
            QTest::newRow(kind) << QString::fromLatin1(kind);
    }

    void invalidMountedVolume()
    {
        QFETCH(QString, kind);
        addPartition(objects_, first, firstNumber);
        QVariantMap& member = objects_[first][blockInterface];
        QByteArray path = "/mnt/usb";
        if (kind == QStringLiteral("missing-system")) member.remove(QStringLiteral("HintSystem"));
        else if (kind == QStringLiteral("missing-ignore")) member.remove(QStringLiteral("HintIgnore"));
        else if (kind == QStringLiteral("missing-readonly")) member.remove(QStringLiteral("ReadOnly"));
        else if (kind == QStringLiteral("readonly")) member[QStringLiteral("ReadOnly")] = true;
        else if (kind == QStringLiteral("malformed-flag")) member[QStringLiteral("HintSystem")] = QStringLiteral("false");
        else if (kind == QStringLiteral("zero-number")) member[QStringLiteral("DeviceNumber")] = quint64(0);
        else if (kind == QStringLiteral("missing-number")) member.remove(QStringLiteral("DeviceNumber"));
        else if (kind == QStringLiteral("invalid-number")) member[QStringLiteral("DeviceNumber")] = QStringLiteral("unknown");
        else if (kind == QStringLiteral("no-table")) objects_[first].remove(partitionInterface);
        else if (kind == QStringLiteral("wrong-table"))
            objects_[first][partitionInterface][QStringLiteral("Table")] = QVariant::fromValue(second);
        else if (kind == QStringLiteral("relative-path")) path = "mnt/usb";
        else if (kind == QStringLiteral("empty-path")) path.clear();
        else if (kind == QStringLiteral("embedded-nul")) path.append('\0').append("suffix");
        else if (kind == QStringLiteral("dot-path")) path = "/mnt/./usb";
        else if (kind == QStringLiteral("parent-path")) path = "/mnt/../usb";
        else if (kind == QStringLiteral("root-path")) path = "/";
        else if (kind == QStringLiteral("duplicate-slash")) path = "//";
        if (kind != QStringLiteral("unterminated-path")) path.append('\0');
        addFilesystem(objects_, first, {path});
        QStringList mounts{QStringLiteral("stale")};
        QVERIFY(!rufus::ListUDisksTargetMounts(objects_, target_, 0, mounts).isEmpty());
        QVERIFY(mounts.isEmpty());
        QCOMPARE(rufus::ListUDisksTargetMounts(objects_, target_, 0, mounts, false, rufus::DeviceAccess::Read).isEmpty(), kind == QStringLiteral("readonly"));
    }

    void nativeMatching_data()
    {
        QTest::addColumn<QString>("kind");
        QTest::addColumn<bool>("accepted");
        for (const char* kind : {"empty", "whole-disk", "partition", "reordered", "byte-path", "btrfs-anonymous"})
            QTest::newRow(kind) << QString::fromLatin1(kind) << true;
        for (const char* kind : {"disk", "sequence", "zero-sequence", "missing-native", "extra-native",
                 "missing-service", "wrong-number", "wrong-path", "swapped-paths", "duplicate-device",
                 "duplicate-id", "duplicate-path", "zero-id", "zero-device", "service-alias",
                 "duplicate-object", "protected", "source", "stale-identity", "btrfs-wrong-member", "btrfs-missing-member"})
            QTest::newRow(kind) << QString::fromLatin1(kind) << false;
    }

    void nativeMatching()
    {
        QFETCH(QString, kind);
        QFETCH(bool, accepted);
        constexpr quint64 sequence = 42;
        rufus::NativeTargetMounts native{diskNumber, sequence, {}};
        quint64 expectedSequence = sequence;
        quint64 sourceDevice = 0;
        if (kind == QStringLiteral("empty"))
        {
            QVERIFY(rufus::MatchUDisksTargetMounts(objects_, target_, 0, sequence, native).isEmpty());
            return;
        }
        const bool wholeDisk = kind == QStringLiteral("whole-disk");
        const QDBusObjectPath member = wholeDisk ? disk : first;
        const quint64 number = wholeDisk ? diskNumber : firstNumber;
        if (!wholeDisk)
            addPartition(objects_, first, firstNumber);
        QByteArray path = "/run/media/user/USB stick";
        if (kind == QStringLiteral("byte-path"))
            path.append(char(0xff)); // Compare kernel bytes, without a Unicode round trip.
        addFilesystem(objects_, member, {path + '\0'});
        native.volumes.append({path, number, 10});
        if (kind == QStringLiteral("reordered") || kind == QStringLiteral("swapped-paths"))
        {
            addPartition(objects_, second, secondNumber);
            addFilesystem(objects_, second, {QByteArray("/mnt/second") + '\0'});
            native.volumes.prepend({"/mnt/second", secondNumber, 11});
            if (kind == QStringLiteral("swapped-paths"))
                std::swap(native.volumes[0].path, native.volumes[1].path);
        }
        if (kind.startsWith(QStringLiteral("btrfs-"))) {
            native.volumes[0].device = quint64(makedev(0, 900));
            native.volumes[0].backing = kind == QStringLiteral("btrfs-anonymous") ? number
                : kind == QStringLiteral("btrfs-wrong-member") ? secondNumber : 0;
        }
        else if (kind == QStringLiteral("disk")) native.device++;
        else if (kind == QStringLiteral("sequence")) native.diskSequence++;
        else if (kind == QStringLiteral("zero-sequence")) expectedSequence = native.diskSequence = 0;
        else if (kind == QStringLiteral("missing-native")) native.volumes.clear();
        else if (kind == QStringLiteral("missing-service")) addFilesystem(objects_, first, {});
        else if (kind == QStringLiteral("extra-native")) native.volumes.append({"/mnt/extra", secondNumber, 11});
        else if (kind == QStringLiteral("wrong-number")) native.volumes[0].device++;
        else if (kind == QStringLiteral("wrong-path")) native.volumes[0].path.append("-other");
        else if (kind == QStringLiteral("zero-id")) native.volumes[0].id = 0;
        else if (kind == QStringLiteral("zero-device")) native.volumes[0].device = 0;
        else if (kind == QStringLiteral("service-alias"))
            addFilesystem(objects_, first, {path + '\0', QByteArray("/mnt/alias") + '\0'});
        else if (kind.startsWith(QStringLiteral("duplicate-")))
        {
            addPartition(objects_, second, secondNumber);
            addFilesystem(objects_, second, {QByteArray("/mnt/second") + '\0'});
            native.volumes.append({"/mnt/second", secondNumber, 11});
            if (kind == QStringLiteral("duplicate-device")) native.volumes[1].device = firstNumber;
            else if (kind == QStringLiteral("duplicate-id")) native.volumes[1].id = 10;
            else if (kind == QStringLiteral("duplicate-path")) native.volumes[1].path = path;
            else objects_[second][blockInterface][QStringLiteral("DeviceNumber")] = firstNumber;
        }
        else if (kind == QStringLiteral("protected")) objects_[first][blockInterface][QStringLiteral("HintSystem")] = true;
        else if (kind == QStringLiteral("source")) sourceDevice = firstNumber;
        else if (kind == QStringLiteral("stale-identity")) target_.identity.append(QStringLiteral("changed"));
        const QString problem = rufus::MatchUDisksTargetMounts(objects_, target_, sourceDevice, expectedSequence, native);
        QCOMPARE(problem.isEmpty(), accepted);
    }

    void staleTarget_data()
    {
        QTest::addColumn<QString>("kind");
        for (const char* kind : {"removed", "number", "path", "size", "identity", "system"})
            QTest::newRow(kind) << QString::fromLatin1(kind);
    }

    void staleTarget()
    {
        QFETCH(QString, kind);
        addPartition(objects_, first, firstNumber);
        addFilesystem(objects_, first, {QByteArray("/mnt/usb") + '\0'});
        QVariantMap& member = objects_[disk][blockInterface];
        if (kind == QStringLiteral("removed")) objects_.remove(disk);
        else if (kind == QStringLiteral("number")) member[QStringLiteral("DeviceNumber")] = quint64(9999);
        else if (kind == QStringLiteral("path")) member[QStringLiteral("Device")] = QByteArray("/dev/replaced") + '\0';
        else if (kind == QStringLiteral("size")) member[QStringLiteral("Size")] = quint64(32 * 1024 * 1024);
        else if (kind == QStringLiteral("identity")) member[QStringLiteral("Id")] = QStringLiteral("new-id");
        else if (kind == QStringLiteral("system")) member[QStringLiteral("HintSystem")] = true;
        QStringList mounts{QStringLiteral("stale")};
        QVERIFY(!rufus::ListUDisksTargetMounts(objects_, target_, 0, mounts).isEmpty());
        QVERIFY(mounts.isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestUDisksMounts)
#include "test_udisks_mounts.moc"
