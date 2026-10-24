// SPDX-License-Identifier: GPL-3.0-or-later
#include "qt/prepare_image_process.h"
#include <QFile>
#include <QDir>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <cerrno>

namespace
{
QString fixturePath;
int launches = 0;
constexpr quint64 imageBytes = 4096;

int protocolCompression()
{
    if (qgetenv("RUFUS_TEST_FFU_PROTOCOL") == "1")
        return IMG_COMPRESSION_FFU;
    if (qgetenv("RUFUS_TEST_VHD_PROTOCOL") == "1")
        return IMG_COMPRESSION_VHD;
    if (qgetenv("RUFUS_TEST_VTSI_PROTOCOL") == "1")
        return BLED_COMPRESSION_VTSI;
    if (qgetenv("RUFUS_TEST_ZSTD_PROTOCOL") == "1")
        return BLED_COMPRESSION_ZSTD;
    if (qgetenv("RUFUS_TEST_ZIP_PROTOCOL") == "1")
        return BLED_COMPRESSION_ZIP;
    if (qgetenv("RUFUS_TEST_LZW_PROTOCOL") == "1")
        return BLED_COMPRESSION_LZW;
    if (qgetenv("RUFUS_TEST_LZMA_PROTOCOL") == "1")
        return BLED_COMPRESSION_LZMA;
    if (qgetenv("RUFUS_TEST_BZIP2_PROTOCOL") == "1")
        return BLED_COMPRESSION_BZIP2;
    return qgetenv("RUFUS_TEST_XZ_PROTOCOL") == "1" ? BLED_COMPRESSION_XZ : BLED_COMPRESSION_GZIP;
}

const char* protocolName()
{
    const int compression = protocolCompression();
    return compression == IMG_COMPRESSION_FFU ? "ffu" : compression == IMG_COMPRESSION_VHD ? "vhd" : compression == BLED_COMPRESSION_VTSI ? "vtsi" : compression == BLED_COMPRESSION_ZSTD ? "zstd" : compression == BLED_COMPRESSION_ZIP ? "zip" : compression == BLED_COMPRESSION_XZ ? "xz" : compression == BLED_COMPRESSION_BZIP2 ? "bzip2"
        : compression == BLED_COMPRESSION_LZMA ? "lzma" : compression == BLED_COMPRESSION_LZW ? "lzw" : "gzip";
}

rufus::PrepareImageRequest rawRequest()
{
    rufus::PrepareImageRequest request;
    request.rawWrite = request.isDevice = true;
    request.path = QStringLiteral("/dev/fixture");
    request.device = 123;
    request.size = 64 * 1024 * 1024;
    request.diskSequence = 1;
    request.objectPath = QStringLiteral("/org/freedesktop/UDisks2/block_devices/fixture");
    request.serviceOwner = QStringLiteral(":1.2");
    request.identity = {QStringLiteral("fixture")};
    request.imagePath = QStringLiteral("/tmp/source.img");
    request.imageDevice = 7;
    request.imageInode = 8;
    request.imageSize = imageBytes;
    request.imageModified = request.imageChanged = QStringLiteral("1:0");
    return request;
}

rufus::PrepareImageRequest compressedRequest(int compression = BLED_COMPRESSION_GZIP)
{
    auto request = rawRequest();
    request.imagePath = compression == IMG_COMPRESSION_FFU ? QStringLiteral("/tmp/source.ffu")
        : compression == IMG_COMPRESSION_VHD ? QStringLiteral("/tmp/source.vhd")
        : compression == BLED_COMPRESSION_VTSI ? QStringLiteral("/tmp/source.vtsi")
        : compression == BLED_COMPRESSION_ZSTD ? QStringLiteral("/tmp/source.zst")
        : compression == BLED_COMPRESSION_ZIP ? QStringLiteral("/tmp/source.zip")
        : compression == BLED_COMPRESSION_XZ ? QStringLiteral("/tmp/source.xz")
        : compression == BLED_COMPRESSION_BZIP2 ? QStringLiteral("/tmp/source.bz2")
        : compression == BLED_COMPRESSION_LZMA ? QStringLiteral("/tmp/source.lzma")
        : compression == BLED_COMPRESSION_LZW ? QStringLiteral("/tmp/source.Z") : QStringLiteral("/tmp/source.gz");
    request.imageSize = 61; // Stored, decoded and padded counts must differ.
    request.imageCompression = compression;
    request.decodedImageSize = 4097;
    request.imageSectorSize = 512;
    request.decodedImageSha256 = QByteArray(32, char(0xab));
    if (compression == BLED_COMPRESSION_VTSI || compression == IMG_COMPRESSION_FFU)
        request.imageLayoutSha256 = QByteArray(32, char(0xcd));
    return request;
}

QJsonObject changedCompression(QJsonObject object, const QString& key, const QJsonValue& value)
{
    QJsonObject source = object.value("source").toObject();
    QJsonObject compression = source.value("compression").toObject();
    compression.insert(key, value);
    source.insert("compression", compression);
    object.insert("source", source);
    return object;
}
}

