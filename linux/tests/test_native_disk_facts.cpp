// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/native_disk_facts.h"
#include "platform/application_devices.h"
#include <QDir>
#include <QFile>
#include <QElapsedTimer>
#include <QThread>
#include <QTemporaryDir>
#include <QTest>
#include <cerrno>
#include <sys/sysmacros.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

namespace
{
constexpr quint64 gibibyte = 1024ULL * 1024 * 1024;
constexpr quint64 diskBytes = 32 * gibibyte;
using Filter = rufus::NativeDiskFilter;

struct ScoreCase
{
    const char* name;
    const char* vendor;
    const char* model;
    const char* removable;
    quint64 size;
    const char* vid;
    const char* pid;
    int score;
    Filter filter;
};

// Expected numbers come from the original Rufus reference cases. These also
// exercise real fixture-file reads, rather than passing preassembled facts.
const ScoreCase scoreCases[] = {
    {"small-fixed", "", "", "0", diskBytes, "", "", -12, Filter::Include},
    {"fixed-medium", "", "", "0", 256 * gibibyte, "", "", 3, Filter::LikelyHdd},
    {"removable-medium", "", "", "1", 256 * gibibyte, "", "", 0, Filter::Include},
    {"large-removable", "", "", "1", 2000 * gibibyte, "", "", 30, Filter::LikelyHdd},
    {"vendor-model-padding", "  WDC  \n", " Portable  \n", "0", 256 * gibibyte, "", "", 13, Filter::LikelyHdd},
    {"model-only", "", "WDC Portable", "0", 256 * gibibyte, "", "", 13, Filter::LikelyHdd},
    {"vendor-before-model", "USB", "WDC Portable", "0", 256 * gibibyte, "", "", 3, Filter::LikelyHdd},
    {"case-insensitive", "wdc", "Portable", "1", 256 * gibibyte, "", "", 10, Filter::LikelyHdd},
    {"known-flash-pair", "", "", "0", 256 * gibibyte, "0781", "5580", -22, Filter::Include},
    {"known-hdd-pair", "", "", "0", 256 * gibibyte, "1058", "0702", 18, Filter::LikelyHdd},
    {"invalid-pair-stays-unknown", "", "", "0", 256 * gibibyte, "1058", "bad", 3, Filter::LikelyHdd},
    {"cumulative-model-adjustments", "Flash", "SSD SATA", "0", 256 * gibibyte, "", "", 33, Filter::LikelyHdd},
    {"no-windows-suffix", "PNY", "USB", "1", 256 * gibibyte, "", "", 0, Filter::Include},
    {"native-suffix-present", "PNY", "USB Device", "1", 256 * gibibyte, "", "", -15, Filter::Include},
    {"preserve-real-scsi", "USB", "SCSI Flash", "0", 256 * gibibyte, "", "", 13, Filter::LikelyHdd},
    {"missing-removal", "Flash", "SSD", "", diskBytes, "0781", "5580", 0, Filter::Unknown},
    {"malformed-removal", "Flash", "SSD", "2", diskBytes, "0781", "5580", 0, Filter::Unknown},
};

class DiskFixture
{
public:
    QTemporaryDir directory;
    QString root = directory.path();
    QString hub = root + QStringLiteral("/devices/pci/usb1/1-2");
    QString usb = hub + QStringLiteral("/1-2.3");
    QString interface = usb + QStringLiteral("/1-2.3:1.0");
    QString scsi = interface + QStringLiteral("/host0/target0/0:0:0:0");
    QString block = scsi + QStringLiteral("/block/sdz");
    QString link = root + QStringLiteral("/dev/block/8:240");

    bool write(const QString& path, const QByteArray& bytes)
    {
        QFile file(path);
        return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size();
    }

