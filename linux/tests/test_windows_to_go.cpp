// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/windows_to_go.h"
#include "windows_to_go_fixture.h"
#include "platform/prepare_source.h"
#include "platform/format_options.h"
#include "platform/fat_format.h"
#include "unattend.h"
#include <hivex.h>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

static bool cancelled;
extern "C" int IsRufusJobCancelled(void) { return cancelled; }
extern "C" int IsIsoScanCancelled(void) { return cancelled; }
extern "C" void uprintf(const char*, ...) {}
extern "C" void uprintfs(const char*) {}
extern "C" void _UpdateProgressWithInfo(int, int, uint64_t, uint64_t, int) {}

class WindowsToGoTest : public QObject
{
    Q_OBJECT
    QTemporaryDir temporary_;
    QString source_;

    void makeImage(bool solid = false, const QString& change = {})
    {
        source_ = temporary_.filePath(solid ? "install.esd" : "install.wim");
        QFile::remove(source_);
        QVERIFY(CreateWindowsToGoFixture(source_, solid, change));
    }

    rufus::PrepareImageRequest request()
    {
        rufus::PrepareImageRequest request;
        request.windowsToGo = request.isDevice = true;
        request.path = QStringLiteral("/dev/fixture");
        request.device = 123;
        request.diskSequence = 7;
        request.objectPath = QStringLiteral("/org/freedesktop/UDisks2/block_devices/fixture");
        request.serviceOwner = QStringLiteral(":1.2");
        request.identity = {QStringLiteral("fixture")};
        request.size = 1024ULL * 1024 * 1024;
        request.fileSystem = FS_NTFS;
        request.clusterSize = 4096;
        request.windowsEdition = 2;
        const int captured = rufus::CapturePrepareSource(source_, request);
        if (captured != 0) qFatal("Could not retain fixture source: %d", captured);
        return request;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(temporary_.isValid());
        QCOMPARE(wimlib_global_init(WIMLIB_INIT_FLAG_ASSUME_UTF8 | WIMLIB_INIT_FLAG_DEFAULT_CASE_INSENSITIVE), 0);
    }
    void cleanupTestCase() { wimlib_global_cleanup(); }
    void init() { cancelled = false; }

    void sources_data()
    {
        QTest::addColumn<bool>("solid");
        QTest::addColumn<bool>("iso");
        QTest::newRow("WIM") << false << false;
        QTest::newRow("solid-ESD") << true << false;
        QTest::newRow("ISO-WIM") << false << true;
        QTest::newRow("ISO-solid-ESD") << true << true;
    }
    void sources()
    {
        QFETCH(bool, solid); QFETCH(bool, iso);
        makeImage(solid);
        if (iso)
        {
            const QString tree = temporary_.filePath("iso-tree");
            QVERIFY(QDir(tree).removeRecursively());
            QVERIFY(QDir().mkpath(tree + "/sources"));
            QVERIFY(QFile::copy(source_, tree + (solid ? "/sources/install.esd" : "/sources/install.wim")));
            source_ = temporary_.filePath("source.iso");
            QProcess creator;
            creator.start(QStringLiteral(RUFUS_MKISOFS_PATH), {"-quiet", "-R", "-o", source_, tree});
            QVERIFY(creator.waitForFinished(10000));
            QCOMPARE(creator.exitCode(), 0);
        }
        auto selected = request();
        if (solid) selected.windowsFlags = UNATTEND_FORCE_S_MODE;
        if (iso) selected.windowsInstallPath = solid ? QStringLiteral("/sources/install.esd") : QStringLiteral("/sources/install.wim");
        rufus::PrepareIsoSource retained(selected);
        QCOMPARE(retained.open(getuid()), 0);
        QCOMPARE(lseek(retained.retainedSource().fd, 17, SEEK_SET), off_t(17));
        if (!iso)
        {
            ISO_ANALYSIS preview = {};
            WINDOWS_IMAGE_ANALYSIS windows = {};
            QCOMPARE(rufus::ScanWindowsStandaloneImage(retained.retainedSource(), QFile::encodeName(source_).constData(), preview, windows), ISO_SCAN_OK);
            QVERIFY(preview.report.is_windows_img && HAS_WINTOGO(preview.report));
            QCOMPARE(windows.state, WINDOWS_IMAGE_KNOWN);
        }
        rufus::WindowsToGoImage image;
        auto discovery = selected;
        discovery.windowsInstallPath.clear();
        QCOMPARE(image.open(discovery, retained, false), 0);
        QCOMPARE(image.archive, selected.windowsInstallPath);
        QCOMPARE(image.editions.editions.size(), 2);
        QCOMPARE(image.plan.bytes, quint64(0));
        image.close();
        QCOMPARE(image.open(selected, retained), 0);
        QVERIFY(image.handle() != nullptr);
        QCOMPARE(image.architecture, int(ARCH_X86_64));
        QVERIFY(image.bios && image.uefi);
        QCOMPARE(image.editions.editions.size(), 2);
        QCOMPARE(image.editions.editions.last().image, 2);
        QCOMPARE(image.plan.metadata.win_version.major, quint16(11));
        QCOMPARE(lseek(retained.retainedSource().fd, 0, SEEK_CUR), off_t(17));
        image.close();
        QCOMPARE(retained.check(), 0);
        QCOMPARE(retained.close(), 0);
    }

