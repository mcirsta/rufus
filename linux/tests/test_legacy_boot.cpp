// SPDX-License-Identifier: GPL-3.0-or-later
#include "qt/msdos_selection.h"
#include "qt/prepare_image_protocol.h"
#include "platform/fat_boot.h"
#include "platform/format_options.h"
#include "platform/legacy_iso.h"
#include "platform/iso_extract.h"
#include "platform/syslinux_native.h"
#include "platform/syslinux_resources.h"
#include "dos_locale_policy.h"
#include "http_fixture.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkProxy>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QProcess>
#include <QTest>
#include <cerrno>
#include <cstring>
#include <unistd.h>

using namespace rufus;
using namespace rufus_test;
namespace
{
QByteArray fixture()
{
    QFile file(qEnvironmentVariable("RUFUS_MSDOS_DLL"));
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
PrepareImageRequest request(int boot, int filesystem)
{
    PrepareImageRequest job;
    job.path = QStringLiteral("/dev/sdz"); job.device = 123; job.size = 320ULL*1024*1024;
    job.clusterSize = filesystem == FS_FAT16 ? 8192 : 4096;
    job.fileSystem = filesystem; job.isDevice = job.mbr = true;
    job.msDos = boot == BT_MSDOS; job.reactOs = boot == BT_REACTOS;
    job.diskSequence = 77;
    job.objectPath = QStringLiteral("/org/freedesktop/UDisks2/block_devices/sdz");
    job.serviceOwner = QStringLiteral(":1.77"); job.identity = QStringList{QStringLiteral("fixture")};
    if (job.msDos) {
        job.dosImagePath = QStringLiteral("/tmp/diskcopy.dll");
        job.dosKeyboard = QStringLiteral("us"); job.dosCodepage = 437;
    }
    return job;
}
bool writeFixture(const QString& path, const QByteArray& bytes)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
bool makeIso(const QString& root, const QString& image)
{
    QProcess process;
    process.start(QString::fromUtf8(RUFUS_RESOURCE_MKISOFS),
        {QStringLiteral("-quiet"),QStringLiteral("-R"),QStringLiteral("-J"),QStringLiteral("-o"),image,root});
    return process.waitForFinished(30000) && process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}
QByteArray readFixture(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
}
class TestLegacyBoot : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase() { QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy); }
    void patches()
    {
        QByteArray command(93040,'\0');
        const QByteArray pattern = QByteArray::fromHex("1580fa037510b80e");
        command.replace(0x650c,8,pattern);
        QVERIFY(PatchMsDosFile("COMMAND.COM",reinterpret_cast<uint8_t*>(command.data()),size_t(command.size())));
        QCOMPARE(quint8(command[0x6510]),quint8(0xeb));
        QVERIFY(!PatchMsDosFile("COMMAND.COM",reinterpret_cast<uint8_t*>(command.data()),size_t(command.size())));
        QByteArray shortFile(1,'\0');
        QVERIFY(!PatchMsDosFile("IO.SYS",reinterpret_cast<uint8_t*>(shortFile.data()),1));
    }
    void localeTables()
    {
        for (bool freedos : {false,true}) {
            unsigned keyboards = 0, pages = 0;
            for (size_t index = 0; const char* keyboard = GetDOSKeyboard(index,freedos); ++index) {
                const auto locale = SelectDOSLocale(keyboard,437,freedos);
                QVERIFY(!locale.keyboard_fallback); QCOMPARE(QByteArray(locale.keyboard),QByteArray(keyboard)); ++keyboards;
            }
            for (size_t index = 0; const uint32_t page = GetDOSCodepage(index,freedos); ++index) {
                const auto locale = SelectDOSLocale("us",page,freedos);
                QVERIFY(!locale.codepage_fallback); QCOMPARE(locale.codepage,page); ++pages;
            }
            QVERIFY(keyboards > 20 && pages > 5);
        }
    }
    void resourceAndCache()
    {
        const QByteArray bytes = fixture(); if (bytes.isEmpty()) QSKIP("Provide RUFUS_MSDOS_DLL for the pinned Microsoft resource.");
        QVector<MsDosFile> files; QCOMPARE(ReadMsDosFiles(bytes,files),0); QCOMPARE(files.size(),13);
        QCOMPARE(files[1].bytes.size(),93040); QCOMPARE(quint8(files[1].bytes[0x6510]),quint8(0xeb));
        QCOMPARE(files[2].bytes.size(),116736); QCOMPARE(quint8(files[2].bytes[0x3aa]),quint8(0xeb));
        QCOMPARE(files[2].attributes,quint8(39)); QCOMPARE(files[0].bytes,QByteArray(";W98EBD\r\n"));
        QByteArray changed = bytes; changed[10] ^= 1;
        QCOMPARE(ReadMsDosFiles(changed,files),EBADMSG); QVERIFY(files.isEmpty());
        QTemporaryDir directory; QVERIFY(directory.isValid());
        QCOMPARE(StoreMsDosImage(directory.path(),bytes),0);
        QByteArray read; const QString path = directory.filePath(QStringLiteral("diskcopy.dll"));
        QCOMPARE(ReadMsDosImage(path,getuid(),read),0); QCOMPARE(read,bytes);
        QCOMPARE(StoreMsDosImage(directory.path(),changed),EBADMSG);
        QCOMPARE(ReadMsDosImage(path,getuid(),read),0); QCOMPARE(read,bytes);
        QVERIFY(QFile::remove(path)); QVERIFY(::symlink("/dev/null",QFile::encodeName(path).constData())==0);
        QVERIFY(ReadMsDosImage(path,getuid(),read)!=0); QVERIFY(read.isEmpty()); QVERIFY(StoreMsDosImage(directory.path(),bytes)!=0);
    }
    void protocol_data()
    {
        QTest::addColumn<int>("boot"); QTest::addColumn<int>("filesystem");
        for (int boot : {BT_MSDOS,BT_REACTOS}) for (int fs : {FS_FAT16,FS_FAT32})
            QTest::newRow(qPrintable(QStringLiteral("%1-%2").arg(boot).arg(fs))) << boot << fs;
    }
    void protocol()
    {
        QFETCH(int,boot); QFETCH(int,filesystem); auto job = request(boot,filesystem);
        const auto encoded = EncodePrepareRequest(job); QVERIFY(!encoded.isEmpty());
        PrepareImageRequest decoded; QVERIFY(DecodePrepareRequest(encoded,decoded)); QCOMPARE(EncodePrepareRequest(decoded),encoded);
        QCOMPARE(decoded.msDos,job.msDos); QCOMPARE(decoded.reactOs,job.reactOs); QVERIFY(decoded.isSourceFree());
        job.freeDos = true; QVERIFY(EncodePrepareRequest(job).isEmpty()); job.freeDos = false;
        job.grub2 = true; QVERIFY(EncodePrepareRequest(job).isEmpty()); job.grub2 = false;
        job.fileSystem = FS_NTFS; QVERIFY(EncodePrepareRequest(job).isEmpty()); job.fileSystem = filesystem;
        job.mbr = false; QVERIFY(EncodePrepareRequest(job).isEmpty()); job.mbr = true;
        if (job.msDos) { job.clusterSize = 65536; QVERIFY(EncodePrepareRequest(job).isEmpty()); }
        const auto sizes = NativeFileSystemClusterSizes(320ULL*1024*1024,filesystem,false,boot,PartitionScheme::Mbr);
        QVERIFY(sizes.Allowed != 0);
        if (job.msDos) QVERIFY((sizes.Allowed & 65536)==0);
    }
    void download_data()
    {
        QTest::addColumn<QString>("mode");
        for (const char* mode : {"accept","decline","cancel","bad-hash","missing","warm-cache"}) QTest::newRow(mode) << QString::fromLatin1(mode);
    }
    void download()
    {
        QFETCH(QString,mode); const QByteArray bytes = fixture(); if (bytes.isEmpty()) QSKIP("Provide RUFUS_MSDOS_DLL for the pinned Microsoft resource.");
        QTemporaryDir directory; QVERIFY(directory.isValid()); HttpFixture server; QVERIFY(server.listen(QHostAddress::LocalHost));
        if (mode == QStringLiteral("warm-cache")) QCOMPARE(StoreMsDosImage(directory.path(),bytes),0);
        server.responses["/diskcopy.dll"] = response(mode == QStringLiteral("bad-hash") ? QByteArray(bytes.size(),'x') : bytes);
        if (mode == QStringLiteral("missing")) server.responses["/diskcopy.dll"] = response("missing",{},-1,404);
        MsDosSelection selection; QSignalSpy ask(&selection,&MsDosSelection::downloadRequired),done(&selection,&MsDosSelection::completed);
        QCOMPARE(selection.start(directory.path(),server.url("/diskcopy.dll")),0);
        QCOMPARE(server.requests.size(),0);
        if (mode == QStringLiteral("warm-cache")) QCOMPARE(ask.size(),0);
        else {
            QCOMPARE(ask.size(),1);
            if (mode == QStringLiteral("cancel")) selection.cancel();
            else selection.choose(mode != QStringLiteral("decline"));
        }
        QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,5000);
        const auto result = qvariant_cast<MsDosSelectionResult>(done.first().first());
        const bool success = mode == QStringLiteral("accept") || mode == QStringLiteral("warm-cache");
        QCOMPARE(result.error==0,success); QVERIFY(!selection.isActive());
        if (success) { QCOMPARE(result.bytes,bytes); QVERIFY(!result.path.isEmpty()); }
        else { QVERIFY(result.bytes.isEmpty()); QVERIFY(result.path.isEmpty()); }
    }
    void reactOsRecords_data()
    {
        QTest::addColumn<int>("filesystem"); QTest::newRow("FAT16") << int(FS_FAT16); QTest::newRow("FAT32-sector14-tail") << int(FS_FAT32);
    }
    void reactOsRecords()
    {
        QFETCH(int,filesystem); QTemporaryFile image; QVERIFY(image.open()); QVERIFY(image.resize(320ULL*1024*1024));
        MBR_IMAGE_OPTIONS layout = {}; layout.disk_size = quint64(image.size()); layout.sector_size = 512;
        layout.sectors_per_track = 63; layout.cluster_size = filesystem == FS_FAT16 ? 8192 : 4096;
        layout.file_system = filesystem; layout.boot_code = MBR_BOOT_REACTOS;
        MBR_IMAGE_RESULT partition = {}; QCOMPARE(CreateMbrImage(image.handle(),&layout,&partition),0);
        FAT_FORMAT_OPTIONS options = {}; options.offset = partition.offset; options.size = partition.size;
        options.cluster_size = layout.cluster_size; options.volume_id = 0x1234abcd; memcpy(options.label,"REACTOS",8);
        FORMAT_RESULT formatted = {};
        const int error = filesystem == FS_FAT16 ? FormatFat16Image(image.handle(),RUFUS_MKFS_FAT_PATH,&options,&formatted)
            : FormatFat32Image(image.handle(),RUFUS_MKFS_FAT_PATH,&options,&formatted);
        QCOMPARE(error,0);
        QVERIFY(image.seek(qint64(partition.offset+12*512))); const QByteArray sector12 = image.read(512);
        FAT_BOOT_RESULT installed = {}; QCOMPARE(InstallFatBootRecords(image.handle(),filesystem,FAT_BOOT_REACTOS,&options,&installed),0);
        QCOMPARE(CheckMbrImage(image.handle(),&layout,&partition),0);
        QCOMPARE(installed.bytes_written,quint64(filesystem == FS_FAT16 ? 512 : 4096));
        QVERIFY(image.seek(qint64(partition.offset))); const QByteArray record = image.read(filesystem == FS_FAT16 ? 512 : 15*512);
        QVERIFY(record.contains("FREELDR"));
        if (filesystem == FS_FAT32) {
            QVERIFY(image.seek(qint64(partition.offset+12*512))); QCOMPARE(image.read(512),sector12);
            QVERIFY(image.seek(qint64(partition.offset+14*512))); const auto tail = image.read(512);
            QVERIFY(image.seek(qint64(partition.offset+20*512))); QCOMPARE(image.read(512),tail);
            QVERIFY(tail != QByteArray(512,'\0'));
        }
    }
    void reactOsIsoResources_data()
    {
        QTest::addColumn<QByteArray>("path");
        QTest::newRow("freeldr") << QByteArray("/loader/freeldr.sys");
        QTest::newRow("setupldr") << QByteArray("/reactos/setupldr.sys");
    }
    void reactOsIsoResources()
    {
        QFETCH(QByteArray,path);
        RUFUS_IMG_REPORT report = {};
        memcpy(report.reactos_path,path.constData(),size_t(path.size()+1));
        QByteArray mboot, config;
        QCOMPARE(ReadReactOsIsoFiles(report,mboot,config),0);
        QCOMPARE(mboot.size(),34140);
        QCOMPARE(QCryptographicHash::hash(mboot,QCryptographicHash::Sha256).toHex(),
            QByteArray("d555749052e1835742b0cb0975db7a3b7937aef20840fd7959de720d5ea94990"));
        QCOMPARE(config,QByteArray("DEFAULT ReactOS\nLABEL ReactOS\n  KERNEL mboot.c32\n  APPEND ")+path);
        const auto selected = SyslinuxSourceReport(report);
        QVERIFY(!HAS_SYSLINUX(report)); QCOMPARE(report.sl_version,quint16(0));
        QVERIFY(HAS_SYSLINUX(selected)); QCOMPARE(selected.sl_version,quint16(0x0407));
        QCOMPARE(QByteArray(selected.cfg_path),QByteArray("/syslinux.cfg"));
        SyslinuxResources resources;
        QCOMPARE(ReadEmbeddedSyslinuxResources(report,resources),0);
        QCOMPARE(ValidateSyslinuxResources(report,resources),0);
        QString problem;
        QCOMPARE(CheckNativeSyslinuxSelection(report,FS_FAT16,512,1024*1024,problem),0);
        report.has_syslinux = 1; report.sl_version = 0x0603;
        const auto modern = SyslinuxSourceReport(report);
        QCOMPARE(modern.sl_version,quint16(0x0603));
    }
    void kolibriProtocol_data()
    {
        QTest::addColumn<int>("filesystem"); QTest::addColumn<bool>("uefi");
        QTest::newRow("FAT32") << int(FS_FAT32) << false;
        QTest::newRow("NTFS") << int(FS_NTFS) << false;
        QTest::newRow("NTFS-with-EFI") << int(FS_NTFS) << true;
    }
    void kolibriProtocol()
    {
        QFETCH(int,filesystem); QFETCH(bool,uefi);
        auto job = request(BT_IMAGE,filesystem); job.kolibriOs = true; job.uefiNtfs = uefi;
        job.ntfsCompression = job.fileIndexing = filesystem == FS_NTFS;
        job.windowsRufusMbr = false;
        job.imagePath = QStringLiteral("/tmp/kolibri.iso"); job.imageDevice = 9;
        job.imageInode = 456; job.imageSize = 1024*1024;
        job.imageModified = QStringLiteral("100.000000001"); job.imageChanged = QStringLiteral("101.000000002");
        const auto encoded = EncodePrepareRequest(job); QVERIFY(!encoded.isEmpty());
        PrepareImageRequest decoded; QVERIFY(DecodePrepareRequest(encoded,decoded));
        QVERIFY(decoded.kolibriOs); QCOMPARE(decoded.uefiNtfs,uefi);
        QCOMPARE(decoded.windowsRufusMbr,job.windowsRufusMbr);
        QCOMPARE(decoded.ntfsCompression,job.ntfsCompression); QCOMPARE(decoded.fileIndexing,job.fileIndexing);
        QVERIFY(!decoded.isSourceFree()); QCOMPARE(EncodePrepareRequest(decoded),encoded);
        job.fileSystem = FS_FAT16; QVERIFY(EncodePrepareRequest(job).isEmpty()); job.fileSystem = filesystem;
        job.mbr = false; QVERIFY(EncodePrepareRequest(job).isEmpty()); job.mbr = true;
        job.reactOs = true; QVERIFY(EncodePrepareRequest(job).isEmpty()); job.reactOs = false;
        job.imageInode = 0; QVERIFY(EncodePrepareRequest(job).isEmpty()); job.imageInode = 456;
        job.syslinux = true; QVERIFY(EncodePrepareRequest(job).isEmpty()); job.syslinux = false;
        job.enhancedInstall = true; QVERIFY(EncodePrepareRequest(job).isEmpty()); job.enhancedInstall = false;
        job.windowsFlags = 1; QVERIFY(EncodePrepareRequest(job).isEmpty());
    }
    void kolibriRecords_data()
    {
        QTest::addColumn<int>("filesystem");
        QTest::newRow("FAT32-original-PBR") << int(FS_FAT32);
        QTest::newRow("FAT16-refused-before-write") << int(FS_FAT16);
    }
    void kolibriRecords()
    {
        QFETCH(int,filesystem);
        QTemporaryFile image; QVERIFY(image.open()); QVERIFY(image.resize(320ULL*1024*1024));
        MBR_IMAGE_OPTIONS layout = {}; layout.disk_size = quint64(image.size()); layout.sector_size = 512;
        layout.sectors_per_track = 63; layout.cluster_size = filesystem == FS_FAT16 ? 8192 : 4096;
        layout.file_system = filesystem; layout.boot_code = MBR_BOOT_KOLIBRIOS;
        MBR_IMAGE_RESULT partition = {}; QCOMPARE(CreateMbrImage(image.handle(),&layout,&partition),0);
        FAT_FORMAT_OPTIONS options = {}; options.offset = partition.offset; options.size = partition.size;
        options.cluster_size = layout.cluster_size; options.volume_id = 0xabcdef01; memcpy(options.label,"KOLIBRI",8);
        FORMAT_RESULT formatted = {};
        const int error = filesystem == FS_FAT16 ? FormatFat16Image(image.handle(),RUFUS_MKFS_FAT_PATH,&options,&formatted)
            : FormatFat32Image(image.handle(),RUFUS_MKFS_FAT_PATH,&options,&formatted);
        QCOMPARE(error,0);
        QVERIFY(image.seek(qint64(partition.offset))); const QByteArray before = image.read(24*512);
        FAT_BOOT_RESULT installed = {};
        QCOMPARE(InstallFatBootRecords(image.handle(),filesystem,FAT_BOOT_KOLIBRI,&options,&installed),
            filesystem == FS_FAT16 ? ENOTSUP : 0);
        QVERIFY(image.seek(qint64(partition.offset))); const QByteArray after = image.read(24*512);
        if (filesystem == FS_FAT16)
        {
            QCOMPARE(installed.bytes_written,quint64(0)); QCOMPARE(after,before);
        }
        else
        {
            QCOMPARE(installed.bytes_written,quint64(3072));
            QVERIFY(after.first(512).contains("MTLD_F32"));
            QCOMPARE(after.mid(512,2*512),before.mid(512,2*512));
            QCOMPARE(after.mid(3*512,3*512),before.mid(3*512,3*512));
            QCOMPARE(after.mid(9*512),before.mid(9*512));
            QCOMPARE(after.first(512),after.mid(6*512,512));
            QCOMPARE(CheckMbrImage(image.handle(),&layout,&partition),0);
        }
    }
    void legacyIsoSourceAndOrdering_data()
    {
        QTest::addColumn<QString>("mode");
        for (const char* mode : {"generated-reactos","source-overrides-reactos","reactos-link-refused","reactos-reserved-directory",
            "kolibri","missing-kolibri","kolibri-directory-refused"}) QTest::newRow(mode) << QString::fromLatin1(mode);
    }
    void legacyIsoSourceAndOrdering()
    {
        QFETCH(QString,mode);
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const QString root = directory.filePath(QStringLiteral("tree")); QVERIFY(QDir().mkpath(root));
        const bool reactos = mode.contains(QStringLiteral("reactos"));
        if (reactos)
        {
            QVERIFY(writeFixture(root+QStringLiteral("/reactos/setupldr.sys"),QByteArray("reactos-loader")));
            if (mode == QStringLiteral("source-overrides-reactos"))
            {
                QVERIFY(writeFixture(root+QStringLiteral("/syslinux.cfg"),QByteArray("DEFAULT source\n")));
                QVERIFY(writeFixture(root+QStringLiteral("/mboot.c32"),QByteArray("source-mboot")));
            }
            if (mode == QStringLiteral("reactos-link-refused"))
            {
                QVERIFY(QFile::remove(root+QStringLiteral("/reactos/setupldr.sys")));
                QVERIFY(writeFixture(root+QStringLiteral("/reactos/real.sys"),QByteArray("loader")));
                QVERIFY(::symlink("real.sys",QFile::encodeName(root+QStringLiteral("/reactos/setupldr.sys")).constData()) == 0);
            }
            if (mode == QStringLiteral("reactos-reserved-directory"))
                QVERIFY(QDir().mkpath(root+QStringLiteral("/ldlinux.sys")));
        }
        else
        {
            QVERIFY(writeFixture(root+QStringLiteral("/kolibri.img"),QByteArray("kolibri-marker")));
            if (mode == QStringLiteral("kolibri"))
                QVERIFY(writeFixture(root+QStringLiteral("/HD_Load/USB_Boot/MTLD_F32"),QByteArray(100003,'K')));
            if (mode == QStringLiteral("kolibri-directory-refused"))
                QVERIFY(QDir().mkpath(root+QStringLiteral("/HD_Load/USB_Boot/MTLD_F32")));
        }
        const QString iso = directory.filePath(QStringLiteral("source.iso")); QVERIFY(makeIso(root,iso));
        ISO_ANALYSIS_OPTIONS options = {true,true,{0x0407,0x0604}};
        ISO_ANALYSIS analysis = {}; ISO_ANALYSIS_SOURCE source = {};
        const int status = AnalyzeIsoImageWithSource(QFile::encodeName(iso).constData(),quint64(QFileInfo(iso).size()),&options,&analysis,&source);
        QVERIFY(status == ISO_SCAN_OK || status == ISO_SCAN_PARTIAL);
        QString problem; quint64 extra = 0;
        const int checked = CheckLegacyIsoFiles(source,analysis.report,reactos,problem,true,&extra);
        const bool accepted = mode == QStringLiteral("generated-reactos") || mode == QStringLiteral("source-overrides-reactos") || mode == QStringLiteral("kolibri");
        QCOMPARE(checked == 0,accepted);
        if (!accepted) { QVERIFY(!problem.isEmpty()); QCOMPARE(extra,quint64(0)); }
        if (mode == QStringLiteral("kolibri")) QCOMPARE(extra,quint64(100003));
        if (accepted && reactos)
        {
            QByteArray mboot, config; QCOMPARE(ReadReactOsIsoFiles(analysis.report,mboot,config),0);
            QCOMPARE(extra,quint64(mboot.size()+config.size()));
            const QString output = directory.filePath(QStringLiteral("output")); QVERIFY(QDir().mkpath(output));
            QVERIFY(writeFixture(output+QStringLiteral("/mboot.c32"),mboot));
            QVERIFY(writeFixture(output+QStringLiteral("/syslinux.cfg"),config));
            ISO_EXTRACT_OPTIONS extract = {}; extract.analysis = options;
            ISO_EXTRACT_RESULT copied = {};
            QCOMPARE(ExtractIsoTree(QFile::encodeName(iso).constData(),QFile::encodeName(output).constData(),&extract,&copied),ENOTEMPTY);
            ISO_PREPARED_REACTOS prepared = {reinterpret_cast<const uint8_t*>(mboot.constData()),size_t(mboot.size()),
                reinterpret_cast<const uint8_t*>(config.constData()),size_t(config.size())};
            extract.prepared_reactos = &prepared;
            if (mode == QStringLiteral("generated-reactos"))
            {
                QVERIFY(writeFixture(output+QStringLiteral("/extra"),QByteArray("unrelated")));
                QCOMPARE(ExtractIsoTree(QFile::encodeName(iso).constData(),QFile::encodeName(output).constData(),&extract,&copied),ENOTEMPTY);
                QVERIFY(QFile::remove(output+QStringLiteral("/extra")));
                QByteArray changed = mboot; changed[0] ^= 1;
                QVERIFY(writeFixture(output+QStringLiteral("/mboot.c32"),changed));
                QCOMPARE(ExtractIsoTree(QFile::encodeName(iso).constData(),QFile::encodeName(output).constData(),&extract,&copied),EBADMSG);
                QVERIFY(QFile::remove(output+QStringLiteral("/mboot.c32")));
                QVERIFY(::symlink("/dev/null",QFile::encodeName(output+QStringLiteral("/mboot.c32")).constData()) == 0);
                QVERIFY(ExtractIsoTree(QFile::encodeName(iso).constData(),QFile::encodeName(output).constData(),&extract,&copied) != 0);
                QVERIFY(QFile::remove(output+QStringLiteral("/mboot.c32")));
                QVERIFY(writeFixture(output+QStringLiteral("/mboot.c32"),mboot));
            }
            QCOMPARE(ExtractIsoTree(QFile::encodeName(iso).constData(),QFile::encodeName(output).constData(),&extract,&copied),0);
            QCOMPARE(readFixture(output+QStringLiteral("/mboot.c32")),mode == QStringLiteral("source-overrides-reactos") ? QByteArray("source-mboot") : mboot);
            QCOMPARE(readFixture(output+QStringLiteral("/syslinux.cfg")),mode == QStringLiteral("source-overrides-reactos") ? QByteArray("DEFAULT source\n") : config);
        }
        CloseIsoAnalysisSource(&source);
    }
};
QTEST_GUILESS_MAIN(TestLegacyBoot)
#include "test_legacy_boot.moc"