    bool create()
    {
        if (!directory.isValid() || !QDir().mkpath(block)) return false;
        for (const QString& suffix : {QStringLiteral("/class/block"), QStringLiteral("/bus/usb"),
             QStringLiteral("/dev/block")})
            if (!QDir().mkpath(root + suffix)) return false;
        for (const QString& path : {hub, usb, interface})
            if (!QFile::link(root + QStringLiteral("/bus/usb"), path + QStringLiteral("/subsystem"))) return false;
        if (!QFile::link(root + QStringLiteral("/class/block"), block + QStringLiteral("/subsystem"))) return false;
        if (!QFile::link(block, link) || !QFile::link(scsi, block + QStringLiteral("/device"))) return false;
        const struct Attribute { QString path; QByteArray value; } attributes[] = {
            {hub + "/uevent", "DEVTYPE=usb_device\n"},
            {hub + "/idVendor", "9999\n"}, {hub + "/idProduct", "8888\n"},
            {usb + "/uevent", "BUSNUM=001\nDEVTYPE=usb_device\nDEVNUM=003\n"},
            {usb + "/idVendor", "0951\n"}, {usb + "/idProduct", "16aB\n"},
            {interface + "/uevent", "DEVTYPE=usb_interface\nDRIVER=uas\n"},
            {block + "/dev", "8:240\n"}, {block + "/size", "67108864\n"},
            {block + "/diskseq", "42\n"}, {block + "/removable", "0\n"},
            {scsi + "/vendor", "Kingston  \n"}, {scsi + "/model", "DataTraveler 3.0  \n"}
        };
        for (const Attribute& attribute : attributes)
            if (!write(attribute.path, attribute.value)) return false;
        return true;
    }

    int read(rufus::NativeDiskFacts& facts, quint64 size = diskBytes)
    {
        return rufus::ReadNativeDiskFacts(quint64(makedev(8, 240)), size, facts, root);
    }
};

// The reader reaches this FIFO only after capturing identity. Change the fake
// device before releasing its attribute read, without a production test hook.
class ChangeDisk : public QThread
{
public:
    DiskFixture* disk = nullptr;
    QString attribute;
    QByteArray value;
    bool changed = false;

    void run() override
    {
        const QByteArray fifo = QFile::encodeName(disk->block + "/removable");
        constexpr int waitMs = 2000;
        QElapsedTimer timer;
        timer.start();
        int fd = -1;
        while (timer.elapsed() < waitMs)
        {
            fd = ::open(fifo.constData(), O_WRONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd >= 0) break;
            if (errno != ENXIO) return;
            msleep(1);
        }
        if (fd < 0) return;
        if (attribute == "lookup")
        {
            const bool removed = QFile::remove(disk->link);
            changed = removed && QFile::link(disk->scsi, disk->link);
        }
        else
            changed = disk->write(disk->block + '/' + attribute, value);
        const ssize_t written = ::write(fd, "0\n", 2);
        changed = changed && written == 2;
        ::close(fd);
    }
};
}

