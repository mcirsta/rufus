// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/iso_write_check.h"
#include "platform/prepare_source.h"
#include "iso_file.h"
#include "platform/windows_customization.h"
#include "platform/wim_read_file.h"
#include "platform/windows_bootloaders.h"
#include "config_patch.h"
#include "winpe_setup.h"
#include "platform/winpe_prepare.h"

#include <QDir>
#include <QDirIterator>
#include <QCryptographicHash>
#include <QFile>
#include <QProcess>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTest>
#include <QStringConverter>
#include <QtEndian>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <cstring>
#include <unistd.h>

namespace
{
constexpr quint64 testMiB = 1024 * 1024;
constexpr int testTimeoutMs = 10000;
constexpr int legacyLoaderBytes = 9000;
constexpr quint32 legacyFatClusterBytes = 512;
constexpr quint32 legacyNtfsClusterBytes = 4096;
enum RootSyslinuxConfig { NoRootConfig, KeepRootConfig, ReplaceRootConfig };
enum GrubDataProvider { NoGrubData = 0, Grub2Data = 1, Grub4DosData = 2, BothGrubData = 3 };
enum class Fault { None, ContentPending, Truncated, NoTree, FatlessGrub, NtfslessGrub, NeedsNtfs,
                   LargeFile, Windows, ReactOs, Kolibri, ReadError, CancelRead,
                   ChangeAfterTree, TooLargeTree, InstallReadError, InstallReadCancelled, InstallSourceChanged,
                   LargeWim, LargeWimSpace, UnknownGrub, GrubWithSyslinux, LegacyWindows,
                   WindowsEfiReadError, WindowsEfiCancel, WindowsRepairCancel, WinPeOptionsChanged,
                   WinPeRootReadError, WinPeRootReadCancel, WinPeRootSifReadError, WinPeRootSifReadCancel,
                   RootEditStageSpace, WinPeRootLoaderReadError, WinPeRootLoaderReadCancel };
Fault fault = Fault::None;
bool cancelled = false;
QString sourcePath;
int fallbackReads = 0;
int wimOpens = 0;
int detectorReads = 0;
int loaderReads = 0;
uint64_t extractionLimit = 0;
bool splitSelected = false;
const QByteArray rootSifFixture(";root-only\r\n[SetupData]\r\nRootValue=keep\r\n");
quint64 stagedTreeBytes = 0;
enum class RepairFault { None, NoTrigger, NoInstaller, InvalidPath, UnterminatedPath, MissingArchive,
                         BrokenArchive, ReadError, CancelBeforeRead, CancelDuringRead, CancelAfterRead };

QByteArray efiHeader()
{
    // Independent minimal PE32+ header. A structural test fixture, not a
    // bootable executable; real payload qualification is recorded separately.
    QByteArray bytes(512, '\0');
    bytes.replace(0, 2, "MZ");
    qToLittleEndian<quint32>(128, bytes.data() + 60);
    bytes.replace(128, 4, QByteArray("PE\0\0", 4));
    qToLittleEndian<quint16>(0x8664, bytes.data() + 132);
    qToLittleEndian<quint16>(240, bytes.data() + 148);
    qToLittleEndian<quint16>(0x20b, bytes.data() + 152);
    qToLittleEndian<quint16>(10, bytes.data() + 220);
    return bytes;
}

QMap<QString, QByteArray> fileTree(const QString& root)
{
    QMap<QString, QByteArray> files;
    QDirIterator entries(root, QDir::Files, QDirIterator::Subdirectories);
    while (entries.hasNext())
    {
        QFile file(entries.next());
        if (!file.open(QIODevice::ReadOnly))
            qFatal("Cannot read extracted-tree fixture");
        files.insert(QDir(root).relativeFilePath(file.fileName()), file.readAll());
    }
    return files;
}

int descriptors()
{
    return QDir(QStringLiteral("/proc/self/fd")).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size();
}
}

extern "C" void _UpdateProgressWithInfo(int, int, uint64_t, uint64_t, int) {}
extern "C" int IsIsoScanCancelled(void) { return cancelled; }
extern "C" int IsRufusJobCancelled(void) { return cancelled; }
extern "C" void uprintf(const char*, ...) {}
extern "C" int __real_AnalyzeIsoImageWithSource(const char*, uint64_t, const ISO_ANALYSIS_OPTIONS*, ISO_ANALYSIS*, ISO_ANALYSIS_SOURCE*);
extern "C" int __real_ReadIsoFileRange(ISO_FILE_READER*, uint64_t, void*, size_t);
extern "C" int __real_CheckIsoTree(const char*, const ISO_EXTRACT_OPTIONS*, uint64_t, ISO_EXTRACT_RESULT*);
extern "C" int __real_OpenWimFromIsoReader(ISO_FILE_READER*, int, WIMStruct**);
extern "C" int __real_ReadWimFile(WIMStruct*, int, const char*, uint8_t**, size_t*);

extern "C" int __wrap_ReadWimFile(WIMStruct* wim, int image, const char* path, uint8_t** data, size_t* size)
{
    const int error = __real_ReadWimFile(wim, image, path, data, size);
    if (fault == Fault::WindowsRepairCancel)
        cancelled = true; // Exercise cleanup after the successful owned allocation.
    return error;
}

extern "C" int __wrap_OpenWimFromIsoReader(ISO_FILE_READER* reader, int flags, WIMStruct** wim)
{
    // First open supplies the version; second supplies repair or edition data.
    const bool readingAdditionalData = ++wimOpens == 2;
    if (readingAdditionalData && fault == Fault::InstallReadError)
        return WIMLIB_ERR_READ;
    if (readingAdditionalData && fault == Fault::InstallReadCancelled)
    {
        cancelled = true;
        return WIMLIB_ERR_ABORTED_BY_PROGRESS;
    }
    if (readingAdditionalData && fault == Fault::InstallSourceChanged)
    {
        QFile source(sourcePath);
        if (!source.open(QIODevice::Append) || source.write("changed") != 7)
            qFatal("Cannot alter source fixture during edition read");
    }
    return __real_OpenWimFromIsoReader(reader, flags, wim);
}

extern "C" int __wrap_AnalyzeIsoImageWithSource(const char* path, uint64_t size,
    const ISO_ANALYSIS_OPTIONS* options, ISO_ANALYSIS* analysis, ISO_ANALYSIS_SOURCE* source)
{
    const int status = __real_AnalyzeIsoImageWithSource(path, size, options, analysis, source);
    if (status != ISO_SCAN_OK && status != ISO_SCAN_PARTIAL)
        return status;
    switch (fault)
    {
    case Fault::ContentPending: analysis->pending |= ISO_PENDING_CONTENT; break;
    case Fault::Truncated: analysis->pending |= ISO_PENDING_TRUNCATED; break;
    case Fault::NoTree: analysis->tree_complete = false; break;
    case Fault::FatlessGrub: analysis->report.has_grub2 = 0x80; analysis->report.has_grub2_fs = 4; break;
    case Fault::NtfslessGrub: analysis->report.has_grub2 = 0x80; analysis->report.has_grub2_fs = 1; break;
    case Fault::NeedsNtfs: analysis->report.needs_ntfs = true; break;
    case Fault::LargeFile: analysis->report.has_4GB_file = true; break;
    case Fault::LargeWim:
    case Fault::LargeWimSpace: analysis->report.has_4GB_file = 0x11; break;
    case Fault::Windows: analysis->report.has_bootmgr = true; break;
    case Fault::ReactOs: strcpy(analysis->report.reactos_path, "/freeldr.sys"); break;
    case Fault::Kolibri: analysis->report.has_kolibrios = true; break;
    case Fault::UnknownGrub: analysis->report.grub2_version[0] = 0; break;
    case Fault::GrubWithSyslinux: analysis->report.has_syslinux = true; break;
    case Fault::LegacyWindows: analysis->report.uses_minint = true; break;
    case Fault::WinPeOptionsChanged: analysis->report.uses_minint = !analysis->report.uses_minint; break;
    default: break;
    }
    return analysis->pending ? ISO_SCAN_PARTIAL : status;
}

extern "C" int __wrap_ReadIsoFileRange(ISO_FILE_READER* reader, uint64_t offset, void* buffer, size_t size)
{
    ++fallbackReads;
    const bool rootLoaderFault = fault == Fault::WinPeRootLoaderReadError || fault == Fault::WinPeRootLoaderReadCancel;
    const bool rootLoaderRead = rootLoaderFault && reader->size == legacyLoaderBytes && ++loaderReads == 2;
    if (rootLoaderRead)
    {
        if (fault == Fault::WinPeRootLoaderReadError)
            return ISO_SCAN_ERROR;
        cancelled = true;
        return ISO_SCAN_CANCELLED;
    }
    const bool rootSifFault = fault == Fault::WinPeRootSifReadError || fault == Fault::WinPeRootSifReadCancel;
    if (rootSifFault && reader->size == uint64_t(rootSifFixture.size()))
    {
        if (fault == Fault::WinPeRootSifReadError)
            return ISO_SCAN_ERROR;
        cancelled = true;
        return ISO_SCAN_CANCELLED;
    }
    const bool rootDetectorFault = fault == Fault::WinPeRootReadError || fault == Fault::WinPeRootReadCancel;
    const bool rootDetectorRead = rootDetectorFault && reader->size == sizeof("detector") - 1 && ++detectorReads == 2;
    if (rootDetectorRead)
    {
        if (fault == Fault::WinPeRootReadError)
            return ISO_SCAN_ERROR;
        cancelled = true;
        return ISO_SCAN_CANCELLED;
    }
    const bool windowsEfi = reader->size == 512;
    if (windowsEfi && fault == Fault::WindowsEfiReadError)
        return ISO_SCAN_ERROR;
    if (windowsEfi && fault == Fault::WindowsEfiCancel)
    {
        cancelled = true;
        return ISO_SCAN_CANCELLED;
    }
    if (fault == Fault::ReadError)
        return ISO_SCAN_ERROR;
    if (fault == Fault::CancelRead)
    {
        cancelled = true;
        return ISO_SCAN_CANCELLED;
    }
    return __real_ReadIsoFileRange(reader, offset, buffer, size);
}

extern "C" int __wrap_CheckIsoTree(const char* path, const ISO_EXTRACT_OPTIONS* options,
    uint64_t limit, ISO_EXTRACT_RESULT* result)
{
    extractionLimit = limit;
    splitSelected = options->split_install_wim;
    const int error = __real_CheckIsoTree(path, options, limit, result);
    if (fault == Fault::ChangeAfterTree)
    {
        QFile source(sourcePath);
        if (!source.open(QIODevice::Append) || source.write("changed") != 7)
            qFatal("Cannot alter source fixture");
    }
    if (fault == Fault::TooLargeTree)
        result->bytes_written = result->required_bytes = 65 * testMiB;
    if (fault == Fault::RootEditStageSpace)
        result->bytes_written = result->required_bytes = stagedTreeBytes;
    if (fault == Fault::LargeWimSpace)
        result->required_bytes = 65 * testMiB;
    return error;
}

class TestIsoWriteCheck : public QObject
{
    Q_OBJECT
    QTemporaryDir directory_;
    rufus::PrepareImageRequest request_;

    void put(const QString& path, const QByteArray& bytes)
    {
        QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(file.write(bytes), bytes.size());
    }

    void makeImage(const QString& name = QStringLiteral("EFI/BOOT/BOOTX64.EFI"),
        const QByteArray& header = efiHeader(), bool udf = false, bool hybrid = false,
        const QString& label = QStringLiteral("SOURCE"))
    {
        const QString tree = directory_.filePath(QStringLiteral("tree"));
        put(tree + QLatin1Char('/') + name, header);
        put(tree + QStringLiteral("/payload"), QByteArray("ordinary payload\n"));
        put(tree + QStringLiteral("/loader/entries/test.conf"), QByteArray("options LABEL=SOURCE\n"));
        sourcePath = directory_.filePath(QStringLiteral("selected.iso"));
        QStringList arguments = {QStringLiteral("-quiet"), QStringLiteral("-R"), QStringLiteral("-J"),
            QStringLiteral("-V"), label, QStringLiteral("-o"), sourcePath};
        if (udf)
            arguments.append(QStringLiteral("-udf"));
        arguments.append(tree);
        QProcess process;
        process.start(QStringLiteral(RUFUS_MKISOFS), arguments);
        QVERIFY(process.waitForFinished(testTimeoutMs));
        QCOMPARE(process.exitCode(), 0);
        if (hybrid)
        {
            QFile file(sourcePath);
            QVERIFY(file.open(QIODevice::ReadWrite));
            QVERIFY(file.seek(510));
            QCOMPARE(file.write(QByteArray::fromHex("55aa")), 2);
        }
        QCOMPARE(rufus::CapturePrepareSource(sourcePath, request_), 0);
    }

    void putWindows(const QString& kind, const QByteArray& repair = QByteArray())
    {
        const QString tree = directory_.filePath(QStringLiteral("tree"));
        QVERIFY(QDir().mkpath(tree + QStringLiteral("/sources")));
        QStringList arguments = {tree + QStringLiteral("/sources/install.wim"), kind};
        if (!repair.isNull())
        {
            const QString loader = directory_.filePath(QStringLiteral("repair-loader"));
            put(loader, repair);
            arguments.append({QStringLiteral("efi"), loader});
        }
        QProcess wim;
        wim.start(QStringLiteral(RUFUS_WIM_FIXTURE), arguments);
        QVERIFY(wim.waitForFinished(testTimeoutMs));
        QCOMPARE(wim.exitCode(), 0);
        put(tree + QStringLiteral("/bootmgr.efi"), efiHeader());
    }

    int check(rufus::IsoWriteCheck& result)
    {
        const int before = descriptors();
        const int error = rufus::CheckIsoWriteImage(request_, getuid(), result);
        if (descriptors() != before)
            qFatal("Preflight leaked a descriptor");
        return error;
    }

    void selectWindowsBios(bool enhanced, bool payload)
    {
        request_.isDevice = request_.mbr = request_.windowsBios = true;
        request_.fileSystem = FS_NTFS;
        request_.clusterSize = 4096;
        request_.enhancedInstall = enhanced;
        request_.uefiNtfs = payload;
        request_.device = 123;
        request_.diskSequence = 7;
        request_.objectPath = QStringLiteral("/org/freedesktop/UDisks2/block_devices/fixture");
        request_.serviceOwner = QStringLiteral(":1.2");
        request_.identity = {QStringLiteral("fixture")};
    }

    void selectGrub4dos(int filesystem)
    {
        request_.isDevice = request_.mbr = request_.grub4dos = true;
        request_.fileSystem = filesystem;
        request_.clusterSize = filesystem == FS_NTFS ? legacyNtfsClusterBytes
            : filesystem == FS_FAT16 ? 2048 : legacyFatClusterBytes;
        if (filesystem == FS_NTFS)
            request_.volumeId = 0;
        request_.device = 123;
        request_.diskSequence = 7;
        request_.objectPath = QStringLiteral("/org/freedesktop/UDisks2/block_devices/fixture");
        request_.serviceOwner = QStringLiteral(":1.2");
        request_.identity = {QStringLiteral("fixture")};
    }

    void putWinPe(const QString& family, const QByteArray& sif)
    {
        const QString tree = directory_.filePath(QStringLiteral("tree"));
        const QString loader = family == QStringLiteral("minint") ? family : QStringLiteral("i386");
        put(tree + QLatin1Char('/') + loader + QStringLiteral("/setupldr.bin"), QByteArray(legacyLoaderBytes, 'x'));
        put(tree + QLatin1Char('/') + loader + QStringLiteral("/ntdetect.com"), QByteArray("detector"));
        put(tree + QLatin1Char('/') + family + QStringLiteral("/txtsetup.sif"), sif);
    }

    int checkLegacy(rufus::IsoWriteCheck& result, ISO_ANALYSIS_SOURCE* view = nullptr)
    {
        QFile source(sourcePath);
        if (!source.open(QIODevice::ReadOnly))
            qFatal("Cannot open source fingerprint fixture");
        const QByteArray beforeHash = QCryptographicHash::hash(source.readAll(), QCryptographicHash::Sha256);
        const int before = descriptors();
        const int error = rufus::CheckLegacyWinPeImage(request_, getuid(), result, view);
        const int retainedDescriptors = view != nullptr && (view->iso != nullptr || view->udf != nullptr) ? 1 : 0;
        if (descriptors() != before + retainedDescriptors)
            qFatal("Legacy preflight leaked a descriptor");
        if (!source.seek(0) || QCryptographicHash::hash(source.readAll(), QCryptographicHash::Sha256) != beforeHash)
        {
            if (fault != Fault::ChangeAfterTree)
                qFatal("Legacy preflight changed the source");
        }
        return error;
    }

    void makeRepairArchive(const QString& name, const QByteArray& first,
        const QByteArray& second, int flags = 0, bool mixedCase = false, bool modernInstaller = false)
    {
        QTemporaryDir images;
        QVERIFY(images.isValid());
        WIMStruct* wim = nullptr;
        int error = wimlib_create_new_wim(WIMLIB_COMPRESSION_TYPE_LZX, &wim);
        for (int image = 1; image <= 2 && error == 0; ++image)
        {
            const QString tree = images.filePath(QString::number(image));
            const QByteArray& bytes = image == 1 ? first : second;
            QVERIFY(QDir().mkpath(tree));
            put(tree + QStringLiteral("/keep.txt"), QByteArray::number(image));
            if (!bytes.isNull())
                put(tree + (mixedCase ? QStringLiteral("/windows/boot/efi/BOOTMGFW.EFI")
                    : QStringLiteral("/Windows/Boot/EFI/bootmgfw.efi")), bytes);
            error = wimlib_add_image(wim, tree.toUtf8().constData(),
                QByteArray::number(image).constData(), nullptr, 0);
            if (error == 0 && modernInstaller)
            {
                const char* properties[] = {"WINDOWS/VERSION/MAJOR", "WINDOWS/VERSION/MINOR",
                    "WINDOWS/VERSION/BUILD", "WINDOWS/VERSION/SPBUILD"};
                const char* values[] = {"10", "0", "22000", "1"};
                for (int field = 0; field < 4 && error == 0; ++field)
                    error = wimlib_set_image_property(wim, image, properties[field], values[field]);
            }
        }
        struct wimlib_wim_info info = {};
        info.boot_index = 2; // Repair must use image 1, unlike Setup updates.
        if (error == 0)
            error = wimlib_set_wim_info(wim, &info, WIMLIB_CHANGE_BOOT_INDEX);
        const QString path = directory_.filePath(QStringLiteral("tree/") + name);
        QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
        if (error == 0)
            error = wimlib_write(wim, path.toUtf8().constData(), WIMLIB_ALL_IMAGES, flags, 1);
        wimlib_free(wim);
        QCOMPARE(error, 0);
    }

private slots:
    void initTestCase()
    {
        QCOMPARE(wimlib_global_init(WIMLIB_INIT_FLAG_ASSUME_UTF8 |
            WIMLIB_INIT_FLAG_DEFAULT_CASE_INSENSITIVE), 0);
        // Qt's bundled PCRE JIT can retain /dev/zero after its first match.
        // Initialize it before the per-preflight descriptor baseline.
        const QRegularExpression expression(QStringLiteral("[a-z]+"));
        QVERIFY(expression.match(QStringLiteral("qt")).hasMatch());
    }

    void cleanupTestCase() { wimlib_global_cleanup(); }

    void init()
    {
        QVERIFY(directory_.isValid());
        request_ = {};
        request_.size = 64 * testMiB;
        request_.label = QStringLiteral("NEW LABEL");
        // Deliberately nonexistent: preflight must not open a target.
        request_.path = directory_.filePath(QStringLiteral("do-not-open-target"));
        fault = Fault::None;
        cancelled = false;
        fallbackReads = 0;
        wimOpens = detectorReads = loaderReads = 0;
        stagedTreeBytes = 0;
        extractionLimit = UINT64_MAX;
        splitSelected = false;
    }

    void sourceArchitectures_data()
    {
        QTest::addColumn<QString>("architecture"); QTest::addColumn<int>("machine"); QTest::addColumn<bool>("wide"); QTest::addColumn<int>("defect");
        const char* names[] = {"ia32","x64","arm","aa64","ia64","riscv64","loongarch64","ebc","arm"};
        const int machines[] = {0x14c,0x8664,0x1c0,0xaa64,0x200,0x5064,0x6264,0xebc,0x1c2};
        for (int i = 0; i < 9; ++i) for (int defect = 0; defect < 3; ++defect)
            QTest::newRow(qPrintable(QStringLiteral("%1-machine-%2-defect-%3").arg(QString::fromLatin1(names[i])).arg(machines[i]).arg(defect))) << QString::fromLatin1(names[i]) << machines[i] << (i != 0 && i != 2 && i != 8) << defect;
    }
    void sourceArchitectures()
    {
        QFETCH(QString,architecture); QFETCH(int,machine); QFETCH(bool,wide); QFETCH(int,defect);
        QByteArray header = efiHeader();
        qToLittleEndian<quint16>(quint16(defect == 1 ? 0xffff : machine),header.data()+132);
        qToLittleEndian<quint16>(quint16((wide != (defect == 2)) ? 0x20b : 0x10b),header.data()+152);
        const QString path = QStringLiteral("EFI/BOOT/boot%1.efi").arg(architecture);
        makeImage(path,header);
        rufus::IsoWriteCheck result;
        QCOMPARE(rufus::CheckIsoWriteImage(request_,getuid(),result),defect == 0 ? 0 : defect == 1 ? ENOTSUP : EBADMSG);
        if (!defect) QCOMPARE(result.efiPath,'/'+path);
    }

    void cleanup()
    {
        QVERIFY(!QFile::exists(request_.path));
        cancelled = false;
        QVERIFY(QDir(directory_.filePath(QStringLiteral("tree"))).removeRecursively());
        QFile::remove(sourcePath);
    }

    void legacyWinPe_data()
    {
        QTest::addColumn<QString>("family");
        QTest::addColumn<bool>("udf");
        QTest::addColumn<bool>("fat32");
        QTest::addColumn<bool>("enhanced");
        const char* families[] = {"i386", "amd64", "minint", "mixed", "mixed-case",
            "absent-options", "i386-minint", "small-loader"};
        for (const char* family : families)
        {
            for (bool fat32 : {false, true})
            for (bool enhanced : {false, true})
            {
                if (fat32 && !enhanced) continue;
                const QByteArray name = QByteArray(family) + (fat32 ? "-fat32" : (enhanced ? "-ntfs-alt-e" : "-ntfs"));
                QTest::newRow((name + "-iso").constData()) << QString::fromLatin1(family) << false << fat32 << enhanced;
                QTest::newRow((name + "-udf").constData()) << QString::fromLatin1(family) << true << fat32 << enhanced;
            }
        }
    }

    void legacyWinPe()
    {
        QFETCH(QString, family);
        QFETCH(bool, udf);
        QFETCH(bool, fat32);
        QFETCH(bool, enhanced);
        const bool minint = family == QStringLiteral("minint") || family == QStringLiteral("i386-minint");
        QByteArray sif = minint ? QByteArray("[SetupData]\r\nOsLoadOptions=/minint\r\n")
            : QByteArray("[SetupData]\r\nOsLoadOptions=/fastdetect\r\n");
        if (family == QStringLiteral("absent-options")) sif = "[SetupData]\r\na=b\r\n";
        const QString tree = directory_.filePath(QStringLiteral("tree"));
        if (family == QStringLiteral("mixed"))
        {
            putWinPe(QStringLiteral("i386"), sif); // Original scan's options source.
            putWinPe(QStringLiteral("amd64"), QByteArray("[SetupData]\r\nOsLoadOptions=/minint\r\n"));
        }
        else if (family == QStringLiteral("amd64") || family == QStringLiteral("minint"))
            putWinPe(family, sif);
        else
            putWinPe(QStringLiteral("i386"), sif);
        const bool smallLoader = family == QStringLiteral("small-loader");
        if (smallLoader) put(tree + QStringLiteral("/i386/setupldr.bin"), QByteArray(33, 'x'));
        if (minint) put(tree + QStringLiteral("/TXTSETUP.SIF"), "existing unused root SIF");
        if (family == QStringLiteral("mixed-case"))
        {
            QVERIFY(QFile::rename(tree + QStringLiteral("/i386/setupldr.bin"), tree + QStringLiteral("/i386/SeTuPlDr.BiN")));
            QVERIFY(QFile::rename(tree + QStringLiteral("/i386/ntdetect.com"), tree + QStringLiteral("/i386/NtDeTeCt.CoM")));
            QVERIFY(QFile::rename(tree + QStringLiteral("/i386/txtsetup.sif"), tree + QStringLiteral("/i386/TxTsEtUp.SiF")));
            QVERIFY(QDir(tree).rename(QStringLiteral("i386"), QStringLiteral("I386")));
        }
        makeImage(QStringLiteral("marker"), QByteArray("image marker"), udf);
        selectWindowsBios(enhanced, false);
        if (fat32)
        {
            request_.fileSystem = FS_FAT32;
            request_.clusterSize = 512;
        }
        rufus::IsoWriteCheck result;
        ISO_ANALYSIS_SOURCE view = {};
        QCOMPARE(checkLegacy(result, &view), 0);
        QVERIFY(result.legacyWinPe);
        QCOMPARE(result.windows.state, WINDOWS_IMAGE_UNKNOWN);
        QCOMPARE(result.analysis.report.win_version.major, uint32_t(0));
        QVERIFY(result.analysis.report.winpe_options_known);
        QCOMPARE(bool(result.analysis.report.uses_minint), minint);
        QCOMPARE(result.extraPartitions, quint8(0));
        QVERIFY(result.windowsInstallPath.isEmpty());
        QCOMPARE(wimOpens, 0);
        const QByteArray copiedSif = family == QStringLiteral("mixed")
            ? QByteArray("[SetupData]\r\nOsLoadOptions=/minint\r\n") : sif;
        const quint64 sifBytes = minint ? 0 : quint64(copiedSif.size())
            + sizeof("SetupSourceDevice = \"\\device\\harddisk1\\partition1\"\r\n") - 1;
        QCOMPARE(result.tree.required_bytes - result.tree.bytes_written, quint64((smallLoader ? 33 : 9000) + 8) + sifBytes);
        QVERIFY(!rufus::IsWindowsBiosImage(result.analysis, result.windows, true));
        QVERIFY(rufus::IsLegacyWinPeImage(result.analysis));
        QVERIFY(rufus::IsWindowsBiosImage(result.analysis, result.windows, true, true));
        QVERIFY(view.iso != nullptr || view.udf != nullptr);
        CloseIsoAnalysisSource(&view);
        QCOMPARE(check(result), ENOTSUP); // Default/GUI preflight remains gated.
        QVERIFY(!result.legacyWinPe);
        QCOMPARE(rufus::CheckIsoWriteImage(request_, getuid(), result, nullptr, true), 0);
        QVERIFY(result.legacyWinPe); // Backend opt-in derives the same accepted source.
    }