// Only this test executable redirects authorization to an ordinary protocol child.
extern "C" void __real__ZN8QProcess5startERK7QStringRK5QListIS0_E6QFlagsIN13QIODeviceBase12OpenModeFlagEE(
    QProcess*, const QString&, const QStringList&, QIODevice::OpenMode);
extern "C" void __wrap__ZN8QProcess5startERK7QStringRK5QListIS0_E6QFlagsIN13QIODeviceBase12OpenModeFlagEE(
    QProcess* process, const QString& program, const QStringList& arguments, QIODevice::OpenMode mode)
{
    const QStringList expected = {QStringLiteral("--disable-internal-agent"), fixturePath, QStringLiteral("--device")};
    if (program != QStringLiteral("/usr/bin/pkexec") || arguments != expected)
        qFatal("Unexpected raw helper launch");
    ++launches;
    __real__ZN8QProcess5startERK7QStringRK5QListIS0_E6QFlagsIN13QIODeviceBase12OpenModeFlagEE(process, fixturePath, {}, mode);
}

class TestRawHelperProcess : public QObject
{
    Q_OBJECT
    QTemporaryDir directory_;

private slots:
    void badBlocksProtocol()
    {
        auto request = rawRequest();
        for (int pattern = 0; pattern < 5; ++pattern)
        {
            request.badBlocksPattern = pattern;
            for (bool fake : {false, true})
            {
                request.detectFakes = fake;
                const auto object = rufus::EncodePrepareRequest(request);
                rufus::PrepareImageRequest decoded;
                QVERIFY(rufus::DecodePrepareRequest(object, decoded));
                QCOMPARE(decoded.badBlocksPattern, pattern); QCOMPARE(decoded.detectFakes, fake);
                for (const QJsonValue& value : {QJsonValue(-1), QJsonValue(5), QJsonValue(0.5), QJsonValue("1")})
                {
                    auto bad = object; auto test = bad.value("badBlocks").toObject();
                    test.insert("pattern", value); bad.insert("badBlocks", test);
                    QVERIFY(!rufus::DecodePrepareRequest(bad, decoded));
                }
            }
        }
        request.badBlocksPattern = -1;
        QVERIFY(!rufus::EncodePrepareRequest(request).contains("badBlocks"));
        request.badBlocksPattern = 5;
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
    }

    void badBlocksLifecycle_data()
    {
        QTest::addColumn<QString>("mode");
        for (const char* mode : {"clean", "ignore", "retry", "abort", "cancel", "missing", "duplicate", "stale", "counts", "log-failure", "format"})
            QTest::newRow(mode) << QString::fromLatin1(mode);
    }