class TestNativeDiskFacts : public QObject
{
    Q_OBJECT
private slots:
    void networkBackingDeclarations()
    {
        for (const QByteArray& options : {QByteArray("rw,addr=127.0.0.1"), QByteArray("addr=::1"), QByteArray("addr=::ffff:127.0.0.1"),
             QByteArray("addr=0.0.0.0"), QByteArray("addr=::"), QByteArray("addr=localhost"), QByteArray("rw")})
            QCOMPARE(rufus::CheckNativeRemoteBacking(options), ENOTSUP);
        bool external = false;
        for (const QByteArray& address : {QByteArray("203.0.113.253"), QByteArray("198.51.100.253"), QByteArray("192.0.2.253")})
            external = external || rufus::CheckNativeRemoteBacking(QByteArray("rw,addr=") + address) == 0;
        QVERIFY(external);
    }
    void fileBackedVirtualsAndOptical()
    {
        DiskFixture disk; QVERIFY(disk.create());
        const QString original = disk.block;
        const QString parent = disk.root + QStringLiteral("/devices/virtual/block");
        QVERIFY(QDir().mkpath(parent));
        const QString loop = parent + QStringLiteral("/loop9");
        QVERIFY(QDir().rename(original, loop)); QVERIFY(QFile::remove(disk.link)); QVERIFY(QFile::link(loop, disk.link));
        disk.block = loop;
        QVERIFY(QDir().mkpath(loop + QStringLiteral("/loop")));
        QFile backing(disk.directory.filePath(QStringLiteral("backing.raw")));
        QVERIFY(backing.open(QIODevice::ReadWrite)); QVERIFY(backing.resize(4096));
        QVERIFY(disk.write(loop + QStringLiteral("/loop/backing_file"), QFile::encodeName(backing.fileName()) + '\n'));
        QVERIFY(disk.write(loop + QStringLiteral("/loop/offset"), "0\n"));
        QVERIFY(disk.write(loop + QStringLiteral("/loop/sizelimit"), "0\n"));
        rufus::NativeDiskFacts facts;
        QCOMPARE(disk.read(facts), 0); QCOMPARE(facts.deviceClass, rufus::DeviceClass::Virtual);
        QVERIFY(!facts.usbTransport); QVERIFY(!facts.backingIdentity.isEmpty());
        const QString nbd = parent + QStringLiteral("/nbd9");
        QVERIFY(QDir().rename(loop, nbd)); QVERIFY(QFile::remove(disk.link)); QVERIFY(QFile::link(nbd, disk.link));
        QVERIFY(disk.write(nbd + QStringLiteral("/pid"), QByteArray::number(getpid()) + '\n'));
        QCOMPARE(disk.read(facts), 0); QCOMPARE(facts.deviceClass, rufus::DeviceClass::Virtual);
        QVERIFY(disk.write(nbd + QStringLiteral("/pid"), "0\n")); QCOMPARE(disk.read(facts), ENOTSUP);
        DiskFixture disc; QVERIFY(disc.create());
        QVERIFY(disc.write(disc.scsi + QStringLiteral("/type"), "5\n"));
        QCOMPARE(disc.read(facts), 0); QCOMPARE(facts.deviceClass, rufus::DeviceClass::Optical);
    }

    void logicalSector_data()
    {
        QTest::addColumn<QByteArray>("value");
        QTest::addColumn<unsigned>("expected");
        QTest::newRow("512") << QByteArray("512\n") << 512U;
        QTest::newRow("4096") << QByteArray("4096\n") << 4096U;
        QTest::newRow("65536") << QByteArray("65536\n") << 65536U;
        QTest::newRow("missing") << QByteArray() << 0U;
        QTest::newRow("zero") << QByteArray("0\n") << 0U;
        QTest::newRow("small") << QByteArray("256\n") << 0U;
        QTest::newRow("unaligned") << QByteArray("513\n") << 0U;
        QTest::newRow("large") << QByteArray("131072\n") << 0U;
        QTest::newRow("negative") << QByteArray("-512\n") << 0U;
        QTest::newRow("junk") << QByteArray("512x\n") << 0U;
    }

    void logicalSector()
    {
        QFETCH(QByteArray, value);
        QFETCH(unsigned, expected);
        DiskFixture disk;
        QVERIFY(disk.create());
        if (!value.isEmpty())
        {
            QVERIFY(QDir().mkpath(disk.block + QStringLiteral("/queue")));
            QVERIFY(disk.write(disk.block + QStringLiteral("/queue/logical_block_size"), value));
        }
        rufus::NativeDiskFacts facts;
        QCOMPARE(disk.read(facts), 0);
        QCOMPARE(facts.logicalSectorSize, expected);
        QCOMPARE(facts.physicalSectorSize, 0U); // Missing physical data stays unknown.
        QCOMPARE(facts.diskSequence, quint64(42));
    }

