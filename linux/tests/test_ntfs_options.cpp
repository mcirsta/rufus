// SPDX-License-Identifier: GPL-3.0-or-later
#include "qt/main_window.h"
#include "qt/selection_controller.h"
#include "qt/application_storage.h"
#include "platform/ntfs_format.h"
#include <QFile>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTest>
#include <sys/sysmacros.h>

namespace
{
rufus::PrepareImageRequest target()
{
    rufus::PrepareImageRequest request;
    request.isDevice = request.nonBootable = true; request.path = QStringLiteral("/test/no-device");
    request.device = makedev(8, 239); request.size = 64ULL * 1024 * 1024; request.diskSequence = 42;
    request.objectPath = QStringLiteral("/org/freedesktop/UDisks2/block_devices/fixture");
    request.serviceOwner = QStringLiteral(":1.42"); request.identity = {QStringLiteral("fixture")};
    request.fileSystem = FS_NTFS; request.clusterSize = 4096; request.label = QStringLiteral("OPTIONS");
    return request;
}
}
class TestNtfsOptions : public QObject
{
    Q_OBJECT
    QTemporaryDir directory_;
private slots:
    void initTestCase()
    {
        QVERIFY(directory_.isValid()); QString problem;
        QCOMPARE(rufus::ConfigureApplicationStorage(directory_.filePath("rufus-linux"), true, problem), 0);
    }
    void init()
    {
        rufus::ApplicationSettings settings; settings.clear(); settings.sync();
    }
    void protocol()
    {
        for (bool compression : {false, true}) for (bool indexing : {false, true}) {
            auto request = target(); request.ntfsCompression = compression; request.fileIndexing = indexing;
            const auto packet = rufus::EncodePrepareRequest(request); rufus::PrepareImageRequest decoded;
            QVERIFY(!packet.isEmpty()); QVERIFY(rufus::DecodePrepareRequest(packet, decoded));
            QCOMPARE(decoded.ntfsCompression, compression); QCOMPARE(decoded.fileIndexing, indexing);
            QCOMPARE(packet.contains("ntfsOptions"), compression || indexing);
            QCOMPARE(rufus::EncodePrepareRequest(decoded), packet);
        }
        auto request = target(); request.ntfsCompression = true;
        auto packet = rufus::EncodePrepareRequest(request); auto options = packet["ntfsOptions"].toObject();
        rufus::PrepareImageRequest decoded;
        options["compression"] = 1; packet["ntfsOptions"] = options; QVERIFY(!rufus::DecodePrepareRequest(packet, decoded));
        packet = rufus::EncodePrepareRequest(request); options = {{"compression", false}, {"indexing", false}};
        packet["ntfsOptions"] = options; QVERIFY(!rufus::DecodePrepareRequest(packet, decoded));
        request.fileSystem = FS_FAT32; QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        packet["filesystem"] = FS_FAT32; QVERIFY(!rufus::DecodePrepareRequest(packet, decoded));
        request = target(); request.ntfsCompression = true; request.rawWrite = true;
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
    }
    void udfIndexingProtocol()
    {
        for (quint16 revision : {0x0102, 0x0150, 0x0200, 0x0201})
        for (int layout = 0; layout < 3; ++layout)
        for (bool indexing : {false, true}) {
            auto request = target();
            request.fileSystem = FS_UDF; request.clusterSize = 256; request.udfRevision = revision;
            request.mbr = layout == 1; request.sfd = layout == 2; request.fileIndexing = indexing;
            const auto packet = rufus::EncodePrepareRequest(request);
            QVERIFY(!packet.isEmpty()); QVERIFY(!packet.contains("ntfsOptions"));
            QCOMPARE(packet.contains("udfIndexing"), indexing);
            rufus::PrepareImageRequest decoded;
            QVERIFY(rufus::DecodePrepareRequest(packet, decoded));
            QCOMPARE(decoded.fileIndexing, indexing); QVERIFY(!decoded.ntfsCompression);
            QCOMPARE(decoded.udfRevision, revision);
            QCOMPARE(rufus::EncodePrepareRequest(decoded), packet);
        }
        auto request = target(); request.fileSystem = FS_UDF; request.clusterSize = 256;
        const auto defaultPacket = rufus::EncodePrepareRequest(request);
        rufus::PrepareImageRequest decoded;
        for (const QJsonValue value : {QJsonValue(false), QJsonValue(1), QJsonValue("true"), QJsonValue(QJsonObject())}) {
            auto invalid = defaultPacket; invalid.insert("udfIndexing", value);
            QVERIFY(!rufus::DecodePrepareRequest(invalid, decoded));
        }
        auto invalid = defaultPacket;
        invalid.insert("ntfsOptions", QJsonObject{{"compression", false}, {"indexing", true}});
        QVERIFY(!rufus::DecodePrepareRequest(invalid, decoded));
        invalid.insert("udfIndexing", true);
        QVERIFY(!rufus::DecodePrepareRequest(invalid, decoded));
        request.fileIndexing = request.ntfsCompression = true;
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        request.ntfsCompression = false; request.nonBootable = false;
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        request.nonBootable = true; request.rawWrite = true;
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        request.rawWrite = false; request.isDevice = false;
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        for (int filesystem : {FS_NTFS, FS_FAT32, FS_EXFAT, FS_EXT2, FS_EXT3}) {
            request = target(); request.fileSystem = filesystem;
            if (filesystem == FS_EXT2 || filesystem == FS_EXT3) request.clusterSize = 256;
            auto packet = rufus::EncodePrepareRequest(request);
            QVERIFY(!packet.isEmpty()); packet.insert("udfIndexing", true);
            QVERIFY(!rufus::DecodePrepareRequest(packet, decoded));
            request.fileIndexing = true;
            QCOMPARE(rufus::EncodePrepareRequest(request).isEmpty(), filesystem != FS_NTFS);
        }
    }
    void shortcutsAndPersistence()
    {
        rufus::SelectionController controller; rufus::MainWindow window(controller);
        QVERIFY(!controller.ntfsCompression() && !controller.fileIndexing()); window.show(); window.activateWindow();
        auto* select = window.findChild<QPushButton*>(QStringLiteral("selectImageButton")); QVERIFY(select); select->setFocus();
        QTRY_VERIFY(window.isActiveWindow());
        QTest::keyClick(select, Qt::Key_N, Qt::AltModifier); QTest::keyClick(select, Qt::Key_Q, Qt::AltModifier);
        QVERIFY(controller.ntfsCompression() && controller.fileIndexing());
        rufus::SelectionController next; QVERIFY(!next.ntfsCompression() && next.fileIndexing());
    }
    void formatter_data()
    {
        QTest::addColumn<bool>("compression"); QTest::addColumn<bool>("indexing"); QTest::addColumn<int>("cluster");
        for (int cluster : {4096, 8192}) for (bool compression : {false, true}) for (bool indexing : {false, true})
            QTest::newRow(qPrintable(QStringLiteral("%1-C%2-I%3").arg(cluster).arg(compression).arg(indexing))) << compression << indexing << cluster;
    }
    void formatter()
    {
        QFETCH(bool, compression); QFETCH(bool, indexing); QFETCH(int, cluster);
        const QString path = directory_.filePath(QStringLiteral("%1-%2-%3.ntfs").arg(cluster).arg(compression).arg(indexing));
        QFile volume(path); QVERIFY(volume.open(QIODevice::ReadWrite)); QVERIFY(volume.resize(64 * 1024 * 1024));
        rufus::NtfsFormatOptions options = {quint64(volume.size()), 1024 * 1024, quint32(cluster), QStringLiteral("OPTIONS"), true, compression, indexing};
        rufus::NtfsFormatResult result;
        QCOMPARE(rufus::FormatNtfsImage(volume.handle(), QStringLiteral(RUFUS_MKNTFS), options, result), 0); volume.close();
        QProcess info; info.start(QStringLiteral(RUFUS_NTFSINFO), {"-f", "-i", "5", path});
        QVERIFY(info.waitForFinished(10000)); QCOMPARE(info.exitCode(), 0);
        const QString output = QString::fromUtf8(info.readAllStandardOutput());
        const auto match = QRegularExpression(QStringLiteral("File attributes:[^\\r\\n]+" )).match(output); QVERIFY(match.hasMatch());
        const QString attributes = match.captured();
        QCOMPARE(attributes.contains(QStringLiteral("COMPRESSED")), compression && cluster == 4096);
        QCOMPARE(attributes.contains(QStringLiteral("NOT_CONTENT_INDEXED")), !indexing);
        QVERIFY(QFile::remove(path));
    }
};
QTEST_MAIN(TestNtfsOptions)
#include "test_ntfs_options.moc"
