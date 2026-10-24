// SPDX-License-Identifier: GPL-3.0-or-later
#include "qt/prepare_image_protocol.h"
#include "platform/prepare_source.h"
#include "ms-sys/inc/mbr_rufus.h"
#include "ms-sys/inc/mbr_grub.h"
#include "ms-sys/inc/mbr_win7.h"
#include "ms-sys/inc/br_ntfs_0x54.h"
#include "ms-sys/inc/br_fat32_0x0.h"
#include "ms-sys/inc/br_fat32pe_0x52.h"
#include "ms-sys/inc/br_fat32pe_0x3f0.h"
#include "ms-sys/inc/br_fat32pe_0x1800.h"
#include "wimlib.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>
#include <cerrno>
#include <unistd.h>

namespace
{
constexpr quint64 mib = 1024 * 1024;
constexpr int helperTimeoutMs = 10000;
constexpr quint64 coreOffset = 512;
constexpr quint64 sectorBytes = 512;
constexpr quint32 fat16ClusterBytes = 2048;
constexpr quint64 fatFsinfoHintOffset = sectorBytes + 488;
constexpr qsizetype fatFsinfoHintBytes = 8;
constexpr quint64 isoBlockBytes = 2048;
constexpr qsizetype secondPartitionOffset = 462;
constexpr qsizetype ntfsCodeOffset = 0x54;
constexpr qsizetype diskSignatureOffset = 440;
constexpr qsizetype firstPartitionOffset = 446;
constexpr quint32 fatVolumeId = 0x1234abcd;
constexpr qsizetype winPeLoaderBytes = 9000;
constexpr qsizetype winPePathOffset = 128, winPeDiskOffset = 256, winPeCrcOffset = 0x2060;
constexpr qsizetype winPeDiskNumberOffset = 6;
}

class TestPrepareGrub2Image : public QObject
{
    Q_OBJECT
    QTemporaryDir directory_;
    rufus::PrepareImageRequest request_;
    QByteArray core_;
    bool legacyWinPe_ = false, patchedWinPe_ = false, winPeMinintOptions_ = false;
    QByteArray expectedLoader_;
    bool grub4dos_ = false;

    void put(const QString& path, const QByteArray& bytes)
    {
        QVERIFY(QDir().mkpath(QFileInfo(path).path()));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(file.write(bytes), bytes.size());
    }

    QByteArray read(const QString& path, qint64 offset = 0, qint64 count = -1)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly) || !file.seek(offset)) return {};
        return count < 0 ? file.readAll() : file.read(count);
    }

    void checkSplitArchive(const QString& original, const QString& split)
    {
        WIMStruct* input = nullptr;
        WIMStruct* output = nullptr;
        const int inputError = wimlib_open_wim(QFile::encodeName(original).constData(), 0, &input);
        const int outputError = wimlib_open_wim(QFile::encodeName(split).constData(), 0, &output);
        bool same = false;
        int verified = EIO;
        if (inputError == 0 && outputError == 0)
        {
            const QByteArray before(wimlib_get_image_property(input, 1, "DESCRIPTION"));
            const QByteArray after(wimlib_get_image_property(output, 1, "DESCRIPTION"));
            same = before.size() == 40000 && before == after;
            verified = wimlib_verify_wim(output, 0);
        }
        wimlib_free(output);
        wimlib_free(input);
        QCOMPARE(inputError, 0);
        QCOMPARE(outputError, 0);
        QVERIFY(same);
        QCOMPARE(verified, 0);
    }