    void physicalSector_data()
    {
        QTest::addColumn<QByteArray>("value"); QTest::addColumn<unsigned>("expected");
        QTest::newRow("512") << QByteArray("512\n") << 512U;
        QTest::newRow("4096") << QByteArray("4096\n") << 4096U;
        QTest::newRow("65536") << QByteArray("65536\n") << 65536U;
        QTest::newRow("missing") << QByteArray() << 0U;
        QTest::newRow("zero") << QByteArray("0\n") << 0U;
        QTest::newRow("small") << QByteArray("256\n") << 0U;
        QTest::newRow("unaligned") << QByteArray("4097\n") << 0U;
        QTest::newRow("large") << QByteArray("131072\n") << 0U;
        QTest::newRow("negative") << QByteArray("-4096\n") << 0U;
        QTest::newRow("junk") << QByteArray("4096x\n") << 0U;
    }

    void physicalSector()
    {
        QFETCH(QByteArray, value); QFETCH(unsigned, expected);
        DiskFixture disk; QVERIFY(disk.create());
        QVERIFY(QDir().mkpath(disk.block + QStringLiteral("/queue")));
        QVERIFY(disk.write(disk.block + QStringLiteral("/queue/logical_block_size"), "512\n"));
        if (!value.isEmpty()) QVERIFY(disk.write(disk.block + QStringLiteral("/queue/physical_block_size"), value));
        rufus::NativeDiskFacts facts;
        QCOMPARE(disk.read(facts), 0);
        QCOMPARE(facts.logicalSectorSize, 512U); QCOMPARE(facts.physicalSectorSize, expected);
        QCOMPARE(facts.diskSequence, quint64(42));
    }

    void logicalAndPhysicalGeometry_data()
    {
        QTest::addColumn<unsigned>("logical"); QTest::addColumn<unsigned>("physical");
        QTest::newRow("512-native") << 512U << 512U;
        QTest::newRow("512-emulated") << 512U << 4096U;
        QTest::newRow("4096-native") << 4096U << 4096U;
        QTest::newRow("1024-logical") << 1024U << 4096U;
        QTest::newRow("2048-logical") << 2048U << 4096U;
    }

    void logicalAndPhysicalGeometry()
    {
        QFETCH(unsigned, logical); QFETCH(unsigned, physical);
        DiskFixture disk; QVERIFY(disk.create());
        QVERIFY(QDir().mkpath(disk.block + QStringLiteral("/queue")));
        QVERIFY(disk.write(disk.block + QStringLiteral("/queue/logical_block_size"), QByteArray::number(logical) + '\n'));
        QVERIFY(disk.write(disk.block + QStringLiteral("/queue/physical_block_size"), QByteArray::number(physical) + '\n'));
        rufus::NativeDiskFacts facts;
        QCOMPARE(disk.read(facts), 0);
        QCOMPARE(facts.logicalSectorSize, logical); QCOMPARE(facts.physicalSectorSize, physical);
        QCOMPARE(facts.size, diskBytes); // sysfs size always counts 512-byte units.
        QCOMPARE(facts.diskSequence, quint64(42));
    }

    void scoreInput_data()
    {
        QTest::addColumn<int>("index");
        for (size_t i = 0; i < sizeof(scoreCases) / sizeof(scoreCases[0]); ++i)
            QTest::newRow(scoreCases[i].name) << int(i);
    }

    void scoreInput()
    {
        QFETCH(int, index);
        const ScoreCase& expected = scoreCases[index];
        DiskFixture disk;
        QVERIFY(disk.create());
        QVERIFY(disk.write(disk.block + "/size", QByteArray::number(expected.size / 512)));
        QVERIFY(disk.write(disk.block + "/removable", expected.removable));
        QVERIFY(disk.write(disk.scsi + "/vendor", expected.vendor));
        QVERIFY(disk.write(disk.scsi + "/model", expected.model));
        QVERIFY(disk.write(disk.usb + "/idVendor", expected.vid));
        QVERIFY(disk.write(disk.usb + "/idProduct", expected.pid));
        rufus::NativeDiskFacts facts;
        QCOMPARE(disk.read(facts, expected.size), 0);
        const rufus::MediaRemoval originalRemoval = facts.mediaRemoval;
        HDD_SCORE score = {99, 1, {99}};
        const Filter filter = rufus::EvaluateNativeDiskFilter(facts, {}, {}, score);
        QCOMPARE(filter, expected.filter);
        QCOMPARE(score.total, expected.score);
        int sum = 0;
        for (size_t i = 0; i < score.count; ++i)
            sum += score.parts[i];
        QCOMPARE(sum, expected.score);
        if (filter == Filter::Unknown)
            QCOMPARE(score.count, size_t(0));
        QCOMPARE(facts.mediaRemoval, originalRemoval); // Scoring does not make fixed media removable.
    }

