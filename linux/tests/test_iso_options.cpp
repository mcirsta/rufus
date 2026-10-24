// SPDX-License-Identifier: GPL-3.0-or-later
#include "qt/selection_controller.h"
#include "qt/main_window.h"
#include "qt/application_storage.h"
#include "platform/iso_extract.h"
#include "platform/prepare_source.h"
#include "platform/format_options.h"
#include <QCheckBox>
#include <QCryptographicHash>
#include <QDirIterator>
#include <QDBusConnection>
#include <QFile>
#include <QJsonObject>
#include <QProcess>
#include <QPushButton>
#include <QSignalSpy>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTest>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

namespace
{
constexpr time_t sourceTime = 1234567890; // 2009-02-13 UTC, even FAT seconds checked separately.
const QByteArray payload = "Rufus ISO read-option payload\n";
struct View
{
    ISO_ANALYSIS_SOURCE source = {};
    ~View() { CloseIsoAnalysisSource(&source); }
};
bool writeFile(const QString& path, const QByteArray& bytes)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
    QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
QByteArray readFile(const QString& path)
{
    QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
rufus::PrepareImageRequest target()
{
    rufus::PrepareImageRequest request;
    request.isDevice = true; request.path = QStringLiteral("/test/no-device"); request.device = makedev(8, 239);
    request.size = 512ULL * 1024 * 1024; request.diskSequence = 42;
    request.objectPath = QStringLiteral("/org/freedesktop/UDisks2/block_devices/fixture");
    request.serviceOwner = QStringLiteral(":1.42"); request.identity = {QStringLiteral("fixture")};
    request.fileSystem = FS_FAT32; request.clusterSize = 4096; request.label = QStringLiteral("OPTIONS");
    return request;
}
}
class TestIsoOptions : public QObject
{
    Q_OBJECT
    QTemporaryDir directory_;
    QString shortIso_, longIso_, largeIso_, udfIso_;
    QString longName_ = QString(75, QLatin1Char('a')) + QStringLiteral(".txt");
    QString makeImage(const QString& name, bool longNames, bool large = false, bool udf = false)
    {
        const QString tree = directory_.filePath(name + QStringLiteral("-tree"));
        if (!QDir().mkpath(tree + QStringLiteral("/empty")) || !QDir().mkpath(tree + QStringLiteral("/sub"))) return {};
        if (!writeFile(tree + QLatin1Char('/') + (longNames ? longName_ : QStringLiteral("Mixed-Case.txt")), payload)
            || !writeFile(tree + QStringLiteral("/sub/child.txt"), payload)) return {};
        if (large) {
            QFile file(tree + QStringLiteral("/large.bin"));
            if (!file.open(QIODevice::WriteOnly) || !file.resize(32 * 1024 * 1024)) return {};
        }
        QDirIterator entries(tree, QDir::AllEntries | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (entries.hasNext()) {
            const QByteArray path = QFile::encodeName(entries.next());
            const timespec times[2] = {{sourceTime, 0}, {sourceTime, 0}};
            if (utimensat(AT_FDCWD, path.constData(), times, AT_SYMLINK_NOFOLLOW) != 0) return {};
        }
        const QString image = directory_.filePath(name + QStringLiteral(".iso"));
        QStringList arguments = {"-quiet", "-R", "-J", "-V", "OPTIONS", "-o", image, tree};
        if (udf) arguments.prepend(QStringLiteral("-udf"));
        QProcess process; process.start(QStringLiteral(RUFUS_MKISOFS), arguments);
        if (!process.waitForFinished(30000) || process.exitCode() != 0) return {};
        QFile marker(image); if (!marker.open(QIODevice::ReadWrite) || !marker.seek(510) || marker.write(QByteArray::fromHex("55aa")) != 2) return {};
        return image;
    }
private slots:
    void initTestCase()
    {
        QVERIFY(directory_.isValid()); qputenv("TZ", "UTC"); tzset();
        QString problem; QCOMPARE(rufus::ConfigureApplicationStorage(directory_.filePath("rufus-linux"), true, problem), 0);
        shortIso_ = makeImage("short", false); longIso_ = makeImage("long", true); largeIso_ = makeImage("large", false, true);
        udfIso_ = makeImage("udf", false, false, true);
        QVERIFY(!shortIso_.isEmpty() && !longIso_.isEmpty() && !largeIso_.isEmpty() && !udfIso_.isEmpty());
    }
    void init()
    {
        rufus::ApplicationSettings settings; settings.clear(); settings.sync();
    }
    void selectedViews_data()
    {
        QTest::addColumn<bool>("longNames"); QTest::addColumn<bool>("joliet"); QTest::addColumn<bool>("rock");
        for (bool longNames : {false, true}) for (bool joliet : {false, true}) for (bool rock : {false, true})
            QTest::newRow(qPrintable(QStringLiteral("long%1-J%2-R%3").arg(longNames).arg(joliet).arg(rock))) << longNames << joliet << rock;
    }
    void selectedViews()
    {
        QFETCH(bool, longNames); QFETCH(bool, joliet); QFETCH(bool, rock);
        const QString image = longNames ? longIso_ : shortIso_;
        const QByteArray path = QFile::encodeName(image); View view; ISO_ANALYSIS analysis;
        const ISO_ANALYSIS_OPTIONS options = {rock, joliet, {0, 0}};
        const int result = AnalyzeIsoImageWithSource(path.constData(), quint64(QFileInfo(image).size()), &options, &analysis, &view.source);
        QVERIFY(result == ISO_SCAN_OK || result == ISO_SCAN_PARTIAL); QVERIFY(view.source.iso);
        const bool useJoliet = joliet && !(rock && longNames);
        QCOMPARE(iso9660_ifs_get_joliet_level(view.source.iso) != 0, useJoliet);
        const QString output = directory_.filePath(QStringLiteral("out-%1-%2-%3").arg(longNames).arg(joliet).arg(rock));
        QVERIFY(QDir().mkdir(output)); ISO_EXTRACT_RESULT copied;
        const ISO_EXTRACT_OPTIONS extract = {options, nullptr, nullptr, false, 0, nullptr, false};
        QCOMPARE(ExtractIsoTree(path.constData(), QFile::encodeName(output).constData(), &extract, &copied), 0);
        QCOMPARE(copied.files, 2ULL);
        const QStringList names = QDir(output).entryList(QDir::Files);
        QCOMPARE(names.size(), 1); QCOMPARE(readFile(output + QLatin1Char('/') + names[0]), payload);
        if (rock && !useJoliet) QCOMPARE(names[0], longNames ? longName_ : QStringLiteral("Mixed-Case.txt"));
        if (longNames && useJoliet) QVERIFY(names[0].size() <= 64);
    }
    void timestamps_data()
    {
        QTest::addColumn<bool>("preserve"); QTest::addColumn<bool>("udf");
        QTest::newRow("iso-current") << false << false; QTest::newRow("iso-source") << true << false;
        QTest::newRow("udf-current") << false << true; QTest::newRow("udf-source") << true << true;
    }
    void timestamps()
    {
        QFETCH(bool, preserve); QFETCH(bool, udf);
        const QString output = directory_.filePath(QStringLiteral("time-%1-%2").arg(udf).arg(preserve));
        QVERIFY(QDir().mkdir(output)); ISO_EXTRACT_RESULT copied;
        const ISO_EXTRACT_OPTIONS options = {{true, true, {0, 0}}, nullptr, nullptr, false, 0, nullptr, preserve};
        QCOMPARE(ExtractIsoTree(QFile::encodeName(udf ? udfIso_ : shortIso_).constData(), QFile::encodeName(output).constData(), &options, &copied), 0);
        for (const QString& name : {QStringLiteral("Mixed-Case.txt"), QStringLiteral("sub/child.txt"), QStringLiteral("empty"), QStringLiteral("sub")}) {
            struct stat info; QCOMPARE(stat(QFile::encodeName(output + QLatin1Char('/') + name).constData(), &info), 0);
            if (preserve) QCOMPARE(info.st_mtim.tv_sec, sourceTime);
            else QVERIFY(info.st_mtim.tv_sec > sourceTime);
        }
    }
    void protocolAndDosOverride()
    {
        auto request = target(); QCOMPARE(rufus::CapturePrepareSource(shortIso_, request), 0);
        request.preserveTimestamps = true; request.joliet = false;
        auto encoded = rufus::EncodePrepareRequest(request); rufus::PrepareImageRequest decoded;
        QVERIFY(rufus::DecodePrepareRequest(encoded, decoded)); QVERIFY(decoded.preserveTimestamps && !decoded.joliet && decoded.rockRidge);
        auto source = encoded["source"].toObject(); source["preserveTimes"] = false; encoded["source"] = source;
        QVERIFY(!rufus::DecodePrepareRequest(encoded, decoded));
        request = target(); request.msDos = request.mbr = true; request.fileSystem = FS_FAT16; request.clusterSize = 65536;
        request.dosImagePath = QStringLiteral("/tmp/diskcopy.dll"); request.dosKeyboard = QStringLiteral("us"); request.dosCodepage = 437;
        GPT_IMAGE_RESULT partition; QCOMPARE(rufus::PlanFormatPartition(request, partition), 0);
        QVERIFY(rufus::CheckFormatSettings(request, partition) != 0);
        request.sizeChecks = false; QCOMPARE(rufus::CheckFormatSettings(request, partition), 0);
        encoded = rufus::EncodePrepareRequest(request); QVERIFY(rufus::DecodePrepareRequest(encoded, decoded)); QVERIFY(!decoded.sizeChecks);
        encoded["skipSizeChecks"] = false; QVERIFY(!rufus::DecodePrepareRequest(encoded, decoded));
        const auto standard = rufus::NativeFileSystemClusterSizes(request.size, FS_FAT16, false, BT_MSDOS, rufus::PartitionScheme::Mbr);
        const auto forced = rufus::NativeFileSystemClusterSizes(request.size, FS_FAT16, false, BT_MSDOS, rufus::PartitionScheme::Mbr, false, MBR_BOOT_ZERO, false, false, TT_UEFI, false);
        QVERIFY(!(standard.Allowed & 65536)); QVERIFY(forced.Allowed & 65536);
    }
    void controllerAndShortcuts()
    {
        rufus::SelectionController controller; rufus::MainWindow window(controller); window.show(); window.activateWindow();
        auto* select = window.findChild<QPushButton*>(QStringLiteral("selectImageButton")); QVERIFY(select); select->setFocus();
        QTRY_VERIFY(window.isActiveWindow());
        for (Qt::Key key : {Qt::Key_I, Qt::Key_J, Qt::Key_K, Qt::Key_T, Qt::Key_S}) QTest::keyClick(select, key, Qt::AltModifier);
        QVERIFY(!controller.isoSupportEnabled() && !controller.jolietEnabled() && !controller.rockRidgeEnabled() && !controller.sizeChecksEnabled());
        QVERIFY(controller.preserveTimestamps()); QVERIFY(window.statusBar()->currentMessage().contains(QLatin1String("Size checks disabled")));
        rufus::SelectionController next;
        QVERIFY(next.isoSupportEnabled() && next.jolietEnabled() && next.rockRidgeEnabled() && next.sizeChecksEnabled());
        QVERIFY(next.preserveTimestamps());
    }
    void frozenScanAndRawChoice()
    {
        rufus::SelectionController controller; controller.selectImage(shortIso_); QVERIFY(controller.isAnalyzing());
        controller.toggleJoliet(); QVERIFY(controller.jolietEnabled()); // Busy guard, frozen worker options.
        QTRY_VERIFY_WITH_TIMEOUT(!controller.isAnalyzing(), 10000);
        QCOMPARE(controller.selection().imageState, rufus::ImageState::Complete); QVERIFY(!controller.selection().rawImage);
        controller.setVolumeLabel(QStringLiteral("KEEP")); controller.toggleJoliet();
        QTRY_VERIFY_WITH_TIMEOUT(!controller.isAnalyzing(), 10000);
        QVERIFY(!controller.selection().imageSource.joliet); QCOMPARE(controller.selection().volumeLabel, QStringLiteral("KEEP"));
        controller.toggleIsoSupport(); QTRY_VERIFY_WITH_TIMEOUT(!controller.isAnalyzing(), 10000);
        QVERIFY(controller.selection().rawImage); QCOMPARE(controller.selection().volumeLabel, QStringLiteral("KEEP"));
        controller.toggleIsoSupport(); QTRY_VERIFY_WITH_TIMEOUT(!controller.isAnalyzing(), 10000);
        QVERIFY(!controller.selection().rawImage); QVERIFY(!controller.selection().imageSource.joliet);
    }
    void sizeHeuristicAndHardBounds()
    {
        QProcess daemon;
        daemon.start(QStringLiteral("dbus-daemon"), {"--session", "--nofork", "--print-address=1"});
        QVERIFY(daemon.waitForReadyRead(3000));
        const QString name = QStringLiteral("iso-options");
        const auto bus = QDBusConnection::connectToBus(QString::fromUtf8(daemon.readLine()).trimmed(), name);
        QVERIFY(bus.isConnected());
        rufus::SelectionController controller;
        controller.writer().setConnection(bus);
        controller.writer().setHelper(QStringLiteral(RUFUS_PREPARE_HELPER));
        controller.isoWriter().configure(bus, QStringLiteral(RUFUS_PREPARE_HELPER));
        rufus::Device device{QStringLiteral("fixture"), QStringLiteral("/test/no-device"), QStringLiteral("Fixture"), 16ULL * 1024 * 1024, true, false};
        device.identity = {QStringLiteral("fixture")}; device.mediaRemovable = device.defaultVisible = true;
        controller.setDiscovery({device}, {}); controller.selectDevice(device.id); controller.selectImage(largeIso_);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.isAnalyzing(), 10000); QVERIFY(controller.canStartWrite());
        QSignalSpy rejected(&controller, &rufus::SelectionController::imageOptionRejected);
        controller.startWrite(); QCOMPARE(rejected.size(), 1); QVERIFY(!controller.isWritingActive());
        controller.toggleSizeChecks(); controller.startWrite(); QCOMPARE(rejected.size(), 1);
        QVERIFY(!controller.writer().isWriting()); // An oversized raw copy still cannot write beyond the target.
        QDBusConnection::disconnectFromBus(name); daemon.terminate(); QVERIFY(daemon.waitForFinished(3000));
    }
};
QTEST_MAIN(TestIsoOptions)
#include "test_iso_options.moc"