private slots:
    void initTestCase() { QCOMPARE(wimlib_global_init(WIMLIB_INIT_FLAG_ASSUME_UTF8), 0); }
    void cleanupTestCase() { wimlib_global_cleanup(); }

    void standaloneGrub4dos_data()
    {
        QTest::addColumn<QString>("mode"); QTest::addColumn<int>("error");
        QTest::addColumn<int>("filesystem");
        for (int filesystem : {FS_FAT16, FS_FAT32, FS_NTFS})
        {
            const QString prefix = filesystem == FS_NTFS ? QStringLiteral("ntfs") : filesystem == FS_FAT16 ? QStringLiteral("fat16") : QStringLiteral("fat32");
            for (const char* name : {"ok", "core-substitution"})
                QTest::newRow(qPrintable(QStringLiteral("%1-%2").arg(prefix).arg(name)))
                    << QString::fromLatin1(name) << 0 << filesystem;
            for (const char* name : {"hash", "size"})
                QTest::newRow(qPrintable(QStringLiteral("%1-%2").arg(prefix).arg(name)))
                    << QString::fromLatin1(name) << ESTALE << filesystem;
            QTest::newRow(qPrintable(QStringLiteral("%1-missing").arg(prefix))) << QStringLiteral("missing") << ENOENT << filesystem;
            QTest::newRow(qPrintable(QStringLiteral("%1-resource-error").arg(prefix))) << QStringLiteral("resource-error") << EIO << filesystem;
            QTest::newRow(qPrintable(QStringLiteral("%1-mount-support-error").arg(prefix))) << QStringLiteral("mount-support-error") << ENOTSUP << filesystem;
            for (const char* name : {"format-error", "core-error", "boot-error", "mount-error", "sync-error"})
                QTest::newRow(qPrintable(QStringLiteral("%1-%2").arg(prefix).arg(name)))
                    << QString::fromLatin1(name) << EIO << filesystem;
            QTest::newRow(qPrintable(QStringLiteral("%1-copy-error").arg(prefix))) << QStringLiteral("copy-error") << EACCES << filesystem;
            QTest::newRow(qPrintable(QStringLiteral("%1-cleanup-error").arg(prefix))) << QStringLiteral("cleanup-error") << EBUSY << filesystem;
        }
    }

    void standaloneGrub4dos()
    {
        QFETCH(QString, mode); QFETCH(int, error); QFETCH(int, filesystem);
        if (mode == QStringLiteral("boot-error"))
            mode = filesystem == FS_NTFS ? QStringLiteral("ntfs-boot-error") : QStringLiteral("fat-boot-error");
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        rufus::PrepareImageRequest request;
        request.path = temporary.filePath(QStringLiteral("disk.bin"));
        request.size = 64 * mib; request.device = 123;
        request.diskSequence = 7;
        request.objectPath = QStringLiteral("/org/freedesktop/UDisks2/block_devices/fixture");
        request.serviceOwner = QStringLiteral(":1.2"); request.identity = {QStringLiteral("fixture")};
        request.isDevice = request.mbr = request.grub4dos = true;
        request.fileSystem = filesystem; request.clusterSize = filesystem == FS_NTFS ? 4096 : filesystem == FS_FAT16 ? fat16ClusterBytes : sectorBytes;
        request.volumeId = filesystem == FS_NTFS ? 0 : fatVolumeId; request.label = QStringLiteral("GRUB4DOS");
        const QByteArray loader = read(QStringLiteral(RUFUS_GRUB4DOS_LOADER));
        QCOMPARE(loader.size(), qsizetype(322965));
        request.grub2CorePath = temporary.filePath(QStringLiteral("grldr"));
        request.grub2CoreSize = quint64(loader.size());
        request.grub2CoreSha256 = QCryptographicHash::hash(loader, QCryptographicHash::Sha256);
        put(request.grub2CorePath, loader);
        if (mode == QStringLiteral("hash")) request.grub2CoreSha256[0] ^= 1;
        if (mode == QStringLiteral("size")) ++request.grub2CoreSize;
        if (mode == QStringLiteral("missing")) QVERIFY(QFile::remove(request.grub2CorePath));
        QFile disk(request.path);
        QVERIFY(disk.open(QIODevice::WriteOnly));
        QCOMPARE(disk.write(QByteArray(512, '\x5a')), qint64(512));
        QVERIFY(disk.resize(qint64(request.size))); disk.close();
        QProcess helper;
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("RUFUS_TEST_GRUB_IMAGE"), mode);
        environment.insert(QStringLiteral("PKEXEC_UID"), QString::number(getuid()));
        helper.setProcessEnvironment(environment);
        helper.start(QStringLiteral(RUFUS_GRUB_IMAGE_PROBE), {QStringLiteral("--device")});
        QVERIFY(helper.waitForStarted());
        QByteArray output;
        while (!output.contains('\n'))
        {
            QVERIFY(helper.waitForReadyRead(helperTimeoutMs));
            output += helper.readAllStandardOutput();
        }
        QCOMPARE(output, QByteArray("{\"event\":\"ready\"}\n"));
        const QByteArray message = QJsonDocument(rufus::EncodePrepareRequest(request)).toJson(QJsonDocument::Compact) + '\n';
        QVERIFY(message.size() > 3);
        QCOMPARE(helper.write(message), message.size());
        QVERIFY(helper.waitForFinished(helperTimeoutMs));
        output = helper.readAllStandardOutput();
        const QByteArray diagnostics = helper.readAllStandardError();
        QCOMPARE(helper.exitStatus(), QProcess::NormalExit);
        QCOMPARE(helper.exitCode(), error == 0 ? 0 : 1);
        QStringList phases;
        QJsonObject result;
        for (const QByteArray& line : output.split('\n'))
        {
            const QJsonObject frame = QJsonDocument::fromJson(line).object();
            if (frame.value("event") == "phase") phases.append(frame.value("phase").toString());
            if (frame.value("event") == "result") result = frame;
        }
        rufus::PrepareImageOutcome outcome;
        QVERIFY(rufus::DecodePrepareOutcome(result, outcome));
        QCOMPARE(outcome.error, error);
        const bool beforeTarget = mode == QStringLiteral("hash") || mode == QStringLiteral("size")
            || mode == QStringLiteral("missing") || mode == QStringLiteral("resource-error");
        QCOMPARE(diagnostics.contains("GRUB_FIXTURE target-open"), !beforeTarget);
        const bool beforeWrite = beforeTarget || mode == QStringLiteral("mount-support-error");
        QCOMPARE(outcome.started, !beforeWrite);
        if (beforeWrite) QCOMPARE(read(request.path, 0, 512), QByteArray(512, '\x5a'));
        const bool mounted = phases.contains(QStringLiteral("mounting"));
        QCOMPARE(diagnostics.contains("GRUB_FIXTURE finish"), mounted);
        QCOMPARE(outcome.syncError, mode == QStringLiteral("sync-error") ? EIO : 0);
        QCOMPARE(outcome.cleanupError, mode == QStringLiteral("cleanup-error") ? EBUSY : 0);
        const QByteArray copied = read(request.path + QStringLiteral(".tree/grldr"));
        QCOMPARE(outcome.copiedBytes, quint64(copied.size()));
        if (!copied.isEmpty()) QCOMPARE(copied, loader);
        if (error == 0)
        {
            QCOMPARE(phases, QStringList({"checking", "partitioning", "formatting", "bootloader",
                "mounting", "extracting", "finalizing"}));
            QCOMPARE(outcome.copiedBytes, quint64(loader.size()));
            QCOMPARE(read(request.path, 0, sizeof(mbr_grub_0x0)),
                QByteArray(reinterpret_cast<const char*>(mbr_grub_0x0), sizeof(mbr_grub_0x0)));
            const QByteArray resource = read(QStringLiteral(RUFUS_GRUB4DOS_MBR));
            QCOMPARE(resource.size(), qsizetype(8192));
            QCOMPARE(read(request.path, 512, 7680), resource.mid(512));
            QCOMPARE(read(request.path, 8192, mib - 8192), QByteArray(mib - 8192, '\0'));
            QCOMPARE(QDir(request.path + QStringLiteral(".tree")).entryList(QDir::Files), QStringList({"grldr"}));
        }
    }

    void init()
    {
        if (QByteArray(QTest::currentTestFunction()).startsWith("standaloneGrub4dos"))
            return;
        QFETCH(QString, mode);
        QFETCH(bool, uefi);
        QFETCH(bool, windows);
        QFETCH(bool, rufusMbr);
        QFETCH(int, filesystem);
        grub4dos_ = mode.startsWith(QStringLiteral("winpe-grub4dos-"));
        if (grub4dos_) mode.replace(QStringLiteral("winpe-grub4dos-"), QStringLiteral("winpe-"));
        legacyWinPe_ = mode.startsWith(QStringLiteral("winpe-"));
        patchedWinPe_ = mode.startsWith(QStringLiteral("winpe-patch-"));
        winPeMinintOptions_ = mode.startsWith(QStringLiteral("winpe-patch-minint-options-"));
        QVERIFY(directory_.isValid());
        QDir root(directory_.path());
        for (const QString& name : root.entryList(QDir::AllEntries | QDir::NoDotAndDotDot))
        {
            const QString path = root.filePath(name);
            if (QFileInfo(path).isDir()) QVERIFY(QDir(path).removeRecursively());
            else QVERIFY(QFile::remove(path));
        }
        request_ = {};
        request_.path = root.filePath(QStringLiteral("disk.bin"));
        request_.size = 64 * mib;
        request_.device = 123;
        request_.diskSequence = 7;
        request_.objectPath = QStringLiteral("/org/freedesktop/UDisks2/block_devices/fixture");
        request_.serviceOwner = QStringLiteral(":1.2");
        request_.identity = {QStringLiteral("fixture")};
        request_.label = QStringLiteral("NEWLABEL");
        request_.isDevice = request_.mbr = true;
        request_.grub2 = !windows;
        request_.windowsBios = windows;
        request_.windowsRufusMbr = rufusMbr;
        const bool windowsFat = windows && filesystem == FS_FAT32;
        const bool fatBiosOnly = windowsFat && !uefi;
        request_.enhancedInstall = windows && (legacyWinPe_ || windowsFat || uefi || mode == QStringLiteral("bios-only-enhanced"));
        QFile disk(request_.path);
        QVERIFY(disk.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(disk.write(QByteArray(512, '\x5a')), qint64(512));
        QVERIFY(disk.resize(qint64(request_.size)));
        disk.close();
        QByteArray module(4096, '\0');
        const QByteArray marker("GRUB  version %s"), version("2.12");
        constexpr qsizetype markerOffset = 64;
        module.replace(markerOffset, marker.size(), marker);
        module.replace(markerOffset + marker.size() + 1, version.size(), version);
        const QString tree = root.filePath(QStringLiteral("source-tree"));
        const bool olderInstaller = mode.startsWith(QStringLiteral("win7-")) || mode.startsWith(QStringLiteral("win8"));
        if (legacyWinPe_)
        {
            const QString setupFamily = mode.startsWith(QStringLiteral("winpe-patch-amd64-"))
                ? QStringLiteral("amd64") : patchedWinPe_ ? QStringLiteral("i386") : QStringLiteral("minint");
            const QString loaderFamily = patchedWinPe_ ? QStringLiteral("i386") : QStringLiteral("minint");
            QByteArray loader(winPeLoaderBytes, 'x');
            if (patchedWinPe_)
            {
                const char path[] = "\\minint\\txtsetup.sif", disk[] = "rdisk(0)";
                loader.replace(winPePathOffset, sizeof(path), QByteArray(path, sizeof(path)));
                loader.replace(winPeDiskOffset, sizeof(disk), QByteArray(disk, sizeof(disk)));
                loader.replace(winPeCrcOffset, 2, QByteArray::fromHex("7403"));
            }
            put(tree + QLatin1Char('/') + loaderFamily + QStringLiteral("/setupldr.bin"), loader);
            put(tree + QLatin1Char('/') + loaderFamily + QStringLiteral("/ntdetect.com"), "Native WinPE detector\n");
            const bool minint = !patchedWinPe_ || winPeMinintOptions_;
            put(tree + QLatin1Char('/') + setupFamily + QStringLiteral("/txtsetup.sif"), minint
                ? "[SetupData]\r\nOsLoadOptions=/minint\r\n" : "[SetupData]\r\nOsLoadOptions=/fastdetect\r\n");
            expectedLoader_ = loader;
            if (patchedWinPe_)
            {
                const QByteArray prepared = QByteArray("\\") + setupFamily.toLatin1() + "\\txtsetup.sif" + '\0';
                expectedLoader_.replace(winPePathOffset, prepared.size(), prepared);
                expectedLoader_.replace(winPeCrcOffset, 2, QByteArray::fromHex("eb1a"));
                if (!minint) expectedLoader_[winPeDiskOffset + winPeDiskNumberOffset] = '1';
                if (mode.endsWith(QStringLiteral("root-prepared")))
                    put(tree + QStringLiteral("/BOOTMGR"), expectedLoader_);
                if (mode.endsWith(QStringLiteral("root-mismatch")))
                    put(tree + QStringLiteral("/BOOTMGR"), QByteArray(winPeLoaderBytes, 'y'));
            }
            if (grub4dos_)
                put(tree + QStringLiteral("/grldr"), "Original unversioned GRUB4DOS loader\n");
            else if (mode != QStringLiteral("winpe-no-grub"))
            {
                put(tree + QStringLiteral("/boot/grub/i386-pc/normal.mod"), module);
                put(tree + QStringLiteral("/boot/grub/grub.cfg"), "linux /kernel root=LABEL=SOURCE\r\n");
            }
        }
        else if (windows)
        {
            if (mode != QStringLiteral("missing-bootmgr"))
                put(tree + QStringLiteral("/BoOtMgR"), QByteArray("Opaque BIOS BOOTMGR marker\n"));
            if (mode != QStringLiteral("missing-install"))
            {
                QVERIFY(QDir().mkpath(tree + QStringLiteral("/sources")));
                QString kind = QStringLiteral("win11");
                if (olderInstaller)
                    kind = mode.section(QLatin1Char('-'), 0, 0);
                else if (mode == QStringLiteral("unknown-windows"))
                    kind = QStringLiteral("unknown");
                else if (mode == QStringLiteral("win10-udf"))
                    kind = QStringLiteral("win10");
                QProcess wim;
                wim.start(QStringLiteral(RUFUS_WIM_FIXTURE), {tree + QStringLiteral("/sources/install.wim"), kind});
                QVERIFY(wim.waitForFinished(helperTimeoutMs));
                QCOMPARE(wim.exitCode(), 0);
            }
            put(tree + QStringLiteral("/readme.txt"), QByteArray("Synthetic Windows payload\n"));
            if (mode == QStringLiteral("split-collision"))
                put(tree + QStringLiteral("/sources/install2.swm"), QByteArray("Existing split archive"));
        }
        else
        {
            put(tree + QStringLiteral("/boot/grub/i386-pc/normal.mod"), module);
            put(tree + QStringLiteral("/boot/grub/grub.cfg"), QByteArray("linux /kernel root=LABEL=SOURCE\n"));
            put(tree + QStringLiteral("/kernel"), QByteArray("Synthetic kernel payload\n"));
        }
        if ((uefi || windows) && !legacyWinPe_ && mode != QStringLiteral("unexpected-uefi")
            && mode != QStringLiteral("bios-only-enhanced") && !fatBiosOnly)
        {
            QByteArray header(512, '\0');
            header.replace(0, 2, "MZ");
            qToLittleEndian<quint32>(128, header.data() + 60);
            header.replace(128, 4, QByteArray("PE\0\0", 4));
            qToLittleEndian<quint16>(0x8664, header.data() + 132);
            qToLittleEndian<quint16>(240, header.data() + 148);
            qToLittleEndian<quint16>(0x20b, header.data() + 152);
            qToLittleEndian<quint16>(10, header.data() + 220);
            put(tree + QStringLiteral("/EFI/BOOT/BOOTX64.EFI"), header);
            if (windows) put(tree + QStringLiteral("/bootmgr.efi"), header);
        }
        if (mode == QStringLiteral("syslinux-precedence") || mode == QStringLiteral("winpe-syslinux-precedence"))
        {
            put(tree + QStringLiteral("/isolinux/isolinux.cfg"), QByteArray("default linux\n"));
            put(tree + QStringLiteral("/isolinux/isolinux.bin"), QByteArray("unknown loader version"));
        }
        const QString iso = root.filePath(QStringLiteral("source.iso"));
        QProcess mkisofs;
        QStringList arguments = {QStringLiteral("-quiet"), QStringLiteral("-R"), QStringLiteral("-J"),
            QStringLiteral("-V"), QStringLiteral("SOURCE"), QStringLiteral("-o"), iso, tree};
        if (mode.endsWith(QStringLiteral("-udf"))) arguments.prepend(QStringLiteral("-udf"));
        mkisofs.start(QStringLiteral(RUFUS_MKISOFS), arguments);
        QVERIFY(mkisofs.waitForFinished(helperTimeoutMs));
        QCOMPARE(mkisofs.exitCode(), 0);
        QCOMPARE(rufus::CapturePrepareSource(iso, request_), 0);
        if (windows && !legacyWinPe_) return;
        if (grub4dos_)
        {
            core_ = read(QString::fromUtf8(RUFUS_GRUB4DOS_MBR)).mid(qsizetype(sectorBytes));
            QCOMPARE(core_.size(), qsizetype(7680));
            return;
        }
        core_ = QByteArray(700, '\x17'); // Writer placement fixture, not a bootable GRUB core.
        request_.grub2CorePath = root.filePath(QStringLiteral("core.img"));
        request_.grub2CoreSize = quint64(core_.size());
        request_.grub2CoreSha256 = QCryptographicHash::hash(core_, QCryptographicHash::Sha256);
        put(request_.grub2CorePath, core_);
    }

    void imageJob_data()
    {
        QTest::addColumn<int>("filesystem");
        QTest::addColumn<bool>("uefi");
        QTest::addColumn<QString>("mode");
        QTest::addColumn<int>("expected");
        QTest::addColumn<bool>("windows");
        QTest::addColumn<bool>("rufusMbr");
        for (int fs : {int(FS_NTFS), int(FS_FAT32)})
        for (bool rufusMbr : {false, true})
        for (const char* mode : {"ok", "ok-udf", "patch-ok", "patch-amd64-ok", "patch-minint-options-ok"})
            QTest::newRow(qPrintable(QStringLiteral("grub4dos-%1-%2-%3").arg(fs).arg(rufusMbr).arg(mode)))
                << fs << false << QStringLiteral("winpe-grub4dos-") + QString::fromLatin1(mode) << 0 << true << rufusMbr;
        for (int fs : {int(FS_NTFS), int(FS_FAT32)})
        for (const char* mode : {"core-error", "format-error", "copy-error", "sync-error", "resource-error",
            "patch-masquerade-error", "patch-masquerade-write-error", "patch-masquerade-sync-error"})
            QTest::newRow(qPrintable(QStringLiteral("grub4dos-%1-%2").arg(fs).arg(mode)))
                << fs << false << QStringLiteral("winpe-grub4dos-") + QString::fromLatin1(mode) << EIO << true << true;
        for (int fs : {int(FS_NTFS), int(FS_FAT32)})
        {
            QTest::newRow(qPrintable(QStringLiteral("grub4dos-%1-cleanup-error").arg(fs)))
                << fs << false << QStringLiteral("winpe-grub4dos-cleanup-error") << EBUSY << true << true;
            QTest::newRow(qPrintable(QStringLiteral("grub4dos-%1-cancel-copy").arg(fs)))
                << fs << false << QStringLiteral("winpe-grub4dos-cancel-copy") << ECANCELED << true << true;
        }
        for (bool rufusMbr : {false, true})
        for (const char* mode : {"ok", "ok-udf", "core-substitution"})
            QTest::newRow(qPrintable(QStringLiteral("winpe-%1-%2").arg(rufusMbr).arg(QString::fromLatin1(mode))))
                << int(FS_NTFS) << false << QStringLiteral("winpe-") + QString::fromLatin1(mode) << 0 << true << rufusMbr;
        for (const char* mode : {"core-error", "format-error", "ntfs-boot-error", "mount-error", "copy-error", "sync-error"})
            QTest::newRow((QByteArray("winpe-") + mode).constData()) << int(FS_NTFS) << false
                << QStringLiteral("winpe-") + QString::fromLatin1(mode) << EIO << true << true;
        QTest::newRow("winpe-cleanup-error") << int(FS_NTFS) << false << QStringLiteral("winpe-cleanup-error") << EBUSY << true << true;
        QTest::newRow("winpe-bad-core-hash") << int(FS_NTFS) << false << QStringLiteral("winpe-bad-core-hash") << ESTALE << true << true;
        QTest::newRow("winpe-prewrite-source-change") << int(FS_NTFS) << false << QStringLiteral("winpe-prewrite-source-change") << ESTALE << true << true;
        QTest::newRow("winpe-cancel-copy") << int(FS_NTFS) << false << QStringLiteral("winpe-cancel-copy") << ECANCELED << true << true;
        for (const char* mode : {"no-grub", "syslinux-precedence"})
            QTest::newRow((QByteArray("winpe-") + mode).constData()) << int(FS_NTFS) << false
                << QStringLiteral("winpe-") + QString::fromLatin1(mode) << ENOTSUP << true << true;
        for (bool rufusMbr : {false, true})
        for (const char* mode : {"ok", "ok-udf", "core-substitution"})
            QTest::newRow(qPrintable(QStringLiteral("winpe-fat32-%1-%2").arg(rufusMbr).arg(QString::fromLatin1(mode))))
                << int(FS_FAT32) << false << QStringLiteral("winpe-") + QString::fromLatin1(mode) << 0 << true << rufusMbr;
        for (const char* mode : {"core-error", "format-error", "mount-error", "copy-error", "sync-error",
            "fat-boot-error", "fat-boot-write-error", "fat-boot-backup-error", "fat-boot-sync-error"})
            QTest::newRow((QByteArray("winpe-fat32-") + mode).constData()) << int(FS_FAT32) << false
                << QStringLiteral("winpe-") + QString::fromLatin1(mode) << EIO << true << true;
        for (const char* mode : {"bad-core-hash", "prewrite-source-change"})
            QTest::newRow((QByteArray("winpe-fat32-") + mode).constData()) << int(FS_FAT32) << false
                << QStringLiteral("winpe-") + QString::fromLatin1(mode) << ESTALE << true << true;
        for (const char* mode : {"cancel-copy", "cancel-fat-boot"})
            QTest::newRow((QByteArray("winpe-fat32-") + mode).constData()) << int(FS_FAT32) << false
                << QStringLiteral("winpe-") + QString::fromLatin1(mode) << ECANCELED << true << true;
        QTest::newRow("winpe-fat32-cleanup-error") << int(FS_FAT32) << false << QStringLiteral("winpe-cleanup-error") << EBUSY << true << true;
        QTest::newRow("winpe-fat32-fat-boot-corrupt") << int(FS_FAT32) << false << QStringLiteral("winpe-fat-boot-corrupt") << EBADMSG << true << true;
        for (const char* mode : {"no-grub", "syslinux-precedence"})
            QTest::newRow((QByteArray("winpe-fat32-") + mode).constData()) << int(FS_FAT32) << false
                << QStringLiteral("winpe-") + QString::fromLatin1(mode) << ENOTSUP << true << true;
        for (int filesystem : {FS_NTFS, FS_FAT32})
        {
            const QByteArray prefix = QByteArray("patch-") + QByteArray::number(filesystem) + '-';
            for (bool rufusMbr : {false, true})
            for (const char* mode : {"ok", "ok-udf", "amd64-ok", "minint-options-ok", "root-prepared", "core-substitution"})
                QTest::newRow((prefix + QByteArray::number(rufusMbr) + '-' + mode).constData())
                    << filesystem << false << QStringLiteral("winpe-patch-") + QString::fromLatin1(mode) << 0 << true << rufusMbr;
            for (const char* mode : {"core-error", "copy-error", "sync-error", "masquerade-error", "masquerade-write-error", "masquerade-sync-error"})
                QTest::newRow((prefix + mode).constData()) << filesystem << false
                    << QStringLiteral("winpe-patch-") + QString::fromLatin1(mode) << EIO << true << false;
            for (const char* mode : {"masquerade-corrupt", "masquerade-mbr-before"})
                QTest::newRow((prefix + mode).constData()) << filesystem << false
                    << QStringLiteral("winpe-patch-") + QString::fromLatin1(mode) << EBADMSG << true << false;
            for (const char* mode : {"cancel-copy", "cancel-masquerade"})
                QTest::newRow((prefix + mode).constData()) << filesystem << false
                    << QStringLiteral("winpe-patch-") + QString::fromLatin1(mode) << ECANCELED << true << false;
            QTest::newRow((prefix + "cleanup-error").constData()) << filesystem << false << QStringLiteral("winpe-patch-cleanup-error") << EBUSY << true << false;
            QTest::newRow((prefix + "root-mismatch").constData()) << filesystem << false << QStringLiteral("winpe-patch-root-mismatch") << EEXIST << true << false;
        }
        for (int filesystem : {FS_FAT16, FS_FAT32, FS_NTFS})
        for (bool uefi : {false, true})
        {
            if (uefi && filesystem != FS_NTFS) continue;
            const QByteArray prefix = QByteArray::number(filesystem) + (uefi ? "-uefi-" : "-");
            for (const char* mode : {"ok", "core-substitution"})
                QTest::newRow((prefix + mode).constData()) << filesystem << uefi << QString::fromLatin1(mode) << 0 << false << true;
            for (const char* mode : {"core-error", "format-error", "mount-error", "copy-error", "sync-error"})
                QTest::newRow((prefix + mode).constData()) << filesystem << uefi << QString::fromLatin1(mode) << EIO << false << true;
            QTest::newRow((prefix + "cleanup-error").constData()) << filesystem << uefi << QStringLiteral("cleanup-error") << EBUSY << false << true;
            for (const char* mode : {"source-change", "prewrite-source-change", "postcopy-source-change", "stale-source", "bad-core-hash"})
                QTest::newRow((prefix + mode).constData()) << filesystem << uefi << QString::fromLatin1(mode) << ESTALE << false << true;
            QTest::newRow((prefix + "mount-support-error").constData()) << filesystem << uefi << QStringLiteral("mount-support-error") << ENOTSUP << false << true;
            QTest::newRow((prefix + "syslinux-precedence").constData()) << filesystem << uefi << QStringLiteral("syslinux-precedence") << ENOTSUP << false << true;
            QTest::newRow((prefix + "cancel-copy").constData()) << filesystem << uefi << QStringLiteral("cancel-copy") << ECANCELED << false << true;
            if (filesystem == FS_NTFS)
                QTest::newRow((prefix + "ntfs-boot-error").constData()) << filesystem << uefi << QStringLiteral("ntfs-boot-error") << EIO << false << true;
            if (uefi)
            {
                QTest::newRow((prefix + "payload-error").constData()) << filesystem << uefi << QStringLiteral("payload-error") << EIO << false << true;
                QTest::newRow((prefix + "payload-sync-error").constData()) << filesystem << uefi << QStringLiteral("payload-sync-error") << EIO << false << true;
                for (const char* mode : {"missing-uefi", "unexpected-uefi"})
                    QTest::newRow((prefix + mode).constData()) << filesystem << uefi << QString::fromLatin1(mode) << ENOTSUP << false << true;
            }
        }
        for (bool rufusMbr : {false, true})
        for (const char* kind : {"win7", "win8", "win8.1"})
        {
            const QByteArray prefix = QByteArray(rufusMbr ? "older-rufus-" : "older-win7-") + kind + '-';
            for (const char* format : {"iso", "udf"})
                QTest::newRow((prefix + format).constData()) << int(FS_NTFS) << false
                    << QString::fromLatin1(kind) + '-' + QString::fromLatin1(format) << 0 << true << rufusMbr;
            QTest::newRow((prefix + "dual").constData()) << int(FS_NTFS) << true
                << QString::fromLatin1(kind) + QStringLiteral("-dual") << 0 << true << rufusMbr;
            QTest::newRow((prefix + "fat32").constData()) << int(FS_FAT32) << false
                << QString::fromLatin1(kind) + QStringLiteral("-fat32") << 0 << true << rufusMbr;
        }

        for (bool rufusMbr : {false, true})
        for (bool uefi : {false, true})
        {
            const QByteArray prefix = QByteArray(rufusMbr ? "windows-rufus-" : "windows-win7-")
                + (uefi ? "dual-" : "bios-");
            for (const char* mode : {"ok", "win10-udf"})
                QTest::newRow((prefix + mode).constData()) << int(FS_NTFS) << uefi << QString::fromLatin1(mode) << 0 << true << rufusMbr;
            for (const char* mode : {"format-error", "ntfs-boot-error", "mount-error", "copy-error", "sync-error"})
                QTest::newRow((prefix + mode).constData()) << int(FS_NTFS) << uefi << QString::fromLatin1(mode) << EIO << true << rufusMbr;
            QTest::newRow((prefix + "cleanup-error").constData()) << int(FS_NTFS) << uefi << QStringLiteral("cleanup-error") << EBUSY << true << rufusMbr;
            for (const char* mode : {"source-change", "prewrite-source-change", "postcopy-source-change", "stale-source"})
                QTest::newRow((prefix + mode).constData()) << int(FS_NTFS) << uefi << QString::fromLatin1(mode) << ESTALE << true << rufusMbr;
            for (const char* mode : {"missing-bootmgr", "missing-install", "unknown-windows", "syslinux-precedence", "mount-support-error"})
                QTest::newRow((prefix + mode).constData()) << int(FS_NTFS) << uefi << QString::fromLatin1(mode) << ENOTSUP << true << rufusMbr;
            QTest::newRow((prefix + "cancel-copy").constData()) << int(FS_NTFS) << uefi << QStringLiteral("cancel-copy") << ECANCELED << true << rufusMbr;
            if (uefi)
            {
                for (const char* mode : {"payload-error", "payload-sync-error"})
                    QTest::newRow((prefix + mode).constData()) << int(FS_NTFS) << uefi << QString::fromLatin1(mode) << EIO << true << rufusMbr;
                for (const char* mode : {"missing-uefi", "unexpected-uefi"})
                    QTest::newRow((prefix + mode).constData()) << int(FS_NTFS) << uefi << QString::fromLatin1(mode) << ENOTSUP << true << rufusMbr;
            }
            else
                QTest::newRow((prefix + "bios-only-enhanced").constData()) << int(FS_NTFS) << uefi << QStringLiteral("bios-only-enhanced") << 0 << true << rufusMbr;
        }

        for (bool rufusMbr : {false, true})
        for (bool uefi : {false, true})
        {
            const QByteArray prefix = QByteArray(rufusMbr ? "windows-fat32-rufus-" : "windows-fat32-win7-")
                + (uefi ? "dual-" : "bios-");
            for (const char* mode : {"ok", "win10-udf", "split-wim"})
                QTest::newRow((prefix + mode).constData()) << int(FS_FAT32) << uefi << QString::fromLatin1(mode) << 0 << true << rufusMbr;
            for (const char* mode : {"format-error", "mount-error", "copy-error", "sync-error", "fat-boot-error",
                    "fat-boot-write-error", "fat-boot-backup-error", "fat-boot-sync-error"})
                QTest::newRow((prefix + mode).constData()) << int(FS_FAT32) << uefi << QString::fromLatin1(mode) << EIO << true << rufusMbr;
            for (const char* mode : {"fat-boot-corrupt", "fat-boot-mbr-before", "fat-boot-mbr-change"})
                QTest::newRow((prefix + mode).constData()) << int(FS_FAT32) << uefi << QString::fromLatin1(mode) << EBADMSG << true << rufusMbr;
            for (const char* mode : {"source-change", "prewrite-source-change", "postcopy-source-change", "stale-source", "fat-boot-source-change"})
                QTest::newRow((prefix + mode).constData()) << int(FS_FAT32) << uefi << QString::fromLatin1(mode) << ESTALE << true << rufusMbr;
            for (const char* mode : {"missing-bootmgr", "missing-install", "unknown-windows", "syslinux-precedence", "mount-support-error"})
                QTest::newRow((prefix + mode).constData()) << int(FS_FAT32) << uefi << QString::fromLatin1(mode) << ENOTSUP << true << rufusMbr;
            for (const char* mode : {"cancel-copy", "cancel-fat-boot"})
                QTest::newRow((prefix + mode).constData()) << int(FS_FAT32) << uefi << QString::fromLatin1(mode) << ECANCELED << true << rufusMbr;
            QTest::newRow((prefix + "cleanup-error").constData()) << int(FS_FAT32) << uefi << QStringLiteral("cleanup-error") << EBUSY << true << rufusMbr;
            QTest::newRow((prefix + "split-collision").constData()) << int(FS_FAT32) << uefi << QStringLiteral("split-collision") << EEXIST << true << rufusMbr;
        }

    }

    void imageJob()
    {
        QFETCH(int, filesystem); QFETCH(bool, uefi); QFETCH(QString, mode); QFETCH(int, expected);
        QFETCH(bool, windows);
        if (grub4dos_) mode.replace(QStringLiteral("winpe-grub4dos-"), QStringLiteral("winpe-"));
        if (patchedWinPe_)
        {
            mode.remove(0, QStringLiteral("winpe-patch-").size());
            if (mode.startsWith(QStringLiteral("amd64-"))) mode.remove(0, QStringLiteral("amd64-").size());
            if (winPeMinintOptions_) mode.remove(0, QStringLiteral("minint-options-").size());
        }
        else if (legacyWinPe_) mode.remove(0, QStringLiteral("winpe-").size());
        request_.fileSystem = filesystem;
        request_.uefiNtfs = uefi && filesystem == FS_NTFS && mode != QStringLiteral("missing-uefi");
        const bool windowsFat = windows && filesystem == FS_FAT32;
        if (windowsFat) request_.volumeId = fatVolumeId;
        request_.clusterSize = filesystem == FS_FAT16 ? 2048 : filesystem == FS_NTFS ? 4096 : 512;
        if (mode == QStringLiteral("stale-source")) ++request_.imageInode;
        if (mode == QStringLiteral("bad-core-hash")) request_.grub2CoreSha256[0] ^= 1;
        const QByteArray before = read(request_.path, 0, 512);
        QProcess helper;
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("PKEXEC_UID"), QString::number(getuid()));
        environment.insert(QStringLiteral("RUFUS_TEST_GRUB_IMAGE"), mode);
        helper.setProcessEnvironment(environment);
        helper.start(QStringLiteral(RUFUS_GRUB_IMAGE_PROBE), {QStringLiteral("--device")});
        QVERIFY(helper.waitForStarted(helperTimeoutMs));
        QVERIFY(helper.waitForReadyRead(helperTimeoutMs));
        QCOMPARE(helper.readLine(), QByteArray("{\"event\":\"ready\"}\n"));
        const QJsonObject request = rufus::EncodePrepareRequest(request_);
        QVERIFY(!request.isEmpty());
        const QByteArray message = QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n';
        QCOMPARE(helper.write(message), message.size());
        QByteArray diagnostics, output;
        if (mode == QStringLiteral("cancel-copy") || mode == QStringLiteral("cancel-fat-boot")
            || mode == QStringLiteral("cancel-masquerade"))
        {
            const QByteArray waiting = mode == QStringLiteral("cancel-copy") ? "GRUB_FIXTURE copy-wait"
                : mode == QStringLiteral("cancel-fat-boot") ? "GRUB_FIXTURE fat-boot-wait" : "GRUB_FIXTURE masquerade-wait";
            QTRY_VERIFY_WITH_TIMEOUT((diagnostics += helper.readAllStandardError()).contains(waiting),
                helperTimeoutMs);
            helper.closeWriteChannel();
        }
        QVERIFY(helper.waitForFinished(helperTimeoutMs));
        output += helper.readAllStandardOutput();
        diagnostics += helper.readAllStandardError();
        if (windows && !legacyWinPe_)
            QVERIFY(!diagnostics.contains("GRUB_FIXTURE core"));
        QCOMPARE(helper.exitStatus(), QProcess::NormalExit);
        QCOMPARE(helper.exitCode(), expected == 0 ? 0 : 1);
        rufus::PrepareImageOutcome outcome;
        QStringList phases;
        quint64 previous = 0, total = 0;
        bool result = false;
        for (const QByteArray& line : output.split('\n'))
        {
            const QJsonObject frame = QJsonDocument::fromJson(line).object();
            if (frame.value("event") == QStringLiteral("phase")) phases.append(frame.value("phase").toString());
            if (frame.value("event") == QStringLiteral("result")) result = rufus::DecodePrepareOutcome(frame, outcome);
            if (frame.value("event") == QStringLiteral("progress"))
            {
                int operation;
                quint64 processed, denominator;
                QVERIFY(rufus::DecodePrepareProgress(frame, operation, processed, denominator));
                QCOMPARE(operation, int(OP_FILE_COPY));
                QVERIFY(denominator > 0 && processed >= previous && processed <= denominator);
                QVERIFY(total == 0 || total == denominator);
                previous = processed; total = denominator;
            }
        }
        QVERIFY2(result, output.constData());
        QVERIFY2(outcome.error == expected, diagnostics.constData());
        QCOMPARE(outcome.error, expected);
        const bool windowsRefusal = mode == QStringLiteral("missing-bootmgr")
            || mode == QStringLiteral("missing-install") || mode == QStringLiteral("unknown-windows");
        const bool legacyRefusal = legacyWinPe_ && (mode == QStringLiteral("no-grub") || mode == QStringLiteral("root-mismatch"));
        const bool beforeWrites = mode == QStringLiteral("resource-error") || windowsRefusal || legacyRefusal || mode == QStringLiteral("stale-source") || mode == QStringLiteral("bad-core-hash")
            || mode == QStringLiteral("prewrite-source-change") || mode == QStringLiteral("mount-support-error")
            || mode == QStringLiteral("syslinux-precedence") || mode == QStringLiteral("missing-uefi")
            || mode == QStringLiteral("unexpected-uefi") || mode == QStringLiteral("split-collision");
        QCOMPARE(outcome.started, !beforeWrites);
        if (beforeWrites) QCOMPARE(read(request_.path, 0, 512), before);
        if (legacyWinPe_)
        {
            const bool beforeTarget = mode == QStringLiteral("resource-error") || legacyRefusal || mode == QStringLiteral("bad-core-hash")
                || mode == QStringLiteral("syslinux-precedence");
            if (beforeTarget) QVERIFY(!diagnostics.contains("GRUB_FIXTURE target-open"));
            const bool copied = diagnostics.contains("GRUB_FIXTURE copy");
            const bool mounted = diagnostics.contains("GRUB_FIXTURE mount\n");
            QCOMPARE(diagnostics.contains("GRUB_FIXTURE finish"), mounted);
            QCOMPARE(phases.count(QStringLiteral("finalizing")), mounted ? 1 : 0);
            const bool syncFailure = mode == QStringLiteral("sync-error") || mode == QStringLiteral("fat-boot-sync-error")
                || mode == QStringLiteral("masquerade-sync-error");
            QCOMPARE(outcome.syncError, syncFailure ? EIO : 0);
            QCOMPARE(outcome.cleanupError, mode == QStringLiteral("cleanup-error") ? EBUSY : 0);
            if (windowsFat)
            {
                const bool finalRecords = mode == QStringLiteral("ok") || mode == QStringLiteral("ok-udf")
                    || mode == QStringLiteral("core-substitution") || mode.startsWith(QStringLiteral("fat-boot-"))
                    || mode == QStringLiteral("cancel-fat-boot")
                    || (mode.startsWith(QStringLiteral("masquerade-")) && mode != QStringLiteral("masquerade-mbr-before"))
                    || mode == QStringLiteral("cancel-masquerade") || mode == QStringLiteral("root-prepared");
                QCOMPARE(diagnostics.contains("GRUB_FIXTURE winpe-fat-boot\n"), finalRecords);
            }
            const bool masquerade = patchedWinPe_ && !winPeMinintOptions_;
            if (masquerade)
            {
                const bool finalMbr = expected == 0 || mode.startsWith(QStringLiteral("masquerade-"))
                    || mode == QStringLiteral("cancel-masquerade");
                QCOMPARE(diagnostics.contains("GRUB_FIXTURE masquerade\n"), finalMbr && mode != QStringLiteral("masquerade-mbr-before"));
                if (diagnostics.contains("GRUB_FIXTURE masquerade\n"))
                    QVERIFY(diagnostics.indexOf("GRUB_FIXTURE finish\n") < diagnostics.indexOf("GRUB_FIXTURE masquerade\n"));
            }
            else QVERIFY(!diagnostics.contains("GRUB_FIXTURE masquerade\n"));
            if (mounted) QVERIFY(diagnostics.contains("GRUB_FIXTURE MOUNT_ACTIVE=80"));
            if (expected != 0) return;
            QVERIFY(copied && mounted && outcome.copiedBytes > 0);
            QCOMPARE(previous, total);
            QCOMPARE(phases, QStringList({"checking", "partitioning", "formatting", "bootloader", "mounting", "extracting", "finalizing"}));
            const QByteArray mbr = read(request_.path, 0, qint64(sectorBytes));
            const QByteArray code = masquerade || request_.windowsRufusMbr
                ? QByteArray(reinterpret_cast<const char*>(mbr_rufus_0x0), sizeof(mbr_rufus_0x0))
                : QByteArray(reinterpret_cast<const char*>(mbr_win7_0x0), sizeof(mbr_win7_0x0));
            QCOMPARE(mbr.left(diskSignatureOffset), code);
            QCOMPARE(quint8(mbr[firstPartitionOffset]), quint8(masquerade ? 0x81 : 0x80));
            QCOMPARE(quint8(mbr[firstPartitionOffset + 4]), quint8(windowsFat ? 0x0c : 0x07));
            QCOMPARE(mbr.mid(510, 2), QByteArray::fromHex("55aa"));
            QCOMPARE(read(request_.path, qint64(coreOffset), core_.size()), core_);
            const quint64 remainingGap = outcome.offset - coreOffset - quint64(core_.size());
            QCOMPARE(read(request_.path, qint64(coreOffset + quint64(core_.size())), qint64(remainingGap)), QByteArray(qsizetype(remainingGap), '\0'));
            if (windowsFat)
            {
                const quint64 positions[] = {0, 0x52, 0x3f0, 0x1800};
                const unsigned char* bytes[] = {br_fat32_0x0, br_fat32pe_0x52, br_fat32pe_0x3f0, br_fat32pe_0x1800};
                const size_t lengths[] = {sizeof(br_fat32_0x0), sizeof(br_fat32pe_0x52),
                    sizeof(br_fat32pe_0x3f0), sizeof(br_fat32pe_0x1800)};
                for (quint64 copy : {quint64(0), quint64(6 * sectorBytes)})
                {
                    const quint64 base = outcome.offset + copy;
                    const QByteArray boot = read(request_.path, qint64(base), qint64(sectorBytes));
                    QCOMPARE(qFromLittleEndian<quint32>(boot.constData() + 67), fatVolumeId);
                    QCOMPARE(qFromLittleEndian<quint32>(boot.constData() + 28), quint32(outcome.offset / sectorBytes));
                    QCOMPARE(quint8(boot[64]), quint8(masquerade ? 0x81 : 0x80));
                    const QByteArray marker = QByteArray("GRUB_FIXTURE WINPE_HINTS_")
                        + QByteArray::number(copy / sectorBytes) + '=';
                    const qsizetype hintPosition = diagnostics.indexOf(marker);
                    QVERIFY(hintPosition >= 0);
                    const QByteArray hints = QByteArray::fromHex(diagnostics.mid(hintPosition + marker.size(), 2 * fatFsinfoHintBytes));
                    QCOMPARE(hints.size(), fatFsinfoHintBytes);
                    QCOMPARE(read(request_.path, qint64(base + fatFsinfoHintOffset), fatFsinfoHintBytes), hints);
                    for (unsigned region = 0; region < 4; region++)
                    {
                        QByteArray expected(reinterpret_cast<const char*>(bytes[region]), qsizetype(lengths[region]));
                        const quint64 hintStart = qMax(positions[region], fatFsinfoHintOffset);
                        const quint64 hintEnd = qMin(positions[region] + lengths[region], fatFsinfoHintOffset + fatFsinfoHintBytes);
                        if (hintStart < hintEnd)
                            expected.replace(qsizetype(hintStart - positions[region]), qsizetype(hintEnd - hintStart),
                                hints.mid(qsizetype(hintStart - fatFsinfoHintOffset), qsizetype(hintEnd - hintStart)));
                        QCOMPARE(read(request_.path, qint64(base + positions[region]), qint64(lengths[region])), expected);
                    }
                }
                QVERIFY(diagnostics.indexOf("GRUB_FIXTURE core\n") < diagnostics.indexOf("GRUB_FIXTURE mount\n"));
                QVERIFY(diagnostics.indexOf("GRUB_FIXTURE finish\n") < diagnostics.indexOf("GRUB_FIXTURE winpe-fat-boot\n"));
                QVERIFY(!diagnostics.contains("GRUB_FIXTURE fat-boot\n"));
                QVERIFY(!diagnostics.contains("GRUB_FIXTURE ntfs-boot"));
            }
            else
            {
                QCOMPARE(read(request_.path + QStringLiteral(".ntfs"), ntfsCodeOffset, sizeof(br_ntfs_0x54)),
                    QByteArray(reinterpret_cast<const char*>(br_ntfs_0x54), sizeof(br_ntfs_0x54)));
                QVERIFY(diagnostics.indexOf("GRUB_FIXTURE core\n") < diagnostics.indexOf("GRUB_FIXTURE ntfs-boot"));
                QVERIFY(diagnostics.indexOf("GRUB_FIXTURE ntfs-boot") < diagnostics.indexOf("GRUB_FIXTURE mount\n"));
            }
            const QString destination = request_.path + QStringLiteral(".tree");
            QCOMPARE(read(destination + QStringLiteral("/BOOTMGR")), expectedLoader_);
            QCOMPARE(read(destination + QStringLiteral("/ntdetect.com")), QByteArray("Native WinPE detector\n"));
            if (masquerade)
                QCOMPARE(read(destination + QStringLiteral("/TXTSETUP.SIF")), QByteArray("[SetupData]\r\nSetupSourceDevice = \"\\device\\harddisk1\\partition1\"\r\nOsLoadOptions=/fastdetect\r\n"));
            else QVERIFY(!QFile::exists(destination + QStringLiteral("/TXTSETUP.SIF")));
            if (grub4dos_)
                QCOMPARE(read(destination + QStringLiteral("/grldr")), QByteArray("Original unversioned GRUB4DOS loader\n"));
            else
                QVERIFY(read(destination + QStringLiteral("/boot/grub/grub.cfg")).contains("root=LABEL=NEWLABEL"));
            QCOMPARE(diagnostics.count("GRUB_FIXTURE core\n"), 1);
            return;
        }
        if (expected == 0)
        {
            QCOMPARE(phases, QStringList({"checking", "partitioning", "formatting", "bootloader", "mounting", "extracting", "finalizing"}));
            if (windows)
            {
                const QByteArray mbr = read(request_.path, 0, 512);
                const QByteArray expectedMbr = request_.windowsRufusMbr
                    ? QByteArray(reinterpret_cast<const char*>(mbr_rufus_0x0), sizeof(mbr_rufus_0x0))
                    : QByteArray(reinterpret_cast<const char*>(mbr_win7_0x0), sizeof(mbr_win7_0x0));
                QCOMPARE(mbr.left(diskSignatureOffset), expectedMbr);
                QCOMPARE(quint8(mbr[firstPartitionOffset]), quint8(0x80));
                QCOMPARE(quint8(mbr[firstPartitionOffset + 4]), quint8(windowsFat ? 0x0c : 0x07));
                QCOMPARE(mbr.mid(510, 2), QByteArray::fromHex("55aa"));
                QCOMPARE(quint64(qFromLittleEndian<quint32>(mbr.constData() + firstPartitionOffset + 8)) * sectorBytes, outcome.offset);
                QCOMPARE(quint64(qFromLittleEndian<quint32>(mbr.constData() + firstPartitionOffset + 12)) * sectorBytes, outcome.size);
                if (windowsFat)
                {
                    const quint64 positions[] = {0, 0x52, 0x3f0, 0x1800};
                    const unsigned char* bytes[] = {br_fat32_0x0, br_fat32pe_0x52, br_fat32pe_0x3f0, br_fat32pe_0x1800};
                    const size_t lengths[] = {sizeof(br_fat32_0x0), sizeof(br_fat32pe_0x52),
                        sizeof(br_fat32pe_0x3f0), sizeof(br_fat32pe_0x1800)};
                    for (quint64 copy : {quint64(0), quint64(6 * sectorBytes)})
                    {
                        const quint64 base = outcome.offset + copy;
                        const QByteArray boot = read(request_.path, qint64(base), qint64(sectorBytes));
                        QCOMPARE(qFromLittleEndian<quint32>(boot.constData() + 67), fatVolumeId);
                        QCOMPARE(qFromLittleEndian<quint32>(boot.constData() + 28), quint32(outcome.offset / sectorBytes));
                        QCOMPARE(quint8(boot[64]), quint8(0x80));
                        QCOMPARE(boot.mid(71, 11), QByteArray("NO NAME    "));
                        for (unsigned region = 0; region < 4; region++)
                            QCOMPARE(read(request_.path, qint64(base + positions[region]), qint64(lengths[region])),
                                QByteArray(reinterpret_cast<const char*>(bytes[region]), qsizetype(lengths[region])));
                    }
                    const int bootStage = diagnostics.indexOf("GRUB_FIXTURE fat-boot\n");
                    const int mountStage = diagnostics.indexOf("GRUB_FIXTURE mount\n");
                    QVERIFY(bootStage >= 0 && mountStage > bootStage);
                    QVERIFY(!diagnostics.contains("GRUB_FIXTURE payload"));
                }
                else
                {
                    const QByteArray ntfsCode(reinterpret_cast<const char*>(br_ntfs_0x54), sizeof(br_ntfs_0x54));
                    QCOMPARE(read(request_.path + QStringLiteral(".ntfs"), ntfsCodeOffset, ntfsCode.size()), ntfsCode);
                    QVERIFY(diagnostics.indexOf("GRUB_FIXTURE ntfs-boot") < diagnostics.indexOf("GRUB_FIXTURE copy"));
                }
                if (!uefi || windowsFat)
                {
                    QVERIFY(!diagnostics.contains("GRUB_FIXTURE payload"));
                    QCOMPARE(mbr.mid(secondPartitionOffset, 16), QByteArray(16, '\0'));
                }
            }
            else
                QCOMPARE(read(request_.path, qint64(coreOffset), core_.size()), core_);
            QVERIFY(outcome.copiedBytes > 0);
            QVERIFY(total > 0);
            if (mode == QStringLiteral("split-wim"))
            {
                // This fixture has no file resources. Windows WimProgressFunc
                // counts compressed resources, excluding archive metadata.
                const quint64 wimBytes = quint64(QFileInfo(directory_.filePath(QStringLiteral("source-tree/sources/install.wim"))).size());
                const quint64 wimBlocks = wimBytes / isoBlockBytes + (wimBytes % isoBlockBytes != 0);
                QVERIFY(total >= wimBlocks);
                QCOMPARE(previous, total - wimBlocks);
            }
            else
                QCOMPARE(previous, total);
            const QString destination = request_.path + QStringLiteral(".tree");
            if (windows)
            {
                const QString tree = directory_.filePath(QStringLiteral("source-tree"));
                QCOMPARE(read(destination + QStringLiteral("/BoOtMgR")), read(tree + QStringLiteral("/BoOtMgR")));
                if (mode == QStringLiteral("split-wim"))
                {
                    QVERIFY(!QFile::exists(destination + QStringLiteral("/sources/install.wim")));
                    checkSplitArchive(tree + QStringLiteral("/sources/install.wim"), destination + QStringLiteral("/sources/install.swm"));
                }
                else
                    QCOMPARE(read(destination + QStringLiteral("/sources/install.wim")), read(tree + QStringLiteral("/sources/install.wim")));
                QCOMPARE(read(destination + QStringLiteral("/readme.txt")), QByteArray("Synthetic Windows payload\n"));
                if (mode != QStringLiteral("bios-only-enhanced") && (!windowsFat || uefi))
                    QCOMPARE(read(destination + QStringLiteral("/EFI/BOOT/BOOTX64.EFI")), read(tree + QStringLiteral("/EFI/BOOT/BOOTX64.EFI")));
            }
            else
            {
                QCOMPARE(read(destination + QStringLiteral("/kernel")), QByteArray("Synthetic kernel payload\n"));
                QCOMPARE(read(destination + QStringLiteral("/boot/grub/i386-pc/normal.mod")),
                    read(directory_.filePath(QStringLiteral("source-tree/boot/grub/i386-pc/normal.mod"))));
                const QByteArray config = read(destination + QStringLiteral("/boot/grub/grub.cfg"));
                QVERIFY(config.contains("root=LABEL=NEWLABEL"));
                QVERIFY(!config.contains("LABEL=SOURCE"));
                QVERIFY(diagnostics.indexOf("GRUB_FIXTURE core") < diagnostics.indexOf("GRUB_FIXTURE copy"));
            }
            if (uefi && !windowsFat)
            {
                const QByteArray mbr = read(request_.path, 0, 512);
                QCOMPARE(mbr.size(), qsizetype(sectorBytes));
                const quint64 payloadOffset = quint64(qFromLittleEndian<quint32>(mbr.constData() + secondPartitionOffset + 8)) * sectorBytes;
                const quint64 payloadSize = quint64(qFromLittleEndian<quint32>(mbr.constData() + secondPartitionOffset + 12)) * sectorBytes;
                const QByteArray payload = read(QStringLiteral(RUFUS_UEFI_NTFS_PAYLOAD));
                QCOMPARE(quint8(mbr[secondPartitionOffset]), quint8(0));
                QCOMPARE(quint8(mbr[secondPartitionOffset + 4]), quint8(0xef));
                QVERIFY(payloadOffset >= outcome.offset + outcome.size && payloadSize >= quint64(payload.size()));
                QCOMPARE(read(request_.path, qint64(payloadOffset), payload.size()), payload);
                QVERIFY(diagnostics.indexOf("GRUB_FIXTURE ntfs-boot") < diagnostics.indexOf("GRUB_FIXTURE payload"));
                QVERIFY(diagnostics.indexOf("GRUB_FIXTURE payload") < diagnostics.indexOf("GRUB_FIXTURE copy"));
            }
        }
        const bool attemptedMount = phases.contains(QStringLiteral("mounting"));
        QCOMPARE(diagnostics.contains("GRUB_FIXTURE finish"), attemptedMount);
        QCOMPARE(phases.count(QStringLiteral("finalizing")), attemptedMount ? 1 : 0);
        const bool syncFailure = mode == QStringLiteral("sync-error") || mode == QStringLiteral("payload-sync-error") || mode == QStringLiteral("fat-boot-sync-error");
        QCOMPARE(outcome.syncError, syncFailure ? EIO : 0);
        QCOMPARE(outcome.cleanupError, mode == QStringLiteral("cleanup-error") ? EBUSY : 0);
        if (mode == QStringLiteral("ntfs-boot-error")) QVERIFY(!diagnostics.contains("GRUB_FIXTURE copy"));
        if (mode.startsWith(QStringLiteral("fat-boot-")) || mode == QStringLiteral("cancel-fat-boot"))
        {
            QVERIFY(!diagnostics.contains("GRUB_FIXTURE copy"));
            QVERIFY(!phases.contains(QStringLiteral("mounting")));
        }
        if (mode == QStringLiteral("fat-boot-backup-error"))
            QVERIFY(diagnostics.contains("FAT_BYTES=1536 FAT_SYNC=0"));
        if (mode == QStringLiteral("cancel-fat-boot"))
            QVERIFY(diagnostics.contains("FAT_BYTES=17 FAT_SYNC=0"));
        if (mode == QStringLiteral("fat-boot-backup-error") || mode == QStringLiteral("cancel-fat-boot"))
            QVERIFY(diagnostics.contains("GRUB_FIXTURE fat-boot-sync"));
        if (mode == QStringLiteral("fat-boot-mbr-before"))
            QVERIFY(!diagnostics.contains("GRUB_FIXTURE fat-boot\n"));
        if (mode.startsWith(QStringLiteral("payload-"))) QVERIFY(!diagnostics.contains("GRUB_FIXTURE copy"));
    }
};

QTEST_GUILESS_MAIN(TestPrepareGrub2Image)
#include "test_prepare_grub2_image.moc"