    void cardMedia_data()
    {
        QTest::addColumn<QString>("media");
        QTest::addColumn<bool>("card");
        for (const char* kind : {"flash", "flash_cf", "flash_ms", "flash_sm", "flash_sd",
             "flash_sdhc", "flash_sdxc", "flash_sdio", "flash_sd_combo", "flash_mmc"})
            QTest::newRow(kind) << QString::fromLatin1(kind) << true;
        for (const char* kind : {"thumb", "", "optical", "floppy", "FLASH_SD", "flash_unknown", "flash_sd ", "SD-CARD"})
            QTest::newRow(*kind == 0 ? "unknown" : kind) << QString::fromLatin1(kind) << false;
    }

    void cardMedia()
    {
        QFETCH(QString, media);
        QFETCH(bool, card);
        rufus::NativeDiskFacts facts;
        facts.size = 256 * gibibyte;
        facts.mediaRemoval = rufus::MediaRemoval::Fixed;
        facts.vendor = "WDC";
        facts.model = "Portable HDD";
        facts.usbIdsKnown = true;
        facts.usbVendor = 0x1058;
        facts.usbProduct = 0x0702;
        HDD_SCORE score;
        const Filter expected = card ? Filter::Include : Filter::LikelyHdd;
        QCOMPARE(rufus::EvaluateNativeDiskFilter(facts, media, {}, score), expected);
        if (card) QCOMPARE(score.count, size_t(0)); // Cards bypass even a strongly positive HDD score.
        QCOMPARE(rufus::EvaluateNativeDiskFilter(facts, {}, {QStringLiteral("unknown"), media}, score), expected);
        if (card) QCOMPARE(score.count, size_t(0));
    }

    void cardSizeAndCurrentMedia()
    {
        rufus::NativeDiskFacts facts;
        facts.size = 500 * gibibyte;
        // Positive card metadata takes the card branch even without fixed/removable facts.
        HDD_SCORE score = {99, 1, {99}};
        QCOMPARE(rufus::EvaluateNativeDiskFilter(facts, "flash_sdxc", {}, score), Filter::Include);
        QCOMPARE(score.count, size_t(0));
        facts.size += 512;
        QCOMPARE(rufus::EvaluateNativeDiskFilter(facts, {}, {"flash_sdxc"}, score), Filter::LargeCard);
        QCOMPARE(score.count, size_t(0));
        facts.size -= 1024;
        QCOMPARE(rufus::EvaluateNativeDiskFilter(facts, "flash_sdxc", {}, score), Filter::Include);
        facts.mediaRemoval = rufus::MediaRemoval::Fixed;
        // Known current media wins over reader capabilities, including mixed readers.
        QCOMPARE(rufus::EvaluateNativeDiskFilter(facts, "thumb", {"flash_sdxc"}, score), Filter::LikelyHdd);
        QCOMPARE(rufus::EvaluateNativeDiskFilter(facts, "future-medium", {"flash_sdxc"}, score), Filter::LikelyHdd);
        facts.size = 0;
        QCOMPARE(rufus::EvaluateNativeDiskFilter(facts, "flash_sdxc", {"flash_sdxc"}, score), Filter::Unknown);
        QCOMPARE(score.count, size_t(0));
    }