    void legacyGrub2Sbr_data()
    {
        QTest::addColumn<QString>("mode");
        QTest::addColumn<bool>("udf");
        QTest::addColumn<bool>("rufusMbr");
        QTest::addColumn<bool>("fat32");
        for (const char* mode : {"selected", "both", "no-core", "no-grub", "syslinux", "grub4dos", "patch"})
        for (bool fat32 : {false, true})
        for (bool udf : {false, true})
        for (bool rufusMbr : {false, true})
            QTest::newRow(qPrintable(QStringLiteral("%1-%2-%3-%4").arg(QString::fromLatin1(mode)).arg(udf).arg(rufusMbr).arg(fat32)))
                << QString::fromLatin1(mode) << udf << rufusMbr << fat32;
    }

    void legacyGrub2Sbr()
    {
        QFETCH(QString, mode); QFETCH(bool, udf); QFETCH(bool, rufusMbr); QFETCH(bool, fat32);
        const bool patch = mode == QStringLiteral("patch");
        putWinPe(patch ? QStringLiteral("i386") : QStringLiteral("minint"),
            patch ? "[SetupData]\r\nOsLoadOptions=/fastdetect\r\n" : "[SetupData]\r\nOsLoadOptions=/minint\r\n");
        const QString tree = directory_.filePath(QStringLiteral("tree"));
        const bool hasGrub2 = mode != QStringLiteral("grub4dos") && mode != QStringLiteral("no-grub");
        if (hasGrub2)
        {
            QByteArray module(4096, '\0');
            const char banner[] = "GRUB  version %s\0" "2.12";
            module.replace(64, sizeof(banner), QByteArray(banner, sizeof(banner)));
            put(tree + QStringLiteral("/boot/grub/i386-pc/normal.mod"), module);
            put(tree + QStringLiteral("/boot/grub/grub.cfg"), "linux /vmlinuz LABEL=SOURCE\r\n");
        }
        if (mode == QStringLiteral("both") || mode == QStringLiteral("grub4dos"))
            put(tree + QStringLiteral("/grldr"), "unchanged GRUB4DOS data");
        if (mode == QStringLiteral("syslinux"))
            put(tree + QStringLiteral("/isolinux/isolinux.cfg"), "DEFAULT linux\r\n");
        makeImage(QStringLiteral("marker"), "image marker", udf);
        selectWindowsBios(true, false);
        request_.windowsRufusMbr = rufusMbr;
        if (fat32)
        {
            request_.fileSystem = FS_FAT32;
            request_.clusterSize = legacyFatClusterBytes;
        }
        if (mode != QStringLiteral("no-core"))
        {
            const QByteArray core(700, '\x17');
            request_.grub2CorePath = directory_.filePath(QStringLiteral("core.img"));
            put(request_.grub2CorePath, core);
            request_.grub2CoreSize = quint64(core.size());
            request_.grub2CoreSha256 = QCryptographicHash::hash(core, QCryptographicHash::Sha256);
        }
        rufus::IsoWriteCheck checked;
        const bool accepted = mode == QStringLiteral("selected") || mode == QStringLiteral("both")
            || mode == QStringLiteral("grub4dos") || patch;
        QCOMPARE(checkLegacy(checked), accepted ? 0 : ENOTSUP);
        QVERIFY(!QFile::exists(request_.path));
        if (!accepted) return;
        const RUFUS_IMG_REPORT& report = checked.analysis.report;
        QVERIFY(checked.legacyWinPe && HAS_WINPE(report) && bool(report.uses_minint) == !patch && !HAS_SYSLINUX(report));
        QCOMPARE(QString::fromLatin1(report.grub2_version), hasGrub2 ? QStringLiteral("2.12") : QString());
        QCOMPARE(checked.windows.state, WINDOWS_IMAGE_UNKNOWN);
        QCOMPARE(GetSecondaryBootType(BT_IMAGE, &report, false), hasGrub2 ? int(BT_GRUB2) : int(BT_GRUB4DOS));
        const RUFUS_BOOT_OPTIONS boot = {BT_IMAGE, request_.fileSystem, TT_BIOS, false, true, rufusMbr, false};
        QCOMPARE(GetMasterBootRecord(&report, &boot), rufusMbr ? RUFUS_MBR_RUFUS : RUFUS_MBR_WIN7);
        QCOMPARE(GetPartitionBootRecord(BT_IMAGE, request_.fileSystem, &report), fat32 ? RUFUS_PBR_FAT32_NT : RUFUS_PBR_NTFS);
        QVERIFY(!rufus::IsLegacyWinPeImage(checked.analysis, true)); // Qt remains gated.
        QVERIFY(rufus::IsLegacyWinPeImage(checked.analysis, true, true, true));
        QCOMPARE(wimOpens, 0);
        QCOMPARE(check(checked), ENOTSUP);
        QCOMPARE(rufus::CheckIsoWriteImage(request_, getuid(), checked, nullptr, true), 0);
    }

    void legacyGrub4dosSbr_data()
    {
        QTest::addColumn<QString>("mode"); QTest::addColumn<bool>("udf");
        QTest::addColumn<bool>("fat32"); QTest::addColumn<bool>("rufusMbr");
        for (const char* mode : {"keep", "patch", "alt-e-off", "wrong-core", "syslinux", "uefi"})
        for (bool udf : {false, true})
        for (bool fat32 : {false, true})
        for (bool rufusMbr : {false, true})
            QTest::newRow(qPrintable(QStringLiteral("%1-%2-%3-%4").arg(mode).arg(udf).arg(fat32).arg(rufusMbr)))
                << QString::fromLatin1(mode) << udf << fat32 << rufusMbr;
    }

    void legacyGrub4dosSbr()
    {
        QFETCH(QString, mode); QFETCH(bool, udf); QFETCH(bool, fat32); QFETCH(bool, rufusMbr);
        const bool patch = mode == QStringLiteral("patch");
        putWinPe(patch ? QStringLiteral("i386") : QStringLiteral("minint"),
            patch ? "[SetupData]\r\nOsLoadOptions=/fastdetect\r\n" : "[SetupData]\r\nOsLoadOptions=/minint\r\n");
        const QString tree = directory_.filePath(QStringLiteral("tree"));
        const QByteArray loader("Original unversioned image loader");
        put(tree + QStringLiteral("/grldr"), loader);
        const bool syslinux = mode == QStringLiteral("syslinux");
        if (syslinux) put(tree + QStringLiteral("/isolinux/isolinux.cfg"), "DEFAULT linux\r\n");
        makeImage(QStringLiteral("marker"), "image marker", udf);
        selectWindowsBios(mode != QStringLiteral("alt-e-off"), mode == QStringLiteral("uefi"));
        request_.windowsRufusMbr = rufusMbr;
        if (fat32) { request_.fileSystem = FS_FAT32; request_.clusterSize = legacyFatClusterBytes; }
        if (mode == QStringLiteral("wrong-core"))
        {
            const QByteArray core(700, '\x17');
            request_.grub2CorePath = directory_.filePath(QStringLiteral("core.img"));
            put(request_.grub2CorePath, core); request_.grub2CoreSize = quint64(core.size());
            request_.grub2CoreSha256 = QCryptographicHash::hash(core, QCryptographicHash::Sha256);
        }
        const bool accepted = mode == QStringLiteral("keep") || patch || syslinux || mode == QStringLiteral("wrong-core");
        rufus::IsoWriteCheck checked;
        const bool invalidRequest = mode == QStringLiteral("uefi") && fat32;
        QCOMPARE(checkLegacy(checked), accepted ? 0 : invalidRequest ? EINVAL : ENOTSUP);
        QVERIFY(!QFile::exists(request_.path));
        if (!accepted) return;
        QVERIFY(checked.legacyWinPe && checked.analysis.report.has_grub4dos);
        QVERIFY(!checked.analysis.report.has_grub2);
        QCOMPARE(checked.windows.state, WINDOWS_IMAGE_UNKNOWN);
        QCOMPARE(GetSecondaryBootType(BT_IMAGE, &checked.analysis.report, false), int(syslinux ? BT_IMAGE : BT_GRUB4DOS));
        QCOMPARE(rufus::IsLegacyWinPeImage(checked.analysis, true, true), syslinux); // Qt gate.
        QVERIFY(rufus::IsLegacyWinPeImage(checked.analysis, true, true, true));
        QVERIFY(!rufus::IsLegacyWinPeImage(checked.analysis, false, true, true));
        QCOMPARE(wimOpens, 0);
        QCOMPARE(check(checked), ENOTSUP);
        QCOMPARE(rufus::CheckIsoWriteImage(request_, getuid(), checked, nullptr, true), 0);
    }

    void legacyGrub2Source_data()
    {
        QTest::addColumn<QString>("mode");
        QTest::addColumn<bool>("udf");
        QTest::addColumn<bool>("rufusMbr");
        QTest::addColumn<bool>("fat32");
        for (const char* mode : {"unselected", "selected", "both", "unknown-version", "patch", "patch-amd64", "patch-minint-options", "patch-root-prepared",
            "patch-root-mismatch", "patch-options-missing", "patch-short-loader", "patch-partial-minint",
            "patch-root-sif", "patch-root-sif-invalid",
            "root-ntldr", "root-io.sys", "root-msdos.sys", "root-loader", "alt-e-off", "efi", "customized", "unknown-options", "missing-detector",
            "root-mismatch", "pending", "source-change", "read-error", "cancel", "bad-core",
            "syslinux-core", "grub4dos-core", "no-grub-core"})
        for (bool fat32 : {false, true})
        for (bool udf : {false, true})
        for (bool rufusMbr : {false, true})
            QTest::newRow(qPrintable(QStringLiteral("%1-%2-%3-%4").arg(QString::fromLatin1(mode)).arg(udf).arg(rufusMbr).arg(fat32)))
                << QString::fromLatin1(mode) << udf << rufusMbr << fat32;
    }

    void legacyGrub2Source()
    {
        QFETCH(QString, mode); QFETCH(bool, udf); QFETCH(bool, rufusMbr); QFETCH(bool, fat32);
        const bool patch = mode.startsWith(QStringLiteral("patch"));
        const bool minintOptions = mode == QStringLiteral("patch-minint-options");
        const bool unknownOptions = mode == QStringLiteral("unknown-options");
        const QByteArray sif = unknownOptions ? QByteArray("[SetupData]\r\nUnrecognized=value\r\n")
            : patch && !minintOptions ? QByteArray("[SetupData]\r\nOsLoadOptions=/fastdetect\r\n")
            : QByteArray("[SetupData]\r\nOsLoadOptions=/minint\r\n");
        const QString family = mode == QStringLiteral("patch-amd64") ? QStringLiteral("amd64")
            : patch ? QStringLiteral("i386") : QStringLiteral("minint");
        putWinPe(family, sif);
        const QString tree = directory_.filePath(QStringLiteral("tree"));
        if (patch)
        {
            constexpr int pathOffset = 128, diskOffset = 256, crcOffset = 0x2060, diskNumberOffset = 6;
            QByteArray loader(legacyLoaderBytes, 'x');
            loader.replace(pathOffset, sizeof("\\minint\\txtsetup.sif"), QByteArray("\\minint\\txtsetup.sif", sizeof("\\minint\\txtsetup.sif")));
            loader.replace(diskOffset, sizeof("rdisk(0)"), QByteArray("rdisk(0)", sizeof("rdisk(0)")));
            loader.replace(crcOffset, 2, QByteArray::fromHex("7403"));
            const QString loaderPath = tree + QStringLiteral("/i386/setupldr.bin");
            put(loaderPath, mode == QStringLiteral("patch-short-loader") ? QByteArray("short") : loader);
            if (mode == QStringLiteral("patch-root-prepared"))
            {
                loader.replace(pathOffset, sizeof("\\i386\\txtsetup.sif"), QByteArray("\\i386\\txtsetup.sif", sizeof("\\i386\\txtsetup.sif")));
                loader[diskOffset + diskNumberOffset] = '1';
                loader.replace(crcOffset, 2, QByteArray::fromHex("eb1a"));
                put(tree + QStringLiteral("/BoOtMgR"), loader);
            }
            if (mode == QStringLiteral("patch-root-mismatch"))
                put(tree + QStringLiteral("/BoOtMgR"), QByteArray(legacyLoaderBytes, 'y'));
            if (mode == QStringLiteral("patch-options-missing"))
                put(tree + QLatin1Char('/') + family + QStringLiteral("/txtsetup.sif"), "[SetupData]\r\nOther=value\r\n");
            if (mode == QStringLiteral("patch-partial-minint"))
                put(tree + QStringLiteral("/minint/txtsetup.sif"), "unused SIF");
            if (mode == QStringLiteral("patch-root-sif") || mode == QStringLiteral("patch-root-sif-invalid"))
                put(tree + QStringLiteral("/TxTsEtUp.SiF"), mode.endsWith(QStringLiteral("invalid"))
                    ? QByteArray("[Other]\r\nKeep=yes\r\n") : QByteArray(";root-only\r\n[SetupData]\r\nKeep=yes\r\n"));
        }
        QByteArray module(4096, '\0');
        const char banner[] = "GRUB  version %s\0" "2.12";
        module.replace(64, sizeof(banner), QByteArray(banner, sizeof(banner)));
        const bool hasGrub2 = mode != QStringLiteral("grub4dos-core") && mode != QStringLiteral("no-grub-core");
        if (hasGrub2)
        {
            put(tree + QStringLiteral("/boot/grub/i386-pc/normal.mod"), module);
            put(tree + QStringLiteral("/boot/grub/grub.cfg"), "linux /vmlinuz LABEL=SOURCE\r\n");
        }
        if (mode == QStringLiteral("both") || mode == QStringLiteral("grub4dos-core"))
            put(tree + QStringLiteral("/grldr"), "unchanged GRUB4DOS data");
        if (mode == QStringLiteral("syslinux-core"))
            put(tree + QStringLiteral("/isolinux/isolinux.cfg"), "DEFAULT linux\r\n");
        if (mode == QStringLiteral("missing-detector"))
            QVERIFY(QFile::remove(tree + QStringLiteral("/minint/ntdetect.com")));
        if (mode == QStringLiteral("root-mismatch"))
            put(tree + QStringLiteral("/BOOTMGR"), QByteArray(legacyLoaderBytes, 'y'));
        if (mode == QStringLiteral("efi"))
            put(tree + QStringLiteral("/EFI/BOOT/BOOTX64.EFI"), efiHeader());
        if (mode.startsWith(QStringLiteral("root-")) && mode != QStringLiteral("root-mismatch"))
        {
            const QString name = mode == QStringLiteral("root-loader") ? QStringLiteral("BOOTMGR") : mode.mid(5);
            put(tree + QLatin1Char('/') + name, mode == QStringLiteral("root-loader")
                ? QByteArray(legacyLoaderBytes, 'x') : QByteArray("legacy loader"));
        }
        makeImage(QStringLiteral("marker"), "image marker", udf);
        selectWindowsBios(mode != QStringLiteral("alt-e-off"), false);
        request_.windowsRufusMbr = rufusMbr;
        if (fat32)
        {
            request_.fileSystem = FS_FAT32;
            request_.clusterSize = legacyFatClusterBytes;
        }

        if (mode == QStringLiteral("customized"))
            request_.windowsFlags = UNATTEND_NO_DATA_COLLECTION;
        const bool suppliedCore = mode == QStringLiteral("selected") || mode.endsWith(QStringLiteral("-core"));
        if (suppliedCore)
        {
            const QByteArray core(700, '\x17');
            request_.grub2CorePath = directory_.filePath(QStringLiteral("core.img"));
            put(request_.grub2CorePath, core);
            request_.grub2CoreSize = quint64(core.size());
            request_.grub2CoreSha256 = mode == QStringLiteral("bad-core") ? QByteArray("invalid")
                : QCryptographicHash::hash(core, QCryptographicHash::Sha256);
        }
        if (mode == QStringLiteral("unknown-version")) fault = Fault::UnknownGrub;
        if (mode == QStringLiteral("pending")) fault = Fault::ContentPending;
        if (mode == QStringLiteral("source-change")) fault = Fault::ChangeAfterTree;
        if (mode == QStringLiteral("read-error")) fault = Fault::ReadError;
        if (mode == QStringLiteral("cancel")) fault = Fault::CancelRead;
        QFile source(sourcePath);
        QVERIFY(source.open(QIODevice::ReadOnly));
        const QByteArray before = QCryptographicHash::hash(source.readAll(), QCryptographicHash::Sha256);
        const int openDescriptors = descriptors();
        ISO_ANALYSIS_SOURCE view = {};
        rufus::IsoWriteCheck checked;
        const int error = rufus::CheckIsoWriteSource(request_, getuid(), checked, &view, true);
        const bool accepted = mode == QStringLiteral("unselected") || mode == QStringLiteral("selected")
            || mode == QStringLiteral("both") || mode == QStringLiteral("unknown-version")
            || mode == QStringLiteral("grub4dos-core") || (fat32 && mode == QStringLiteral("efi"))
            || mode == QStringLiteral("patch") || mode == QStringLiteral("patch-amd64")
            || minintOptions || mode == QStringLiteral("patch-root-prepared") || mode == QStringLiteral("patch-root-sif")
            || mode == QStringLiteral("patch-options-missing")
            || mode == QStringLiteral("root-loader") || (mode.startsWith(QStringLiteral("root-"))
                && mode != QStringLiteral("root-mismatch"));
        const bool retained = view.iso != nullptr || view.udf != nullptr;
        QCOMPARE(descriptors(), openDescriptors + int(retained));
        QVERIFY(!QFile::exists(request_.path)); // No simulated target is opened or created.
        QCOMPARE(retained, accepted);
        if (retained)
        {
            rufus::IsoWriteCheck repeated;
            QCOMPARE(rufus::CheckIsoWriteSource(request_, getuid(), repeated, &view, true), EINVAL);
            QCOMPARE(descriptors(), openDescriptors + 1); // Do not replace the caller's live view.
        }
        CloseIsoAnalysisSource(&view);
        QCOMPARE(descriptors(), openDescriptors);
        QVERIFY(source.seek(0));
        const bool unchanged = before == QCryptographicHash::hash(source.readAll(), QCryptographicHash::Sha256);
        QCOMPARE(unchanged, mode != QStringLiteral("source-change"));
        if (!accepted)
        {
            QVERIFY2(error != 0, qPrintable(mode));
            QVERIFY(!checked.legacyWinPe && !checked.problem.isEmpty());
            if (mode == QStringLiteral("source-change")) QCOMPARE(error, ESTALE);
            if (mode == QStringLiteral("cancel")) QCOMPARE(error, ECANCELED);
            if (mode == QStringLiteral("bad-core")) QCOMPARE(error, EINVAL);
            return;
        }
        QCOMPARE(error, 0);
        QVERIFY(checked.legacyWinPe && checked.analysis.tree_complete);
        QCOMPARE(checked.windows.state, WINDOWS_IMAGE_UNKNOWN);
        QCOMPARE(GetSecondaryBootType(BT_IMAGE, &checked.analysis.report, false), hasGrub2 ? int(BT_GRUB2) : int(BT_GRUB4DOS));
        const QString expectedVersion = mode == QStringLiteral("unknown-version") || !hasGrub2 ? QString() : QStringLiteral("2.12");
        QCOMPARE(QString::fromLatin1(checked.analysis.report.grub2_version), expectedVersion);
        QCOMPARE(request_.hasGrub2Core(), suppliedCore); // Source checks never invent a payload.
        QCOMPARE(wimOpens, 0);
        QCOMPARE(rufus::CheckIsoWriteImage(request_, getuid(), checked, nullptr, true), suppliedCore ? 0 : ENOTSUP);
        QCOMPARE(rufus::CheckIsoWriteSource(request_, getuid(), checked), ENOTSUP); // Legacy remains opt-in.
        QCOMPARE(rufus::CheckIsoWriteSource(request_, getuid() + 1, checked, nullptr, true), EACCES);
    }

    void legacySyslinuxExtraction_data()
    {
        QTest::addColumn<QString>("family");
        QTest::addColumn<bool>("udf");
        QTest::addColumn<int>("rootConfig");
        QTest::addColumn<bool>("rufusMbr");
        QTest::addColumn<bool>("fat32");
        QTest::addColumn<int>("grubData");
        for (const char* family : {"i386", "amd64", "minint"})
        for (bool udf : {false, true})
        for (int rootConfig : {NoRootConfig, KeepRootConfig, ReplaceRootConfig})
        for (bool rufusMbr : {false, true})
        for (bool fat32 : {false, true})
        {
            const QByteArray name = QByteArray(family) + (udf ? "-udf-" : "-iso-")
                + QByteArray::number(rootConfig) + (rufusMbr ? "-rufus" : "-win7") + (fat32 ? "-fat32" : "-ntfs");
            QTest::newRow(name.constData()) << QString::fromLatin1(family) << udf << rootConfig << rufusMbr << fat32 << int(NoGrubData);
        }
        for (const char* family : {"i386", "minint"})
        for (bool udf : {false, true})
        for (bool fat32 : {false, true})
        for (int grubData : {Grub2Data, Grub4DosData, BothGrubData})
            QTest::newRow(qPrintable(QStringLiteral("%1-%2-%3-grub-%4").arg(QString::fromLatin1(family)).arg(udf).arg(fat32).arg(grubData)))
                << QString::fromLatin1(family) << udf << int(ReplaceRootConfig) << false << fat32 << grubData;
    }