    void badBlocksLifecycle()
    {
        QFETCH(QString, mode);
        QTemporaryDir reports;
        QVERIFY(reports.isValid());
        auto request = rawRequest(); request.badBlocksPattern = 4;
        const bool format = mode == QStringLiteral("format");
        if (format)
        {
            request.rawWrite = false; request.nonBootable = request.mbr = true;
            request.imagePath.clear(); request.imageDevice = request.imageInode = request.imageSize = 0;
            request.imageModified.clear(); request.imageChanged.clear();
        }
        fixturePath = directory_.filePath(QStringLiteral("badblocks-helper"));
        QFile script(fixturePath);
        QVERIFY(script.open(QIODevice::WriteOnly | QIODevice::Truncate));
        const QByteArray code = QByteArray(R"PY(#!/usr/bin/python3
import json,sys,time
mode=')PY") + mode.toUtf8() + R"PY('
def send(o): print(json.dumps(o),flush=True)
send({'event':'ready'})
r=json.loads(sys.stdin.readline())
assert r['badBlocks']=={'pattern':4,'detectFakes':True}
def phase(p): send({'event':'phase','phase':p})
phase('checking')
error=0
if mode!='missing':
 for round in (1,2):
  phase('badblocks')
  total=int(r['size'])*8
  send({'event':'progress','operation':1,'processed':'0','total':str(total)})
  send({'event':'progress','operation':1,'processed':str(total),'total':str(total)})
  bad=mode in ('ignore','retry','abort','cancel','duplicate','stale','counts') and round==1
  if bad:
   event={'event':'badblocks-error','round':round,'block':'3','type':2}
   send(event)
   if mode=='duplicate': send(event)
  report={'event':'badblocks-report','round':round,'error':0,'count':int(bad),'read':0,'write':0,'corrupt':int(bad),'written':str(total//2),'readBytes':str(total//2),'threshold':False}
  if mode=='stale': report['round']=2
  if mode=='counts': report['count']=report['corrupt']=2
  send(report)
  if bad:
   line=sys.stdin.readline()
   if not line: error=125; break
   answer=json.loads(line); assert answer['round']==round
   if answer['badBlocks']=='abort': error=125; break
   if answer['badBlocks']=='retry': continue
  break
if r['operation'].startswith('raw-'):
 if not error:
  for p,n,t in ((1,0,4096),(1,4096,4096),(2,0,0),(3,0,4096),(3,4096,4096)):
   send({'event':'raw-progress','phase':p,'processed':str(n),'total':str(t)})
 send({'event':'raw-result','error':error,'syncError':0,'started':True,'phase':0 if error else 4,'written':'0' if error else '4096','verified':'0' if error else '4096'})
else:
 for p in ('partitioning','formatting','finalizing'): phase(p)
 send({'event':'result','error':0,'syncError':0,'cleanupError':0,'started':True,'offset':'1048576','size':'66060288','filesystemBytes':'65000000','copiedBytes':'0'})
time.sleep(.1)
sys.exit(1 if error else 0)
)PY";
        QCOMPARE(script.write(code), code.size());
        QVERIFY(script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner)); script.close();
        launches = 0;
        rufus::PrepareImageProcess process(fixturePath);
        QString reportDirectory = reports.path();
        if (mode == QStringLiteral("log-failure"))
        {
            reportDirectory = reports.filePath(QStringLiteral("not-a-directory"));
            QFile blocker(reportDirectory);
            QVERIFY(blocker.open(QIODevice::WriteOnly));
            blocker.close();
        }
        process.setReportDirectory(reportDirectory);
        QSignalSpy done(&process, &rufus::PrepareImageProcess::finished);
        QSignalSpy review(&process, &rufus::PrepareImageProcess::badBlocksReviewRequired);
        if (mode == QStringLiteral("log-failure"))
        {
            QVERIFY(!process.start(request)); QCOMPARE(launches, 0); QVERIFY(!process.problem().isEmpty()); return;
        }
        QVERIFY(process.start(request));
        const bool decision = mode == "ignore" || mode == "retry" || mode == "abort" || mode == "cancel";
        if (decision)
        {
            QTRY_COMPARE_WITH_TIMEOUT(review.size(), 1, 5000);
            QVERIFY(process.isActive()); QCOMPARE(done.size(), 0);
            const QString path = review[0][1].toString(); QVERIFY(QFile::exists(path));
            if (mode == "cancel") process.cancel();
            else process.answerBadBlocks(mode == "retry" ? rufus::BadBlocksRetry : mode == "ignore" ? rufus::BadBlocksIgnore : rufus::BadBlocksAbort);
        }
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 5000);
        const bool success = mode == "clean" || mode == "ignore" || mode == "retry" || format;
        const bool cancel = mode == "abort" || mode == "cancel";
        using State = rufus::PrepareImageProcess::State;
        QCOMPARE(process.state(), success ? State::Complete : cancel ? State::Cancelled : State::Failed);
        QCOMPARE(launches, 1);
        const QStringList logs = QDir(reports.path()).entryList({QStringLiteral("*.log")}, QDir::Files);
        if (mode == "clean" || mode == "missing" || format) QVERIFY(logs.isEmpty());
        else QCOMPARE(logs.size(), 1);
    }