    void wholeUsbDisk()
    {
        DiskFixture disk;
        QVERIFY(disk.create());
        rufus::NativeDiskFacts facts;
        QCOMPARE(disk.read(facts), 0);
        QCOMPARE(facts.sysfsPath, disk.block);
        QCOMPARE(facts.diskSequence, quint64(42));
        QCOMPARE(facts.size, diskBytes);
        QCOMPARE(facts.mediaRemoval, rufus::MediaRemoval::Fixed);
        QVERIFY(facts.usbIdsKnown);
        QCOMPARE(facts.usbVendor, quint16(0x0951));
        QCOMPARE(facts.usbProduct, quint16(0x16ab));
        QCOMPARE(facts.vendor, QByteArray("Kingston"));
        QCOMPARE(facts.model, QByteArray("DataTraveler 3.0"));
        QVERIFY(disk.write(disk.block + "/removable", "1\n"));
        QCOMPARE(disk.read(facts), 0);
        QCOMPARE(facts.mediaRemoval, rufus::MediaRemoval::Removable);
    }

    void invalidIdentity_data()
    {
        QTest::addColumn<QString>("attribute");
        QTest::addColumn<QByteArray>("value");
        QTest::newRow("partition") << QString("partition") << QByteArray("1\n");
        QTest::newRow("wrong-dev") << QString("dev") << QByteArray("8:241\n");
        QTest::newRow("missing-dev") << QString("dev") << QByteArray();
        QTest::newRow("zero-size") << QString("size") << QByteArray("0\n");
        QTest::newRow("wrong-size") << QString("size") << QByteArray("1\n");
        QTest::newRow("size-overflow") << QString("size") << QByteArray("18446744073709551615\n");
        QTest::newRow("decimal-overflow") << QString("size") << QByteArray("18446744073709551616\n");
        QTest::newRow("signed-size") << QString("size") << QByteArray("+67108864\n");
        QTest::newRow("zero-sequence") << QString("diskseq") << QByteArray("0\n");
        QTest::newRow("missing-sequence") << QString("diskseq") << QByteArray();
        QTest::newRow("negative-sequence") << QString("diskseq") << QByteArray("-42\n");
        QTest::newRow("bad-sequence") << QString("diskseq") << QByteArray("42x\n");
    }

    void invalidIdentity()
    {
        QFETCH(QString, attribute);
        QFETCH(QByteArray, value);
        DiskFixture disk;
        QVERIFY(disk.create());
        rufus::NativeDiskFacts facts;
        QCOMPARE(disk.read(facts), 0);
        QVERIFY(disk.write(disk.block + '/' + attribute, value));
        QCOMPARE(disk.read(facts), ESTALE);
        QVERIFY(facts.sysfsPath.isEmpty());
        QCOMPARE(facts.diskSequence, quint64(0));
        QVERIFY(!facts.usbIdsKnown);
    }

    void unknownIds_data()
    {
        QTest::addColumn<QByteArray>("value");
        QTest::newRow("missing") << QByteArray();
        QTest::newRow("short") << QByteArray("951");
        QTest::newRow("long") << QByteArray("00951");
        QTest::newRow("prefix") << QByteArray("0x51");
        QTest::newRow("signed") << QByteArray("+951");
        QTest::newRow("non-hex") << QByteArray("095g");
        QTest::newRow("embedded-null") << QByteArray("0951\0x", 6);
        QTest::newRow("oversized") << QByteArray(4097, '0');
    }

    void unknownIds()
    {
        QFETCH(QByteArray, value);
        DiskFixture disk;
        QVERIFY(disk.create());
        for (const QString& attribute : {QStringLiteral("idVendor"), QStringLiteral("idProduct")})
        {
            QVERIFY(disk.write(disk.usb + "/idVendor", "0951"));
            QVERIFY(disk.write(disk.usb + "/idProduct", "16ab"));
            QVERIFY(disk.write(disk.usb + '/' + attribute, value));
            rufus::NativeDiskFacts facts;
            QCOMPARE(disk.read(facts), 0);
            QVERIFY(!facts.usbIdsKnown); // Never use the enclosing hub's valid IDs.
            QCOMPARE(facts.usbVendor, quint16(0));
            QCOMPARE(facts.usbProduct, quint16(0));
        }
    }