    void legacySyslinuxExtraction()
    {
        QFETCH(QString, family);
        QFETCH(bool, udf);
        QFETCH(int, rootConfig);
        QFETCH(bool, rufusMbr);
        QFETCH(bool, fat32);
        QFETCH(int, grubData);
        constexpr int pathOffset = 16, driveOffset = 64, crcOffset = 0x2060;
        const bool minint = family == QStringLiteral("minint");
        const QByteArray sif = minint ? QByteArray("[SetupData]\r\nOsLoadOptions=/minint\r\n")
            : QByteArray("[SetupData]\r\nOsLoadOptions=/fastdetect\r\n");
        putWinPe(family, sif);
        const QString tree = directory_.filePath(QStringLiteral("tree"));
        const QString loaderDirectory = minint ? family : QStringLiteral("i386");
        QByteArray loader(legacyLoaderBytes, 'x');
        const QByteArray originalPath = QByteArray("\\minint\\txtsetup.sif") + char(0);
        loader.replace(pathOffset, originalPath.size(), originalPath);
        loader.replace(driveOffset, 9, QByteArray("rdisk(0)\0", 9));
        loader[crcOffset] = 0x74;
        loader[crcOffset + 1] = 0x03;
        put(tree + '/' + loaderDirectory + QStringLiteral("/setupldr.bin"), loader);
        const bool existingRootLoader = rootConfig == KeepRootConfig;
        if (existingRootLoader)
            put(tree + QStringLiteral("/BoOtMgR"), loader);
        const QString configPath = rootConfig == ReplaceRootConfig
            ? QStringLiteral("boot/i386/loader/isolinux.cfg") : QStringLiteral("isolinux/isolinux.cfg");
        put(tree + '/' + configPath, "DEFAULT test\r\nappend LABEL=SOURCE\r\n");
        if (rootConfig != NoRootConfig)
            put(tree + QStringLiteral("/syslinux.cfg"), "DEFAULT root\r\nappend LABEL=SOURCE\r\n");
        if (grubData & Grub2Data)
        {
            QByteArray module(4096, '\0');
            const char banner[] = "GRUB  version %s\0" "2.12";
            module.replace(64, sizeof(banner), QByteArray(banner, sizeof(banner)));
            put(tree + QStringLiteral("/boot/grub/i386-pc/normal.mod"), module);
            put(tree + QStringLiteral("/boot/grub/grub.cfg"), "linux /vmlinuz LABEL=SOURCE\r\n");
        }
        if (grubData & Grub4DosData)
        {
            put(tree + QStringLiteral("/grldr"), "original GRUB4DOS loader data");
            put(tree + QStringLiteral("/menu.lst"), "kernel /vmlinuz LABEL=SOURCE\r\n");
        }
        makeImage(QStringLiteral("marker"), "image marker", udf);
        selectWindowsBios(true, false);
        request_.fileSystem = fat32 ? FS_FAT32 : FS_NTFS;
        request_.clusterSize = fat32 ? legacyFatClusterBytes : legacyNtfsClusterBytes;
        request_.windowsRufusMbr = rufusMbr;
        rufus::IsoWriteCheck result;
        const int checked = checkLegacy(result);
        QVERIFY2(checked == 0, qPrintable(result.problem));
        QVERIFY(result.legacyWinPe && !request_.syslinux && !result.partition.started);
        const RUFUS_IMG_REPORT& report = result.analysis.report;
        QVERIFY(HAS_WINPE(report) && HAS_SYSLINUX(report));
        QCOMPARE(bool(report.has_grub2), bool(grubData & Grub2Data));
        QCOMPARE(bool(report.has_grub4dos), bool(grubData & Grub4DosData));
        if (grubData & Grub2Data) QCOMPARE(QString::fromLatin1(report.grub2_version), QStringLiteral("2.12"));
        QCOMPARE(report.sl_detected_version, quint16(0));
        QCOMPARE(report.sl_version, quint16(0));
        QVERIFY(!report.sl_version_inferred); // No installer is selected for unknown Syslinux data.
        QCOMPARE(result.windows.state, WINDOWS_IMAGE_UNKNOWN);
        QCOMPARE(wimOpens, 0);
        QVERIFY(!rufus::IsLegacyWinPeImage(result.analysis)); // Ordinary admission retains the mixed-loader gate.
        QVERIFY(rufus::IsLegacyWinPeImage(result.analysis, true));
        QVERIFY(!rufus::IsWindowsBiosImage(result.analysis, result.windows, true, true));
        QVERIFY(rufus::IsWindowsBiosImage(result.analysis, result.windows, true, true, true));
        RUFUS_BOOT_OPTIONS boot = {BT_IMAGE, request_.fileSystem, TT_BIOS, false, true, rufusMbr, false};
        QCOMPARE(GetMasterBootRecord(&report, &boot), rufusMbr ? RUFUS_MBR_RUFUS : RUFUS_MBR_WIN7);
        QCOMPARE(GetBootInstallType(&report, &boot), RUFUS_BOOT_INSTALL_PBR);
        QCOMPARE(GetSecondaryBootType(BT_IMAGE, &report, false), int(BT_IMAGE));
        QCOMPARE(bool(HAS_BOOTMGR_BIOS(report)), existingRootLoader);
        const rufus_pbr_type expectedPbr = fat32
            ? (existingRootLoader ? RUFUS_PBR_FAT32_PE : RUFUS_PBR_FAT32_NT) : RUFUS_PBR_NTFS;
        QCOMPARE(GetPartitionBootRecord(BT_IMAGE, request_.fileSystem, &report), expectedPbr);
        boot.needs_masquerading = !minint;
        QCOMPARE(GetMasterBootRecord(&report, &boot), (!minint || rufusMbr) ? RUFUS_MBR_RUFUS : RUFUS_MBR_WIN7);
        boot.needs_masquerading = false;
        boot.allow_dual_uefi_bios = false;
        QCOMPARE(GetMasterBootRecord(&report, &boot), RUFUS_MBR_SYSLINUX);
        QCOMPARE(GetBootInstallType(&report, &boot), RUFUS_BOOT_INSTALL_SYSLINUX);

        const QMap<QString, QByteArray> sourceFiles = fileTree(tree);
        QMap<QString, QByteArray> expected = sourceFiles;
        expected[configPath] = "DEFAULT test\nappend LABEL=NEW\\x20LABEL\n";
        if (grubData & Grub2Data) expected[QStringLiteral("boot/grub/grub.cfg")] = "linux /vmlinuz LABEL=NEW\\x20LABEL\n";
        const QByteArray rootText("DEFAULT root\nappend LABEL=NEW\\x20LABEL\n");
        if (rootConfig == KeepRootConfig)
            expected[QStringLiteral("syslinux.cfg")] = rootText;
        else
        {
            if (rootConfig == ReplaceRootConfig)
                expected[QStringLiteral("syslinux.org")] = rootText;
            expected[QStringLiteral("syslinux.cfg")] = "DEFAULT loadconfig\n\nLABEL loadconfig\n  CONFIG /"
                + configPath.toUtf8() + "\n  APPEND /" + QFileInfo(configPath).path().toUtf8() + "/\n";
        }
        expected[QStringLiteral("loader/entries/test.conf")] = "options LABEL=NEW\\x20LABEL\n";
        expected[QStringLiteral("ntdetect.com")] = "detector";
        if (!minint)
        {
            loader[crcOffset] = char(0xeb);
            loader[crcOffset + 1] = 0x1a;
            const QByteArray path = "\\" + family.toUtf8() + "\\txtsetup.sif" + char(0);
            loader.replace(pathOffset, path.size(), path);
            loader[driveOffset + 6] = '1';
            expected[QStringLiteral("TXTSETUP.SIF")] = "[SetupData]\r\n"
                "SetupSourceDevice = \"\\device\\harddisk1\\partition1\"\r\nOsLoadOptions=/fastdetect\r\n";
        }
        expected[existingRootLoader ? QStringLiteral("BoOtMgR") : QStringLiteral("BOOTMGR")] = loader;
        QTemporaryDir destination;
        QVERIFY(destination.isValid());
        const QByteArray target = QFile::encodeName(destination.path());
        const QByteArray image = QFile::encodeName(request_.imagePath);
        const QByteArray label = request_.label.toUtf8();
        const ISO_EXTRACT_OPTIONS options = {{true, true, {0, 0}}, label.constData(), image.constData(), false};
        ISO_EXTRACT_RESULT extracted = {};
        QCOMPARE(ExtractIsoTree(image.constData(), target.constData(), &options, &extracted), 0);
        const int root = ::open(target.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        QVERIFY(root >= 0);
        const int error = rufus::PrepareWinPeRootFiles(root, report.winpe, report.uses_minint, report.winpe_options_known);
        const int closed = ::close(root);
        QCOMPARE(error, 0);
        QCOMPARE(closed, 0);
        const QMap<QString, QByteArray> actual = fileTree(destination.path());
        QCOMPARE(actual.keys(), expected.keys());
        for (auto item = expected.cbegin(); item != expected.cend(); ++item)
            QVERIFY2(actual.value(item.key()) == item.value(), qPrintable(item.key()));
        QCOMPARE(fileTree(tree), sourceFiles);
        QVERIFY(!QFile::exists(destination.filePath(QStringLiteral("ldlinux.sys"))));
        QVERIFY(!QFile::exists(request_.path)); // No device/authorization in this functional test.
        rufus::IsoWriteCheck backend;
        QCOMPARE(rufus::CheckIsoWriteImage(request_, getuid(), backend, nullptr, true), 0);
        QVERIFY(backend.legacyWinPe);
        QCOMPARE(check(backend), ENOTSUP); // Unopted callers remain gated.
    }

    void legacySyslinuxRefusal_data()
    {
        QTest::addColumn<QString>("change");
        QTest::addColumn<bool>("udf");
        QTest::addColumn<bool>("fat32");
        for (const char* change : {"alt-e-off", "uefi-ntfs", "grub-sbr", "grub4dos-sbr", "both-grub-sbr",
            "fatless-grub", "ntfsless-grub", "reactos", "kolibri",
            "efi", "customized", "bad-config", "symlink", "partial"})
        for (bool udf : {false, true})
        for (bool fat32 : {false, true})
        {
            if (strcmp(change, "fatless-grub") == 0 && !fat32) continue;
            if (strcmp(change, "ntfsless-grub") == 0 && fat32) continue;
            const QByteArray name = QByteArray(change) + (udf ? "-udf" : "-iso") + (fat32 ? "-fat32" : "-ntfs");
            QTest::newRow(name.constData()) << QString::fromLatin1(change) << udf << fat32;
        }
    }

    void legacySyslinuxRefusal()
    {
        QFETCH(QString, change);
        QFETCH(bool, udf);
        QFETCH(bool, fat32);
        const QString tree = directory_.filePath(QStringLiteral("tree"));
        putWinPe(QStringLiteral("i386"), "[SetupData]\r\nOsLoadOptions=/fastdetect\r\n");
        QByteArray config("append LABEL=SOURCE\r\n");
        if (change == QStringLiteral("bad-config")) config.append('\0');
        const bool grubSbr = change.endsWith(QStringLiteral("-sbr"));
        if (!grubSbr) put(tree + QStringLiteral("/isolinux/isolinux.cfg"), config);
        if (grubSbr)
        {
            if (change != QStringLiteral("grub4dos-sbr"))
            {
                const char banner[] = "GRUB  version %s\0" "2.12";
                QByteArray module(4096, '\0');
                module.replace(64, sizeof(banner), QByteArray(banner, sizeof(banner)));
                put(tree + QStringLiteral("/boot/grub/i386-pc/normal.mod"), module);
            }
            if (change != QStringLiteral("grub-sbr")) put(tree + QStringLiteral("/grldr"), "GRUB4DOS loader data");
        }
        if (change == QStringLiteral("efi")) put(tree + QStringLiteral("/EFI/BOOT/BOOTX64.EFI"), efiHeader());
        if (change == QStringLiteral("symlink"))
            QVERIFY(::symlink("isolinux.cfg", QFile::encodeName(tree + QStringLiteral("/isolinux/link")).constData()) == 0);
        if (change == QStringLiteral("partial"))
            QVERIFY(QFile::remove(tree + QStringLiteral("/i386/ntdetect.com")));
        makeImage(QStringLiteral("marker"), "image marker", udf);
        selectWindowsBios(true, false);
        request_.fileSystem = fat32 ? FS_FAT32 : FS_NTFS;
        request_.clusterSize = fat32 ? legacyFatClusterBytes : legacyNtfsClusterBytes;
        if (change == QStringLiteral("alt-e-off")) request_.enhancedInstall = false;
        if (change == QStringLiteral("uefi-ntfs")) request_.uefiNtfs = true;
        if (change == QStringLiteral("customized")) request_.windowsFlags = UNATTEND_NO_DATA_COLLECTION;
        if (change == QStringLiteral("fatless-grub")) fault = Fault::FatlessGrub;
        if (change == QStringLiteral("ntfsless-grub")) fault = Fault::NtfslessGrub;
        if (change == QStringLiteral("reactos")) fault = Fault::ReactOs;
        if (change == QStringLiteral("kolibri")) fault = Fault::Kolibri;
        rufus::IsoWriteCheck result;
        ISO_ANALYSIS_SOURCE view = {};
        if (change == QStringLiteral("grub4dos-sbr"))
        {
            QCOMPARE(checkLegacy(result, &view), 0);
            QVERIFY(result.legacyWinPe);
            QCOMPARE(GetSecondaryBootType(BT_IMAGE, &result.analysis.report, false), int(BT_GRUB4DOS));
            QVERIFY(!rufus::IsLegacyWinPeImage(result.analysis, true, true));
            QVERIFY(rufus::IsLegacyWinPeImage(result.analysis, true, true, true));
            CloseIsoAnalysisSource(&view);
            return;
        }
        if (change == QStringLiteral("symlink") || change == QStringLiteral("reactos")
            || (fat32 && change == QStringLiteral("efi"))) {
            QCOMPARE(checkLegacy(result,&view),0); QVERIFY(result.legacyWinPe); CloseIsoAnalysisSource(&view); return;
        }
        QVERIFY(checkLegacy(result, &view) != 0);
        QVERIFY(!result.problem.isEmpty() && !result.legacyWinPe);
        if (grubSbr)
        {
            QVERIFY(HAS_GRUB(result.analysis.report) && !HAS_SYSLINUX(result.analysis.report));
            const int expectedSbr = change == QStringLiteral("grub4dos-sbr") ? BT_GRUB4DOS : BT_GRUB2;
            QCOMPARE(GetSecondaryBootType(BT_IMAGE, &result.analysis.report, false), expectedSbr);
            QVERIFY(!rufus::IsLegacyWinPeImage(result.analysis, true));
        }
        QVERIFY(view.iso == nullptr && view.udf == nullptr);
        QVERIFY(!QFile::exists(request_.path));
    }

    void legacyGrubFilesystemAllowed_data()
    {
        QTest::addColumn<bool>("udf");
        QTest::addColumn<bool>("fat32");
        for (bool udf : {false, true})
        for (bool fat32 : {false, true})
            QTest::newRow(qPrintable(QStringLiteral("%1-%2").arg(udf).arg(fat32))) << udf << fat32;
    }

    void legacyGrubFilesystemAllowed()
    {
        QFETCH(bool, udf); QFETCH(bool, fat32);
        putWinPe(QStringLiteral("i386"), "[SetupData]\r\nOsLoadOptions=/fastdetect\r\n");
        put(directory_.filePath(QStringLiteral("tree/isolinux/isolinux.cfg")), "append LABEL=SOURCE\r\n");
        makeImage(QStringLiteral("marker"), "image marker", udf);
        selectWindowsBios(true, false);
        request_.fileSystem = fat32 ? FS_FAT32 : FS_NTFS;
        request_.clusterSize = fat32 ? legacyFatClusterBytes : legacyNtfsClusterBytes;
        // The other filesystem is missing from the original GRUB capability
        // report. Retain that fact even though this route installs a Windows PBR.
        fault = fat32 ? Fault::NtfslessGrub : Fault::FatlessGrub;
        rufus::IsoWriteCheck result;
        QCOMPARE(checkLegacy(result), 0);
        QVERIFY(result.legacyWinPe && HAS_GRUB(result.analysis.report) && HAS_SYSLINUX(result.analysis.report));
        QVERIFY(!QFile::exists(request_.path));
    }

    void legacyPartialMinint_data()
    {
        QTest::addColumn<QString>("family");
        QTest::addColumn<int>("files");
        QTest::addColumn<bool>("usesMinint");
        QTest::addColumn<bool>("udf");
        constexpr int completeMinintFiles = 7;
        for (const char* family : {"i386", "amd64"})
        for (int files = 1; files < completeMinintFiles; ++files)
        for (bool usesMinint : {false, true})
        for (bool udf : {false, true})
            QTest::newRow(qPrintable(QStringLiteral("%1-%2-%3-%4").arg(family).arg(files).arg(usesMinint).arg(udf)))
                << QString::fromLatin1(family) << files << usesMinint << udf;
    }
    void legacyPartialMinint()
    {
        QFETCH(QString, family); QFETCH(int, files); QFETCH(bool, usesMinint); QFETCH(bool, udf);
        const QByteArray options = usesMinint ? QByteArray("/minint") : QByteArray("/fastdetect");
        const QByteArray sif = QByteArray("[SetupData]\r\nOsLoadOptions=") + options + "\r\n";
        putWinPe(family, sif);
        const QString tree = directory_.filePath(QStringLiteral("tree"));
        const char* names[] = {"ntdetect.com", "setupldr.bin", "txtsetup.sif"};
        constexpr int setupFileCount = int(sizeof(names) / sizeof(names[0]));
        for (int i = 0; i < setupFileCount; ++i)
            if (files & (1 << i))
                put(tree + QStringLiteral("/MiNiNt/") + QString::fromLatin1(names[i]), "unselected minint bytes");
        makeImage(QStringLiteral("marker"), "partial minint source", udf);
        constexpr int minintBitShift = 6;
        const uint16_t familyMask = family == QStringLiteral("amd64") ? WINPE_AMD64 : WINPE_I386;
        const uint16_t mask = familyMask | (files << minintBitShift);
        for (bool fat32 : {false, true})
        {
            selectWindowsBios(fat32, false);
            if (fat32) { request_.fileSystem = FS_FAT32; request_.clusterSize = 512; }
            rufus::IsoWriteCheck result;
            QCOMPARE(checkLegacy(result), usesMinint ? 0 : ENOTSUP);
            QCOMPARE(result.analysis.report.winpe, mask);
            QVERIFY(result.analysis.report.winpe_options_known);
            QCOMPARE(bool(result.analysis.report.uses_minint), usesMinint);
            const WINPE_SETUP_PLAN plan = GetWinPeSetupPlan(mask, usesMinint);
            QCOMPARE(plan.action, usesMinint ? WINPE_KEEP_LOADER : WINPE_UNSUPPORTED_LAYOUT);
            QCOMPARE(plan.index, family == QStringLiteral("amd64") ? 1u : 0u);
            QCOMPARE(bool(plan.masquerade_disk), !usesMinint);
            QCOMPARE(result.legacyWinPe, usesMinint);
            QVERIFY(!result.partition.started && !QFile::exists(request_.path));
            QCOMPARE(result.analysis.report.win_version.major, uint16_t(0));
            QCOMPARE(result.windows.state, WINDOWS_IMAGE_UNKNOWN);
            if (usesMinint)
            {
                QCOMPARE(result.extraPartitions, quint8(0));
                QCOMPARE(result.tree.required_bytes - result.tree.bytes_written,
                    quint64(legacyLoaderBytes + sizeof("detector") - 1)); // No root SIF stage or patch.
                QVERIFY(rufus::IsLegacyWinPeImage(result.analysis));
            }
            else QVERIFY(!result.problem.isEmpty());
            QCOMPARE(wimOpens, 0);
        }
        for (int i = 0; i < setupFileCount; ++i)
            if (files & (1 << i))
            {
                QFile unused(tree + QStringLiteral("/MiNiNt/") + QString::fromLatin1(names[i]));
                QVERIFY(unused.open(QIODevice::ReadOnly));
                QCOMPARE(unused.readAll(), QByteArray("unselected minint bytes"));
            }
    }

    void legacyRootFiles_data()
    {
        QTest::addColumn<bool>("minint");
        QTest::addColumn<QString>("rootName");
        QTest::addColumn<bool>("identical");
        QTest::addColumn<bool>("fat32");
        QTest::addColumn<bool>("udf");
        for (bool minint : {false, true})
        for (const char* name : {"NtDetect.Com", "TxTsEtUp.SiF", "BoOtMgR"})
        for (bool identical : {false, true})
        for (bool fat32 : {false, true})
        for (bool udf : {false, true})
            QTest::newRow(qPrintable(QStringLiteral("%1-%2-%3-%4-%5").arg(minint).arg(name).arg(identical).arg(fat32).arg(udf)))
                << minint << QString::fromLatin1(name) << identical << fat32 << udf;
    }

    void legacyRootFiles()
    {
        QFETCH(bool, minint); QFETCH(QString, rootName); QFETCH(bool, identical);
        QFETCH(bool, fat32); QFETCH(bool, udf);
        const QString family = minint ? QStringLiteral("minint") : QStringLiteral("i386");
        // Options use ASCII syntax; the keep plan must not decode localized SIF bytes.
        const QByteArray sif = minint ? QByteArray("[SetupData]\r\nOsLoadOptions=/minint\r\n;\x82\xa0\r\n")
            : QByteArray("[SetupData]\r\nOsLoadOptions=/fastdetect\r\n");
        putWinPe(family, sif);
        const bool bootmgr = rootName == QStringLiteral("BoOtMgR");
        const bool rootSif = rootName == QStringLiteral("TxTsEtUp.SiF");
        const QString inputName = bootmgr ? QStringLiteral("setupldr.bin") : rootName.toLower();
        const QString tree = directory_.filePath(QStringLiteral("tree"));
        QFile input(tree + QLatin1Char('/') + family + QLatin1Char('/') + inputName);
        QVERIFY(input.open(QIODevice::ReadOnly));
        const QByteArray original = input.readAll();
        input.close();
        const bool unusedRootSif = minint && rootSif;
        QByteArray rootBytes = identical ? original : QByteArray("different existing root file\r\n");
        if (unusedRootSif && !identical)
            rootBytes = "[SetupData]\r\n[NLS]\r\nAnsiCodepage=c_1252.nls\r\n;\xe9\r\n";
        put(tree + QLatin1Char('/') + rootName, rootBytes);
        makeImage(QStringLiteral("marker"), "legacy root audit", udf);
        QFile iso(sourcePath);
        QVERIFY(iso.open(QIODevice::ReadOnly));
        const QByteArray before = QCryptographicHash::hash(iso.readAll(), QCryptographicHash::Sha256);
        selectWindowsBios(fat32, false);
        if (fat32)
        {
            request_.fileSystem = FS_FAT32;
            request_.clusterSize = 512;
        }
        const bool retainedDetector = !bootmgr && !rootSif;
        const bool editedRootSif = rootSif && !minint && identical;
        const bool sameLoader = bootmgr && identical;
        const bool supportedRoot = unusedRootSif || retainedDetector || editedRootSif || sameLoader;
        const int expected = supportedRoot ? 0 : rootSif ? ENOENT : EEXIST;
        rufus::IsoWriteCheck result;
        QCOMPARE(checkLegacy(result), expected);
        QCOMPARE(bool(HAS_BOOTMGR_BIOS(result.analysis.report)), bootmgr);
        QCOMPARE(result.analysis.report.win_version.major, uint32_t(0));
        QCOMPARE(result.tree.files > 0, expected == 0);
        QVERIFY(!result.partition.started && !QFile::exists(request_.path));
        QCOMPARE(wimOpens, 0);
        QVERIFY(iso.seek(0));
        QCOMPARE(QCryptographicHash::hash(iso.readAll(), QCryptographicHash::Sha256), before);
        QVERIFY(input.open(QIODevice::ReadOnly));
        QCOMPARE(input.readAll(), original);
        QFile root(tree + QLatin1Char('/') + rootName);
        QVERIFY(root.open(QIODevice::ReadOnly));
        QCOMPARE(root.readAll(), rootBytes);
    }

    void legacyRootLoaderReport_data()
    {
        QTest::addColumn<QString>("family");
        QTest::addColumn<QString>("rootKind");
        QTest::addColumn<bool>("udf");
        for (const char* family : {"i386", "amd64", "minint"})
        for (const char* kind : {"absent", "identical", "one-byte-change", "patched", "partial", "prepatched-tail", "wrong-family", "second-pass", "wrong-drive", "opaque", "efi"})
        for (bool udf : {false, true})
        {
            if (strcmp(family, "minint") == 0 && strcmp(kind, "wrong-family") == 0)
                continue;
            QTest::newRow(qPrintable(QStringLiteral("%1-%2-%3").arg(family).arg(kind).arg(udf)))
                << QString::fromLatin1(family) << QString::fromLatin1(kind) << udf;
        }
    }

    void legacyRootLoaderReport()
    {
        QFETCH(QString, family); QFETCH(QString, rootKind); QFETCH(bool, udf);
        const bool minint = family == QStringLiteral("minint");
        const QByteArray sif = minint ? QByteArray("[SetupData]\r\nOsLoadOptions=/minint\r\n")
            : QByteArray("[SetupData]\r\nOsLoadOptions=/fastdetect\r\n");
        putWinPe(family, sif);
        const QString tree = directory_.filePath(QStringLiteral("tree"));
        const QString loaderDirectory = minint ? family : QStringLiteral("i386");
        const QString loaderPath = tree + QLatin1Char('/') + loaderDirectory + QStringLiteral("/setupldr.bin");
        constexpr int loaderBytes = 9000, pathOffset = 128, driveOffset = 256, crcOffset = 0x2060;
        const char pathText[] = "\\minint\\txtsetup.sif", driveText[] = "rdisk(0)";
        const QByteArray pathMarker(pathText, sizeof(pathText));
        const QByteArray driveMarker(driveText, sizeof(driveText));
        QByteArray directoryLoader(loaderBytes, 'x');
        directoryLoader.replace(pathOffset, pathMarker.size(), pathMarker);
        directoryLoader.replace(driveOffset, driveMarker.size(), driveMarker);
        directoryLoader.replace(crcOffset, 2, QByteArray::fromHex("7403"));
        if (rootKind == QStringLiteral("second-pass"))
        {
            const QByteArray path(pathText);
            directoryLoader.replace(pathOffset, path.size() * 2, path + path);
        }
        put(loaderPath, directoryLoader);
        const uint16_t mask = minint ? WINPE_MININT : family == QStringLiteral("amd64") ? WINPE_AMD64 : WINPE_I386;
        const WINPE_SETUP_PLAN plan = GetWinPeSetupPlan(mask, minint);
        QByteArray rootLoader = directoryLoader;
        if (rootKind == QStringLiteral("one-byte-change")) rootLoader[loaderBytes - 1] = 'y';
        const bool prepared = rootKind == QStringLiteral("patched") || rootKind == QStringLiteral("prepatched-tail")
            || rootKind == QStringLiteral("second-pass") || rootKind == QStringLiteral("wrong-family");
        if (prepared)
        {
            const unsigned patchIndex = minint ? 0 : rootKind == QStringLiteral("wrong-family") ? 1 - plan.index : plan.index;
            QVERIFY(PatchWinPeLoader(rootLoader.data(), uint32_t(rootLoader.size()), patchIndex, minint));
            QVERIFY(rootLoader != directoryLoader);
        }
        if (rootKind == QStringLiteral("partial")) rootLoader.replace(crcOffset, 2, QByteArray::fromHex("eb1a"));
        if (rootKind == QStringLiteral("prepatched-tail")) rootLoader[loaderBytes - 1] = 'y';
        if (rootKind == QStringLiteral("wrong-drive")) rootLoader[driveOffset + 6] = '2';
        if (rootKind == QStringLiteral("opaque")) rootLoader = "unqualified opaque boot manager\r\n";
        const bool efi = rootKind == QStringLiteral("efi");
        const bool absent = rootKind == QStringLiteral("absent");
        const QString rootName = efi ? QStringLiteral("BoOtMgR.EfI") : QStringLiteral("BoOtMgR");
        if (efi) rootLoader = efiHeader();
        if (!absent) put(tree + QLatin1Char('/') + rootName, rootLoader);
        makeImage(QStringLiteral("marker"), "root loader classification audit", udf);

        // Filename reporting precedes content qualification. Keep the BOOTMGR
        // flag for record selection; a candidate still needs full comparison.
        for (bool fat32 : {false, true})
        {
            selectWindowsBios(fat32, false);
            if (fat32)
            {
                request_.fileSystem = FS_FAT32;
                request_.clusterSize = 512;
            }
            rufus::IsoWriteCheck result;
            const bool identical = rootKind == QStringLiteral("identical");
            const bool samePrepared = !minint && (rootKind == QStringLiteral("patched") || rootKind == QStringLiteral("partial"));
            const int expected = efi ? ENOTSUP : absent || identical || samePrepared ? 0 : EEXIST;
            QCOMPARE(checkLegacy(result), expected);
            const RUFUS_IMG_REPORT& report = result.analysis.report;
            QCOMPARE(report.winpe, mask);
            QVERIFY(report.winpe_options_known);
            QCOMPARE(bool(report.uses_minint), minint);
            QCOMPARE(bool(HAS_BOOTMGR_BIOS(report)), !absent && !efi);
            QCOMPARE(bool(HAS_BOOTMGR_EFI(report)), efi);
            QCOMPARE(bool(IS_EFI_BOOTABLE(report)), efi);
            QCOMPARE(report.win_version.major, uint16_t(0));
            QCOMPARE(result.windows.state, WINDOWS_IMAGE_UNKNOWN);
            QVERIFY(rufus::IsLegacyWinPeImage(result.analysis));
            QVERIFY(rufus::IsWindowsBiosImage(result.analysis, result.windows, true, true));
            QCOMPARE(result.legacyWinPe, expected == 0);
            if (expected == 0)
            {
                const quint64 loaderStage = minint && identical ? 0 : quint64(directoryLoader.size());
                const quint64 sifStage = minint ? 0 : quint64(sif.size())
                    + sizeof("SetupSourceDevice = \"\\device\\harddisk1\\partition1\"\r\n") - 1;
                QCOMPARE(result.tree.required_bytes - result.tree.bytes_written,
                    loaderStage + sifStage + sizeof("detector") - 1);
            }
            QCOMPARE(GetWinPeSetupPlan(report.winpe, report.uses_minint).masquerade_disk, !minint);
            QVERIFY(!result.partition.started && !QFile::exists(request_.path));
            QCOMPARE(wimOpens, 0);
        }
        QFile original(loaderPath);
        QVERIFY(original.open(QIODevice::ReadOnly));
        QCOMPARE(original.readAll(), directoryLoader);
        if (!absent)
        {
            QFile root(tree + QLatin1Char('/') + rootName);
            QVERIFY(root.open(QIODevice::ReadOnly));
            QCOMPARE(root.readAll(), rootLoader);
        }
    }

    void legacyExistingLoaderIssues_data()
    {
        QTest::addColumn<QString>("issue");
        QTest::addColumn<int>("error");
        QTest::addColumn<bool>("fat32");
        QTest::addColumn<bool>("udf");
        const struct { const char* issue; int error; } cases[] = {
            {"empty", EBADMSG}, {"large", EFBIG}, {"directory", EINVAL},
            {"duplicate", EEXIST}, {"read", EIO}, {"cancel", ECANCELED},
            {"efi-companion", ENOTSUP}, {"install-companion", 0}, {"syslinux-companion", ENOTSUP}
        };
        for (const auto& test : cases)
        for (bool fat32 : {false, true})
        for (bool udf : {false, true})
        {
            const bool syslinuxPbr = fat32 && strcmp(test.issue, "syslinux-companion") == 0;
            const bool biosEfiCompanion = !fat32 && strcmp(test.issue, "efi-companion") == 0;
            QTest::newRow(qPrintable(QStringLiteral("%1-%2-%3").arg(test.issue).arg(fat32).arg(udf)))
                << QString::fromLatin1(test.issue) << (syslinuxPbr || biosEfiCompanion ? 0 : test.error) << fat32 << udf;
        }
    }
    void legacyExistingLoaderIssues()
    {
        QFETCH(QString, issue); QFETCH(int, error); QFETCH(bool, fat32); QFETCH(bool, udf);
        putWinPe(QStringLiteral("i386"), "[SetupData]\r\nOsLoadOptions=/fastdetect\r\n");
        const QString tree = directory_.filePath(QStringLiteral("tree"));
        QByteArray loader(legacyLoaderBytes, 'x');
        if (issue == "empty") loader.clear();
        if (issue == "large") loader = QByteArray(CONFIG_PATCH_MAX_BYTES + 1, 'x');
        const QString root = tree + QStringLiteral("/BoOtMgR");
        if (issue == "directory") QVERIFY(QDir().mkdir(root));
        else put(root, loader);
        if (issue == "duplicate") put(tree + QStringLiteral("/BOOTMGR"), loader);
        if (issue == "efi-companion") put(tree + QStringLiteral("/bootmgr.efi"), efiHeader());
        if (issue == "install-companion") put(tree + QStringLiteral("/sources/install.wim"), "unqualified archive");
        if (issue == "syslinux-companion") put(tree + QStringLiteral("/isolinux/isolinux.cfg"), "default linux\n");
        makeImage(QStringLiteral("marker"), "root loader refusal", udf);
        selectWindowsBios(fat32, false);
        if (fat32) { request_.fileSystem = FS_FAT32; request_.clusterSize = 512; }
        if (issue == "read") fault = Fault::WinPeRootLoaderReadError;
        if (issue == "cancel") fault = Fault::WinPeRootLoaderReadCancel;
        rufus::IsoWriteCheck result;
        QCOMPARE(checkLegacy(result), error);
        QCOMPARE(result.problem.isEmpty(), error == 0);
        QCOMPARE(result.legacyWinPe, error == 0);
        QVERIFY(!result.partition.started && !QFile::exists(request_.path));
        QCOMPARE(result.tree.files > 0, error == 0);
        if (error == 0)
        {
            QVERIFY(HAS_BOOTMGR_BIOS(result.analysis.report));
            QCOMPARE(rufus::IsLegacyWinPeImage(result.analysis), issue != "syslinux-companion");
            QFile unchanged(root);
            QVERIFY(unchanged.open(QIODevice::ReadOnly));
            QCOMPARE(unchanged.readAll(), loader);
        }
        QCOMPARE(wimOpens, issue == "install-companion" ? 1 : 0);
        if (issue == "read" || issue == "cancel") QCOMPARE(loaderReads, 2);
    }
    void legacyExistingLoaderStageSpace_data()
    {
        QTest::addColumn<bool>("minint");
        QTest::addColumn<bool>("fat32");
        QTest::addColumn<bool>("udf");
        QTest::addColumn<bool>("prepatched");
        for (bool minint : {false, true})
        for (bool fat32 : {false, true})
        for (bool udf : {false, true})
        for (bool prepatched : {false, true})
        {
            if (minint && prepatched) continue;
            QTest::newRow(qPrintable(QStringLiteral("%1-%2-%3-%4").arg(minint).arg(fat32).arg(udf).arg(prepatched)))
                << minint << fat32 << udf << prepatched;
        }
    }
    void legacyExistingLoaderStageSpace()
    {
        QFETCH(bool, minint); QFETCH(bool, fat32); QFETCH(bool, udf); QFETCH(bool, prepatched);
        const QString family = minint ? QStringLiteral("minint") : QStringLiteral("i386");
        putWinPe(family, minint ? "[SetupData]\r\nOsLoadOptions=/minint\r\n"
            : "[SetupData]\r\nOsLoadOptions=/fastdetect\r\n");
        QByteArray rootLoader(legacyLoaderBytes, 'x');
        if (prepatched)
        {
            constexpr int pathOffset = 128;
            const char path[] = "\\minint\\txtsetup.sif";
            rootLoader.replace(pathOffset, sizeof(path), QByteArray(path, sizeof(path)));
            put(directory_.filePath(QStringLiteral("tree/i386/setupldr.bin")), rootLoader);
            QVERIFY(PatchWinPeLoader(rootLoader.data(), uint32_t(rootLoader.size()), 0, false));
        }
        put(directory_.filePath(QStringLiteral("tree/BoOtMgR")), rootLoader);
        makeImage(QStringLiteral("marker"), "root loader staging budget", udf);
        selectWindowsBios(fat32, false);
        if (fat32) { request_.fileSystem = FS_FAT32; request_.clusterSize = 512; }
        rufus::IsoWriteCheck accepted;
        QCOMPARE(checkLegacy(accepted), 0);
        const quint64 fullStage = accepted.tree.required_bytes - accepted.tree.bytes_written;
        constexpr quint64 finalSpareBytes = 64;
        const quint64 retainedOutputs = minint ? fullStage : fullStage - legacyLoaderBytes;
        stagedTreeBytes = accepted.partition.size - retainedOutputs - finalSpareBytes;
        fault = Fault::RootEditStageSpace;
        rufus::IsoWriteCheck nearFull;
        QCOMPARE(checkLegacy(nearFull), minint ? 0 : ENOSPC);
        QVERIFY(!nearFull.partition.started && !QFile::exists(request_.path));
        if (minint)
            QCOMPARE(nearFull.tree.required_bytes, nearFull.partition.size - finalSpareBytes);
        else
        {
            QVERIFY(nearFull.tree.required_bytes > nearFull.partition.size);
            stagedTreeBytes -= legacyLoaderBytes;
            rufus::IsoWriteCheck withStageSpace;
            QCOMPARE(checkLegacy(withStageSpace), 0);
            QVERIFY(withStageSpace.tree.required_bytes <= withStageSpace.partition.size);
        }
    }

    void legacyExistingSif_data()
    {
        QTest::addColumn<bool>("fat32");
        QTest::addColumn<bool>("udf");
        QTest::addColumn<int>("encoding");
        QTest::addColumn<bool>("directoryHasSection");
        for (bool fat32 : {false, true})
        for (bool udf : {false, true})
        for (int encoding = 0; encoding < 3; ++encoding)
        for (bool directoryHasSection : {false, true})
            QTest::newRow(qPrintable(QStringLiteral("%1-%2-%3-%4").arg(fat32).arg(udf).arg(encoding).arg(directoryHasSection)))
                << fat32 << udf << encoding << directoryHasSection;
    }
    void legacyExistingSif()
    {
        QFETCH(bool, fat32); QFETCH(bool, udf); QFETCH(int, encoding); QFETCH(bool, directoryHasSection);
        const QByteArray directorySif = directoryHasSection ? QByteArray("[SetupData]\r\nOsLoadOptions=/fastdetect\r\n")
            : QByteArray("OsLoadOptions=/fastdetect\r\n");
        putWinPe(QStringLiteral("i386"), directorySif);
        const QString localized = encoding == 0 ? QString() : QString::fromUtf8(";é😀\n[NLS]\nAnsiCodepage=c_1252.nls\n");
        const QString rootText = localized + QStringLiteral(";root-only\n[SetupData]\nRootValue=keep\n");
        QString localizedCrLf = localized;
        localizedCrLf.replace(QStringLiteral("\n"), QStringLiteral("\r\n"));
        const QString editedText = localizedCrLf + QStringLiteral(";root-only\r\n[SetupData]\r\n")
            + "SetupSourceDevice = \"\\device\\harddisk1\\partition1\"\r\nRootValue=keep\r\n";
        QByteArray original = rootText.toUtf8(), edited = editedText.toUtf8();
        if (encoding == 1)
        {
            original.prepend(QByteArray("\xef\xbb\xbf", 3));
            edited.prepend(QByteArray("\xef\xbb\xbf", 3));
        }
        if (encoding == 2)
        {
            QStringEncoder encoder(QStringConverter::Utf16LE);
            original = QByteArray("\xff\xfe", 2) + encoder(rootText);
            edited = QByteArray("\xff\xfe", 2) + encoder(editedText);
        }
        const QString root = directory_.filePath(QStringLiteral("tree/TxTsEtUp.SiF"));
        put(root, original);
        makeImage(QStringLiteral("marker"), "existing root SIF", udf);
        selectWindowsBios(fat32, false);
        if (fat32) { request_.fileSystem = FS_FAT32; request_.clusterSize = 512; }
        rufus::IsoWriteCheck result;
        QCOMPARE(checkLegacy(result), 0);
        QCOMPARE(result.tree.required_bytes - result.tree.bytes_written, quint64(9000 + 8 + edited.size()));
        QCOMPARE(result.analysis.report.win_version.major, uint32_t(0));
        QCOMPARE(bool(result.analysis.report.uses_minint), false);
        QVERIFY(result.legacyWinPe && !result.partition.started);
        QCOMPARE(wimOpens, 0);
        QFile file(root);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), original); // Preflight never edits source content.
    }
    void legacyExistingSifStageSpace_data()
    {
        QTest::addColumn<bool>("fat32");
        QTest::addColumn<bool>("udf");
        for (bool fat32 : {false, true})
        for (bool udf : {false, true})
            QTest::newRow(qPrintable(QStringLiteral("%1-%2").arg(fat32).arg(udf))) << fat32 << udf;
    }
    void legacyExistingSifStageSpace()
    {
        QFETCH(bool, fat32); QFETCH(bool, udf);
        putWinPe(QStringLiteral("i386"), "[SetupData]\r\nOsLoadOptions=/fastdetect\r\n");
        const QByteArray rootBytes = rootSifFixture + QByteArray(4096, ';');
        put(directory_.filePath(QStringLiteral("tree/TxTsEtUp.SiF")), rootBytes);
        makeImage(QStringLiteral("marker"), "root SIF staging budget", udf);
        selectWindowsBios(fat32, false);
        if (fat32) { request_.fileSystem = FS_FAT32; request_.clusterSize = 512; }
        rufus::IsoWriteCheck accepted;
        QCOMPARE(checkLegacy(accepted), 0);
        constexpr quint64 loaderBytes = 9000, detectorBytes = 8, finalSpareBytes = 64;
        constexpr quint64 addedLineBytes = sizeof("SetupSourceDevice = \"\\device\\harddisk1\\partition1\"\r\n") - 1;
        stagedTreeBytes = accepted.partition.size - loaderBytes - detectorBytes - addedLineBytes - finalSpareBytes;
        fault = Fault::RootEditStageSpace;
        rufus::IsoWriteCheck rejected;
        QCOMPARE(checkLegacy(rejected), ENOSPC);
        QVERIFY(rejected.tree.required_bytes > rejected.partition.size);
        QVERIFY(!rejected.partition.started && !QFile::exists(request_.path));
        stagedTreeBytes -= quint64(rootBytes.size());
        rufus::IsoWriteCheck withStageSpace;
        QCOMPARE(checkLegacy(withStageSpace), 0);
        QVERIFY(withStageSpace.tree.required_bytes <= withStageSpace.partition.size);
        QVERIFY(!withStageSpace.partition.started && !QFile::exists(request_.path));
    }

    void legacyExistingSifRefusal_data()
    {
        QTest::addColumn<QString>("issue");
        QTest::addColumn<int>("error");
        QTest::addColumn<bool>("fat32");
        QTest::addColumn<bool>("udf");
        const struct { const char* issue; int error; } cases[] = {
            {"no-section", ENOENT}, {"empty", EBADMSG}, {"localized", ENOTSUP}, {"bad-utf8", EILSEQ}, {"bad-utf16", EILSEQ},
            {"nls-utf8", ENOTSUP}, {"nls-1252", EILSEQ}, {"nls-932", EILSEQ},
            {"large", EFBIG}, {"directory", EINVAL}, {"duplicate", EEXIST}, {"read", EIO}, {"cancel", ECANCELED}
        };
        for (const auto& test : cases)
        for (bool fat32 : {false, true})
        for (bool udf : {false, true})
            QTest::newRow(qPrintable(QStringLiteral("%1-%2-%3").arg(test.issue).arg(fat32).arg(udf)))
                << QString::fromLatin1(test.issue) << test.error << fat32 << udf;
    }
    void legacyExistingSifRefusal()
    {
        QFETCH(QString, issue); QFETCH(int, error); QFETCH(bool, fat32); QFETCH(bool, udf);
        putWinPe(QStringLiteral("i386"), "[SetupData]\r\nOsLoadOptions=/fastdetect\r\n");
        const QString root = directory_.filePath(QStringLiteral("tree/TxTsEtUp.SiF"));
        QByteArray bytes = rootSifFixture;
        if (issue == "no-section") bytes = "[Other]\r\nKeep=yes\r\n";
        if (issue == "empty") bytes.clear();
        if (issue == "localized") bytes.append(QString::fromUtf8("é").toUtf8());
        if (issue == "bad-utf8") bytes.append(char(0xe9));
        if (issue == "nls-utf8") bytes += "[NLS]\r\nAnsiCodepage=c_1252.nls\r\n;\xc3\xa9\r\n";
        if (issue == "nls-1252") bytes += "[NLS]\r\nAnsiCodepage=c_1252.nls\r\n;\xe9\r\n";
        if (issue == "nls-932") bytes += "[NLS]\r\nAnsiCodepage=c_932.nls\r\n;\x82\xa0\r\n";
        if (issue == "bad-utf16") bytes = QByteArray("\xff\xfe\x5b", 3);
        if (issue == "large") bytes = QByteArray(CONFIG_PATCH_MAX_BYTES + 1, 'x');
        if (issue == "directory") QVERIFY(QDir().mkdir(root));
        else put(root, bytes);
        if (issue == "duplicate") put(directory_.filePath(QStringLiteral("tree/TXTSETUP.SIF")), bytes);
        makeImage(QStringLiteral("marker"), "root SIF refusal", udf);
        selectWindowsBios(fat32, false);
        if (fat32) { request_.fileSystem = FS_FAT32; request_.clusterSize = 512; }
        if (issue == "read") fault = Fault::WinPeRootSifReadError;
        if (issue == "cancel") fault = Fault::WinPeRootSifReadCancel;
        rufus::IsoWriteCheck result;
        QCOMPARE(checkLegacy(result), error);
        QVERIFY(!result.partition.started && result.tree.files == 0);
        QVERIFY(!QFile::exists(request_.path));
        if (issue != "directory")
        {
            QFile file(root);
            QVERIFY(file.open(QIODevice::ReadOnly));
            QCOMPARE(file.readAll(), bytes);
        }
    }

    void legacyRootDetectorIssues_data()
    {
        QTest::addColumn<QString>("issue");
        QTest::addColumn<int>("error");
        QTest::addColumn<bool>("fat32");
        QTest::addColumn<bool>("udf");
        const struct { const char* issue; int error; } cases[] = {
            {"different-same-size", 0}, {"multiple-buffers", 0}, {"empty", EBADMSG}, {"large", EFBIG},
            {"directory", EINVAL}, {"duplicate", EEXIST}, {"read-error", EIO}, {"cancel", ECANCELED}
        };
        for (const auto& test : cases)
        for (bool fat32 : {false, true})
        for (bool udf : {false, true})
            QTest::newRow(qPrintable(QStringLiteral("%1-%2-%3").arg(test.issue).arg(fat32).arg(udf)))
                << QString::fromLatin1(test.issue) << test.error << fat32 << udf;
    }
    void legacyRootDetectorIssues()
    {
        QFETCH(QString, issue); QFETCH(int, error); QFETCH(bool, fat32); QFETCH(bool, udf);
        putWinPe(QStringLiteral("i386"), "[SetupData]\r\nOsLoadOptions=/fastdetect\r\n");
        const QString tree = directory_.filePath(QStringLiteral("tree"));
        const QString root = tree + QStringLiteral("/NtDetect.Com");
        QByteArray rootBytes("detector");
        if (issue == "empty") rootBytes.clear();
        if (issue == "large") rootBytes = QByteArray(CONFIG_PATCH_MAX_BYTES + 1, 'x');
        if (issue == "different-same-size") rootBytes = "differen";
        if (issue == "multiple-buffers") rootBytes = QByteArray(2 * 64 * 1024 + 17, char(0xa5));
        if (issue == "directory") QVERIFY(QDir().mkdir(root));
        else put(root, rootBytes);
        if (issue == "duplicate") put(tree + QStringLiteral("/NTDETECT.COM"), "detector");
        makeImage(QStringLiteral("marker"), "retained root detector", udf);
        selectWindowsBios(fat32, false);
        if (fat32)
        {
            request_.fileSystem = FS_FAT32;
            request_.clusterSize = 512;
        }
        if (issue == "read-error") fault = Fault::WinPeRootReadError;
        if (issue == "cancel") fault = Fault::WinPeRootReadCancel;
        const int before = descriptors();
        rufus::IsoWriteCheck result;
        QCOMPARE(checkLegacy(result), error);
        QCOMPARE(descriptors(), before);
        QVERIFY(!result.partition.started && !QFile::exists(request_.path));
        QCOMPARE(result.tree.files > 0, error == 0);
        if (error == 0)
        {
            QCOMPARE(result.tree.required_bytes - result.tree.bytes_written,
                quint64(legacyLoaderBytes + sizeof("[SetupData]\r\nOsLoadOptions=/fastdetect\r\n") - 1
                    + sizeof("SetupSourceDevice = \"\\device\\harddisk1\\partition1\"\r\n") - 1));
            QCOMPARE(result.analysis.report.win_version.major, uint32_t(0));
            QVERIFY(result.legacyWinPe);
        }
        if (issue != "directory")
        {
            QFile retained(root);
            QVERIFY(retained.open(QIODevice::ReadOnly));
            QCOMPARE(retained.readAll(), rootBytes);
        }
        QCOMPARE(wimOpens, 0);
        if (issue == "read-error" || issue == "cancel") QCOMPARE(detectorReads, 2);
    }

    void legacyWinPeRefusal_data()
    {
        QTest::addColumn<QString>("change");
        QTest::addColumn<bool>("udf");
        const char* changes[] = {"partial", "no-options", "bad-encoding", "missing-section",
            "localized-sif", "large-options", "short-loader", "large-loader", "empty-detector",
            "duplicate-loader", "duplicate-options", "duplicate-directory", "root-detector-empty", "root-sif", "root-bootmgr",
            "unsupported-minint", "partial-minint-only", "options-changed", "mixed-sif-section", "exfat", "uefi",
            "fat32", "username", "flags", "regional", "pending", "no-tree", "read-error",
            "cancel", "source-changed", "capacity", "symlink"};
        for (const char* change : changes)
        {
            QTest::newRow((QByteArray(change) + "-iso").constData()) << QString::fromLatin1(change) << false;
            QTest::newRow((QByteArray(change) + "-udf").constData()) << QString::fromLatin1(change) << true;
        }
    }

    void legacyWinPeRefusal()
    {
        QFETCH(QString, change);
        QFETCH(bool, udf);
        const QString tree = directory_.filePath(QStringLiteral("tree"));
        QByteArray sif("[SetupData]\r\nOsLoadOptions=/fastdetect\r\n");
        if (change == QStringLiteral("missing-section")) sif = "OsLoadOptions=/fastdetect\r\n";
        if (change == QStringLiteral("no-options")) sif.clear();
        if (change == QStringLiteral("bad-encoding")) sif.prepend(QByteArray::fromHex("fffe"));
        if (change == QStringLiteral("localized-sif")) sif.append(QByteArray::fromHex("e9"));
        if (change == QStringLiteral("large-options")) sif.append(QByteArray(64 * 1024, 'x'));
        if (change == QStringLiteral("partial-minint-only")) sif = "[SetupData]\r\nOsLoadOptions=/minint\r\n";
        putWinPe(QStringLiteral("i386"), sif);
        if (change == QStringLiteral("partial")) QVERIFY(QFile::remove(tree + QStringLiteral("/i386/setupldr.bin")));
        if (change == QStringLiteral("short-loader")) put(tree + QStringLiteral("/i386/setupldr.bin"), QByteArray(32, 'x'));
        if (change == QStringLiteral("large-loader")) put(tree + QStringLiteral("/i386/setupldr.bin"), QByteArray(8 * 1024 * 1024 + 1, 'x'));
        if (change == QStringLiteral("empty-detector")) put(tree + QStringLiteral("/i386/ntdetect.com"), QByteArray());
        if (change == QStringLiteral("duplicate-loader")) put(tree + QStringLiteral("/i386/SETUPLDR.BIN"), QByteArray(9000, 'y'));
        if (change == QStringLiteral("duplicate-options")) put(tree + QStringLiteral("/i386/TXTSETUP.SIF"), sif);
        if (change == QStringLiteral("duplicate-directory")) put(tree + QStringLiteral("/I386/other"), "directory collision");
        if (change == QStringLiteral("root-detector-empty")) put(tree + QStringLiteral("/NtDetect.Com"), QByteArray());
        if (change == QStringLiteral("root-sif")) QVERIFY(QDir().mkpath(tree + QStringLiteral("/TxTsEtUp.SiF")));
        if (change == QStringLiteral("root-bootmgr")) put(tree + QStringLiteral("/bOoTmGr"), "keep");
        if (change == QStringLiteral("unsupported-minint")) put(tree + QStringLiteral("/minint/txtsetup.sif"), sif);
        if (change == QStringLiteral("partial-minint-only"))
        {
            put(tree + QStringLiteral("/minint/txtsetup.sif"), sif);
            QVERIFY(QFile::remove(tree + QStringLiteral("/i386/setupldr.bin")));
        }
        if (change == QStringLiteral("mixed-sif-section")) putWinPe(QStringLiteral("amd64"), "OsLoadOptions=/fastdetect\r\n");
        if (change == QStringLiteral("symlink"))
        {
            QVERIFY(QFile::remove(tree + QStringLiteral("/i386/ntdetect.com")));
            QVERIFY(::symlink("setupldr.bin", QFile::encodeName(tree + QStringLiteral("/i386/ntdetect.com")).constData()) == 0);
        }
        makeImage(QStringLiteral("marker"), QByteArray("image marker"), udf);
        selectWindowsBios(false, false);
        if (change == QStringLiteral("exfat")) request_.fileSystem = FS_EXFAT;
        if (change == QStringLiteral("uefi")) request_.uefiNtfs = true;
        if (change == QStringLiteral("fat32")) request_.fileSystem = FS_FAT32;
        if (change == QStringLiteral("username")) request_.windowsUsername = QStringLiteral("someone");
        if (change == QStringLiteral("flags")) request_.windowsFlags = UNATTEND_NO_DATA_COLLECTION;
        if (change == QStringLiteral("regional")) request_.windowsRegion.timezone = QStringLiteral("GMT Standard Time");
        if (change == QStringLiteral("pending")) fault = Fault::ContentPending;
        if (change == QStringLiteral("no-tree")) fault = Fault::NoTree;
        if (change == QStringLiteral("read-error")) fault = Fault::ReadError;
        if (change == QStringLiteral("cancel")) fault = Fault::CancelRead;
        if (change == QStringLiteral("source-changed")) fault = Fault::ChangeAfterTree;
        if (change == QStringLiteral("capacity")) fault = Fault::TooLargeTree;
        if (change == QStringLiteral("options-changed")) fault = Fault::WinPeOptionsChanged;
        rufus::IsoWriteCheck result;
        ISO_ANALYSIS_SOURCE view = {};
        const int error = checkLegacy(result, &view);
        QVERIFY2(error != 0, qPrintable(change));
        QVERIFY2(!result.problem.isEmpty(), qPrintable(change));
        QVERIFY(view.iso == nullptr && view.udf == nullptr);
        QVERIFY(!result.legacyWinPe);
        if (change == QStringLiteral("cancel")) QCOMPARE(error, ECANCELED);
        if (change == QStringLiteral("source-changed")) QCOMPARE(error, ESTALE);
        if (change == QStringLiteral("capacity")) QCOMPARE(error, ENOSPC);
        if (change != QStringLiteral("source-changed"))
        {
            cancelled = false;
            QVERIFY(rufus::CheckIsoWriteImage(request_, getuid(), result, nullptr, true) != 0);
            QVERIFY(!result.legacyWinPe);
        }
    }

    void legacySourceOwnership()
    {
        putWinPe(QStringLiteral("i386"), "[SetupData]\r\nOsLoadOptions=/fastdetect\r\n");
        makeImage(QStringLiteral("marker"), "image marker");
        selectWindowsBios(false, false);
        rufus::IsoWriteCheck result;
        const int before = descriptors();
        QCOMPARE(rufus::CheckLegacyWinPeImage(request_, getuid() + 1, result), EACCES);
        QCOMPARE(descriptors(), before);
        ISO_ANALYSIS_SOURCE view = {};
        QCOMPARE(checkLegacy(result, &view), 0);
        const int retained = descriptors();
        QCOMPARE(rufus::CheckLegacyWinPeImage(request_, getuid(), result, &view), EINVAL);
        QCOMPARE(descriptors(), retained);
        CloseIsoAnalysisSource(&view);
        QCOMPARE(descriptors(), before);
    }

    void legacyFat32SourceExtras_data()
    {
        QTest::addColumn<QString>("change");
        QTest::addColumn<bool>("udf");
        for (const char* change : {"ntldr", "io.sys", "msdos.sys", "plain", "uefi", "customized"})
        {
            QTest::newRow((QByteArray(change) + "-iso").constData()) << QString::fromLatin1(change) << false;
            QTest::newRow((QByteArray(change) + "-udf").constData()) << QString::fromLatin1(change) << true;
        }
    }

    void legacyFat32SourceExtras()
    {
        QFETCH(QString, change);
        QFETCH(bool, udf);
        putWinPe(QStringLiteral("i386"), "[SetupData]\r\nOsLoadOptions=/fastdetect\r\n");
        const bool existingLoader = change == QStringLiteral("ntldr") || change == QStringLiteral("io.sys")
            || change == QStringLiteral("msdos.sys");
        if (existingLoader)
            put(directory_.filePath(QStringLiteral("tree/")) + change.toUpper(), "preserve existing loader");
        makeImage(QStringLiteral("marker"), QByteArray("image marker"), udf);
        selectWindowsBios(true, false);
        request_.fileSystem = FS_FAT32;
        request_.clusterSize = 512;
        if (change == QStringLiteral("plain")) request_.enhancedInstall = false;
        if (change == QStringLiteral("uefi")) request_.uefiNtfs = true;
        if (change == QStringLiteral("customized")) request_.windowsFlags = UNATTEND_NO_DATA_COLLECTION;
        rufus::IsoWriteCheck result;
        ISO_ANALYSIS_SOURCE view = {};
        if (existingLoader)
        {
            QCOMPARE(checkLegacy(result, &view), 0);
            QVERIFY(result.legacyWinPe && result.problem.isEmpty());
            CloseIsoAnalysisSource(&view);
            QFile retained(directory_.filePath(QStringLiteral("tree/")) + change.toUpper());
            QVERIFY(retained.open(QIODevice::ReadOnly));
            QCOMPARE(retained.readAll(), QByteArray("preserve existing loader"));
            return;
        }
        QVERIFY(checkLegacy(result, &view) != 0);
        QVERIFY(!result.problem.isEmpty());
        QVERIFY(!result.legacyWinPe);
        QVERIFY(view.iso == nullptr && view.udf == nullptr);
        QCOMPARE(wimOpens, 0);
    }

    void windowsEfiRepair_data()
    {
        QTest::addColumn<bool>("udf");
        QTest::addColumn<int>("flags");
        QTest::addColumn<bool>("mixedCase");
        QTest::addColumn<int>("version");
        for (bool udf : {false, true})
        {
            const QByteArray view = udf ? "UDF-" : "ISO-";
            QTest::newRow(view + "ordinary") << udf << 0 << false << 0;
            QTest::newRow(view + "mixed-case") << udf << 0 << true << 0;
            QTest::newRow(view + "solid") << udf << int(WIMLIB_WRITE_FLAG_SOLID) << false << 0;
            QTest::newRow(view + "pipable") << udf << int(WIMLIB_WRITE_FLAG_PIPABLE) << false << 0;
        }
        QTest::newRow("version-7") << false << 0 << false << 7;
        QTest::newRow("version-11-same-content-trigger") << false << 0 << false << 11;
    }

    void windowsEfiRepair()
    {
        QFETCH(bool, udf);
        QFETCH(int, flags);
        QFETCH(bool, mixedCase);
        QFETCH(int, version);
        QByteArray first = efiHeader();
        first[511] = '1';
        QByteArray other = first;
        other[511] = '2';
        makeRepairArchive(QStringLiteral("sources/install.wim"), first, other, flags, mixedCase);
        makeRepairArchive(QStringLiteral("sources/install2.wim"), other, other);
        makeRepairArchive(QStringLiteral("sources/boot.wim"), other, other);
        makeImage(QStringLiteral("bootmgr.efi"), other, udf);
        ISO_ANALYSIS analysis = {};
        ISO_ANALYSIS_SOURCE view = {};
        const ISO_ANALYSIS_OPTIONS options = {true, true, {0, 0}};
        const int scanned = AnalyzeIsoImageWithSource(sourcePath.toUtf8().constData(),
            request_.imageSize, &options, &analysis, &view);
        QVERIFY(scanned == ISO_SCAN_OK || scanned == ISO_SCAN_PARTIAL);
        QVERIFY(HAS_WIN7_EFI(analysis.report));
        // Freeze first-archive order explicitly; neither later archives nor
        // boot.wim/boot index 2/root bootmgr.efi may select the payload.
        strcpy(analysis.report.wininst_path[0], "?:/sources/install.wim");
        strcpy(analysis.report.wininst_path[1], "?:/sources/install2.wim");
        analysis.report.wininst_index = 2;
        analysis.report.win_version.major = version;
        const RUFUS_IMG_REPORT before = analysis.report;
        const int descriptorsBefore = descriptors();
        QByteArray payload("stale");
        QString problem("stale");
        const int error = rufus::ReadWindowsEfiRepair(view, analysis.report, payload, problem);
        const int descriptorsAfter = descriptors();
        CloseIsoAnalysisSource(&view);
        QCOMPARE(error, 0);
        QCOMPARE(payload, first);
        QVERIFY(problem.isEmpty());
        QCOMPARE(descriptorsAfter, descriptorsBefore);
        QCOMPARE(memcmp(&analysis.report, &before, sizeof(before)), 0);
        QCOMPARE(int(analysis.report.win_version.major), version); // Unknown remains unknown.
        QCOMPARE(wimOpens, 1);
    }

    void windowsEfiRepairRefusals_data()
    {
        QTest::addColumn<QByteArray>("first");
        QTest::addColumn<int>("injected");
        QTest::addColumn<int>("expected");
        QTest::newRow("missing-in-first-image") << QByteArray() << int(RepairFault::None) << EIO;
        QTest::newRow("empty") << QByteArray("") << int(RepairFault::None) << EBADMSG;
        QTest::newRow("short") << QByteArray(63, '\0') << int(RepairFault::None) << EBADMSG;
        QByteArray bytes = efiHeader();
        bytes[0] = 'X';
        QTest::newRow("not-PE") << bytes << int(RepairFault::None) << ENOTSUP;
        bytes = efiHeader();
        qToLittleEndian<quint16>(0x14c, bytes.data() + 132);
        QTest::newRow("x86") << bytes << int(RepairFault::None) << ENOTSUP;
        bytes = efiHeader();
        qToLittleEndian<quint16>(0xaa64, bytes.data() + 132);
        QTest::newRow("arm64") << bytes << int(RepairFault::None) << ENOTSUP;
        bytes = efiHeader();
        qToLittleEndian<quint16>(11, bytes.data() + 220);
        QTest::newRow("EFI-driver") << bytes << int(RepairFault::None) << EBADMSG;
        bytes = efiHeader();
        qToLittleEndian<quint16>(0x10b, bytes.data() + 152);
        QTest::newRow("PE32") << bytes << int(RepairFault::None) << EBADMSG;
        bytes = efiHeader();
        qToLittleEndian<quint16>(4096, bytes.data() + 148);
        QTest::newRow("truncated-optional-header") << bytes << int(RepairFault::None) << EBADMSG;
        bytes = efiHeader();
        qToLittleEndian<quint32>(UINT32_MAX, bytes.data() + 60);
        QTest::newRow("out-of-range-PE-offset") << bytes << int(RepairFault::None) << EBADMSG;
        constexpr quint32 testHeaderLimit = 1024 * 1024;
        bytes = QByteArray(testHeaderLimit + 512, '\0');
        qToLittleEndian<quint32>(testHeaderLimit, bytes.data() + 60);
        QTest::newRow("excessive-PE-header") << bytes << int(RepairFault::None) << ENOTSUP;
        QTest::newRow("no-trigger") << efiHeader() << int(RepairFault::NoTrigger) << ENOTSUP;
        QTest::newRow("no-install-archive-trigger") << efiHeader() << int(RepairFault::NoInstaller) << ENOTSUP;
        QTest::newRow("malformed-first-path") << efiHeader() << int(RepairFault::InvalidPath) << EINVAL;
        QTest::newRow("unterminated-first-path") << efiHeader() << int(RepairFault::UnterminatedPath) << EINVAL;
        QTest::newRow("missing-first-archive") << efiHeader() << int(RepairFault::MissingArchive) << EIO;
        QTest::newRow("unreadable-first-archive") << efiHeader() << int(RepairFault::BrokenArchive) << EIO;
        QTest::newRow("read-error") << efiHeader() << int(RepairFault::ReadError) << EIO;
        QTest::newRow("cancel-before-read") << efiHeader() << int(RepairFault::CancelBeforeRead) << ECANCELED;
        QTest::newRow("cancel-during-read") << efiHeader() << int(RepairFault::CancelDuringRead) << ECANCELED;
        QTest::newRow("cancel-after-extraction") << efiHeader() << int(RepairFault::CancelAfterRead) << ECANCELED;
    }

    void windowsEfiRepairRefusals()
    {
        QFETCH(QByteArray, first);
        QFETCH(int, injected);
        const RepairFault mode = RepairFault(injected);
        QFETCH(int, expected);
        makeRepairArchive(QStringLiteral("sources/install.wim"), first, efiHeader());
        makeRepairArchive(QStringLiteral("sources/install2.wim"), efiHeader(), efiHeader());
        if (mode == RepairFault::BrokenArchive)
            put(directory_.filePath(QStringLiteral("tree/sources/install.wim")), "broken WIM");
        makeImage(QStringLiteral("bootmgr.efi"), efiHeader());
        ISO_ANALYSIS analysis = {};
        ISO_ANALYSIS_SOURCE view = {};
        const ISO_ANALYSIS_OPTIONS options = {true, true, {0, 0}};
        const int scanned = AnalyzeIsoImageWithSource(sourcePath.toUtf8().constData(),
            request_.imageSize, &options, &analysis, &view);
        QVERIFY(scanned == ISO_SCAN_OK || scanned == ISO_SCAN_PARTIAL);
        strcpy(analysis.report.wininst_path[0], "?:/sources/install.wim");
        strcpy(analysis.report.wininst_path[1], "?:/sources/install2.wim");
        analysis.report.wininst_index = 2;
        switch (mode)
        {
        case RepairFault::NoTrigger: analysis.report.has_efi |= 1u << ARCH_X86_64; break;
        case RepairFault::NoInstaller: analysis.report.wininst_index = 0; break;
        case RepairFault::InvalidPath: strcpy(analysis.report.wininst_path[0], "/sources/install.wim"); break;
        case RepairFault::UnterminatedPath: memset(analysis.report.wininst_path[0], 'x', sizeof(analysis.report.wininst_path[0])); break;
        case RepairFault::MissingArchive: strcpy(analysis.report.wininst_path[0], "?:/sources/missing.wim"); break;
        case RepairFault::ReadError: fault = Fault::ReadError; break;
        case RepairFault::CancelBeforeRead: cancelled = true; break;
        case RepairFault::CancelDuringRead: fault = Fault::CancelRead; break;
        case RepairFault::CancelAfterRead: fault = Fault::WindowsRepairCancel; break;
        default: break;
        }
        const RUFUS_IMG_REPORT before = analysis.report;
        const int descriptorsBefore = descriptors();
        QByteArray payload("stale");
        QString problem;
        const int error = rufus::ReadWindowsEfiRepair(view, analysis.report, payload, problem);
        const int descriptorsAfter = descriptors();
        CloseIsoAnalysisSource(&view);
        QCOMPARE(error, expected);
        QVERIFY(payload.isEmpty());
        if (expected != ECANCELED)
            QVERIFY(!problem.isEmpty());
        QCOMPARE(memcmp(&analysis.report, &before, sizeof(before)), 0);
        QCOMPARE(descriptorsAfter, descriptorsBefore);
        const bool opensArchive = mode != RepairFault::NoTrigger && mode != RepairFault::NoInstaller
            && mode != RepairFault::InvalidPath
            && mode != RepairFault::UnterminatedPath && mode != RepairFault::MissingArchive
            && mode != RepairFault::CancelBeforeRead;
        QCOMPARE(wimOpens, opensArchive ? 1 : 0);
    }

    void windowsEfiRepairPreflight_data()
    {
        QTest::addColumn<bool>("udf");
        QTest::addColumn<int>("filesystem");
        QTest::addColumn<QString>("mode");
        for (bool udf : {false, true})
        for (int filesystem : {FS_FAT32, FS_NTFS})
        for (const char* mode : {"UEFI", "BIOS", "dual"})
            QTest::newRow((QByteArray(udf ? "UDF-" : "ISO-")
                + (filesystem == FS_NTFS ? "NTFS-" : "FAT32-") + mode).constData())
                << udf << filesystem << QString::fromLatin1(mode);
    }

    void windowsEfiRepairPreflight()
    {
        QFETCH(bool, udf); QFETCH(int, filesystem); QFETCH(QString, mode);
        const QByteArray loader = efiHeader();
        makeRepairArchive(QStringLiteral("sources/install.wim"), loader, QByteArray("wrong image"),
            0, false, true);
        put(directory_.filePath(QStringLiteral("tree/bootmgr")), "BIOS manager");
        makeImage(QStringLiteral("bootmgr.efi"), QByteArray("not the fallback payload"), udf);
        const bool bios = mode != QStringLiteral("UEFI");
        const bool enhanced = mode == QStringLiteral("dual");
        if (bios)
            selectWindowsBios(enhanced, enhanced && filesystem == FS_NTFS);
        request_.fileSystem = filesystem;
        request_.clusterSize = filesystem == FS_NTFS ? 4096 : 512;
        rufus::IsoWriteCheck checked;
        const int expected = bios && !enhanced && filesystem == FS_FAT32 ? ENOTSUP : 0;
        QCOMPARE(check(checked), expected);
        QVERIFY(!checked.partition.started);
        if (expected != 0)
            return;
        const bool repairs = !bios || enhanced;
        QCOMPARE(checked.windowsEfiRepair, repairs ? loader : QByteArray());
        QCOMPARE(checked.efiPath, repairs ? QStringLiteral("/EFI/BOOT/BOOTX64.EFI") : QString());
        QVERIFY(HAS_WIN7_EFI(checked.analysis.report));
        QCOMPARE(checked.analysis.report.win_version.major, quint16(11));
        QCOMPARE(checked.extraPartitions, quint8(filesystem == FS_NTFS && repairs ? XP_UEFI_NTFS : 0));
    }

    void windowsEfiRepairPreflightFailures_data()
    {
        QTest::addColumn<QString>("kind");
        QTest::addColumn<QByteArray>("loader");
        QTest::addColumn<int>("injected");
        QTest::addColumn<int>("expected");
        for (const char* kind : {"win11", "win7"})
        {
            const QString version = QString::fromLatin1(kind);
            const QByteArray prefix = QByteArray(kind) + '-';
            QTest::newRow(prefix + "missing-payload") << version << QByteArray() << int(Fault::None) << EIO;
            QTest::newRow(prefix + "wrong-payload") << version << QByteArray("root manager is not a substitute") << int(Fault::None) << EBADMSG;
            QTest::newRow(prefix + "WIM-read-failure") << version << efiHeader() << int(Fault::InstallReadError) << EIO;
            QTest::newRow(prefix + "WIM-read-cancelled") << version << efiHeader() << int(Fault::InstallReadCancelled) << ECANCELED;
            QTest::newRow(prefix + "extraction-cancelled") << version << efiHeader() << int(Fault::WindowsRepairCancel) << ECANCELED;
            QTest::newRow(prefix + "changed-during-WIM-read") << version << efiHeader() << int(Fault::InstallSourceChanged) << ESTALE;
            QTest::newRow(prefix + "changed-after-tree-check") << version << efiHeader() << int(Fault::ChangeAfterTree) << ESTALE;
            QTest::newRow(prefix + "repair-and-tree-exceed-partition") << version << efiHeader() << int(Fault::TooLargeTree) << ENOSPC;
        }
    }

    void windowsEfiRepairPreflightFailures()
    {
        QFETCH(QString, kind); QFETCH(QByteArray, loader); QFETCH(int, injected); QFETCH(int, expected);
        putWindows(kind, loader);
        makeImage(QStringLiteral("bootmgr.efi"), efiHeader());
        request_.fileSystem = FS_FAT32;
        request_.clusterSize = 512;
        fault = Fault(injected);
        rufus::IsoWriteCheck checked;
        QCOMPARE(check(checked), expected);
        QVERIFY(!checked.partition.started);
        QVERIFY(checked.windowsEfiRepair.isEmpty());
        QVERIFY(!checked.problem.isEmpty());
        QCOMPARE(checked.windows.state, WINDOWS_IMAGE_KNOWN);
    }

    void windowsEfiRepairSurvivesCopiedArchiveRemoval()
    {
        const QByteArray loader = efiHeader() + QByteArray(96 * 1024, 'E');
        makeRepairArchive(QStringLiteral("sources/install.wim"), loader, QByteArray("wrong image"),
            0, false, true);
        makeImage(QStringLiteral("bootmgr.efi"), QByteArray("root manager remains unchanged"));
        request_.fileSystem = FS_FAT32;
        request_.clusterSize = 512;
        rufus::IsoWriteCheck checked;
        QCOMPARE(check(checked), 0);
        QTemporaryDir target;
        QVERIFY(target.isValid());
        const QByteArray image = sourcePath.toUtf8();
        const ISO_EXTRACT_OPTIONS options = {{true, true, {0, 0}}, "NEW LABEL", image.constData(), false};
        ISO_EXTRACT_RESULT copied = {};
        QCOMPARE(ExtractIsoTree(sourcePath.toUtf8().constData(), target.path().toUtf8().constData(),
            &options, &copied), 0);
        // Splitting removes the target's install.wim. Its disappearance cannot
        // make repair depend on a second WIM open or a different image.
        QVERIFY(QFile::remove(target.filePath(QStringLiteral("sources/install.wim"))));
        put(target.filePath(QStringLiteral("sources/install.swm")), "stand-in split output");
        const int opensBefore = wimOpens;
        QCOMPARE(rufus::InstallWindowsEfiRepair(checked.windowsEfiRepair,
            target.path().toUtf8().constData()), 0);
        QFile installed(target.filePath(QStringLiteral("EFI/BOOT/BOOTX64.EFI")));
        QVERIFY(installed.open(QIODevice::ReadOnly));
        QCOMPARE(installed.readAll(), loader);
        QFile root(target.filePath(QStringLiteral("bootmgr.efi")));
        QVERIFY(root.open(QIODevice::ReadOnly));
        QCOMPARE(root.readAll(), QByteArray("root manager remains unchanged"));
        QCOMPARE(wimOpens, opensBefore);
        QVERIFY(QFile::exists(target.filePath(QStringLiteral("sources/install.swm"))));
    }

    void acceptedPrerequisites_data()
    {
        QTest::addColumn<bool>("udf");
        QTest::addColumn<bool>("hybrid");
        QTest::newRow("ISO") << false << false;
        QTest::newRow("UDF") << true << false;
        QTest::newRow("ISOHybrid") << false << true;
        QTest::newRow("UDFHybrid") << true << true;
    }

    void unsupportedFilesystemsFailBeforeImageAccess()
    {
        // No source exists: these routes must fail on filesystem support before
        // opening or scanning an ISO, even when bypassing protocol decoding.
        for (int fs : {FS_EXFAT, FS_UNKNOWN, FS_MAX})
        {
            request_.fileSystem = fs;
            rufus::IsoWriteCheck result;
            QCOMPARE(check(result), ENOTSUP);
            QVERIFY(!result.partition.started);
            QCOMPARE(result.tree.files, quint64(0));
            QVERIFY(result.problem.contains(QStringLiteral("filesystem")));
        }
    }

    void fat16Prerequisites_data()
    {
        QTest::addColumn<bool>("udf");
        QTest::addColumn<int>("injected");
        QTest::addColumn<int>("expected");
        for (bool udf : {false, true})
        {
            const QByteArray prefix = udf ? "UDF-" : "ISO-";
            QTest::newRow((prefix + "accepted").constData()) << udf << int(Fault::None) << 0;
            QTest::newRow((prefix + "needs-NTFS").constData()) << udf << int(Fault::NeedsNtfs) << ENOTSUP;
            QTest::newRow((prefix + "large-file").constData()) << udf << int(Fault::LargeFile) << ENOTSUP;
            QTest::newRow((prefix + "capacity").constData()) << udf << int(Fault::TooLargeTree) << ENOSPC;
            QTest::newRow((prefix + "read-error").constData()) << udf << int(Fault::ReadError) << EIO;
            QTest::newRow((prefix + "cancel").constData()) << udf << int(Fault::CancelRead) << ECANCELED;
        }
    }

    void fat16Prerequisites()
    {
        QFETCH(bool, udf);
        QFETCH(int, injected);
        QFETCH(int, expected);
        makeImage(QStringLiteral("EFI/BOOT/BOOTX64.EFI"), efiHeader(), udf);
        request_.fileSystem = FS_FAT16;
        request_.clusterSize = 2048;
        fault = Fault(injected);
        rufus::IsoWriteCheck result;
        QCOMPARE(check(result), expected);
        QVERIFY(!result.partition.started);
        if (expected != 0)
            return;
        QCOMPARE(result.partition.offset, testMiB);
        QCOMPARE(result.partition.size, quint64(66027520));
        QCOMPARE(result.extraPartitions, quint8(0));
        QCOMPARE(result.tree.files, quint64(3));
        QCOMPARE(extractionLimit, uint64_t(UINT32_MAX));
        QVERIFY(!result.splitInstallWim);
        request_.clusterSize = 512; // Too many FAT16 clusters in this volume.
        QCOMPARE(check(result), ENOTSUP);
        request_.clusterSize = 2048;
        request_.syslinux = true; // This EFI-only fixture has no Syslinux loader.
        QCOMPARE(check(result), ENOTSUP);
    }

    void requiresExplicitImageOwner()
    {
        makeImage();
        rufus::IsoWriteCheck result;
        const int before = descriptors();
        QCOMPARE(rufus::CheckIsoWriteImage(request_, getuid() + 1, result), EACCES);
        QCOMPARE(result.tree.files, quint64(0));
        QCOMPARE(fallbackReads, 0);
        QCOMPARE(rufus::CheckIsoWriteImage(request_, getuid(), result), 0);
        QCOMPARE(descriptors(), before);
    }

    void acceptedPrerequisites()
    {
        QFETCH(bool, udf);
        QFETCH(bool, hybrid);
        makeImage(QStringLiteral("eFi/BoOt/BoOtX64.EfI"), efiHeader(), udf, hybrid);
        rufus::IsoWriteCheck result;
        QCOMPARE(check(result), 0);
        QVERIFY(result.problem.isEmpty());
        QCOMPARE(result.efiPath, QStringLiteral("/eFi/BoOt/BoOtX64.EfI"));
        QVERIFY(result.analysis.tree_complete);
        QVERIFY(result.analysis.pending & ISO_PENDING_SECURE_BOOT);
        QCOMPARE(result.analysis.filesystem, udf ? ISO_FILESYSTEM_UDF : ISO_FILESYSTEM_ISO9660);
        QCOMPARE(result.partition.offset, testMiB);
        // 128961 sectors after Rufus's track rounding and GPT reservation.
        QCOMPARE(result.partition.size, quint64(66028032));
        QVERIFY(!result.partition.started);
        QCOMPARE(result.tree.files, quint64(3));
        QCOMPARE(result.tree.bytes_written, quint64(512 + 17 + 21));
        QCOMPARE(result.choices.count, hybrid ? 3 : 2);
        QCOMPARE(result.choices.mode[0], IMAGE_WRITE_ISO);
        QCOMPARE(result.choices.mode[1], IMAGE_WRITE_ESP);
        if (hybrid)
        {
            QCOMPARE(result.choices.mode[2], IMAGE_WRITE_DD);
        }
    }

    void windowsPrerequisites_data()
    {
        QTest::addColumn<bool>("udf");
        QTest::addColumn<bool>("ntfs");
        QTest::newRow("ISO-FAT32") << false << false;
        QTest::newRow("UDF-FAT32") << true << false;
        QTest::newRow("ISO-NTFS") << false << true;
        QTest::newRow("UDF-NTFS") << true << true;
    }

    void windowsBiosPrerequisites_data()
    {
        QTest::addColumn<QString>("kind");
        QTest::addColumn<bool>("udf");
        QTest::addColumn<bool>("imageEfi");
        QTest::addColumn<bool>("enhanced");
        QTest::addColumn<bool>("payload");
        QTest::addColumn<int>("fileSystem");
        for (int fs : {FS_NTFS, FS_FAT32})
        {
            for (bool udf : {false, true})
            for (bool imageEfi : {false, true})
            for (bool enhanced : {false, true})
            for (bool payload : {false, true})
            {
                const QByteArray name = QByteArray(fs == FS_FAT32 ? "FAT32-" : "NTFS-") + (udf ? "UDF-" : "ISO-")
                    + (imageEfi ? "EFI-" : "BIOS-") + (enhanced ? "enhanced-" : "ordinary-")
                    + (payload ? "payload" : "plain");
                QTest::newRow(name.constData()) << QStringLiteral("win11") << udf << imageEfi << enhanced << payload << fs;
            }
            for (bool udf : {false, true})
            for (bool dual : {false, true})
            {
                const QByteArray name = QByteArray(fs == FS_FAT32 ? "FAT32-Win10-" : "NTFS-Win10-") + (udf ? "UDF-" : "ISO-") + (dual ? "dual" : "bios");
                QTest::newRow(name.constData()) << QStringLiteral("win10") << udf << dual << dual << (fs == FS_NTFS && dual) << fs;
            }
        }
    }

    void windowsBiosPrerequisites()
    {
        QFETCH(bool, udf); QFETCH(bool, imageEfi); QFETCH(bool, enhanced); QFETCH(bool, payload);
        QFETCH(QString, kind); QFETCH(int, fileSystem);
        putWindows(kind);
        if (!imageEfi)
            QVERIFY(QFile::remove(directory_.filePath(QStringLiteral("tree/bootmgr.efi"))));
        // BOOTMGR's BIOS container is not a conventional fallback EFI PE.
        put(directory_.filePath(QStringLiteral("tree/BoOtMgR")), QByteArray("opaque BIOS boot manager\n"));
        makeImage(imageEfi ? QStringLiteral("eFi/BoOt/BoOtX64.EfI") : QStringLiteral("bios-payload"),
            imageEfi ? efiHeader() : QByteArray("BIOS-only\n"), udf, true);
        selectWindowsBios(enhanced, payload);
        request_.fileSystem = fileSystem;
        request_.clusterSize = fileSystem == FS_FAT32 ? 512 : 4096;
        const bool fat32 = fileSystem == FS_FAT32;
        const bool validSettings = !payload || (enhanced && !fat32);
        const bool requiredPayload = imageEfi && enhanced && !fat32;
        const bool imageAllowed = !fat32 || enhanced;
        const int expected = !validSettings ? EINVAL : imageAllowed && payload == requiredPayload ? 0 : ENOTSUP;
        for (bool rufusMbr : {true, false})
        {
            request_.windowsRufusMbr = rufusMbr;
            wimOpens = 0;
            rufus::IsoWriteCheck result;
            QCOMPARE(check(result), expected);
            QVERIFY(!result.partition.started);
            QVERIFY(!result.splitInstallWim);
            if (expected != 0)
            {
                QCOMPARE(result.tree.files, quint64(0));
                continue;
            }
            QCOMPARE(result.windows.state, WINDOWS_IMAGE_KNOWN);
            QCOMPARE(result.analysis.report.win_version.major, quint16(kind == QStringLiteral("win10") ? 10 : 11));
            QVERIFY(HAS_BOOTMGR_BIOS(result.analysis.report));
            QCOMPARE(result.extraPartitions, quint8(requiredPayload ? XP_UEFI_NTFS : 0));
            QCOMPARE(result.partition.offset, testMiB);
            QCOMPARE(result.partition.size, quint64(payload ? 64962560 : 66060288));
            QCOMPARE(result.partition.uefi_ntfs_size, quint64(payload ? 1064448 : 0));
            QCOMPARE(result.efiPath.isEmpty(), !(payload || (fat32 && imageEfi && enhanced)));
            QCOMPARE(extractionLimit, quint64(fat32 ? UINT32_MAX : 0));
            QCOMPARE(result.choices.count, 2);
            QCOMPARE(result.choices.mode[0], IMAGE_WRITE_ISO);
            QCOMPARE(result.choices.mode[1], IMAGE_WRITE_DD);
        }
    }

    void legacyWindowsDispatch_data()
    {
        QTest::addColumn<QString>("kind");
        for (const char* kind : {"vista-bios", "vista-uefi", "win10-fat16", "win11-fat16",
                "wim-pe-bios", "wim-pe-uefi", "i386-efi-install", "amd64-install",
                "minint-efi", "i386-syslinux", "i386-grub2", "minint-grub4dos"})
            QTest::newRow(kind) << QString::fromLatin1(kind);
    }

    void legacyWindowsDispatch()
    {
        QFETCH(QString, kind);
        const QString tree = directory_.filePath(QStringLiteral("tree"));
        const bool setup = kind.startsWith(QStringLiteral("i386"))
            || kind.startsWith(QStringLiteral("amd64")) || kind.startsWith(QStringLiteral("minint"));
        const bool minint = kind.startsWith(QStringLiteral("minint"));
        const bool syslinux = kind.endsWith(QStringLiteral("syslinux"));
        const bool grub2 = kind.endsWith(QStringLiteral("grub2"));
        const bool grub4dos = kind.endsWith(QStringLiteral("grub4dos"));
        const bool efiOnly = kind.endsWith(QStringLiteral("uefi"));
        const bool ordinaryPe = kind.startsWith(QStringLiteral("wim-pe"));
        const bool fat16 = kind.endsWith(QStringLiteral("fat16"))
            || kind == QStringLiteral("i386-efi-install") || (minint && !grub4dos);
        const QString metadata = kind.startsWith(QStringLiteral("win10")) ? QStringLiteral("win10")
            : kind.startsWith(QStringLiteral("win11")) ? QStringLiteral("win11") : QStringLiteral("vista");
        if (ordinaryPe)
            makeRepairArchive(QStringLiteral("sources/boot.wim"), QByteArray(), QByteArray());
        else if (!minint && !syslinux)
            putWindows(metadata);
        if (setup)
        {
            const QString family = minint ? QStringLiteral("minint")
                : kind.startsWith(QStringLiteral("amd64")) ? QStringLiteral("amd64") : QStringLiteral("i386");
            putWinPe(family, minint ? QByteArray("[SetupData]\r\nOsLoadOptions=/minint\r\n")
                : QByteArray("[SetupData]\r\nOsLoadOptions=/fastdetect\r\n"));
        }
        else
            put(tree + QStringLiteral("/bootmgr"), "ordinary image-owned BOOTMGR");
        if (syslinux)
        {
            put(tree + QStringLiteral("/isolinux/isolinux.cfg"), "DEFAULT source\r\n");
            QByteArray loader(512, '\0');
            const QByteArray banner("ISOLINUX 4.07");
            loader.replace(96, banner.size(), banner);
            put(tree + QStringLiteral("/isolinux/isolinux.bin"), loader);
        }
        if (grub2)
        {
            QByteArray module(4096, '\0');
            const char banner[] = "GRUB  version %s\0" "2.12";
            module.replace(64, sizeof(banner), QByteArray(banner, sizeof(banner)));
            put(tree + QStringLiteral("/boot/grub/i386-pc/normal.mod"), module);
            put(tree + QStringLiteral("/boot/grub/grub.cfg"), "menuentry source {}\n");
        }
        if (grub4dos)
            put(tree + QStringLiteral("/grldr"), "image-owned GRUB4DOS loader");
        const bool plainSetup = setup && !fat16;
        if (plainSetup && !syslinux && !minint)
            QVERIFY(QFile::remove(tree + QStringLiteral("/bootmgr.efi")));
        makeImage(plainSetup ? QStringLiteral("marker") : QStringLiteral("EFI/BOOT/BOOTX64.EFI"),
            plainSetup ? QByteArray("ordinary BIOS setup") : efiHeader());
        if (!efiOnly)
            selectWindowsBios(fat16, false);
        request_.fileSystem = fat16 ? FS_FAT16 : FS_NTFS;
        request_.clusterSize = fat16 ? 2048 : 4096;
        if (syslinux)
        {
            request_.windowsBios = false;
            request_.mbr = false;
            request_.syslinux = true;
        }
        if (grub2 || grub4dos)
        {
            request_.windowsBios = false;
            request_.grub2 = grub2;
            request_.grub4dos = grub4dos;
        }
        rufus::IsoWriteCheck checked;
        const int checkedError = rufus::CheckIsoWriteImage(request_, getuid(), checked, nullptr, true);
        QVERIFY2(checkedError == 0, qPrintable(checked.problem));
        QCOMPARE(checked.legacyWinPe, setup);
        QVERIFY(!checked.partition.started && !QFile::exists(request_.path));
        QVERIFY(checked.tree.files > 0 && checked.problem.isEmpty());
        QCOMPARE(checked.efiPath.isEmpty(), !efiOnly && !fat16);
        if (ordinaryPe)
        {
            QVERIFY(!HAS_WININST(checked.analysis.report));
            QCOMPARE(checked.windows.state, WINDOWS_IMAGE_UNCHECKED);
            QCOMPARE(wimOpens, 0);
        }
        else if (metadata == QStringLiteral("vista") && !minint && !syslinux)
        {
            QCOMPARE(checked.windows.state, WINDOWS_IMAGE_UNKNOWN);
            QCOMPARE(checked.windows.metadata.raw_version.major, quint16(6));
            QCOMPARE(checked.windows.metadata.raw_version.minor, quint16(0));
            QCOMPARE(checked.analysis.report.win_version.major, quint16(0));
        }
        const RUFUS_BOOT_OPTIONS options = {BT_IMAGE, request_.fileSystem, TT_BIOS,
            false, request_.enhancedInstall, false, false};
        QCOMPARE(GetBootInstallType(&checked.analysis.report, &options),
            syslinux ? RUFUS_BOOT_INSTALL_SYSLINUX : RUFUS_BOOT_INSTALL_PBR);
        if (fat16)
            QCOMPARE(GetPartitionBootRecord(BT_IMAGE, FS_FAT16, &checked.analysis.report), RUFUS_PBR_FAT16_STANDARD);
        if (syslinux)
        {
            QCOMPARE(checked.analysis.report.sl_version, quint16(0x407));
            QVERIFY(!checked.syslinuxResources.loader.isEmpty());
        }
        if (grub2 || grub4dos)
            QCOMPARE(GetSecondaryBootType(BT_IMAGE, &checked.analysis.report, false),
                grub2 ? int(BT_GRUB2) : int(BT_GRUB4DOS));
    }

    void windowsSecondaryBootSource_data()
    {
        QTest::addColumn<bool>("grub2");
        QTest::addColumn<bool>("reactos");
        QTest::newRow("modern-BOOTMGR-GRUB2-select-core") << true << false;
        QTest::newRow("modern-BOOTMGR-GRUB4DOS-embedded-SBR") << false << false;
        QTest::newRow("ReactOS-Syslinux-PBR-GRUB2-select-core") << true << true;
        QTest::newRow("ReactOS-Syslinux-PBR-GRUB4DOS-embedded-SBR") << false << true;
    }

    void windowsSecondaryBootSource()
    {
        QFETCH(bool, grub2);
        QFETCH(bool, reactos);
        const QString tree = directory_.filePath(QStringLiteral("tree"));
        if (reactos)
            put(tree + QStringLiteral("/freeldr.sys"), "image-owned ReactOS loader");
        else
        {
            putWindows(QStringLiteral("win11"));
            QVERIFY(QFile::remove(tree + QStringLiteral("/bootmgr.efi")));
            put(tree + QStringLiteral("/bootmgr"), "image-owned BIOS manager");
        }
        if (grub2)
        {
            QByteArray module(4096, '\0');
            const char banner[] = "GRUB  version %s\0" "2.12";
            module.replace(64, sizeof(banner), QByteArray(banner, sizeof(banner)));
            put(tree + QStringLiteral("/boot/grub/i386-pc/normal.mod"), module);
        }
        else
            put(tree + QStringLiteral("/grldr"), "image-owned GRUB4DOS loader");
        makeImage(QStringLiteral("marker"), "BIOS source");
        selectWindowsBios(true, false);
        if (reactos)
        {
            request_.windowsBios = request_.mbr = request_.enhancedInstall = false;
            request_.syslinux = true;
            request_.fileSystem = FS_FAT32;
            request_.clusterSize = 512;
        }
        rufus::IsoWriteCheck checked;
        QCOMPARE(rufus::CheckIsoWriteSource(request_, getuid(), checked), 0);
        QVERIFY(!checked.legacyWinPe && !checked.partition.started);
        QCOMPARE(checked.windows.state, reactos ? WINDOWS_IMAGE_UNCHECKED : WINDOWS_IMAGE_KNOWN);
        QCOMPARE(GetSecondaryBootType(BT_IMAGE, &checked.analysis.report, false),
            grub2 ? int(BT_GRUB2) : int(BT_GRUB4DOS));
        QCOMPARE(check(checked), grub2 ? ENOTSUP : 0);
        if (grub2)
        {
            const QByteArray core(700, '\x17');
            request_.grub2CorePath = directory_.filePath(QStringLiteral("core.img"));
            put(request_.grub2CorePath, core);
            request_.grub2CoreSize = quint64(core.size());
            request_.grub2CoreSha256 = QCryptographicHash::hash(core, QCryptographicHash::Sha256);
            QCOMPARE(check(checked), 0);
        }
        if (reactos)
        {
            QVERIFY(HAS_REACTOS(checked.analysis.report) && !HAS_SYSLINUX(checked.analysis.report));
            const RUFUS_BOOT_OPTIONS options = {BT_IMAGE, request_.fileSystem, TT_BIOS,
                false, false, true, false};
            QCOMPARE(GetBootInstallType(&checked.analysis.report, &options), RUFUS_BOOT_INSTALL_SYSLINUX);
            request_.windowsRufusMbr = false;
            QCOMPARE(check(checked), 0); // Freeze the same global Alt+A choice as the original.
        }
        QVERIFY(!QFile::exists(request_.path));
    }

    void olderWindowsBiosPrerequisites_data()
    {
        QTest::addColumn<QString>("kind");
        QTest::addColumn<bool>("udf");
        QTest::addColumn<QString>("efiLayout");
        for (const char* kind : {"win7", "win8", "win8.1"})
        for (bool udf : {false, true})
        for (const char* layout : {"bios", "root-efi", "fallback-efi"})
            QTest::newRow((QByteArray(kind) + (udf ? "-UDF-" : "-ISO-") + layout).constData())
                << QString::fromLatin1(kind) << udf << QString::fromLatin1(layout);
    }

    void olderWindowsBiosPrerequisites()
    {
        QFETCH(QString, kind); QFETCH(bool, udf); QFETCH(QString, efiLayout);
        putWindows(kind);
        const bool efi = efiLayout != QStringLiteral("bios");
        const bool fallback = efiLayout == QStringLiteral("fallback-efi");
        if (!efi)
            QVERIFY(QFile::remove(directory_.filePath(QStringLiteral("tree/bootmgr.efi"))));
        put(directory_.filePath(QStringLiteral("tree/BoOtMgR")), QByteArray("opaque BIOS boot manager\n"));
        makeImage(fallback ? QStringLiteral("eFi/BoOt/BoOtX64.EfI") : QStringLiteral("bios-payload"),
            fallback ? efiHeader() : QByteArray("BIOS-only\n"), udf, true);
        selectWindowsBios(false, false);
        fault = Fault::WindowsEfiReadError; // A plain BIOS job must not read a fallback EFI header.
        for (bool rufusMbr : {true, false})
        {
            request_.windowsRufusMbr = rufusMbr;
            wimOpens = 0;
            rufus::IsoWriteCheck result;
            QCOMPARE(check(result), 0);
            QCOMPARE(result.windows.state, WINDOWS_IMAGE_KNOWN);
            QCOMPARE(result.analysis.report.win_version.major, quint16(kind == QStringLiteral("win7") ? 7 : 8));
            QCOMPARE(result.analysis.report.win_version.minor, quint16(kind == QStringLiteral("win8.1") ? 1 : 0));
            QCOMPARE(wimOpens, 1); // Older images offer no customization or edition chooser.
            QVERIFY(result.windowsInstall.editions.isEmpty());
            QVERIFY(!rufus::IsWindowsBiosImage(result.analysis, result.windows)); // Default remains modern-only.
            QVERIFY(rufus::IsWindowsBiosImage(result.analysis, result.windows, true));
            QCOMPARE(result.extraPartitions, quint8(0));
            QCOMPARE(result.partition.offset, testMiB);
            QCOMPARE(result.partition.size, quint64(66060288));
            QCOMPARE(result.partition.uefi_ntfs_size, quint64(0));
            QVERIFY(result.efiPath.isEmpty() && !result.splitInstallWim && !result.partition.started);
            QVERIFY(result.tree.files > 0);
            QCOMPARE(extractionLimit, quint64(0));
            if (efi && !fallback)
                QVERIFY(HAS_WIN7_EFI(result.analysis.report));
        }
    }

    void olderWindowsEfiPrerequisites_data()
    {
        QTest::addColumn<QString>("kind");
        QTest::addColumn<bool>("udf");
        QTest::addColumn<int>("filesystem");
        QTest::addColumn<bool>("bios");
        QTest::addColumn<bool>("repair");
        for (const char* kind : {"win7", "win8", "win8.1"})
        for (bool udf : {false, true})
        for (int filesystem : {FS_FAT16, FS_FAT32, FS_NTFS})
        for (bool bios : {false, true})
        for (bool repair : {false, true})
        {
            const QByteArray name = QByteArray(kind) + (udf ? "-UDF-" : "-ISO-")
                + (filesystem == FS_NTFS ? "NTFS-" : filesystem == FS_FAT16 ? "FAT16-" : "FAT32-")
                + (bios ? "dual-" : "UEFI-") + (repair ? "WIM-repair" : "regular-fallback");
            QTest::newRow(name.constData()) << QString::fromLatin1(kind) << udf << filesystem << bios << repair;
        }
    }

    void olderWindowsEfiPrerequisites()
    {
        QFETCH(QString, kind); QFETCH(bool, udf); QFETCH(int, filesystem);
        QFETCH(bool, bios); QFETCH(bool, repair);
        const QByteArray loader = efiHeader();
        putWindows(kind, repair ? loader : QByteArray());
        if (bios)
        {
            put(directory_.filePath(QStringLiteral("tree/bootmgr")), "BIOS manager");
            selectWindowsBios(true, filesystem == FS_NTFS);
        }
        // UEFI-only sources need root bootmgr.efi, not a BIOS BOOTMGR.
        makeImage(repair ? QStringLiteral("bootmgr.efi") : QStringLiteral("EFI/BOOT/BOOTX64.EFI"),
            repair ? QByteArray("root manager is not the fallback") : loader, udf);
        request_.fileSystem = filesystem;
        request_.clusterSize = filesystem == FS_NTFS ? 4096 : filesystem == FS_FAT16 ? 2048 : 512;
        rufus::IsoWriteCheck result;
        QCOMPARE(check(result), 0);
        QCOMPARE(result.windows.state, WINDOWS_IMAGE_KNOWN);
        QCOMPARE(result.analysis.report.win_version.major, quint16(kind == QStringLiteral("win7") ? 7 : 8));
        QCOMPARE(result.analysis.report.win_version.minor, quint16(kind == QStringLiteral("win8.1") ? 1 : 0));
        QCOMPARE(result.windowsEfiRepair, repair ? loader : QByteArray());
        QCOMPARE(result.efiPath, QStringLiteral("/EFI/BOOT/BOOTX64.EFI"));
        QCOMPARE(bool(HAS_WIN7_EFI(result.analysis.report)), repair);
        QCOMPARE(wimOpens, repair ? 2 : 1); // No older customization/edition read.
        QVERIFY(result.windowsInstallPath.isEmpty() && result.windowsInstall.editions.isEmpty());
        QCOMPARE(result.extraPartitions, quint8(filesystem == FS_NTFS ? XP_UEFI_NTFS : 0));
        QCOMPARE(extractionLimit, quint64(filesystem == FS_NTFS ? 0 : UINT32_MAX));
        QVERIFY(result.tree.files > 0 && !result.partition.started && !result.splitInstallWim);
        QVERIFY(!rufus::IsWindowsBiosImage(result.analysis, result.windows)); // Qt gate stays modern-only.
        QCOMPARE(rufus::IsWindowsBiosImage(result.analysis, result.windows, true), bios);
    }

    void olderWindowsRequestPolicy_data()
    {
        QTest::addColumn<QString>("kind");
        QTest::addColumn<QString>("mode");
        for (const char* kind : {"win7", "win8", "win8.1"})
        for (const char* mode : {"enhanced", "dual", "fat32", "gpt", "customized",
                "fat32-plain", "fat32-bios-only", "fat16-bios", "fat16-uefi"})
            QTest::newRow((QByteArray(kind) + '-' + mode).constData())
                << QString::fromLatin1(kind) << QString::fromLatin1(mode);
    }

    void olderWindowsRequestPolicy()
    {
        QFETCH(QString, kind); QFETCH(QString, mode);
        putWindows(kind);
        put(directory_.filePath(QStringLiteral("tree/bootmgr")), QByteArray("opaque BIOS boot manager\n"));
        const bool biosOnly = mode == QStringLiteral("fat32-bios-only");
        if (biosOnly)
            QVERIFY(QFile::remove(directory_.filePath(QStringLiteral("tree/bootmgr.efi"))));
        makeImage(biosOnly ? QStringLiteral("bios-payload") : QStringLiteral("EFI/BOOT/BOOTX64.EFI"),
            biosOnly ? QByteArray("BIOS-only") : efiHeader());
        const bool enhanced = mode == QStringLiteral("enhanced") || mode == QStringLiteral("dual")
            || mode == QStringLiteral("fat32") || biosOnly;
        selectWindowsBios(enhanced, mode == QStringLiteral("dual"));
        const bool fat32 = mode == QStringLiteral("fat32") || mode == QStringLiteral("fat32-plain") || biosOnly;
        if (fat32)
        {
            request_.fileSystem = FS_FAT32;
            request_.clusterSize = 512;
        }
        const bool fat16Bios = mode == QStringLiteral("fat16-bios");
        const bool fat16Uefi = mode == QStringLiteral("fat16-uefi");
        if (fat16Bios || fat16Uefi)
        {
            request_.fileSystem = FS_FAT16;
            request_.clusterSize = 2048;
        }
        if (mode == QStringLiteral("gpt") || fat16Uefi)
        {
            request_.windowsBios = request_.mbr = false;
            request_.uefiNtfs = !fat16Uefi;
        }
        if (mode == QStringLiteral("customized"))
            request_.windowsFlags = UNATTEND_NO_DATA_COLLECTION;
        rufus::IsoWriteCheck result;
        const bool allowed = mode == QStringLiteral("dual") || mode == QStringLiteral("fat32")
            || mode == QStringLiteral("gpt") || biosOnly || fat16Uefi;
        const int expected = allowed ? 0 : ENOTSUP;
        QCOMPARE(check(result), expected);
        QCOMPARE(result.windows.state, WINDOWS_IMAGE_KNOWN);
        QCOMPARE(result.tree.files > 0, allowed);
        QVERIFY(!result.partition.started);
        QCOMPARE(result.problem.isEmpty(), allowed);
    }

    void olderWindowsSourceRefusals_data()
    {
        QTest::addColumn<QString>("mode");
        QTest::addColumn<bool>("uefi");
        for (bool uefi : {false, true})
        for (const char* mode : {"missing-bootmgr", "missing-install", "unknown", "invalid", "no-revision",
                "mixed", "legacy", "pending"})
        {
            if (uefi && strcmp(mode, "missing-bootmgr") == 0) continue;
            QTest::newRow((QByteArray(uefi ? "UEFI-" : "BIOS-") + mode).constData())
                << QString::fromLatin1(mode) << uefi;
        }
    }

    void olderWindowsSourceRefusals()
    {
        QFETCH(QString, mode); QFETCH(bool, uefi);
        const bool unknown = mode == QStringLiteral("unknown") || mode == QStringLiteral("invalid")
            || mode == QStringLiteral("no-revision");
        putWindows(unknown ? mode : QStringLiteral("win7"));
        const QString tree = directory_.filePath(QStringLiteral("tree"));
        if (mode != QStringLiteral("missing-bootmgr"))
            put(tree + QStringLiteral("/bootmgr"), QByteArray("opaque BIOS boot manager\n"));
        if (mode == QStringLiteral("missing-install"))
            QVERIFY(QFile::remove(tree + QStringLiteral("/sources/install.wim")));
        makeImage();
        if (mode == QStringLiteral("mixed")) fault = Fault::GrubWithSyslinux;
        if (mode == QStringLiteral("legacy")) fault = Fault::LegacyWindows;
        if (mode == QStringLiteral("pending")) fault = Fault::ContentPending;
        if (!uefi)
            selectWindowsBios(false, false);
        rufus::IsoWriteCheck result;
        const bool admitted = mode == QStringLiteral("missing-install") || unknown
            || (uefi && (mode == QStringLiteral("mixed") || mode == QStringLiteral("legacy")));
        QCOMPARE(check(result), admitted ? 0 : ENOTSUP);
        QCOMPARE(rufus::IsWindowsBiosImage(result.analysis, result.windows, true),
            admitted && mode != QStringLiteral("mixed") && mode != QStringLiteral("legacy"));
        QCOMPARE(result.tree.files > 0, admitted);
        QVERIFY(!result.partition.started);
        QCOMPARE(result.problem.isEmpty(), admitted);
    }

    void windowsBiosRefusals_data()
    {
        QTest::addColumn<QString>("change");
        QTest::addColumn<int>("expected");
        QTest::addColumn<bool>("fat32");
        for (bool fat32 : {false, true})
        {
            for (const char* change : {"missing-bootmgr", "grub", "legacy", "vendor-efi"})
                QTest::newRow((QByteArray(fat32 ? "FAT32-" : "NTFS-") + change).constData())
                    << QString::fromLatin1(change) << ENOTSUP << fat32;
            for (const char* change : {"unknown", "invalid", "no-revision", "syslinux", "missing-install"})
                QTest::newRow((QByteArray(fat32 ? "FAT32-" : "NTFS-") + change).constData())
                    << QString::fromLatin1(change) << 0 << fat32;
            QTest::newRow(fat32 ? "FAT32-win7" : "NTFS-win7") << QStringLiteral("win7") << 0 << fat32;
            QTest::newRow(fat32 ? "FAT32-bad-efi-dual" : "NTFS-bad-efi-dual") << QStringLiteral("bad-efi-dual") << EBADMSG << fat32;
            QTest::newRow(fat32 ? "FAT32-bad-efi-bios" : "NTFS-bad-efi-bios") << QStringLiteral("bad-efi-bios") << (fat32 ? EBADMSG : 0) << fat32;
            QTest::newRow(fat32 ? "FAT32-efi-read" : "NTFS-efi-read") << QStringLiteral("efi-read") << EIO << fat32;
            QTest::newRow(fat32 ? "FAT32-efi-cancel" : "NTFS-efi-cancel") << QStringLiteral("efi-cancel") << ECANCELED << fat32;
            QTest::newRow(fat32 ? "FAT32-source-change" : "NTFS-source-change") << QStringLiteral("source-change") << ESTALE << fat32;
            QTest::newRow(fat32 ? "FAT32-capacity" : "NTFS-capacity") << QStringLiteral("capacity") << ENOSPC << fat32;
        }
    }

    void windowsBiosRefusals()
    {
        QFETCH(QString, change); QFETCH(int, expected); QFETCH(bool, fat32);
        const bool differentVersion = change == QStringLiteral("unknown") || change == QStringLiteral("invalid")
            || change == QStringLiteral("no-revision") || change == QStringLiteral("win7");
        putWindows(differentVersion ? change : QStringLiteral("win11"));
        const QString tree = directory_.filePath(QStringLiteral("tree"));
        if (change != QStringLiteral("missing-bootmgr"))
            put(tree + QStringLiteral("/bootmgr"), QByteArray("opaque BIOS boot manager\n"));
        if (change == QStringLiteral("missing-install"))
            QVERIFY(QFile::remove(tree + QStringLiteral("/sources/install.wim")));
        if (change == QStringLiteral("syslinux")) fault = Fault::GrubWithSyslinux;
        if (change == QStringLiteral("grub")) fault = Fault::FatlessGrub;
        if (change == QStringLiteral("legacy")) fault = Fault::LegacyWindows;
        if (change == QStringLiteral("efi-read")) fault = Fault::WindowsEfiReadError;
        if (change == QStringLiteral("efi-cancel")) fault = Fault::WindowsEfiCancel;
        if (change == QStringLiteral("source-change")) fault = Fault::ChangeAfterTree;
        if (change == QStringLiteral("capacity")) fault = Fault::TooLargeTree;
        const bool corruptEfi = change == QStringLiteral("bad-efi-dual") || change == QStringLiteral("bad-efi-bios");
        QByteArray header = efiHeader();
        if (corruptEfi) qToLittleEndian<quint16>(3, header.data() + 220);
        const QString path = change == QStringLiteral("vendor-efi")
            ? QStringLiteral("EFI/vendor/BOOTX64.EFI") : QStringLiteral("EFI/BOOT/BOOTX64.EFI");
        makeImage(path, header);
        const bool dual = change != QStringLiteral("bad-efi-bios");
        selectWindowsBios(dual || fat32, dual && !fat32);
        if (fat32)
        {
            request_.fileSystem = FS_FAT32;
            request_.clusterSize = 512;
        }
        rufus::IsoWriteCheck result;
        QCOMPARE(check(result), expected);
        QVERIFY(!result.partition.started);
        if (differentVersion)
        {
            if (change == QStringLiteral("win7"))
                QCOMPARE(result.analysis.report.win_version.major, quint16(7));
            else
                QVERIFY(result.windows.state != WINDOWS_IMAGE_KNOWN);
        }
        if (expected != 0)
            QVERIFY(!result.problem.isEmpty());
    }

    void windowsBiosInvalidSettingsBeforeSource()
    {
        selectWindowsBios(false, false);
        // Freeze a structurally valid identity for a nonexistent source.
        request_.imagePath = directory_.filePath(QStringLiteral("absent.iso"));
        request_.imageInode = 1;
        request_.imageSize = testMiB;
        request_.imageModified = request_.imageChanged = QStringLiteral("1.0");
        for (int fs : {FS_EXFAT, FS_EXT2})
        {
            request_.fileSystem = fs;
            rufus::IsoWriteCheck result;
            QCOMPARE(check(result), EINVAL);
            QVERIFY(!result.partition.started);
            QCOMPARE(result.tree.files, quint64(0));
            QCOMPARE(wimOpens, 0);
        }
        request_.fileSystem = FS_NTFS;
        request_.uefiNtfs = true;
        rufus::IsoWriteCheck result;
        QCOMPARE(check(result), EINVAL);
        QCOMPARE(wimOpens, 0);
        request_.uefiNtfs = false;
        request_.clusterSize = 512;
        QCOMPARE(check(result), EINVAL);
        QCOMPARE(wimOpens, 0);
    }

    void windowsPrerequisites()
    {
        QFETCH(bool, udf);
        QFETCH(bool, ntfs);
        putWindows(QStringLiteral("win11"));
        makeImage(QStringLiteral("EFI/BOOT/BOOTX64.EFI"), efiHeader(), udf, true);
        request_.fileSystem = ntfs ? FS_NTFS : FS_FAT32;
        request_.clusterSize = ntfs ? 4096 : 512;
        rufus::IsoWriteCheck result;
        QCOMPARE(check(result), 0);
        QCOMPARE(result.windows.state, WINDOWS_IMAGE_KNOWN);
        QCOMPARE(result.windowsInstallPath, QStringLiteral("/sources/install.wim"));
        QCOMPARE(result.windowsInstallError, 0);
        QCOMPARE(result.windowsInstall.editions.size(), 1);
        QCOMPARE(result.windowsInstall.editions[0].image, 1);
        QCOMPARE(result.windowsInstall.editions[0].name.size(), 40000);
        QVERIFY(result.windowsInstall.nonstandardNames);
        QCOMPARE(result.windowsInstall.uiLanguage, QStringLiteral("en-US"));
        QVERIFY(!result.windowsInstall.languageFromImage);
        QCOMPARE(result.windowsInstall.languageError, int(WIMLIB_ERR_OPEN));
        QCOMPARE(result.analysis.report.win_version.major, quint16(11));
        QVERIFY(!(result.analysis.pending & ISO_PENDING_WINDOWS));
        QCOMPARE(result.extraPartitions, quint8(ntfs ? 4 : 0));
        QCOMPARE(result.partition.offset, testMiB);
        QCOMPARE(result.partition.size, quint64(ntfs ? 64929792 : 66028032));
        QCOMPARE(result.partition.uefi_ntfs_size, quint64(ntfs ? 1064448 : 0));
        QCOMPARE(extractionLimit, ntfs ? uint64_t(0) : uint64_t(UINT32_MAX));
        QCOMPARE(result.choices.count, ntfs ? 2 : 3);
        QCOMPARE(result.choices.mode[0], IMAGE_WRITE_ISO);
        QCOMPARE(result.choices.mode[result.choices.count - 1], IMAGE_WRITE_DD);
        QVERIFY(!result.partition.started);
    }

    void silentSelection_data()
    {
        QTest::addColumn<bool>("udf");
        QTest::addColumn<QString>("change");
        QTest::addColumn<int>("expected");
        for (const bool udf : {false, true})
        {
            const QByteArray prefix = udf ? "UDF-" : "ISO-";
            QTest::newRow((prefix + "selected").constData()) << udf << QString() << 0;
            QTest::newRow((prefix + "esd").constData()) << udf << QStringLiteral("esd") << 0;
            QTest::newRow((prefix + "uppercase").constData()) << udf << QStringLiteral("uppercase") << 0;
            QTest::newRow((prefix + "edition").constData()) << udf << QStringLiteral("edition") << ESTALE;
            QTest::newRow((prefix + "archive").constData()) << udf << QStringLiteral("archive") << ESTALE;
            QTest::newRow((prefix + "multiple").constData()) << udf << QStringLiteral("multiple") << ENOTSUP;
            QTest::newRow((prefix + "nonstandard").constData()) << udf << QStringLiteral("nonstandard") << ENOTSUP;
            QTest::newRow((prefix + "unconfirmed").constData()) << udf << QStringLiteral("unconfirmed") << EINVAL;
            QTest::newRow((prefix + "read-error").constData()) << udf << QStringLiteral("read-error") << EIO;
            QTest::newRow((prefix + "cancelled").constData()) << udf << QStringLiteral("cancelled") << ECANCELED;
            QTest::newRow((prefix + "changed-source").constData()) << udf << QStringLiteral("changed-source") << ESTALE;
        }
    }

    void silentSelection()
    {
        QFETCH(bool, udf);
        QFETCH(QString, change);
        QFETCH(int, expected);
        putWindows(QStringLiteral("win11"));
        const QString archive = directory_.filePath(QStringLiteral("tree/sources/install.wim"));
        if (change == QStringLiteral("esd"))
            QVERIFY(QFile::rename(archive, directory_.filePath(QStringLiteral("tree/sources/install.esd"))));
        if (change == QStringLiteral("uppercase"))
        {
            QVERIFY(QFile::rename(archive, directory_.filePath(QStringLiteral("tree/sources/INSTALL.WIM"))));
            QVERIFY(QDir().rename(directory_.filePath(QStringLiteral("tree/sources")),
                directory_.filePath(QStringLiteral("tree/SOURCES"))));
        }
        if (change == QStringLiteral("multiple"))
            QVERIFY(QFile::copy(archive, directory_.filePath(QStringLiteral("tree/sources/install.esd"))));
        if (change == QStringLiteral("nonstandard"))
        {
            QVERIFY(QDir().mkpath(directory_.filePath(QStringLiteral("tree/x64/sources"))));
            QVERIFY(QFile::rename(archive, directory_.filePath(QStringLiteral("tree/x64/sources/install.wim"))));
        }
        makeImage(QStringLiteral("EFI/BOOT/BOOTX64.EFI"), efiHeader(), udf);
        request_.isDevice = true;
        request_.windowsFlags = UNATTEND_SILENT_INSTALL | UNATTEND_SET_USER |
            UNATTEND_NO_DATA_COLLECTION | UNATTEND_DUPLICATE_LOCALE;
        request_.windowsUsername = QStringLiteral("TestUser");
        request_.windowsInstallPath = QStringLiteral("/sources/install.wim");
        request_.windowsEdition = 1;
        request_.windowsSilentConfirmed = true;
        request_.windowsRegion = {QStringLiteral("ro-RO"), QStringLiteral("ro-RO"), QStringLiteral("en-GB"),
            QStringLiteral("en-US"), {}, QStringLiteral("GTB Standard Time")};
        if (change == QStringLiteral("esd")) request_.windowsInstallPath = QStringLiteral("/sources/install.esd");
        if (change == QStringLiteral("uppercase")) request_.windowsInstallPath = QStringLiteral("/SOURCES/INSTALL.WIM");
        if (change == QStringLiteral("edition")) request_.windowsEdition = 2;
        if (change == QStringLiteral("archive")) request_.windowsInstallPath = QStringLiteral("/sources/install.esd");
        if (change == QStringLiteral("unconfirmed")) request_.windowsSilentConfirmed = false;
        if (change == QStringLiteral("read-error")) fault = Fault::InstallReadError;
        if (change == QStringLiteral("cancelled")) fault = Fault::InstallReadCancelled;
        if (change == QStringLiteral("changed-source")) fault = Fault::InstallSourceChanged;
        rufus::IsoWriteCheck result;
        QCOMPARE(check(result), expected);
        QVERIFY(!result.partition.started);
        QCOMPARE(result.problem.isEmpty(), expected == 0);
        if (expected == 0)
        {
            QCOMPARE(result.windowsInstall.editions[0].image, 1);
            QCOMPARE(result.windowsInstall.uiLanguage, QStringLiteral("en-US"));
        }
        if (change == QStringLiteral("multiple") || change == QStringLiteral("nonstandard"))
        {
            request_.windowsFlags = UNATTEND_NO_DATA_COLLECTION;
            request_.windowsUsername.clear();
            request_.windowsInstallPath.clear();
            request_.windowsEdition = 0;
            request_.windowsRegion = {};
            request_.windowsSilentConfirmed = false;
            QCOMPARE(check(result), 0); // Restriction is specific to silent installation.
        }
    }

    void windowsInstallChoicesFailure_data()
    {
        QTest::addColumn<bool>("udf");
        QTest::addColumn<int>("change");
        QTest::addColumn<int>("expected");
        QTest::newRow("ISO-read-failure") << false << int(Fault::InstallReadError) << 0;
        QTest::newRow("UDF-read-failure") << true << int(Fault::InstallReadError) << 0;
        QTest::newRow("ISO-cancel") << false << int(Fault::InstallReadCancelled) << ECANCELED;
        QTest::newRow("UDF-cancel") << true << int(Fault::InstallReadCancelled) << ECANCELED;
        QTest::newRow("ISO-changed-source") << false << int(Fault::InstallSourceChanged) << ESTALE;
        QTest::newRow("UDF-changed-source") << true << int(Fault::InstallSourceChanged) << ESTALE;
    }

    void windowsInstallChoicesFailure()
    {
        QFETCH(bool, udf);
        QFETCH(int, change);
        QFETCH(int, expected);
        putWindows(QStringLiteral("win11"));
        makeImage(QStringLiteral("EFI/BOOT/BOOTX64.EFI"), efiHeader(), udf);
        request_.windowsFlags = UNATTEND_NO_DATA_COLLECTION;
        fault = Fault(change);
        rufus::IsoWriteCheck result;
        QCOMPARE(check(result), expected);
        QCOMPARE(result.windows.state, WINDOWS_IMAGE_KNOWN);
        QCOMPARE(result.windowsInstallPath, QStringLiteral("/sources/install.wim"));
        QCOMPARE(wimOpens, 2); // Version and edition reads; no retry.
        QVERIFY(result.tree.files > 0);
        QVERIFY(!result.partition.started);
        if (fault != Fault::InstallSourceChanged)
        {
            const int readError = cancelled ? WIMLIB_ERR_ABORTED_BY_PROGRESS : WIMLIB_ERR_READ;
            QCOMPARE(result.windowsInstallError, readError);
            QVERIFY(result.windowsInstall.editions.isEmpty());
        }
        if (expected == 0)
            QVERIFY(result.problem.isEmpty()); // Ordinary customization is still available.
        else
            QVERIFY(!result.problem.isEmpty());
    }

    void windowsOemExtraction_data()
    {
        QTest::addColumn<bool>("udf");
        QTest::newRow("ISO9660") << false;
        QTest::newRow("UDF") << true;
    }

    void windowsOemExtraction()
    {
        QFETCH(bool, udf);
        putWindows(QStringLiteral("win11-25h2"));
        makeImage(QStringLiteral("EFI/BOOT/BOOTX64.EFI"), efiHeader(), udf);
        request_.fileSystem = FS_NTFS;
        request_.clusterSize = 4096;
        request_.windowsFlags = rufus::WindowsOemFlags;
        request_.windowsUsername = QStringLiteral("Rufus.Test");
        rufus::IsoWriteCheck result;
        QCOMPARE(check(result), 0);
        QTemporaryDir output;
        QVERIFY(output.isValid());
        const QByteArray target = QFile::encodeName(output.path());
        const QByteArray source = QFile::encodeName(request_.imagePath);
        const QByteArray label = request_.label.toUtf8();
        const ISO_EXTRACT_OPTIONS options = {{true, true, {0, 0}}, label.constData(), source.constData(), false};
        ISO_EXTRACT_RESULT extracted = {};
        QCOMPARE(ExtractIsoTree(source.constData(), target.constData(), &options, &extracted), 0);
        QCOMPARE(extracted.bytes_written, result.tree.bytes_written);
        QCOMPARE(rufus::ApplyWindowsCustomization(target.constData(), ARCH_X86_64, result.analysis.report.win_version.build,
            request_.windowsFlags, request_.windowsUsername), 0);
        QFile file(output.filePath(QStringLiteral("sources/$OEM$/$$/Panther/unattend.xml")));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray xml = file.readAll();
        QVERIFY(xml.contains("<Name>Rufus_Test</Name>"));
        QVERIFY(xml.contains("<ProtectYourPC>3</ProtectYourPC>"));
        QCOMPARE(xml.count("SkuSiPolicy.p7b"), 1);
        QVERIFY(!xml.contains("windowsPE"));
        QVERIFY(!QFile::exists(output.filePath(QStringLiteral("Autounattend.xml"))));
    }

    void windowsOemRefusals()
    {
        putWindows(QStringLiteral("win11"));
        makeImage();
        rufus::IsoWriteCheck result;
        request_.windowsFlags = UNATTEND_DUPLICATE_LOCALE;
        QCOMPARE(check(result), ENOTSUP);
        request_.windowsFlags = UNATTEND_SECUREBOOT_TPM_MINRAM;
        QCOMPARE(check(result), 0);
        request_.windowsFlags = UNATTEND_SET_USER;
        request_.windowsUsername = QStringLiteral("bad\nname");
        QCOMPARE(check(result), EINVAL);
        request_.windowsUsername.clear();
        request_.windowsFlags = UNATTEND_NO_DATA_COLLECTION;
        put(directory_.filePath(QStringLiteral("tree/sources/$OEM$/$$/Panther/unattend.xml")), "original answer");
        makeImage();
        QCOMPARE(check(result), ENOTSUP);
        QVERIFY(result.analysis.report.has_panther_unattend);
        request_.windowsFlags = 0;
        QCOMPARE(check(result), 0); // Existing customized images still copy normally.
    }

    void customizationNeedsWindowsImage()
    {
        makeImage();
        request_.windowsFlags = UNATTEND_NO_DATA_COLLECTION;
        rufus::IsoWriteCheck result;
        QCOMPARE(check(result), ENOTSUP);
        QVERIFY(result.problem.contains(QStringLiteral("Windows customization")));
    }

    void windowsMetadataPolicy_data()
    {
        QTest::addColumn<QString>("kind");
        QTest::addColumn<int>("expected");
        QTest::newRow("unknown") << QStringLiteral("unknown") << 0;
        QTest::newRow("invalid") << QStringLiteral("invalid") << 0;
        QTest::newRow("missing-revision") << QStringLiteral("no-revision") << 0;
        QTest::newRow("Windows-7") << QStringLiteral("win7") << 0;
        QTest::newRow("first-image-Windows-7") << QStringLiteral("multi") << 0;
    }

    void windowsMetadataPolicy()
    {
        QFETCH(QString, kind); QFETCH(int, expected);
        putWindows(kind);
        makeImage();
        request_.fileSystem = FS_NTFS;
        request_.clusterSize = 4096;
        rufus::IsoWriteCheck result;
        QCOMPARE(check(result), expected);
        const bool allowed = expected == 0;
        QCOMPARE(result.problem.isEmpty(), allowed);
        QCOMPARE(result.tree.files > 0, allowed);
        QCOMPARE(extractionLimit, allowed ? quint64(0) : UINT64_MAX);
        if (allowed)
        {
            const quint16 version = kind == QStringLiteral("win7") || kind == QStringLiteral("multi")
                ? 7 : kind == QStringLiteral("no-revision") ? 11 : 0;
            QCOMPARE(result.analysis.report.win_version.major, version);
            QCOMPARE(wimOpens, 1); // No retry with a later archive image or edition lookup.
            QVERIFY(result.windowsInstall.editions.isEmpty());
        }
    }

    void ntfsLargeFileAndFilesystemRestrictions()
    {
        makeImage();
        request_.fileSystem = FS_NTFS;
        request_.clusterSize = 4096;
        fault = Fault::LargeFile;
        rufus::IsoWriteCheck result;
        QCOMPARE(check(result), 0);
        QCOMPARE(extractionLimit, uint64_t(0));
        fault = Fault::NeedsNtfs;
        QCOMPARE(check(result), 0);
        fault = Fault::FatlessGrub;
        QCOMPARE(check(result), 0);
        fault = Fault::TooLargeTree;
        QCOMPARE(check(result), ENOSPC);
        fault = Fault::NtfslessGrub;
        extractionLimit = UINT64_MAX;
        QCOMPARE(check(result), ENOTSUP);
        QVERIFY(result.problem.contains(QStringLiteral("selected filesystem")));
        QCOMPARE(extractionLimit, UINT64_MAX);
        request_.fileSystem = FS_FAT32;
        request_.clusterSize = 512;
        fault = Fault::LargeFile;
        QCOMPARE(check(result), ENOTSUP);
    }

    void enhancedSplitPolicy_data()
    {
        QTest::addColumn<QString>("kind");
        QTest::addColumn<bool>("enhanced");
        QTest::addColumn<bool>("ntfs");
        QTest::addColumn<int>("change");
        QTest::addColumn<int>("expected");
        QTest::addColumn<bool>("bios");
        struct SplitCase
        {
            const char* name;
            bool enhanced;
            bool ntfs;
            Fault fault;
            int expected;
            bool bios;
        };
        const SplitCase cases[] = {
            {"default-large-WIM", false, false, Fault::LargeWim, ENOTSUP, false},
            {"enhanced-large-WIM", true, false, Fault::LargeWim, 0, false},
            {"enhanced-small-WIM", true, false, Fault::None, 0, false},
            {"enhanced-NTFS", true, true, Fault::LargeWim, 0, false},
            {"ordinary-large-file", true, false, Fault::LargeFile, ENOTSUP, false},
            {"split-overhead-does-not-fit", true, false, Fault::LargeWimSpace, ENOSPC, false},
            {"BIOS-default-large-WIM", false, false, Fault::LargeWim, ENOTSUP, true},
            {"BIOS-enhanced-large-WIM", true, false, Fault::LargeWim, 0, true},
            {"BIOS-enhanced-small-WIM", true, false, Fault::None, 0, true},
            {"BIOS-enhanced-NTFS", true, true, Fault::LargeWim, 0, true},
            {"BIOS-ordinary-large-file", true, false, Fault::LargeFile, ENOTSUP, true},
            {"BIOS-split-space", true, false, Fault::LargeWimSpace, ENOSPC, true}
        };
        for (const char* kind : {"win11", "win7"})
        for (const SplitCase& test : cases)
            QTest::newRow((QByteArray(kind) + '-' + test.name).constData())
                << QString::fromLatin1(kind) << test.enhanced << test.ntfs << int(test.fault) << test.expected << test.bios;
    }

    void enhancedSplitPolicy()
    {
        QFETCH(QString, kind);
        QFETCH(bool, bios);
        QFETCH(bool, enhanced);
        QFETCH(bool, ntfs);
        QFETCH(int, change);
        QFETCH(int, expected);
        putWindows(kind);
        if (bios) put(directory_.filePath(QStringLiteral("tree/bootmgr")), QByteArray("BOOTMGR"));
        makeImage();
        if (bios) selectWindowsBios(enhanced, ntfs && enhanced);
        request_.enhancedInstall = enhanced;
        request_.fileSystem = ntfs ? FS_NTFS : FS_FAT32;
        request_.clusterSize = ntfs ? 4096 : 512;
        fault = Fault(change);
        rufus::IsoWriteCheck result;
        QCOMPARE(check(result), expected);
        const bool split = enhanced && !ntfs && (fault == Fault::LargeWim || fault == Fault::LargeWimSpace);
        QCOMPARE(result.splitInstallWim, split);
        QCOMPARE(splitSelected, split);
        QVERIFY(!result.partition.started);
        QVERIFY(!QFile::exists(request_.path));
        if (split)
            QVERIFY(result.tree.required_bytes > result.tree.bytes_written);
        if (expected == ENOSPC)
            QVERIFY(result.tree.bytes_written < result.partition.size);
    }

    void splitArchiveRestrictions_data()
    {
        QTest::addColumn<QString>("layout");
        QTest::addColumn<int>("expected");
        QTest::addColumn<bool>("bios");
        QTest::newRow("nested") << QStringLiteral("nested") << ENOTSUP << false;
        QTest::newRow("BIOS-nested") << QStringLiteral("nested") << ENOTSUP << true;
        QTest::newRow("multiple") << QStringLiteral("multiple") << ENOTSUP << false;
        QTest::newRow("BIOS-multiple") << QStringLiteral("multiple") << ENOTSUP << true;
        QTest::newRow("SWM-collision") << QStringLiteral("collision") << EEXIST << false;
        QTest::newRow("BIOS-SWM-collision") << QStringLiteral("collision") << EEXIST << true;
    }

    void splitArchiveRestrictions()
    {
        QFETCH(bool, bios);
        QFETCH(QString, layout);
        QFETCH(int, expected);
        putWindows(QStringLiteral("win11"));
        const QString tree = directory_.filePath(QStringLiteral("tree"));
        const QString wim = tree + QStringLiteral("/sources/install.wim");
        if (layout == QStringLiteral("nested"))
        {
            QVERIFY(QDir().mkpath(tree + QStringLiteral("/x64/sources")));
            QVERIFY(QFile::rename(wim, tree + QStringLiteral("/x64/sources/install.wim")));
        }
        else if (layout == QStringLiteral("multiple"))
            QVERIFY(QFile::copy(wim, tree + QStringLiteral("/sources/install.esd")));
        else
            put(tree + QStringLiteral("/sources/install2.swm"), "existing part");
        if (bios) put(tree + QStringLiteral("/bootmgr"), QByteArray("BOOTMGR"));
        makeImage();
        if (bios)
        {
            selectWindowsBios(true, false);
            request_.fileSystem = FS_FAT32;
            request_.clusterSize = 512;
        }
        request_.enhancedInstall = true;
        fault = Fault::LargeWim;
        rufus::IsoWriteCheck result;
        QCOMPARE(check(result), expected);
        QVERIFY(!result.problem.isEmpty());
        QVERIFY(!result.partition.started);
    }

    void windowsMetadataReadFailureAndCancellation_data()
    {
        QTest::addColumn<int>("change");
        QTest::addColumn<int>("expected");
        QTest::newRow("unreadable-WIM") << int(Fault::ReadError) << EIO;
        QTest::newRow("cancelled-WIM") << int(Fault::CancelRead) << ECANCELED;
    }

    void windowsMetadataReadFailureAndCancellation()
    {
        QFETCH(int, change);
        QFETCH(int, expected);
        putWindows(QStringLiteral("win11"));
        makeImage();
        request_.fileSystem = FS_NTFS;
        request_.clusterSize = 4096;
        fault = Fault(change);
        rufus::IsoWriteCheck result;
        QCOMPARE(check(result), expected);
        QVERIFY(!result.problem.isEmpty());
        QCOMPARE(extractionLimit, UINT64_MAX);
        QCOMPARE(result.tree.files, quint64(0));
    }

    void ntfsSettingsBeforeReading_data()
    {
        QTest::addColumn<QString>("label");
        QTest::addColumn<quint32>("cluster");
        QTest::addColumn<quint32>("volumeId");
        QTest::newRow("long-label") << QString(33, u'a') << quint32(4096) << quint32(0);
        QTest::newRow("NUL") << QString::fromUtf8("a\0b", 3) << quint32(4096) << quint32(0);
        QTest::newRow("FAT-cluster") << QStringLiteral("label") << quint32(512) << quint32(0);
        QTest::newRow("caller-serial") << QStringLiteral("label") << quint32(4096) << quint32(123);
    }

    void ntfsSettingsBeforeReading()
    {
        QFETCH(QString, label);
        QFETCH(quint32, cluster);
        QFETCH(quint32, volumeId);
        request_.fileSystem = FS_NTFS;
        request_.label = label;
        request_.clusterSize = cluster;
        request_.volumeId = volumeId;
        rufus::IsoWriteCheck result;
        QCOMPARE(check(result), EINVAL);
        QVERIFY(result.problem.contains(QStringLiteral("NTFS")));
        QCOMPARE(extractionLimit, UINT64_MAX);
    }

    void reportRestrictions_data()
    {
        QTest::addColumn<int>("change");
        QTest::addColumn<QString>("reason");
        QTest::newRow("content") << int(Fault::ContentPending) << QStringLiteral("incomplete");
        QTest::newRow("truncated") << int(Fault::Truncated) << QStringLiteral("incomplete");
        QTest::newRow("tree") << int(Fault::NoTree) << QStringLiteral("incomplete");
        QTest::newRow("GRUB-without-FAT") << int(Fault::FatlessGrub) << QStringLiteral("FAT32");
        QTest::newRow("NTFS") << int(Fault::NeedsNtfs) << QStringLiteral("FAT32");
        QTest::newRow("4GiB") << int(Fault::LargeFile) << QStringLiteral("FAT32");
        QTest::newRow("Kolibri-missing-loader") << int(Fault::Kolibri) << QStringLiteral("legacy ISO");
    }

    void reportRestrictions()
    {
        QFETCH(int, change);
        QFETCH(QString, reason);
        makeImage();
        fault = Fault(change);
        rufus::IsoWriteCheck result;
        QCOMPARE(check(result), change == int(Fault::Kolibri) ? ENOENT : ENOTSUP);
        QVERIFY2(result.problem.contains(reason), qPrintable(result.problem));
        QCOMPARE(fallbackReads, 0);
        QCOMPARE(result.tree.files, quint64(0));
    }

    void preservesDdOnlyPolicy()
    {
        makeImage(QStringLiteral("EFI/BOOT/BOOTX64.EFI"), efiHeader(), false, true,
            QStringLiteral("openSUSE-Tumbleweed"));
        rufus::IsoWriteCheck result;
        QCOMPARE(check(result), ENOTSUP);
        QVERIFY(result.problem.contains(QStringLiteral("policy")));
        QCOMPARE(result.choices.count, 1);
        QCOMPARE(result.choices.mode[0], IMAGE_WRITE_DD);
    }

    void fallbackRestrictions_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<QByteArray>("bytes");
        QTest::addColumn<int>("expected");
        QTest::newRow("vendor-directory") << QStringLiteral("EFI/vendor/BOOTX64.EFI") << efiHeader() << ENOTSUP;
        QTest::newRow("other-architecture-path") << QStringLiteral("EFI/BOOT/BOOTAA64.EFI") << efiHeader() << ENOTSUP;
        const QString path = QStringLiteral("EFI/BOOT/BOOTX64.EFI");
        // Rufus flags unreadable/empty loader content before the header check.
        QTest::newRow("empty") << path << QByteArray() << ENOTSUP;
        QTest::newRow("truncated") << path << efiHeader().left(200) << EBADMSG;
        QByteArray bytes = efiHeader();
        bytes[0] = 'x';
        QTest::newRow("bad-DOS") << path << bytes << ENOTSUP;
        bytes = efiHeader();
        bytes[128] = 'x';
        QTest::newRow("bad-PE") << path << bytes << ENOTSUP;
        bytes = efiHeader();
        qToLittleEndian<quint16>(0xaa64, bytes.data() + 132);
        QTest::newRow("wrong-machine") << path << bytes << ENOTSUP;
        bytes = efiHeader();
        qToLittleEndian<quint32>(0xfffffff0, bytes.data() + 60);
        QTest::newRow("offset-overflow") << path << bytes << EBADMSG;
        bytes = efiHeader();
        qToLittleEndian<quint16>(2, bytes.data() + 220);
        QTest::newRow("Windows-subsystem") << path << bytes << EBADMSG;
        bytes = efiHeader();
        qToLittleEndian<quint16>(11, bytes.data() + 220);
        QTest::newRow("EFI-driver") << path << bytes << EBADMSG;
        bytes = efiHeader();
        qToLittleEndian<quint16>(0x10b, bytes.data() + 152);
        QTest::newRow("PE32") << path << bytes << EBADMSG;
        bytes = efiHeader();
        qToLittleEndian<quint16>(70, bytes.data() + 148);
        QTest::newRow("short-optional-header") << path << bytes << EBADMSG;
        bytes = efiHeader();
        qToLittleEndian<quint16>(65535, bytes.data() + 148);
        QTest::newRow("optional-header-beyond-file") << path << bytes << EBADMSG;
        bytes = efiHeader();
        bytes.append(QByteArray(1024 * 1024, '\0'));
        qToLittleEndian<quint32>(1024 * 1024, bytes.data() + 60);
        QTest::newRow("bounded-header-read") << path << bytes << ENOTSUP;
    }

    void fallbackRestrictions()
    {
        QFETCH(QString, name);
        QFETCH(QByteArray, bytes);
        QFETCH(int, expected);
        makeImage(name, bytes);
        rufus::IsoWriteCheck result;
        QCOMPARE(check(result), expected);
        QVERIFY(!result.problem.isEmpty());
        QCOMPARE(result.tree.files, quint64(0));
    }

    void selectedTreeAndConfigAreChecked_data()
    {
        QTest::addColumn<bool>("udf");
        QTest::addColumn<bool>("link");
        QTest::newRow("ISO-malformed-config") << false << false;
        QTest::newRow("UDF-malformed-config") << true << false;
        QTest::newRow("ISO-symlink") << false << true;
        QTest::newRow("UDF-symlink") << true << true;
    }

    void selectedTreeAndConfigAreChecked()
    {
        QFETCH(bool, udf);
        QFETCH(bool, link);
        const QString extra = directory_.filePath(QStringLiteral("tree/extra.cfg"));
        if (link)
        {
            QVERIFY(QDir().mkpath(QFileInfo(extra).absolutePath()));
            QVERIFY(QFile::link(QStringLiteral("payload"), extra));
        }
        else
            put(extra, QByteArray("options SOURCE\0broken", 21));
        makeImage(QStringLiteral("EFI/BOOT/BOOTX64.EFI"), efiHeader(), udf);
        rufus::IsoWriteCheck result;
        if (link && !udf) { QCOMPARE(check(result),0); QVERIFY(result.problem.isEmpty()); return; }
        QVERIFY(check(result) != 0);
        QVERIFY2(result.problem.contains(QStringLiteral("tree or boot configuration")), qPrintable(result.problem));
        QVERIFY(!QString::fromUtf8(result.tree.path).isEmpty());
    }

    void failureAndCancellation_data()
    {
        QTest::addColumn<int>("change");
        QTest::addColumn<int>("expected");
        QTest::newRow("read") << int(Fault::ReadError) << EIO;
        QTest::newRow("cancel") << int(Fault::CancelRead) << ECANCELED;
        QTest::newRow("source-changed-after-tree") << int(Fault::ChangeAfterTree) << ESTALE;
        QTest::newRow("capacity") << int(Fault::TooLargeTree) << ENOSPC;
    }

    void failureAndCancellation()
    {
        QFETCH(int, change);
        QFETCH(int, expected);
        makeImage();
        fault = Fault(change);
        rufus::IsoWriteCheck result;
        QCOMPARE(check(result), expected);
        QVERIFY(!result.problem.isEmpty());
    }

    void staleSelectionAndEarlyCancellation()
    {
        makeImage();
        ++request_.imageInode;
        rufus::IsoWriteCheck result;
        QCOMPARE(check(result), ESTALE);
        --request_.imageInode;
        cancelled = true;
        QCOMPARE(check(result), ECANCELED);
        QCOMPARE(fallbackReads, 0);
    }

    void settingsAreCheckedBeforeReading_data()
    {
        QTest::addColumn<QString>("label");
        QTest::addColumn<quint32>("cluster");
        QTest::addColumn<quint64>("size");
        QTest::newRow("long-label") << QStringLiteral("LABEL TOO LONG") << quint32(512) << 64 * testMiB;
        QTest::newRow("lowercase") << QStringLiteral("lowercase") << quint32(512) << 64 * testMiB;
        QTest::newRow("NUL") << QString::fromUtf8("OK\0BAD", 6) << quint32(512) << 64 * testMiB;
        QTest::newRow("punctuation") << QStringLiteral("A:B") << quint32(512) << 64 * testMiB;
        QTest::newRow("cluster") << QStringLiteral("LABEL") << quint32(768) << 64 * testMiB;
        QTest::newRow("too-few-clusters") << QStringLiteral("LABEL") << quint32(32768) << 64 * testMiB;
        QTest::newRow("tiny-target") << QStringLiteral("LABEL") << quint32(512) << testMiB;
        QTest::newRow("unaligned-target") << QStringLiteral("LABEL") << quint32(512) << 64 * testMiB + 1;
    }

    void settingsAreCheckedBeforeReading()
    {
        QFETCH(QString, label);
        QFETCH(quint32, cluster);
        QFETCH(quint64, size);
        request_.label = label;
        request_.clusterSize = cluster;
        request_.size = size;
        rufus::IsoWriteCheck result;
        QVERIFY(check(result) != 0);
        QVERIFY(!result.problem.isEmpty());
        QVERIFY(!result.problem.contains(QStringLiteral("selected image")));
        QCOMPARE(fallbackReads, 0);
    }

    void unknownSyslinuxDoesNotChooseAnInstaller()
    {
        put(directory_.filePath(QStringLiteral("tree/isolinux/isolinux.cfg")), QByteArray("default linux\n"));
        put(directory_.filePath(QStringLiteral("tree/isolinux/isolinux.bin")), QByteArray("unknown loader version"));
        makeImage();
        rufus::IsoWriteCheck result;
        QCOMPARE(check(result), 0);
        QVERIFY(result.analysis.report.has_syslinux);
        QCOMPARE(result.analysis.report.sl_detected_version, quint16(0));
        QCOMPARE(result.analysis.report.sl_version, quint16(0));
    }

    void persistenceFreshSourcePolicy()
    {
        put(directory_.filePath(QStringLiteral("tree/syslinux.cfg")), QByteArray("append boot=live\n"));
        makeImage();
        request_.isDevice = true;
        request_.device = 123;
        request_.diskSequence = 7;
        request_.objectPath = QStringLiteral("/org/freedesktop/UDisks2/block_devices/fixture");
        request_.serviceOwner = QStringLiteral(":1.2");
        request_.identity = {QStringLiteral("fixture")};
        request_.size = 1024 * testMiB;
        request_.persistenceSize = 256 * testMiB;
        rufus::IsoWriteCheck result;
        QCOMPARE(check(result), 0);
        QVERIFY(result.partition.persistence_size >= 256 * testMiB);
        fault = Fault::Windows;
        QCOMPARE(check(result), ENOTSUP);
        QVERIFY(!QFile::exists(request_.path));
    }

    void grub4dosImagePrerequisites_data()
    {
        QTest::addColumn<QString>("mode");
        QTest::addColumn<bool>("udf");
        QTest::addColumn<int>("filesystem");
        for (bool udf : {false, true})
        for (int filesystem : {FS_FAT16, FS_FAT32, FS_NTFS})
        for (const char* mode : {"own-loader", "missing-loader", "nested-loader", "grub2-precedence",
            "syslinux-precedence", "windows", "efi", "efi-mixed-case", "changed-source"})
            QTest::newRow(qPrintable(QStringLiteral("%1-%2-%3").arg(mode).arg(udf).arg(filesystem)))
                << QString::fromLatin1(mode) << udf << filesystem;
    }

    void grub4dosImagePrerequisites()
    {
        QFETCH(QString, mode);
        QFETCH(bool, udf);
        QFETCH(int, filesystem);
        const bool missing = mode == QStringLiteral("missing-loader");
        const bool nested = mode == QStringLiteral("nested-loader");
        if (!missing)
            put(directory_.filePath(nested ? QStringLiteral("tree/boot/grldr") : QStringLiteral("tree/grldr")), "image-owned loader bytes");
        if (mode == QStringLiteral("grub2-precedence"))
        {
            // Rufus keeps the BIOS GRUB2 flag only when normal.mod has a version.
            QByteArray module(4096, '\0');
            constexpr qsizetype markerOffset = 64;
            const char version[] = "GRUB  version %s\0" "2.12";
            module.replace(markerOffset, sizeof(version), version, sizeof(version));
            put(directory_.filePath(QStringLiteral("tree/boot/grub/i386-pc/normal.mod")), module);
            put(directory_.filePath(QStringLiteral("tree/boot/grub/grub.cfg")), "menuentry image {}\n");
        }
        if (mode == QStringLiteral("syslinux-precedence"))
            put(directory_.filePath(QStringLiteral("tree/isolinux/isolinux.cfg")), "default image\n");
        if (mode == QStringLiteral("windows"))
            put(directory_.filePath(QStringLiteral("tree/bootmgr")), QByteArray(legacyLoaderBytes, 'x'));
        put(directory_.filePath(QStringLiteral("tree/menu.lst")), "title Image menu\n");
        const bool efi = mode.startsWith(QStringLiteral("efi"));
        const QString efiPath = mode == QStringLiteral("efi-mixed-case")
            ? QStringLiteral("eFi/BoOt/BoOtX64.EfI") : QStringLiteral("EFI/BOOT/BOOTX64.EFI");
        makeImage(efi ? efiPath : QStringLiteral("image-marker"),
            efi ? efiHeader() : QByteArray("image marker"), udf);
        selectGrub4dos(filesystem);
        const bool changed = mode == QStringLiteral("changed-source");
        if (changed) ++request_.imageInode;
        rufus::IsoWriteCheck result;
        const bool accepted = mode == QStringLiteral("own-loader") || (efi && filesystem != FS_NTFS)
            || (mode == QStringLiteral("windows") && filesystem == FS_NTFS);
        QCOMPARE(check(result), accepted ? 0 : changed ? ESTALE : ENOTSUP);
        QVERIFY(!result.partition.started && !QFile::exists(request_.path));
        if (accepted)
        {
            QVERIFY(result.analysis.report.has_grub4dos && !result.analysis.report.has_grub2);
            QCOMPARE(result.analysis.report.grub2_version[0], '\0');
            QCOMPARE(GetSecondaryBootType(BT_IMAGE, &result.analysis.report, false), int(BT_GRUB4DOS));
            QCOMPARE(result.extraPartitions, quint8(0));
            QCOMPARE(result.efiPath, efi ? QLatin1Char('/') + efiPath : QString());
            QCOMPARE(extractionLimit, filesystem == FS_NTFS ? uint64_t(0) : uint64_t(UINT32_MAX));
            QCOMPARE(result.partition.uefi_ntfs_size, quint64(0));
            QVERIFY(!result.splitInstallWim && result.windowsEfiRepair.isEmpty());
            QVERIFY(result.tree.files > 0 && result.tree.bytes_written > 0);
            QVERIFY(!request_.hasGrub2Core() && !request_.isSourceFree());
            QCOMPARE(result.partition.offset, quint64(testMiB));
            QVERIFY(result.partition.offset <= request_.size);
            QVERIFY(result.partition.size <= request_.size - result.partition.offset);
        }
        else QVERIFY(!result.problem.isEmpty());
    }

    void grub4dosEfiRestrictions_data()
    {
        fallbackRestrictions_data(); // Reuse the existing invalid paths/PE headers.
    }

    void grub4dosEfiRestrictions()
    {
        QFETCH(QString, name);
        QFETCH(QByteArray, bytes);
        for (bool udf : {false, true})
        for (int filesystem : {FS_FAT16, FS_FAT32})
        {
            put(directory_.filePath(QStringLiteral("tree/grldr")), "image-owned loader bytes");
            makeImage(name, bytes, udf);
            selectGrub4dos(filesystem);
            rufus::IsoWriteCheck result;
            QVERIFY(check(result) != 0);
            QVERIFY(!result.problem.isEmpty());
            QCOMPARE(result.tree.files, quint64(0));
            QVERIFY(!result.partition.started && !QFile::exists(request_.path));
        }
    }

    void grub4dosEfiFailure_data()
    {
        QTest::addColumn<bool>("udf");
        QTest::addColumn<int>("injected");
        QTest::addColumn<int>("expected");
        QTest::addColumn<int>("filesystem");
        for (bool udf : {false, true})
        for (int filesystem : {FS_FAT16, FS_FAT32})
        {
            const QByteArray prefix = QByteArray(udf ? "UDF-" : "ISO-") + (filesystem == FS_FAT16 ? "FAT16-" : "FAT32-");
            QTest::newRow((prefix + "read-error").constData()) << udf << int(Fault::ReadError) << EIO << filesystem;
            QTest::newRow((prefix + "cancel-read").constData()) << udf << int(Fault::CancelRead) << ECANCELED << filesystem;
            QTest::newRow((prefix + "source-changed").constData()) << udf << int(Fault::ChangeAfterTree) << ESTALE << filesystem;
        }
    }

    void grub4dosEfiFailure()
    {
        QFETCH(bool, udf);
        QFETCH(int, injected);
        QFETCH(int, expected);
        QFETCH(int, filesystem);
        put(directory_.filePath(QStringLiteral("tree/grldr")), "image-owned loader bytes");
        makeImage(QStringLiteral("EFI/BOOT/BOOTX64.EFI"), efiHeader(), udf);
        selectGrub4dos(filesystem);
        fault = Fault(injected);
        rufus::IsoWriteCheck result;
        QCOMPARE(check(result), expected);
        QVERIFY(!result.problem.isEmpty());
        QVERIFY(!result.partition.started && !QFile::exists(request_.path));
    }

    void grubImagePrerequisites_data()
    {
        QTest::addColumn<int>("filesystem");
        QTest::addColumn<bool>("udf");
        QTest::addColumn<int>("injected");
        QTest::addColumn<int>("expected");
        for (bool udf : {false, true})
        for (int fs : {FS_FAT16, FS_FAT32, FS_NTFS})
        {
            const QByteArray name = QByteArray(udf ? "UDF-" : "ISO-") + QByteArray::number(fs);
            QTest::newRow(name.constData()) << fs << udf << int(Fault::None) << 0;
        }
        QTest::newRow("unknown-stays-unknown") << int(FS_FAT32) << false << int(Fault::UnknownGrub) << 0;
        QTest::newRow("syslinux-precedence") << int(FS_FAT32) << false << int(Fault::GrubWithSyslinux) << ENOTSUP;
        QTest::newRow("windows") << int(FS_FAT32) << false << int(Fault::Windows) << ENOTSUP;
        QTest::newRow("incomplete") << int(FS_FAT32) << false << int(Fault::NoTree) << ENOTSUP;
        QTest::newRow("pending") << int(FS_FAT32) << false << int(Fault::ContentPending) << ENOTSUP;
        QTest::newRow("capacity") << int(FS_FAT32) << false << int(Fault::TooLargeTree) << ENOSPC;
        QTest::newRow("fat-large-file") << int(FS_FAT32) << false << int(Fault::LargeFile) << ENOTSUP;
        QTest::newRow("fat-module-missing") << int(FS_FAT32) << false << int(Fault::FatlessGrub) << ENOTSUP;
        QTest::newRow("ntfs-module-missing") << int(FS_NTFS) << false << int(Fault::NtfslessGrub) << ENOTSUP;
        QTest::newRow("source-change") << int(FS_FAT32) << false << int(Fault::ChangeAfterTree) << ESTALE;
    }

    void grubImagePrerequisites()
    {
        QFETCH(int, filesystem); QFETCH(bool, udf);
        QFETCH(int, injected); QFETCH(int, expected);
        QByteArray module(4096, '\0');
        constexpr qsizetype markerOffset = 64;
        const QByteArray marker("GRUB  version %s");
        module.replace(markerOffset, marker.size(), marker);
        const QByteArray version("2.12");
        module.replace(markerOffset + marker.size() + 1, version.size(), version);
        put(directory_.filePath(QStringLiteral("tree/boot/grub/i386-pc/normal.mod")), module);
        put(directory_.filePath(QStringLiteral("tree/boot/grub/grub.cfg")), QByteArray("search --label SOURCE --set=root\n"));
        makeImage(QStringLiteral("bios-payload"), QByteArray("BIOS-only image\n"), udf);
        request_.isDevice = request_.mbr = request_.grub2 = true;
        request_.fileSystem = filesystem;
        request_.clusterSize = filesystem == FS_FAT16 ? 2048 : filesystem == FS_NTFS ? 4096 : 512;
        request_.device = 123;
        request_.diskSequence = 7;
        request_.objectPath = QStringLiteral("/org/freedesktop/UDisks2/block_devices/fixture");
        request_.serviceOwner = QStringLiteral(":1.2");
        request_.identity = {QStringLiteral("fixture")};
        fault = Fault(injected);
        rufus::IsoWriteCheck result;
        QCOMPARE(check(result), expected);
        if (expected != 0)
            return;
        QVERIFY(result.analysis.report.has_grub2);
        QCOMPARE(QString::fromLatin1(result.analysis.report.grub2_version),
            injected == int(Fault::UnknownGrub) ? QString() : QStringLiteral("2.12"));
        QCOMPARE(result.partition.offset, testMiB);
        QCOMPARE(result.extraPartitions, quint8(0));
        QCOMPARE(result.tree.files, quint64(5));
        QCOMPARE(extractionLimit, filesystem == FS_NTFS ? uint64_t(0) : uint64_t(UINT32_MAX));
        QVERIFY(result.efiPath.isEmpty() && !result.splitInstallWim);
        ++request_.imageInode;
        QCOMPARE(check(result), ESTALE);
    }

    void grubImageUefiPartitions_data()
    {
        QTest::addColumn<bool>("imageEfi");
        QTest::addColumn<bool>("requestEfi");
        QTest::addColumn<bool>("udf");
        QTest::addColumn<int>("injected");
        QTest::addColumn<int>("expected");
        QTest::addColumn<QString>("efiName");
        QTest::addColumn<QByteArray>("efiBytes");
        QTest::addColumn<bool>("grub4dos");
        for (bool grub4dos : {false, true})
        {
            const QByteArray provider = grub4dos ? "GRUB4DOS-" : "GRUB2-";
            for (bool udf : {false, true})
            for (bool imageEfi : {false, true})
            for (bool requestEfi : {false, true})
            {
                const QByteArray name = provider + QByteArray(udf ? "UDF-" : "ISO-")
                    + (imageEfi ? "efi-" : "bios-") + (requestEfi ? "uefi-partition" : "single-partition");
                QTest::newRow(name.constData()) << imageEfi << requestEfi << udf << int(Fault::None)
                    << (imageEfi == requestEfi ? 0 : ENOTSUP) << QStringLiteral("EFI/BOOT/BOOTX64.EFI") << efiHeader() << grub4dos;
            }
            QTest::newRow((provider + "efi-loader-read-error").constData()) << true << true << false << int(Fault::ReadError) << EIO
                << QStringLiteral("EFI/BOOT/BOOTX64.EFI") << efiHeader() << grub4dos;
            QTest::newRow((provider + "efi-loader-read-cancel").constData()) << true << true << false << int(Fault::CancelRead) << ECANCELED
                << QStringLiteral("EFI/BOOT/BOOTX64.EFI") << efiHeader() << grub4dos;
            QTest::newRow((provider + "efi-vendor-only").constData()) << true << true << false << int(Fault::None) << ENOTSUP
                << QStringLiteral("EFI/vendor/BOOTX64.EFI") << efiHeader() << grub4dos;
            QByteArray invalidApplication = efiHeader();
            qToLittleEndian<quint16>(3, invalidApplication.data() + 220);
            QTest::newRow((provider + "efi-invalid-application").constData()) << true << true << false << int(Fault::None) << EBADMSG
                << QStringLiteral("EFI/BOOT/BOOTX64.EFI") << invalidApplication << grub4dos;
        }
    }

    void grubImageUefiPartitions()
    {
        QFETCH(bool, imageEfi); QFETCH(bool, requestEfi); QFETCH(bool, udf);
        QFETCH(int, injected); QFETCH(int, expected);
        QFETCH(QString, efiName); QFETCH(QByteArray, efiBytes);
        QFETCH(bool, grub4dos);
        const QString tree = directory_.filePath(QStringLiteral("tree"));
        if (grub4dos)
        {
            put(tree + QStringLiteral("/grldr"), "image-owned loader bytes");
            put(tree + QStringLiteral("/menu.lst"), "title Image menu\n");
        }
        else
        {
            QByteArray module(4096, '\0');
            const QByteArray marker("GRUB  version %s"), version("2.12");
            constexpr qsizetype markerOffset = 64;
            module.replace(markerOffset, marker.size(), marker);
            module.replace(markerOffset + marker.size() + 1, version.size(), version);
            put(tree + QStringLiteral("/boot/grub/i386-pc/normal.mod"), module);
            put(tree + QStringLiteral("/boot/grub/grub.cfg"), QByteArray("search --label SOURCE --set=root\n"));
        }
        makeImage(imageEfi ? efiName : QStringLiteral("bios-payload"),
            imageEfi ? efiBytes : QByteArray("BIOS-only image\n"), udf);
        request_.isDevice = request_.mbr = true;
        request_.grub2 = !grub4dos;
        request_.grub4dos = grub4dos;
        request_.uefiNtfs = requestEfi;
        request_.fileSystem = FS_NTFS;
        request_.clusterSize = 4096;
        request_.volumeId = 0;
        request_.device = 123;
        request_.diskSequence = 7;
        request_.objectPath = QStringLiteral("/org/freedesktop/UDisks2/block_devices/fixture");
        request_.serviceOwner = QStringLiteral(":1.2");
        request_.identity = {QStringLiteral("fixture")};
        fault = Fault(injected);
        rufus::IsoWriteCheck result;
        QCOMPARE(check(result), expected);
        QCOMPARE(result.extraPartitions, quint8(imageEfi ? XP_UEFI_NTFS : 0));
        if (expected == 0)
        {
            QCOMPARE(bool(result.analysis.report.has_grub2), !grub4dos);
            QCOMPARE(bool(result.analysis.report.has_grub4dos), grub4dos);
            QCOMPARE(QString::fromLatin1(result.analysis.report.grub2_version), grub4dos ? QString() : QStringLiteral("2.12"));
            QCOMPARE(result.partition.uefi_ntfs_size > 0, requestEfi);
            QCOMPARE(extractionLimit, uint64_t(0));
            QCOMPARE(result.efiPath, imageEfi ? QStringLiteral("/EFI/BOOT/BOOTX64.EFI") : QString());
        }
    }
};

QTEST_GUILESS_MAIN(TestIsoWriteCheck)
#include "test_iso_write_check.moc"
