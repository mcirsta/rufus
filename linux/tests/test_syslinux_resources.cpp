#include "platform/iso_extract.h"
#include <QProcess>
// SPDX-License-Identifier: GPL-3.0-or-later
#include "qt/syslinux_selection.h"
#include "platform/download_signature.h"
#include "http_fixture.h"
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <cstring>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>
#include <unistd.h>
#include <cerrno>
namespace {
QByteArray readFile(const QString& path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray(); }
bool put(const QString& path,const QByteArray& bytes) { if (!QDir().mkpath(QFileInfo(path).path())) return false; QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(); }
QByteArray fixture(const char* version,const char* name) { return readFile(QStringLiteral(RUFUS_RESOURCE_FIXTURES)+QStringLiteral("/syslinux-")+QString::fromLatin1(version)+'/'+QString::fromLatin1(name)); }
}
class TestSyslinuxResources final : public QObject {
    Q_OBJECT
private slots:
    void codecAndUntrustedOffsets() {
        RUFUS_IMG_REPORT report = {}; report.has_syslinux = true; report.sl_version = 0x603;
        rufus::SyslinuxResources resources{fixture("6.03","ldlinux.sys"),fixture("6.03","ldlinux.bss"),fixture("6.03","ldlinux.c32"),{}};
        QCOMPARE(rufus::ValidateSyslinuxResources(report,resources),0);
        const QByteArray bytes = rufus::EncodeSyslinuxResources(resources); QVERIFY(!bytes.isEmpty());
        rufus::SyslinuxResources decoded; QCOMPARE(rufus::DecodeSyslinuxResources(bytes,decoded),0); QCOMPARE(decoded.loader,resources.loader); QCOMPARE(decoded.module,resources.module);
        for (int field : {8,12,16,20,24}) { QByteArray corrupt = bytes; qToLittleEndian<quint32>(UINT32_MAX,corrupt.data()+field); QVERIFY(rufus::DecodeSyslinuxResources(corrupt,decoded) != 0); }
        const int patch = resources.loader.indexOf(QByteArray::fromHex("fe02b23e")); QVERIFY(patch >= 0);
        for (int offset : {patch+22,int(qFromLittleEndian<quint16>(resources.loader.constData()+patch+22))+10}) {
            auto corrupt = resources; qToLittleEndian<quint16>(quint16(offset == patch+22 ? 65535 : qFromLittleEndian<quint16>(resources.loader.constData()+patch+22)),corrupt.loader.data()+offset); QVERIFY(rufus::ValidateSyslinuxResources(report,corrupt) != 0);
        }
        for (int major : {3,4}) { report.sl_version = quint16((major<<8)|7); resources = {}; QCOMPARE(rufus::ReadEmbeddedSyslinuxResources(report,resources),0); QCOMPARE(rufus::ValidateSyslinuxResources(report,resources),0); }
    }
    void originalSignedVersionFiveResources() {
        RUFUS_IMG_REPORT report = {}; report.sl_version = 0x50a; report.has_syslinux = true;
        rufus::SyslinuxResources resources{fixture("5.10","ldlinux.sys"),fixture("5.10","ldlinux.bss"),fixture("5.10","ldlinux.c32"),{}};
        for (const char* name : {"ldlinux.sys","ldlinux.bss","ldlinux.c32"})
            {
                const QByteArray data = fixture("5.10",name), signature = fixture("5.10",(QByteArray(name)+".sig").constData());
                QCOMPARE(VerifyRufusDownloadSignature(data.constData(),size_t(data.size()),signature.constData(),size_t(signature.size())),0);
            }
        QCOMPARE(rufus::ValidateSyslinuxResources(report,resources),0);
    }
    void signedPair_data() {
        QTest::addColumn<QString>("scenario");
        for (const char* value : {"normal","suffix","bad-signature","decline","cancel","source-change"}) QTest::newRow(value) << QString::fromLatin1(value);
    }
    void signedPair() {
        QFETCH(QString,scenario); QTemporaryDir temp; QVERIFY(temp.isValid());
        const QString image = temp.filePath("source.iso"); QVERIFY(put(image,QByteArray(4096,'x')));
        rufus::PrepareImageRequest request; QCOMPARE(rufus::CapturePrepareSource(image,request),0);
        RUFUS_IMG_REPORT report = {}; report.sl_version = 0x603; report.has_syslinux = true;
        if (scenario == "suffix") strcpy(report.sl_version_ext,"/missing-build");
        rufus::SyslinuxResources initial; QCOMPARE(rufus::ReadEmbeddedSyslinuxResources(report,initial),0); initial.module = fixture("6.03","ldlinux.c32");
        rufus_test::HttpFixture server; QVERIFY(server.listen(QHostAddress::LocalHost));
        for (const char* name : {"ldlinux.sys","ldlinux.bss"}) for (const char* suffix : {"",".sig"}) {
            const QByteArray path = QByteArray("/syslinux-6.03/")+name+suffix;
            const QByteArray bytes = fixture("6.03",(QByteArray(name)+suffix).constData()); QVERIFY(!bytes.isEmpty());
            server.responses.insert(path,rufus_test::response(bytes));
        }
        if (scenario == "bad-signature") server.responses["/syslinux-6.03/ldlinux.sys.sig"] = rufus_test::response(QByteArray(256,'x'));
        if (scenario == "source-change" || scenario == "cancel") server.holdBodies.insert("/syslinux-6.03/ldlinux.sys");
        rufus::SyslinuxSelection selection; QSignalSpy prompts(&selection,&rufus::SyslinuxSelection::downloadRequired),done(&selection,&rufus::SyslinuxSelection::completed);
        QCOMPARE(selection.start(request,report,initial,temp.filePath("cache"),server.url("/")),0); QCOMPARE(prompts.size(),1);
        selection.choose(scenario == "decline" ? rufus::Grub2DownloadChoice::No : rufus::Grub2DownloadChoice::Yes);
        if (scenario == "source-change" || scenario == "cancel") {
            QTRY_VERIFY_WITH_TIMEOUT(!server.requests.isEmpty(),10000);
            if (scenario == "cancel") selection.cancel();
            else { QFile changed(image); QVERIFY(changed.open(QIODevice::Append)); QCOMPARE(changed.write("changed"),7); changed.close(); server.releaseBodies(); }
        }
        QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,15000);
        const bool success = scenario == "normal" || scenario == "suffix";
        const int error = done[0][0].toInt(); QCOMPARE(error == 0,success);
        if (scenario == "decline") QCOMPARE(error,ENOENT);
        if (scenario == "cancel") QCOMPARE(error,ECANCELED);
        if (scenario == "source-change") QCOMPARE(error,ESTALE);
        if (!success) { QVERIFY(done[0][1].toByteArray().isEmpty()); return; }
        rufus::SyslinuxResources resources; QCOMPARE(rufus::DecodeSyslinuxResources(done[0][1].toByteArray(),resources),0);
        QCOMPARE(resources.loader,fixture("6.03","ldlinux.sys")); QCOMPARE(resources.boot,fixture("6.03","ldlinux.bss"));
        QCOMPARE(rufus::ValidateSyslinuxResources(report,resources),0);
        const qsizetype requests = server.requests.size(); done.clear(); prompts.clear();
        QCOMPARE(selection.start(request,report,initial,temp.filePath("cache"),server.url("/")),0); QCOMPARE(done.size(),1); QCOMPARE(done[0][0].toInt(),0); QVERIFY(prompts.isEmpty()); QCOMPARE(server.requests.size(),requests);
    }
    void obsoleteModuleExtractionThresholds() {
        QTemporaryDir temp; const QString tree = temp.filePath("tree"), image = temp.filePath("source.iso"), output = temp.filePath("out");
        QVERIFY(QDir().mkpath(tree)); QVERIFY(QDir().mkpath(output));
        const int sizes[] = {53500,53501,148000,148001};
        const char* paths[] = {"a/menu.c32","b/menu.c32","a/vesamenu.c32","b/vesamenu.c32"};
        for (int i = 0; i < 4; ++i) QVERIFY(put(tree+'/'+QString::fromLatin1(paths[i]),QByteArray(sizes[i],'o')));
        QVERIFY(put(tree+"/isolinux/isolinux.cfg","default fixture\n"));
        QVERIFY(put(tree+"/isolinux/isolinux.bin",QByteArray(64,'\0')+"ISOLINUX 3.86 "+QByteArray(1024,'\0')));
        QProcess process; process.start(QStringLiteral(RUFUS_RESOURCE_MKISOFS),{"-quiet","-R","-J","-o",image,tree});
        QVERIFY(process.waitForFinished(30000)); QCOMPARE(process.exitCode(),0);
        const QByteArray menu = fixture("4.07","menu.c32"), vesamenu = fixture("4.07","vesamenu.c32");
        const ISO_BOOT_RESOURCES resources = {{reinterpret_cast<const uint8_t*>(menu.constData()),reinterpret_cast<const uint8_t*>(vesamenu.constData())},{size_t(menu.size()),size_t(vesamenu.size())}};
        ISO_EXTRACT_OPTIONS options = {}; options.analysis.enable_joliet = options.analysis.enable_rockridge = true; options.analysis.embedded_sl_version[0] = 0x407; options.analysis.embedded_sl_version[1] = 0x604; options.boot_resources = &resources;
        const QByteArray source = QFile::encodeName(image), target = QFile::encodeName(output);
        ISO_EXTRACT_RESULT result;
        QCOMPARE(ExtractIsoTree(source.constData(),target.constData(),&options,&result),0);
        QCOMPARE(readFile(output+"/a/menu.c32"),menu); QCOMPARE(readFile(output+"/a/vesamenu.c32"),vesamenu);
        QCOMPARE(readFile(output+"/b/menu.c32"),QByteArray(53501,'o')); QCOMPARE(readFile(output+"/b/vesamenu.c32"),QByteArray(148001,'o'));
    }
    void oldModulesOptionalAndSigned() {
        QTemporaryDir temp; const QString image = temp.filePath("source.iso"); QVERIFY(put(image,QByteArray(4096,'x')));
        rufus::PrepareImageRequest request; QCOMPARE(rufus::CapturePrepareSource(image,request),0);
        RUFUS_IMG_REPORT report = {}; report.sl_version = 0x407; report.has_syslinux = true; report.has_old_c32[0] = report.has_old_c32[1] = true;
        rufus::SyslinuxResources initial; QCOMPARE(rufus::ReadEmbeddedSyslinuxResources(report,initial),0);
        rufus_test::HttpFixture server; QVERIFY(server.listen(QHostAddress::LocalHost));
        for (const char* name : {"menu.c32","vesamenu.c32"}) for (const char* suffix : {"",".sig"}) server.responses.insert(QByteArray("/syslinux-4.07/")+name+suffix,rufus_test::response(fixture("4.07",(QByteArray(name)+suffix).constData())));
        rufus::SyslinuxSelection selection; QSignalSpy done(&selection,&rufus::SyslinuxSelection::completed);
        QCOMPARE(selection.start(request,report,initial,temp.filePath("cache"),server.url("/")),0); selection.choose(rufus::Grub2DownloadChoice::No); QCOMPARE(done.size(),1); QCOMPARE(done[0][0].toInt(),0); QVERIFY(server.requests.isEmpty());
        done.clear(); QCOMPARE(selection.start(request,report,initial,temp.filePath("cache"),server.url("/")),0); selection.choose(rufus::Grub2DownloadChoice::Yes);
        QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,15000); QCOMPARE(done[0][0].toInt(),0);
        rufus::SyslinuxResources resources; QCOMPARE(rufus::DecodeSyslinuxResources(done[0][1].toByteArray(),resources),0);
        QCOMPARE(resources.oldModules[0],fixture("4.07","menu.c32")); QCOMPARE(resources.oldModules[1],fixture("4.07","vesamenu.c32"));
    }
};
QTEST_GUILESS_MAIN(TestSyslinuxResources)
#include "test_syslinux_resources.moc"