    void refusals_data()
    {
        QTest::addColumn<QString>("change"); QTest::addColumn<int>("expected");
        QTest::newRow("wrong-arch") << QStringLiteral("wrong-arch") << EBADMSG;
        QTest::newRow("existing-answer") << QStringLiteral("existing-answer") << EEXIST;
        QTest::newRow("index") << QStringLiteral("index") << EINVAL;
        QTest::newRow("standalone-archive") << QStringLiteral("archive") << EINVAL;
        QTest::newRow("mbr-without-loader") << QStringLiteral("no-loader") << EINVAL;
        QTest::newRow("capacity") << QStringLiteral("capacity") << ENOSPC;
        QTest::newRow("cancel") << QStringLiteral("cancel") << ECANCELED;
    }
    void refusals()
    {
        QFETCH(QString, change); QFETCH(int, expected);
        makeImage(false, change);
        auto selected = request();
        if (change == "existing-answer") selected.windowsFlags = UNATTEND_NO_DATA_COLLECTION;
        if (change == "index") selected.windowsEdition = 3;
        if (change == "archive") selected.windowsInstallPath = QStringLiteral("/sources/install.wim");
        if (change == "no-loader") selected.mbr = true;
        if (change == "capacity") selected.size = 128 * 1024 * 1024;
        rufus::PrepareIsoSource retained(selected);
        QCOMPARE(retained.open(getuid()), 0);
        cancelled = change == "cancel";
        const qsizetype descriptors = QDir("/proc/self/fd").entryList().size();
        rufus::WindowsToGoImage image;
        QCOMPARE(image.open(selected, retained), expected);
        QVERIFY(image.handle() == nullptr);
        QCOMPARE(QDir("/proc/self/fd").entryList().size(), descriptors);
        cancelled = false;
        QCOMPARE(retained.close(), 0);
    }