    void unknownOptionalFacts()
    {
        DiskFixture disk;
        QVERIFY(disk.create());
        QVERIFY(QFile::remove(disk.scsi + "/vendor"));
        QVERIFY(disk.write(disk.scsi + "/model", QByteArray("bad\0name", 8)));
        for (const QByteArray& value : {QByteArray(), QByteArray("2"), QByteArray("yes"), QByteArray("01")})
        {
            QVERIFY(disk.write(disk.block + "/removable", value));
            rufus::NativeDiskFacts facts;
            QCOMPARE(disk.read(facts), 0);
            QCOMPARE(facts.mediaRemoval, rufus::MediaRemoval::Unknown);
            QVERIFY(facts.vendor.isEmpty());
            QVERIFY(facts.model.isEmpty());
        }
    }

    void nonUsbClasses_data()
    {
        QTest::addColumn<QByteArray>("vendor"); QTest::addColumn<QByteArray>("model");
        QTest::addColumn<bool>("removable"); QTest::addColumn<bool>("mmc"); QTest::addColumn<int>("kind");
        QTest::newRow("msft") << QByteArray("Msft") << QByteArray("Virtual Disk") << false << false << 2;
        QTest::newRow("arsenal") << QByteArray("Arsenal") << QByteArray("Virtual Disk") << false << false << 2;
        QTest::newRow("kernsafe") << QByteArray("KernSafe") << QByteArray("Virtual Disk") << false << false << 2;
        QTest::newRow("bhyve") << QByteArray("BHYVE") << QByteArray("SATA DISK") << false << false << 2;
        QTest::newRow("vmware") << QByteArray("VMware") << QByteArray("VMware Virtual S") << false << false << 4;
        QTest::newRow("removable") << QByteArray("ATA") << QByteArray("External SATA") << true << false << 8;
        QTest::newRow("mmc") << QByteArray() << QByteArray() << false << true << 16;
        QTest::newRow("fixed-internal") << QByteArray("ATA") << QByteArray("Internal SSD") << false << false << 0;
        QTest::newRow("unverified-virtual-name") << QByteArray("Other") << QByteArray("Virtual Disk") << false << false << 0;
    }
    void nonUsbClasses()
    {
        QFETCH(QByteArray, vendor); QFETCH(QByteArray, model);
        QFETCH(bool, removable); QFETCH(bool, mmc); QFETCH(int, kind);
        DiskFixture disk; QVERIFY(disk.create());
        for (const QString& path : {disk.usb, disk.hub, disk.interface}) QVERIFY(QFile::remove(path + "/subsystem"));
        QVERIFY(disk.write(disk.scsi + "/vendor", vendor)); QVERIFY(disk.write(disk.scsi + "/model", model));
        QVERIFY(disk.write(disk.block + "/removable", removable ? "1" : "0"));
        if (mmc) {
            QVERIFY(QDir().mkpath(disk.root + "/bus/mmc"));
            QVERIFY(QFile::link(disk.root + "/bus/mmc", disk.scsi + "/subsystem"));
        }
        rufus::NativeDiskFacts facts;
        QCOMPARE(disk.read(facts), kind ? 0 : ENOTSUP);
        if (!kind) { QVERIFY(facts.sysfsPath.isEmpty()); return; }
        QCOMPARE(int(facts.deviceClass), kind); QVERIFY(!facts.usbTransport); QVERIFY(!facts.usbIdsKnown);
        QCOMPARE(facts.diskSequence, quint64(42));
    }