    void requestProtocol()
    {
        auto request = rawRequest();
        const QJsonObject encoded = rufus::EncodePrepareRequest(request);
        QCOMPARE(encoded.size(), 9);
        QCOMPARE(encoded.value("operation").toString(), QStringLiteral("raw-write-device-v1"));
        QCOMPARE(encoded.value("source").toObject().size(), 6);
        rufus::PrepareImageRequest decoded;
        QVERIFY(rufus::DecodePrepareRequest(encoded, decoded));
        QVERIFY(decoded.rawWrite && decoded.isDevice);
        QCOMPARE(decoded.imageSize, imageBytes);
        for (const char* field : {"cluster", "volume", "label", "windows", "enhancedInstall", "inode", "command"})
        {
            QJsonObject invalid = encoded;
            invalid.insert(QLatin1String(field), 1);
            QVERIFY(!rufus::DecodePrepareRequest(invalid, decoded));
            QVERIFY(decoded.rawWrite); // Refusal leaves the destination unchanged.
        }
        for (const char* field : {"path", "device", "size", "sequence", "object", "owner", "identity", "source"})
        {
            QJsonObject invalid = encoded;
            invalid.remove(QLatin1String(field));
            invalid.insert("unknown", 1); // Retain field count to check required keys.
            QVERIFY(!rufus::DecodePrepareRequest(invalid, decoded));
        }
        request.nonBootable = true;
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        request = rawRequest(); request.windowsFlags = 1;
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        request = rawRequest(); request.clusterSize = 4096;
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        request = rawRequest(); request.isDevice = false;
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        request = rawRequest(); request.imagePath.clear();
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
    }

    void zeroProtocol()
    {
        for (int mode : {ZERO_FULL, ZERO_FAST})
        {
            auto request = rawRequest();
            request.imagePath.clear(); request.imageDevice = request.imageInode = request.imageSize = 0;
            request.imageModified.clear(); request.imageChanged.clear();
            request.zeroMode = mode;
            const auto packet = rufus::EncodePrepareRequest(request);
            QCOMPARE(packet.size(), 8);
            QVERIFY(!packet.contains("source"));
            rufus::PrepareImageRequest decoded;
            QVERIFY(rufus::DecodePrepareRequest(packet, decoded));
            QVERIFY(decoded.rawWrite && decoded.isSourceFree());
            QCOMPARE(decoded.zeroMode, mode);
            QCOMPARE(rufus::PrepareRawWriteBytes(decoded), request.size);
            for (const char* field : {"source", "compression", "fast", "label", "cluster", "volume"})
            {
                auto invalid = packet; invalid.insert(QLatin1String(field), true);
                QVERIFY(!rufus::DecodePrepareRequest(invalid, decoded));
            }
            for (const char* size : {"0", "513", "9223372036854775808"})
            {
                auto invalid = packet; invalid.insert("size", QLatin1String(size));
                QVERIFY(!rufus::DecodePrepareRequest(invalid, decoded));
            }
            request.imageInode = 1;
            QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        }
    }

    void rawMarkerOverrideProtocol()
    {
        auto request = rawRequest();
        request.ignoreBootMarker = true;
        const auto packet = rufus::EncodePrepareRequest(request);
        QCOMPARE(packet.value("operation").toString(), QStringLiteral("raw-write-device-v1"));
        QCOMPARE(packet.value("source").toObject().size(), 7);
        rufus::PrepareImageRequest decoded;
        QVERIFY(rufus::DecodePrepareRequest(packet, decoded));
        QVERIFY(decoded.ignoreBootMarker && !decoded.hasCompressedImage());
        for (const QJsonValue& value : {QJsonValue(false), QJsonValue(1), QJsonValue("true"), QJsonValue()})
        {
            auto invalid = packet;
            auto source = invalid.value("source").toObject();
            source.insert("ignoreMarker", value);
            invalid.insert("source", source);
            QVERIFY(!rufus::DecodePrepareRequest(invalid, decoded));
        }
        auto invalid = packet;
        auto source = invalid.value("source").toObject();
        source.remove("ignoreMarker"); source.insert("unknown", true);
        invalid.insert("source", source);
        QVERIFY(!rufus::DecodePrepareRequest(invalid, decoded));
    }

    void fixedUsbRequest()
    {
        auto request = rawRequest();
        request.allowFixedUsb = true;
        const QJsonObject encoded = rufus::EncodePrepareRequest(request);
        QCOMPARE(encoded.size(), 10);
        QCOMPARE(encoded.value("allowFixedUsb"), QJsonValue(true));
        rufus::PrepareImageRequest decoded;
        QVERIFY(rufus::DecodePrepareRequest(encoded, decoded));
        QVERIFY(decoded.allowFixedUsb && decoded.rawWrite);
        for (const QJsonValue& value : {QJsonValue(false), QJsonValue(1), QJsonValue("true"), QJsonValue()})
        {
            QJsonObject invalid = encoded;
            invalid.insert("allowFixedUsb", value);
            QVERIFY(!rufus::DecodePrepareRequest(invalid, decoded));
        }
        request.allowFixedUsb = false;
        QVERIFY(rufus::DecodePrepareRequest(rufus::EncodePrepareRequest(request), decoded));
        QVERIFY(!decoded.allowFixedUsb); // Reusing the destination must reset the policy.
    }