    void protocolAndIdentity_data()
    {
        QTest::addColumn<bool>("mbr"); QTest::addColumn<int>("target");
        QTest::addColumn<quint32>("logical"); QTest::addColumn<quint32>("physical");
        QTest::newRow("GPT-UEFI") << false << int(TT_UEFI) << quint32(512) << quint32(512);
        QTest::newRow("GPT-UEFI-512e") << false << int(TT_UEFI) << quint32(512) << quint32(4096);
        QTest::newRow("GPT-UEFI-4Kn") << false << int(TT_UEFI) << quint32(4096) << quint32(4096);
        QTest::newRow("MBR-BIOS") << true << int(TT_BIOS) << quint32(512) << quint32(512);
        QTest::newRow("MBR-UEFI") << true << int(TT_UEFI) << quint32(512) << quint32(512);
    }
    void protocolAndIdentity()
    {
        QFETCH(bool, mbr); QFETCH(int, target);
        QFETCH(quint32, logical); QFETCH(quint32, physical);
        makeImage();
        auto selected = request();
        selected.mbr = mbr;
        selected.uefiNtfs = mbr;
        selected.windowsToGoTarget = target;
        selected.logicalSectorSize = logical; selected.physicalSectorSize = physical;
        selected.windowsFlags = UNATTEND_FORCE_S_MODE;
        QVERIFY(rufus::ValidFormatGeometry(selected));
        const auto clusters = rufus::NativeFileSystemClusterSizes(selected.size, FS_NTFS, false,
            BT_IMAGE, mbr ? rufus::PartitionScheme::Mbr : rufus::PartitionScheme::Gpt,
            mbr, MBR_BOOT_ZERO, false, true, target, true, logical, physical);
        QVERIFY(clusters.Allowed & selected.clusterSize);
        const auto encoded = rufus::EncodePrepareRequest(selected);
        QVERIFY(!encoded.isEmpty());
        rufus::PrepareImageRequest decoded;
        QVERIFY(rufus::DecodePrepareRequest(encoded, decoded));
        QCOMPARE(decoded.windowsToGoTarget, target);
        QCOMPARE(decoded.logicalSectorSize, logical); QCOMPARE(decoded.physicalSectorSize, physical);
        QCOMPARE(rufus::EncodePrepareRequest(decoded), encoded);
        QVERIFY(decoded.windowsToGo);
        QCOMPARE(decoded.windowsEdition, 2);
        QCOMPARE(decoded.mbr, mbr);
        QCOMPARE(decoded.usesUefiNtfs(), mbr);
        auto bad = encoded;
        auto toGo = bad.value("toGo").toObject();
        toGo.insert("edition", 2); bad.insert("toGo", toGo);
        QVERIFY(!rufus::DecodePrepareRequest(bad, decoded));
        rufus::PrepareImageOutcome complete, parsed;
        complete.started = complete.windowsToGoDone = true;
        complete.copiedBytes = 123;
        const auto outcome = rufus::EncodePrepareOutcome(complete);
        QVERIFY(rufus::DecodePrepareOutcome(outcome, parsed));
        QVERIFY(parsed.windowsToGoDone);
        auto failed = outcome; failed.insert("syncError", EIO);
        QVERIFY(!rufus::DecodePrepareOutcome(failed, parsed));
        selected.device = 0; // Native component file target; no block device is opened.
        const QString disk = temporary_.filePath("disk.img");
        QFile file(disk);
        QVERIFY(file.open(QIODevice::ReadWrite | QIODevice::Truncate));
        QVERIFY(file.resize(qint64(selected.size)));
        GPT_IMAGE_RESULT layout = {};
        constexpr quint32 diskSignature = 0xa1b2c3d4;
        if (mbr)
        {
            const MBR_IMAGE_OPTIONS options = {selected.size, logical, 63, 4096, diskSignature, 0, FS_NTFS,
                rufus::FormatMbrBootCode(selected), RUFUS_UEFI_NTFS_BYTES, 0, false, true, false, physical};
            MBR_IMAGE_RESULT result = {};
            QCOMPARE(CreateMbrImage(file.handle(), &options, &result), 0);
            layout.offset = result.offset;
        }
        else
        {
            const GPT_IMAGE_OPTIONS options = {selected.size, logical, 63, 4096, 0, 0, 0, false, true, physical};
            QCOMPARE(CreateGptImagePartition(file.handle(), &options, &layout), 0);
            QCOMPARE(rufus::CheckFormatSettings(selected, layout), 0);
            // Windows To Go's GPT ESP is a separate FAT32 volume. Its cluster
            // must fit the frozen logical sector, including native 4-KiB media.
            const FAT32_FORMAT_OPTIONS esp = {layout.esp_offset, layout.esp_size,
                qMax(quint32(1024), logical), 0, {0}, false, logical, physical};
            QCOMPARE(CheckFat32FormatOptions(&esp), 0);
            QCOMPARE(layout.esp_offset % physical, quint64(0));
            QCOMPARE(layout.esp_size, quint64(260) * 1024 * 1024);
            if (logical == 4096) {
                FAT32_FORMAT_OPTIONS undersized = esp; undersized.cluster_size = 1024;
                QCOMPARE(CheckFat32FormatOptions(&undersized), EINVAL);
            }
        }
        QVERIFY(file.seek(17));
        rufus::WindowsBcdOptions identity;
        QCOMPARE(rufus::ReadWindowsToGoBootIdentity(file.handle(), selected, diskSignature, identity), 0);
        QCOMPARE(lseek(file.handle(), 0, SEEK_CUR), off_t(17));
        QCOMPARE(identity.windows.gpt, !mbr);
        if (mbr)
        {
            QCOMPARE(identity.windows.offset, layout.offset);
            QCOMPARE(identity.system.mbrSignature, diskSignature);
            QVERIFY(file.seek(0));
            const QByteArray bytes = file.read(512);
            QCOMPARE(qFromLittleEndian<quint32>(bytes.constData() + 440), diskSignature);
            if (target == TT_UEFI) QCOMPARE(bytes.first(440), QByteArray(440, '\0'));
        }
        else
        {
            QVERIFY(!identity.windows.diskGuid.isNull());
            QCOMPARE(identity.system.diskGuid, identity.windows.diskGuid);
            QVERIFY(identity.system.partitionGuid != identity.windows.partitionGuid);
            QVERIFY(file.seek(2 * logical));
            const QByteArray entries = file.read(3 * 128);
            const QUuid esp(qFromLittleEndian<quint32>(entries.constData() + 16),
                qFromLittleEndian<quint16>(entries.constData() + 20), qFromLittleEndian<quint16>(entries.constData() + 22),
                quint8(entries[24]), quint8(entries[25]), quint8(entries[26]), quint8(entries[27]),
                quint8(entries[28]), quint8(entries[29]), quint8(entries[30]), quint8(entries[31]));
            QCOMPARE(identity.system.partitionGuid, esp);
            if (logical == 4096) {
                // A stale 512-byte geometry must not discover plausible GUIDs
                // from the same 4Kn GPT. It cannot become a successful BCD ID.
                auto stale = selected; stale.logicalSectorSize = 512;
                rufus::WindowsBcdOptions refused;
                QVERIFY(rufus::ReadWindowsToGoBootIdentity(file.handle(), stale, diskSignature, refused) != 0);
                QVERIFY(refused.windows.diskGuid.isNull());
                QVERIFY(refused.system.partitionGuid.isNull());
            }
        }
    }
};

QTEST_GUILESS_MAIN(WindowsToGoTest)
#include "test_windows_to_go.moc"