    void invalidUsbAncestry()
    {
        DiskFixture disk;
        QVERIFY(disk.create());
        rufus::NativeDiskFacts facts;
        QVERIFY(disk.write(disk.usb + "/uevent", "DEVTYPE=unknown\n"));
        QCOMPARE(disk.read(facts), ENOTSUP);
        QVERIFY(facts.sysfsPath.isEmpty());
        QVERIFY(disk.write(disk.usb + "/uevent", "DEVTYPE=usb_device\n"));
        QVERIFY(disk.write(disk.interface + "/uevent", "DEVTYPE=usb_interface_extra\n"));
        QCOMPARE(disk.read(facts), ENOTSUP);
        for (const QString& path : {disk.usb, disk.hub, disk.interface})
            QVERIFY(QFile::remove(path + "/subsystem"));
        QCOMPARE(disk.read(facts), ENOTSUP);
    }

    void changedDuringRead_data()
    {
        QTest::addColumn<QString>("attribute");
        QTest::addColumn<QByteArray>("value");
        QTest::newRow("sequence") << QString("diskseq") << QByteArray("43\n");
        QTest::newRow("capacity") << QString("size") << QByteArray("1\n");
        QTest::newRow("device-number") << QString("dev") << QByteArray("8:241\n");
        QTest::newRow("lookup-target") << QString("lookup") << QByteArray();
        QTest::newRow("logical-sector") << QString("queue/logical_block_size") << QByteArray("4096\n");
        QTest::newRow("physical-sector") << QString("queue/physical_block_size") << QByteArray("4096\n");
    }

    void changedDuringRead()
    {
        QFETCH(QString, attribute);
        QFETCH(QByteArray, value);
        DiskFixture disk;
        QVERIFY(disk.create());
        if (attribute.startsWith("queue/"))
        {
            QVERIFY(QDir().mkpath(disk.block + QStringLiteral("/queue")));
            QVERIFY(disk.write(disk.block + QStringLiteral("/queue/logical_block_size"), "512\n"));
            QVERIFY(disk.write(disk.block + QStringLiteral("/queue/physical_block_size"), "512\n"));
        }
        const QByteArray fifo = QFile::encodeName(disk.block + "/removable");
        QVERIFY(QFile::remove(QString::fromLocal8Bit(fifo)));
        QCOMPARE(::mkfifo(fifo.constData(), 0600), 0);
        ChangeDisk change;
        change.disk = &disk;
        change.attribute = attribute;
        change.value = value;
        change.start();
        rufus::NativeDiskFacts facts;
        const int result = disk.read(facts);
        change.wait();
        QVERIFY(change.changed);
        QCOMPARE(result, ESTALE);
        QVERIFY(facts.sysfsPath.isEmpty());
        QCOMPARE(facts.mediaRemoval, rufus::MediaRemoval::Unknown);
        QCOMPARE(facts.logicalSectorSize, 0U); QCOMPARE(facts.physicalSectorSize, 0U);
        QVERIFY(!facts.usbIdsKnown);
    }

    void invalidLookup()
    {
        DiskFixture disk;
        QVERIFY(disk.create());
        rufus::NativeDiskFacts facts;
        QCOMPARE(disk.read(facts, diskBytes + 512), ESTALE);
        QCOMPARE(disk.read(facts, 0), EINVAL);
        QCOMPARE(rufus::ReadNativeDiskFacts(0, diskBytes, facts, disk.root), EINVAL);
        QVERIFY(QFile::remove(disk.link));
        QCOMPARE(disk.read(facts), ENODEV);
        QVERIFY(QFile::link(disk.root, disk.link));
        QCOMPARE(disk.read(facts), ENODEV);
        QVERIFY(QFile::remove(disk.link));
        QVERIFY(QFile::link(disk.block, disk.link));
        QVERIFY(QFile::remove(disk.block + "/subsystem"));
        QCOMPARE(disk.read(facts), ENODEV);
    }
};
QTEST_GUILESS_MAIN(TestNativeDiskFacts)
#include "test_native_disk_facts.moc"