    void compressedProtocol_data()
    {
        QTest::addColumn<int>("compression");
        QTest::newRow(protocolName()) << protocolCompression();
    }

    void ffuStoreProtocol()
    {
        auto request = compressedRequest(IMG_COMPRESSION_FFU);
        rufus::PrepareImageRequest decoded;
        for (quint32 store : {1u, 2u, 65535u})
        {
            request.ffuStore = store;
            QVERIFY(rufus::DecodePrepareRequest(rufus::EncodePrepareRequest(request), decoded));
            QCOMPARE(decoded.ffuStore, store);
        }
        request.ffuStore = 0;
        const auto legacy = rufus::EncodePrepareRequest(request);
        QVERIFY(rufus::DecodePrepareRequest(legacy, decoded));
        QCOMPARE(decoded.ffuStore, 0u);
        for (const QJsonValue& value : {QJsonValue(0), QJsonValue(-1), QJsonValue(65536),
             QJsonValue(1.5), QJsonValue("2"), QJsonValue(true)})
            QVERIFY(!rufus::DecodePrepareRequest(changedCompression(legacy, QStringLiteral("store"), value), decoded));
        auto gzip = compressedRequest(BLED_COMPRESSION_GZIP);
        QVERIFY(!rufus::DecodePrepareRequest(changedCompression(rufus::EncodePrepareRequest(gzip), QStringLiteral("store"), 1), decoded));
        request.ffuStore = 65536;
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
    }

    void compressedProtocol()
    {
        QFETCH(int, compression);
        auto request = compressedRequest(compression);
        const QJsonObject encoded = rufus::EncodePrepareRequest(request);
        QCOMPARE(encoded.size(), 9);
        QCOMPARE(encoded.value("operation"), QJsonValue("compressed-write-device-v1"));
        QCOMPARE(encoded.value("source").toObject().size(), 7);
        QCOMPARE(encoded.value("source").toObject().value("compression").toObject().size(),
            (compression == BLED_COMPRESSION_VTSI || compression == IMG_COMPRESSION_FFU) ? 6 : 5);
        QVERIFY(QJsonDocument(encoded).toJson(QJsonDocument::Compact).size() < rufus::PrepareMessageBytes);
        rufus::PrepareImageRequest decoded;
        QVERIFY(rufus::DecodePrepareRequest(encoded, decoded));
        QCOMPARE(decoded.imageSize, quint64(61));
        QCOMPARE(decoded.decodedImageSize, quint64(4097));
        QCOMPARE(decoded.decodedImageSha256, request.decodedImageSha256);
        QCOMPARE(decoded.imageLayoutSha256, request.imageLayoutSha256);
        QCOMPARE(rufus::PrepareRawWriteBytes(decoded), quint64(4608));
        for (unsigned sector : {512u, 1024u, 2048u, 4096u, 8192u, 16384u, 32768u, 65536u})
        {
            request.imageSectorSize = sector;
            request.ignoreBootMarker = true;
            QVERIFY(rufus::DecodePrepareRequest(rufus::EncodePrepareRequest(request), decoded));
            QCOMPARE(decoded.imageSectorSize, sector);
            QVERIFY(decoded.ignoreBootMarker);
        }
        QList<QPair<QString, QJsonValue>> invalid = {
            {"type", 0}, {"type", 7},
            {"type", 7}, {"type", "1"}, {"type", "2"}, {"type", "3"}, {"type", "4"}, {"type", "5"}, {"type", "6"},
            {"type", 1.5}, {"type", 2.5}, {"type", 3.5}, {"type", 4.5}, {"type", 5.5}, {"type", 6.5},
            {"decoded", "0"}, {"decoded", "511"}, {"decoded", "04097"}, {"decoded", "-1"},
            {"decoded", 4097}, {"decoded", "9223372036854775807"},
            {"sector", 0}, {"sector", 513}, {"sector", 65537}, {"sector", "512"}, {"sector", false},
            {"sha256", ""}, {"sha256", QString(62, QLatin1Char('a'))}, {"sha256", QString(64, QLatin1Char('A'))},
            {"sha256", QString(64, QLatin1Char('x'))}, {"sha256", true},
            {"ignoreMarker", "true"}, {"ignoreMarker", 1}, {"ignoreMarker", QJsonValue()}};
        for (int other : {int(IMG_COMPRESSION_VHD), int(BLED_COMPRESSION_VTSI), int(BLED_COMPRESSION_ZSTD), int(BLED_COMPRESSION_ZIP), int(BLED_COMPRESSION_LZW), int(BLED_COMPRESSION_GZIP),
                          int(BLED_COMPRESSION_LZMA), int(BLED_COMPRESSION_BZIP2), int(BLED_COMPRESSION_XZ)})
            if (other != compression) invalid.append({QStringLiteral("type"), other});
        for (const auto& entry : invalid)
        {
            QVERIFY2(!rufus::DecodePrepareRequest(changedCompression(encoded, entry.first, entry.second), decoded), qPrintable(entry.first));
            QCOMPARE(decoded.imageCompression, compression);
        }
        if (compression == BLED_COMPRESSION_VTSI || compression == IMG_COMPRESSION_FFU)
        {
            for (const QJsonValue& value : {QJsonValue(""), QJsonValue(QString(64, QLatin1Char('A'))),
                 QJsonValue(QString(62, QLatin1Char('c'))), QJsonValue(7)})
                QVERIFY(!rufus::DecodePrepareRequest(changedCompression(encoded, QStringLiteral("layoutSha256"), value), decoded));
        }
        else
            QVERIFY(!rufus::DecodePrepareRequest(changedCompression(encoded, QStringLiteral("layoutSha256"), QString(64, QLatin1Char('c'))), decoded));
        QJsonObject legacyOperation = encoded;
        legacyOperation.insert("operation", "raw-write-device-v1");
        QVERIFY(!rufus::DecodePrepareRequest(legacyOperation, decoded));
        for (const char* key : {"type", "decoded", "sector", "sha256", "ignoreMarker", "layoutSha256"})
        {
            if (qstrcmp(key, "layoutSha256") == 0 && compression != BLED_COMPRESSION_VTSI)
                continue;
            QJsonObject bad = encoded;
            QJsonObject source = bad.value("source").toObject();
            QJsonObject compression = source.value("compression").toObject();
            compression.remove(QLatin1String(key));
            compression.insert("unknown", true);
            source.insert("compression", compression);
            bad.insert("source", source);
            QVERIFY(!rufus::DecodePrepareRequest(bad, decoded));
        }
        request = compressedRequest(compression); request.rawWrite = false;
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        request = compressedRequest(compression); request.decodedImageSha256.clear();
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        request = compressedRequest(compression); request.size = 4097;
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        request = compressedRequest(compression); request.imagePath = QStringLiteral("/tmp/source.img");
        QVERIFY(rufus::EncodePrepareRequest(request).isEmpty());
        request = rawRequest(); request.ignoreBootMarker = true;
        QVERIFY(rufus::DecodePrepareRequest(rufus::EncodePrepareRequest(request), decoded));
        QVERIFY(decoded.ignoreBootMarker && !decoded.hasCompressedImage());
        QVERIFY(rufus::DecodePrepareRequest(rufus::EncodePrepareRequest(rawRequest()), decoded));
        QVERIFY(!decoded.hasCompressedImage()); // Old packets reset all new state.
    }

    void response_data()
    {
        QTest::addColumn<QString>("mode");
        QTest::addColumn<int>("compression");
        for (const char* name : {"success", "refused", "unknown-write-state", "cancel", "destroy", "deny", "dismiss", "sync-error",
            "backwards", "changed-total", "overrun", "skip-sync", "early-verify", "duplicate-sync", "iso-progress",
            "iso-result", "false-started", "short-write", "short-verify", "bad-phase", "no-result", "extra-result", "wrong-exit",
            "cancel-sync-error", "cancel-close-error", "close-error"})
        {
            const int compression = protocolCompression();
            if (compression == BLED_COMPRESSION_GZIP)
                QTest::newRow(name) << QString::fromLatin1(name) << int(BLED_COMPRESSION_NONE);
            const QByteArray row = QByteArray(protocolName()) + "-" + name;
            QTest::newRow(row.constData()) << QString::fromLatin1(name) << compression;
        }
        const int compression = protocolCompression();
        const QByteArray prefix = QByteArray(protocolName()) + "-";
        QTest::newRow((prefix + "stored-total").constData()) << QStringLiteral("stored-total") << compression;
        QTest::newRow((prefix + "decoded-total").constData()) << QStringLiteral("decoded-total") << compression;
        if (compression == BLED_COMPRESSION_VTSI || compression == IMG_COMPRESSION_FFU)
            for (const char* name : {"sparse-success", "sparse-short-verify", "sparse-overwrite", "sparse-short-progress"})
                QTest::newRow(name) << QString::fromLatin1(name) << compression;
    }

    void response()
    {
        QFETCH(QString, mode);
        QFETCH(int, compression);
        const bool compressed = compression != BLED_COMPRESSION_NONE;
        launches = 0;
        const auto request = compressed ? compressedRequest(compression) : rawRequest();
        const quint64 imageBytes = compression == IMG_COMPRESSION_FFU ? request.size : rufus::PrepareRawWriteBytes(request);
        rufus::PrepareImageOutcome outcome;
        outcome.rawWrite = true;
        outcome.raw = {RAW_WRITE_DONE, imageBytes, imageBytes, 0};
        QList<QJsonObject> frames = {{{"event", "phase"}, {"phase", "checking"}},
            rufus::EncodeRawProgress(RAW_WRITE_COPY, 0, imageBytes),
            rufus::EncodeRawProgress(RAW_WRITE_COPY, imageBytes, imageBytes),
            rufus::EncodeRawProgress(RAW_WRITE_SYNC, 0, 0),
            rufus::EncodeRawProgress(RAW_WRITE_VERIFY, 0, imageBytes),
            rufus::EncodeRawProgress(RAW_WRITE_VERIFY, imageBytes, imageBytes)};
        const bool cancel = mode == QStringLiteral("cancel") || mode == QStringLiteral("destroy")
            || mode == QStringLiteral("cancel-sync-error") || mode == QStringLiteral("cancel-close-error");
        if (mode == QStringLiteral("refused") || mode == QStringLiteral("unknown-write-state"))
        {
            frames = {frames.first()};
            outcome.raw = {};
            outcome.error = EBUSY;
            outcome.started = mode == QStringLiteral("unknown-write-state");
        }
        if (cancel || mode == QStringLiteral("sync-error"))
        {
            frames = frames.mid(0, 4);
            frames[2] = rufus::EncodeRawProgress(RAW_WRITE_COPY, 512, imageBytes);
            const int syncError = mode == QStringLiteral("sync-error") || mode == QStringLiteral("cancel-sync-error") ? EIO : 0;
            outcome.raw = {RAW_WRITE_COPY, 512, 0, syncError};
            outcome.error = cancel ? ECANCELED : EIO;
            if (mode == QStringLiteral("cancel-close-error")) outcome.error = EIO;
        }
        if (mode == QStringLiteral("backwards")) frames.insert(3, rufus::EncodeRawProgress(RAW_WRITE_COPY, 512, imageBytes));
        if (mode == QStringLiteral("changed-total")) frames[2] = rufus::EncodeRawProgress(RAW_WRITE_COPY, 512, imageBytes + 512);
        if (mode == QStringLiteral("overrun")) frames[2] = rufus::EncodeRawProgress(RAW_WRITE_COPY, imageBytes + 1, imageBytes);
        if (mode == QStringLiteral("skip-sync")) frames.removeAt(3);
        if (mode == QStringLiteral("early-verify")) frames[2] = rufus::EncodeRawProgress(RAW_WRITE_COPY, 512, imageBytes);
        if (mode == QStringLiteral("duplicate-sync")) frames.insert(4, frames[3]);
        if (mode == QStringLiteral("iso-progress")) frames[2] = rufus::EncodePrepareProgress(OP_FILE_COPY, 1, 10);
        if (mode == QStringLiteral("short-write")) outcome.raw.bytes_written--;
        if (mode == QStringLiteral("short-verify")) outcome.raw.bytes_verified--;
        if (mode.startsWith(QStringLiteral("sparse-")))
            outcome.raw.bytes_written = outcome.raw.bytes_verified = 1024;
        if (mode == QStringLiteral("sparse-short-verify")) outcome.raw.bytes_verified = 512;
        if (mode == QStringLiteral("sparse-overwrite")) outcome.raw.bytes_written = outcome.raw.bytes_verified = imageBytes + 1;
        if (mode == QStringLiteral("sparse-short-progress")) frames[2] = rufus::EncodeRawProgress(RAW_WRITE_COPY, 1024, imageBytes);
        if (mode == QStringLiteral("bad-phase")) outcome.raw.phase = RAW_WRITE_COPY;
        if (mode == QStringLiteral("close-error")) outcome.error = EIO;
        if (mode == QStringLiteral("stored-total")) frames[1] = rufus::EncodeRawProgress(RAW_WRITE_COPY, 0, request.imageSize);
        if (mode == QStringLiteral("decoded-total")) frames[1] = rufus::EncodeRawProgress(RAW_WRITE_COPY, 0, request.decodedImageSize);
        if (mode == QStringLiteral("iso-result")) outcome.rawWrite = false;
        QByteArray messages;
        for (const QJsonObject& frame : frames)
            messages += QJsonDocument(frame).toJson(QJsonDocument::Compact) + '\n';
        QJsonObject resultObject = rufus::EncodePrepareOutcome(outcome);
        if (mode == QStringLiteral("false-started")) resultObject.insert("started", false);
        QByteArray result = QJsonDocument(resultObject).toJson(QJsonDocument::Compact) + '\n';
        if (mode == QStringLiteral("no-result")) result.clear();
        if (mode == QStringLiteral("extra-result")) result += result;
        const QString marker = directory_.filePath(QStringLiteral("result-sent"));
        QFile::remove(marker);
        fixturePath = directory_.filePath(QStringLiteral("raw-helper"));
        QFile script(fixturePath);
        QVERIFY(script.open(QIODevice::WriteOnly | QIODevice::Truncate));
        const int exitCode = mode == QStringLiteral("wrong-exit") ? 1 : outcome.error == 0 ? 0 : 1;
        QByteArray code = "#!/usr/bin/python3\nimport sys,time,json,pathlib\n";
        if (mode == QStringLiteral("deny") || mode == QStringLiteral("dismiss"))
            code += "sys.exit(" + QByteArray::number(mode == QStringLiteral("deny") ? 127 : 126) + ")\n";
        const QByteArray operation = compressed ? "compressed-write-device-v1" : "raw-write-device-v1";
        code += "print('{\"event\":\"ready\"}',flush=True)\nr=json.loads(sys.stdin.readline())\n"
            "assert r['operation']=='" + operation + "' and len(r)==9\n";
        if (compressed)
            code += (compression == BLED_COMPRESSION_VTSI || compression == IMG_COMPRESSION_FFU)
                ? "assert len(r['source']['compression'])==6 and r['source']['size']=='61' and r['source']['compression']['decoded']=='4097'\n"
                : "assert len(r['source']['compression'])==5 and r['source']['size']=='61' and r['source']['compression']['decoded']=='4097'\n";
        code += "sys.stdout.buffer.write(bytes.fromhex('" + messages.toHex() + "'));sys.stdout.flush()\n";
        if (cancel) code += "sys.stdin.read()\n";
        code += "sys.stdout.buffer.write(bytes.fromhex('" + result.toHex() + "'));sys.stdout.flush()\n"
            "pathlib.Path(bytes.fromhex('" + marker.toUtf8().toHex() + "').decode()).touch()\n"
            "time.sleep(0.2)\nsys.exit(" + QByteArray::number(exitCode) + ")\n";
        QCOMPARE(script.write(code), code.size());
        QVERIFY(script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        script.close();
        auto* process = new rufus::PrepareImageProcess(fixturePath, this);
        QSignalSpy completed(process, &rufus::PrepareImageProcess::finished);
        QSignalSpy progress(process, &rufus::PrepareImageProcess::rawProgressChanged);
        QVERIFY(process->start(request));
        if (cancel)
        {
            QTRY_COMPARE_WITH_TIMEOUT(progress.size(), 3, 5000);
            if (mode == QStringLiteral("destroy"))
            {
                delete process; // EOF, wait for child exit and release.
                QVERIFY(QFile::exists(marker));
                QCOMPARE(launches, 1);
                return;
            }
            process->cancel();
        }
        if (mode == QStringLiteral("success"))
        {
            QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(marker), 5000);
            QVERIFY(process->isActive());
            QCOMPARE(completed.size(), 0);
        }
        QTRY_COMPARE_WITH_TIMEOUT(completed.size(), 1, 5000);
        QCOMPARE(launches, 1);
        using State = rufus::PrepareImageProcess::State;
        const bool cleanCancel = cancel && outcome.error == ECANCELED && outcome.raw.sync_error == 0;
        const State expected = mode == QStringLiteral("success") || mode == QStringLiteral("sparse-success") ? State::Complete
            : (cleanCancel || mode == QStringLiteral("dismiss")) ? State::Cancelled : State::Failed;
        QCOMPARE(process->state(), expected);
        if (mode == QStringLiteral("success")) QCOMPARE(process->outcome().raw.bytes_verified, imageBytes);
        if (mode == QStringLiteral("sparse-success")) QCOMPARE(process->outcome().raw.bytes_verified, quint64(1024));
        if (mode == QStringLiteral("refused")) QVERIFY(!process->outcome().started);
        if (mode == QStringLiteral("unknown-write-state")) QVERIFY(process->outcome().started);
        if (mode == QStringLiteral("sync-error")) QCOMPARE(process->outcome().syncError, EIO);
        delete process;
    }
};
QTEST_GUILESS_MAIN(TestRawHelperProcess)
#include "test_raw_helper_process.moc"
