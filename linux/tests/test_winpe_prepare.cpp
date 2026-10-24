// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/winpe_prepare.h"
#include "platform/config_patch.h"
#include "winpe_setup.h"
#include "image_report.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringConverter>
#include <QTemporaryDir>
#include <QTest>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace
{
enum Fault { NoFault, ShortIo, ReadError, WriteError, ZeroWrite, VerifyMismatch,
    FileSyncError, DirectorySyncError, RenameError, CloseError, CancelWrite,
    SourceChange, SourcePathChange, DirectoryPathChange, DestinationCollision,
    ExtraOutput, WriteSifError, WriteLoaderError, CancelLoaderWrite, SourceChangeLater,
    RenameInvalidFlags, RenameMissingSyscall, RenameUnsupported, LinkError, LinkCollision,
    RootDetectorReadError, RootDetectorReadCancel, RootDetectorChange, RootDetectorReplace,
    RootDetectorDuplicate, RootDetectorRemove, RootDetectorChangeLater, DetectorSourceChange,
    RootSifReadError, RootSifReadCancel, RootSifRenameError, RootSifModeError,
    RootSifVerifyError, RootSifSyncError, RootSifDirectorySyncError, RootSifCloseError,
    RootSifChange, RootSifReplace, RootSifDuplicate, RootSifRemove, RootSifChangeLater,
    RootSifCancelWrite, RootLoaderReadError, RootLoaderReadCancel,
    RootLoaderRenameError, RootLoaderModeError, RootLoaderVerifyError,
    RootLoaderSyncError, RootLoaderDirectorySyncError, RootLoaderCloseError,
    RootLoaderChange, RootLoaderReplace, RootLoaderDuplicate, RootLoaderRemove,
    RootLoaderCommittedChange, RootLoaderCancelWrite, RootLoaderChangeDuringWrite,
    RootLoaderReplaceDuringWrite };
enum SourceIssue { UnknownOptions, MissingLoader, MissingSif, MissingDetector, EmptyLoader, ShortLoader, OversizedLoader, AmbiguousDirectory, AmbiguousFile, SymlinkFile, HardlinkFile, SpecialFile, ExistingBootmgr, MissingSection, SymlinkDirectory, SymlinkDestination, ExistingSif };
Fault fault;
bool active, cancelled;
int reads, writes;
constexpr int sifWrite = 2, loaderWrite = 3;
constexpr int rootSifRead = 3;
constexpr int partialMinintMask = 0x40;
constexpr int loaderPathOffset = 10, loaderDriveOffset = 70, loaderCrcOffset = 0x2060;
constexpr int rdiskNumberOffset = 6;
QString sourcePath, rootPath;
const QByteArray rootSifText(";root-only\r\n[SetupData]\r\nRootValue=keep\r\n");

void putFile(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
        qFatal("Could not write WinPE fixture");
}

QByteArray getFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        qFatal("Could not read WinPE fixture");
    return file.readAll();
}

QByteArray encoded(const QString& text, int encoding)
{
    if (encoding == 0)
        return text.toUtf8();
    if (encoding == 1)
        return QByteArray("\xef\xbb\xbf", 3) + text.toUtf8();
    QStringEncoder encoder(QStringConverter::Utf16LE);
    return QByteArray("\xff\xfe", 2) + encoder(text);
}

QByteArray fixtureLoader()
{
    QByteArray bytes(0x2100, '\0');
    bytes.replace(loaderPathOffset, 19, "\\minint\\txtsetup.sif");
    bytes.replace(loaderDriveOffset, 8, "rdisk(0)");
    bytes.replace(100, 12, "$win_nt$.~bt\\");
    bytes[loaderCrcOffset] = 0x74;
    bytes[loaderCrcOffset + 1] = 0x03;
    return bytes;
}

void makeTree(const QString& root, const QString& loaderDirectory, const QString& setupDirectory,
    const QByteArray& sif = "[SetupData]\r\nOsLoadOptions = \"\"\r\n")
{
    if (!QDir().mkpath(root + '/' + loaderDirectory) || !QDir().mkpath(root + '/' + setupDirectory))
        qFatal("Could not create WinPE fixture directories");
    putFile(root + '/' + loaderDirectory + "/NTDETECT.COM", "detector bytes");
    putFile(root + '/' + loaderDirectory + "/SetupLdr.Bin", fixtureLoader());
    putFile(root + '/' + setupDirectory + "/TxtSetup.Sif", sif);
    sourcePath = root + '/' + loaderDirectory + "/SetupLdr.Bin";
    rootPath = root;
}
}

extern "C" int IsIsoScanCancelled(void) { return cancelled; }
extern "C" int IsRufusJobCancelled(void) { return cancelled; }
extern "C" void uprintf(const char*, ...) {}
extern "C" ssize_t __real_write(int, const void*, size_t);
extern "C" int __real_fsync(int);
extern "C" int __real_close(int);
extern "C" int __real_renameat2(int, const char*, int, const char*, unsigned);
extern "C" int __real_renameat(int, const char*, int, const char*);
extern "C" int __real_fchmod(int, mode_t);
extern "C" int __real_linkat(int, const char*, int, const char*, int);
#if defined(__GLIBC__) && defined(__USE_FILE_OFFSET64)
#define WrappedPread __wrap_pread64
#define RealPread __real_pread64
#else
#define WrappedPread __wrap_pread
#define RealPread __real_pread
#endif
extern "C" ssize_t RealPread(int, void*, size_t, off_t);

extern "C" ssize_t WrappedPread(int fd, void* data, size_t size, off_t offset)
{
    if (!active)
        return RealPread(fd, data, size, offset);
    ++reads;
    const bool rootLoader = QFileInfo(QStringLiteral("/proc/self/fd/%1").arg(fd)).symLinkTarget()
        == rootPath + "/BoOtMgR";
    if (rootLoader && fault == RootLoaderReadError)
    {
        errno = EIO;
        return -1;
    }
    if (rootLoader && fault == RootLoaderReadCancel)
        cancelled = true;
    const bool retainedRead = reads == 2;
    if (retainedRead && fault == RootDetectorReadError)
    {
        errno = EIO;
        return -1;
    }
    if (retainedRead && fault == RootDetectorReadCancel)
        cancelled = true;
    if (reads == rootSifRead && fault == RootSifReadError)
    {
        errno = EIO;
        return -1;
    }
    if (reads == rootSifRead && fault == RootSifReadCancel)
        cancelled = true;
    if (fault == ReadError || (fault == ShortIo && reads == 1))
    {
        errno = fault == ReadError ? EIO : EINTR;
        return -1;
    }
    const ssize_t result = RealPread(fd, data, fault == ShortIo ? qMin(size, size_t(7)) : size, offset);
    const bool writable = (fcntl(fd, F_GETFL) & O_ACCMODE) == O_RDWR;
    const bool failedSifVerify = fault == RootSifVerifyError && writes == sifWrite;
    const bool failedLoaderVerify = fault == RootLoaderVerifyError && writes == loaderWrite;
    if (result > 0 && writable && (fault == VerifyMismatch || failedSifVerify || failedLoaderVerify))
        static_cast<char*>(data)[0] ^= 1;
    return result;
}

extern "C" ssize_t __wrap_write(int fd, const void* data, size_t size)
{
    if (!active)
        return __real_write(fd, data, size);
    ++writes;
    const bool laterWriteError = (fault == WriteSifError && writes == sifWrite) ||
        (fault == WriteLoaderError && writes == loaderWrite);
    if (fault == WriteError || laterWriteError || (fault == ShortIo && writes == 1))
    {
        errno = fault == ShortIo ? EINTR : ENOSPC;
        return -1;
    }
    if (fault == ZeroWrite)
        return 0;
    const ssize_t result = __real_write(fd, data, fault == ShortIo ? qMin(size, size_t(9)) : size);
    const bool sifChanged = (writes == sifWrite && fault == RootSifChange)
        || (writes == loaderWrite && fault == RootSifChangeLater);
    if (result > 0 && sifChanged)
    {
        active = false;
        putFile(rootPath + "/TxTsEtUp.SiF", "changed root SIF");
        active = true;
    }
    if (result > 0 && writes == sifWrite)
    {
        active = false;
        if (fault == RootSifCancelWrite) cancelled = true;
        if (fault == RootSifReplace)
        {
            QFile::rename(rootPath + "/TxTsEtUp.SiF", rootPath + "/old-sif");
            putFile(rootPath + "/TxTsEtUp.SiF", rootSifText);
        }
        if (fault == RootSifDuplicate) putFile(rootPath + "/TXTSETUP.SIF", rootSifText);
        if (fault == RootSifRemove) QFile::remove(rootPath + "/TxTsEtUp.SiF");
        active = true;
    }
    if (result > 0 && writes == 2 && fault == RootDetectorChangeLater)
    {
        active = false;
        putFile(rootPath + "/NtDetect.Com", "changed detector");
        active = true;
    }
    if (result > 0 && writes == loaderWrite)
    {
        if (fault == CancelLoaderWrite || fault == RootLoaderCancelWrite)
            cancelled = true;
        if (fault == RootLoaderChangeDuringWrite || fault == RootLoaderReplaceDuringWrite)
        {
            active = false;
            if (fault == RootLoaderReplaceDuringWrite)
            {
                const QByteArray original = getFile(rootPath + "/BoOtMgR");
                QFile::rename(rootPath + "/BoOtMgR", rootPath + "/old-loader");
                putFile(rootPath + "/BoOtMgR", original);
            }
            else
                putFile(rootPath + "/BoOtMgR", "changed root loader");
            active = true;
        }
        if (fault == SourceChangeLater)
        {
            active = false;
            putFile(sourcePath, "changed source");
            active = true;
        }
    }
    if (result <= 0 || writes != 1)
        return result;
    active = false;
    if (fault == CancelWrite)
        cancelled = true;
    if (fault == SourceChange)
        putFile(sourcePath, "changed source");
    if (fault == SourcePathChange)
    {
        QFile::rename(sourcePath, sourcePath + ".old");
        putFile(sourcePath, fixtureLoader());
    }
    if (fault == DirectoryPathChange)
    {
        QDir().rename(rootPath + "/i386", rootPath + "/old-i386");
        QDir().mkdir(rootPath + "/i386");
    }
    if (fault == DestinationCollision)
        putFile(rootPath + "/NtDetect.Com", "keep existing destination");
    if (fault == ExtraOutput)
        __real_write(fd, "extra", 5);
    if (fault == RootDetectorChange)
        putFile(rootPath + "/NtDetect.Com", "changed detector");
    if (fault == RootDetectorReplace)
    {
        const QByteArray original = getFile(rootPath + "/NtDetect.Com");
        QFile::rename(rootPath + "/NtDetect.Com", rootPath + "/old-detector");
        putFile(rootPath + "/NtDetect.Com", original);
    }
    if (fault == RootDetectorDuplicate)
        putFile(rootPath + "/NTDETECT.COM", "detector bytes");
    if (fault == RootDetectorRemove)
        QFile::remove(rootPath + "/NtDetect.Com");
    if (fault == DetectorSourceChange)
        putFile(rootPath + "/i386/NTDETECT.COM", "changed detector");
    if (fault == RootLoaderChange)
        putFile(rootPath + "/BoOtMgR", "changed root loader");
    if (fault == RootLoaderReplace)
    {
        const QByteArray original = getFile(rootPath + "/BoOtMgR");
        QFile::rename(rootPath + "/BoOtMgR", rootPath + "/old-loader");
        putFile(rootPath + "/BoOtMgR", original);
    }
    if (fault == RootLoaderDuplicate)
        putFile(rootPath + "/BOOTMGR", fixtureLoader());
    if (fault == RootLoaderRemove)
        QFile::remove(rootPath + "/BoOtMgR");
    active = true;
    return result;
}

extern "C" int __wrap_fsync(int fd)
{
    struct stat info;
    const bool directory = fstat(fd, &info) == 0 && S_ISDIR(info.st_mode);
    const bool failFile = fault == FileSyncError && !directory;
    const bool failDirectory = fault == DirectorySyncError && directory;
    const bool failSif = writes == sifWrite && (directory
        ? fault == RootSifDirectorySyncError : fault == RootSifSyncError);
    const bool failLoader = writes == loaderWrite && (directory
        ? fault == RootLoaderDirectorySyncError : fault == RootLoaderSyncError);
    if (active && directory && writes == loaderWrite && fault == RootLoaderCommittedChange)
    {
        active = false;
        putFile(rootPath + "/BoOtMgR", "changed root loader");
        active = true;
    }
    if (active && (failFile || failDirectory || failSif || failLoader))
    {
        errno = EIO;
        return -1;
    }
    return __real_fsync(fd);
}

extern "C" int __wrap_renameat2(int oldRoot, const char* oldName, int root, const char* name, unsigned flags)
{
    if (active && fault == RenameError)
    {
        errno = EIO;
        return -1;
    }
    const bool unavailable = fault == RenameInvalidFlags || fault == RenameMissingSyscall
        || fault == RenameUnsupported || fault == LinkError || fault == LinkCollision;
    if (active && unavailable)
    {
        errno = fault == RenameMissingSyscall ? ENOSYS : fault == RenameUnsupported ? EOPNOTSUPP : EINVAL;
        return -1;
    }
    return __real_renameat2(oldRoot, oldName, root, name, flags);
}

extern "C" int __wrap_renameat(int oldRoot, const char* oldName, int root, const char* name)
{
    const bool failLoader = fault == RootLoaderRenameError && writes == loaderWrite;
    if (active && (fault == RootSifRenameError || failLoader))
    {
        errno = EIO;
        return -1;
    }
    return __real_renameat(oldRoot, oldName, root, name);
}

extern "C" int __wrap_fchmod(int fd, mode_t mode)
{
    const bool loaderOutput = writes == loaderWrite - 1;
    if (active && (fault == RootSifModeError || (fault == RootLoaderModeError && loaderOutput)))
    {
        errno = EPERM;
        return -1;
    }
    return __real_fchmod(fd, mode);
}

extern "C" int __wrap_linkat(int oldRoot, const char* oldName, int root, const char* name, int flags)
{
    if (active && fault == LinkError)
    {
        errno = ENOSPC;
        return -1;
    }
    if (active && fault == LinkCollision)
        putFile(rootPath + "/ntdetect.com", "keep existing destination");
    return __real_linkat(oldRoot, oldName, root, name, flags);
}

extern "C" int __wrap_close(int fd)
{
    const bool output = (fcntl(fd, F_GETFL) & O_ACCMODE) == O_RDWR;
    const int result = __real_close(fd);
    const bool failSifClose = fault == RootSifCloseError && writes == sifWrite;
    const bool failLoaderClose = fault == RootLoaderCloseError && writes == loaderWrite;
    if (active && output && (fault == CloseError || failSifClose || failLoaderClose))
    {
        errno = EIO;
        return -1;
    }
    return result;
}

class WinPePrepareTest : public QObject
{
    Q_OBJECT
private slots:
    void init()
    {
        active = cancelled = false;
        reads = writes = 0;
        fault = NoFault;
    }
    void sif_data()
    {
        QTest::addColumn<QString>("text");
        QTest::addColumn<QString>("expected");
        QTest::newRow("crlf") << QStringLiteral("[SetupData]\r\na=b\r\n") << QStringLiteral("[SetupData]\r\ninserted\r\na=b\r\n");
        QTest::newRow("lf") << QStringLiteral("[SetupData]\na=b\n") << QStringLiteral("[SetupData]\r\ninserted\r\na=b\r\n");
        QTest::newRow("prefix-and-repeat") << QStringLiteral(" \t[setUPdata]suffix\n[SetupData]\n") <<
            QStringLiteral(" \t[setUPdata]suffix\r\ninserted\r\n[SetupData]\r\ninserted\r\n");
        QTest::newRow("no-final-newline") << QStringLiteral("[SetupData]") << QStringLiteral("[SetupData]inserted\r\n");
        QTest::newRow("bare-cr") << QStringLiteral("[SetupData]\ra=b\n") << QStringLiteral("[SetupData]\ra=b\r\ninserted\r\n");
        QTest::newRow("comment") << QStringLiteral(";[SetupData]\n[SetupData]\n") << QStringLiteral(";[SetupData]\r\n[SetupData]\r\ninserted\r\n");
        QTest::newRow("chunk-start") << QString(1023, 'x') + "[SetupData]\n" <<
            QString(1023, 'x') + "[SetupData]\r\ninserted\r\n";
        QTest::newRow("chunk-split") << QStringLiteral("[SetupData]") + QString(1013, 'x') + "rest\n" <<
            QStringLiteral("[SetupData]") + QString(1012, 'x') + "inserted\r\nxrest\r\n";
        QTest::newRow("crlf-at-boundary") << QStringLiteral("[SetupData]") + QString(1011, 'x') + "\r\nrest\n" <<
            QStringLiteral("[SetupData]") + QString(1011, 'x') + "\r\ninserted\r\nrest\r\n";
    }
    void sif()
    {
        QFETCH(QString, text);
        QFETCH(QString, expected);
        for (int encoding = 0; encoding < 3; ++encoding)
        {
            QByteArray output;
            QCOMPARE(InsertConfigSectionText(encoded(text, encoding), "[SetupData]", "inserted", output), 0);
            QCOMPARE(output, encoded(expected, encoding));
        }
    }
    void unicodeSif()
    {
        const QString text = QString::fromUtf8(";é😀\n[SetupData]\nname=ă\n[NLS]\nAnsiCodepage=c_1252.nls\n");
        const QString expected = QString::fromUtf8(";é😀\r\n[SetupData]\r\ninserted\r\nname=ă\r\n[NLS]\r\nAnsiCodepage=c_1252.nls\r\n");
        for (int encoding = 1; encoding <= 2; ++encoding)
        {
            QByteArray output;
            QCOMPARE(InsertConfigSectionText(encoded(text, encoding), "[SetupData]", "inserted", output), 0);
            QCOMPARE(output, encoded(expected, encoding));
        }
        // Count UTF-16 units, including a surrogate pair split across chunks.
        const QString split = QString(1022, 'x') + QString::fromUtf8("😀") + "[SetupData]\n[SetupData]\n";
        const QString splitExpected = QString(1022, 'x') + QString::fromUtf8("😀") +
            "[SetupData]\r\n[SetupData]\r\ninserted\r\n";
        QByteArray output;
        QCOMPARE(InsertConfigSectionText(encoded(split, 2), "[SetupData]", "inserted", output), 0);
        QCOMPARE(output, encoded(splitExpected, 2));
    }
    void invalidSif_data()
    {
        QTest::addColumn<QByteArray>("bytes");
        QTest::addColumn<int>("error");
        QTest::newRow("empty") << QByteArray() << ENOENT;
        QTest::newRow("missing-section") << QByteArray("[Other]\n") << ENOENT;
        QTest::newRow("split-section") << QByteArray(1020, ' ') + "[SetupData]\n" << ENOENT;
        QTest::newRow("embedded-nul") << QByteArray("[SetupData]\0hidden", 18) << ENOTSUP;
        QTest::newRow("eof-marker") << QByteArray("[SetupData]\n\x1a") << ENOTSUP;
        QTest::newRow("unknown-ansi") << QByteArray(";\xe9\n[SetupData]\n") << EILSEQ;
        QTest::newRow("utf8-without-bom") << QByteArray(";\xc3\xa9\n[SetupData]\n") << ENOTSUP;
        QTest::newRow("bad-utf8") << QByteArray("\xef\xbb\xbf\xff[SetupData]") << EILSEQ;
        QTest::newRow("utf16be") << QByteArray("\xfe\xff") << ENOTSUP;
        QTest::newRow("utf32le") << QByteArray("\xff\xfe\0\0", 4) << ENOTSUP;
        QTest::newRow("odd-utf16") << QByteArray("\xff\xfe\x5b", 3) << EILSEQ;
        QTest::newRow("surrogate") << QByteArray("\xff\xfe\0\xd8", 4) << EILSEQ;
        QTest::newRow("too-large") << QByteArray(CONFIG_PATCH_MAX_BYTES + 1, 'x') << EFBIG;
        QTest::newRow("expanded-too-large") << QByteArray("[SetupData]\n") + QByteArray(CONFIG_PATCH_MAX_BYTES - 12, '\n') << EFBIG;
    }
    void invalidSif()
    {
        QFETCH(QByteArray, bytes);
        QFETCH(int, error);
        QByteArray output("stale");
        QCOMPARE(InsertConfigSectionText(bytes, "[SetupData]", "inserted", output), error);
        QVERIFY(output.isEmpty());
    }
    void layout_data()
    {
        QTest::addColumn<int>("mask");
        QTest::addColumn<bool>("minint");
        QTest::addColumn<QString>("loaderDirectory");
        QTest::addColumn<QString>("setupDirectory");
        QTest::addColumn<int>("error");
        QTest::newRow("i386") << int(WINPE_I386) << false << QStringLiteral("i386") << QStringLiteral("i386") << 0;
        QTest::newRow("i386-minint-option") << int(WINPE_I386) << true << QStringLiteral("i386") << QStringLiteral("i386") << 0;
        QTest::newRow("amd64-mixed-case") << int(WINPE_AMD64) << false << QStringLiteral("I386") << QStringLiteral("AmD64") << 0;
        QTest::newRow("amd64-first") << int(WINPE_AMD64 | WINPE_I386) << false << QStringLiteral("I386") << QStringLiteral("AmD64") << 0;
        QTest::newRow("minint") << int(WINPE_MININT) << true << QStringLiteral("MiNiNt") << QStringLiteral("MiNiNt") << 0;
        QTest::newRow("minint-without-option") << int(WINPE_MININT) << false << QStringLiteral("minint") << QStringLiteral("minint") << ENOTSUP;
        constexpr int completeMinintFiles = 7;
        for (int files = 1; files < completeMinintFiles; ++files)
        for (bool amd64 : {false, true})
        for (bool minint : {false, true})
            QTest::newRow(qPrintable(QStringLiteral("partial-minint-%1-%2-%3").arg(files).arg(amd64).arg(minint)))
                << int((amd64 ? WINPE_AMD64 : WINPE_I386) | (files * partialMinintMask)) << minint
                << QStringLiteral("i386") << (amd64 ? QStringLiteral("amd64") : QStringLiteral("i386"))
                << (minint ? 0 : ENOTSUP);
        QTest::newRow("incomplete") << 1 << false << QStringLiteral("i386") << QStringLiteral("i386") << ENOTSUP;
    }
    void layout()
    {
        QFETCH(int, mask);
        QFETCH(bool, minint);
        QFETCH(QString, loaderDirectory);
        QFETCH(QString, setupDirectory);
        QFETCH(int, error);
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        makeTree(temporary.path(), loaderDirectory, setupDirectory);
        const QByteArray originalLoader = getFile(sourcePath);
        const QString sifPath = temporary.path() + '/' + setupDirectory + "/TxtSetup.Sif";
        const QByteArray originalSif = getFile(sifPath);
        const unsigned minintFiles = unsigned(mask) & WINPE_MININT;
        const bool partialMinint = minintFiles != 0 && minintFiles != WINPE_MININT;
        const char* unusedNames[] = {"ntdetect.com", "setupldr.bin", "txtsetup.sif"};
        constexpr int setupFileCount = int(sizeof(unusedNames) / sizeof(unusedNames[0]));
        if (partialMinint)
        {
            QVERIFY(QDir().mkdir(temporary.path() + "/MiNiNt"));
            for (int i = 0; i < setupFileCount; ++i)
                if (minintFiles & (partialMinintMask << i))
                    putFile(temporary.path() + "/MiNiNt/" + unusedNames[i], "unselected minint bytes");
        }
        const int root = open(QFile::encodeName(temporary.path()), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        QVERIFY(root >= 0);
        QCOMPARE(rufus::PrepareWinPeRootFiles(root, uint16_t(mask), minint, true), error);
        QCOMPARE(close(root), 0);
        QCOMPARE(getFile(sourcePath), originalLoader);
        QCOMPARE(getFile(sifPath), originalSif);
        if (partialMinint)
            for (int i = 0; i < setupFileCount; ++i)
                if (minintFiles & (partialMinintMask << i))
                    QCOMPARE(getFile(temporary.path() + "/MiNiNt/" + unusedNames[i]), QByteArray("unselected minint bytes"));
        if (error == 0)
        {
            QCOMPARE(getFile(temporary.path() + "/ntdetect.com"), QByteArray("detector bytes"));
            QByteArray expected = originalLoader;
            const WINPE_SETUP_PLAN plan = GetWinPeSetupPlan(uint16_t(mask), minint);
            const bool keepsLoader = minint && (partialMinint || loaderDirectory.compare("minint", Qt::CaseInsensitive) == 0);
            QCOMPARE(plan.action == WINPE_KEEP_LOADER, keepsLoader);
            if (plan.action == WINPE_PATCH_LOADER)
                QVERIFY(PatchWinPeLoader(expected.data(), uint32_t(expected.size()), plan.index, minint));
            QCOMPARE(getFile(temporary.path() + "/BOOTMGR"), expected);
            if (plan.action == WINPE_PATCH_LOADER)
            {
                QCOMPARE(quint8(expected[loaderCrcOffset]), quint8(0xeb));
                QCOMPARE(quint8(expected[loaderCrcOffset + 1]), quint8(0x1a));
                QCOMPARE(expected.mid(70, 8), QByteArray(minint ? "rdisk(0)" : "rdisk(1)"));
                const QByteArray setupPath = plan.index == 1 ? "\\amd64\\txtsetup.sif" : "\\i386\\txtsetup.sif";
                QCOMPARE(expected.mid(10, setupPath.size()), setupPath);
                QCOMPARE(expected[10 + setupPath.size()], '\0');
            }
            QCOMPARE(QFile::exists(temporary.path() + "/TXTSETUP.SIF"), !minint);
            if (!minint)
                QCOMPARE(getFile(temporary.path() + "/TXTSETUP.SIF"), QByteArray("[SetupData]\r\n") +
                    WINPE_SETUP_SOURCE_DEVICE + "\r\nOsLoadOptions = \"\"\r\n");
        }
        else
            QVERIFY(!QFile::exists(temporary.path() + "/ntdetect.com"));
        QVERIFY(QDir(temporary.path()).entryList({".rufus-winpe-*"}, QDir::Files | QDir::Hidden).isEmpty());
    }
    void existingRootFiles_data()
    {
        QTest::addColumn<bool>("minint");
        QTest::addColumn<QString>("rootName");
        QTest::addColumn<bool>("identical");
        for (bool minint : {false, true})
        for (const char* name : {"NtDetect.Com", "TxTsEtUp.SiF", "BoOtMgR"})
        for (bool identical : {false, true})
            QTest::newRow(qPrintable(QStringLiteral("%1-%2-%3").arg(minint).arg(name).arg(identical)))
                << minint << QString::fromLatin1(name) << identical;
    }

    void existingRootFiles()
    {
        QFETCH(bool, minint); QFETCH(QString, rootName); QFETCH(bool, identical);
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString family = minint ? QStringLiteral("minint") : QStringLiteral("i386");
        makeTree(temporary.path(), family, family);
        const QString folder = temporary.path() + QLatin1Char('/') + family;
        const QByteArray originalLoader = getFile(sourcePath);
        const QByteArray originalDetector = getFile(folder + QStringLiteral("/NTDETECT.COM"));
        const QByteArray originalSif = getFile(folder + QStringLiteral("/TxtSetup.Sif"));
        const bool rootSif = rootName == QStringLiteral("TxTsEtUp.SiF");
        const QByteArray corresponding = rootName == QStringLiteral("BoOtMgR") ? originalLoader
            : rootSif ? originalSif : originalDetector;
        const QByteArray rootBytes = identical ? corresponding : QByteArray("keep different root file");
        const QString destination = temporary.path() + QLatin1Char('/') + rootName;
        putFile(destination, rootBytes);
        const bool unusedRootSif = minint && rootSif;
        const bool retainedDetector = rootName == QStringLiteral("NtDetect.Com");
        const bool editedRootSif = rootSif && !minint && identical;
        const bool sameLoader = rootName == QStringLiteral("BoOtMgR") && identical;
        const bool editedRootLoader = sameLoader && !minint;
        const bool accepted = unusedRootSif || retainedDetector || editedRootSif || sameLoader;
        struct stat before;
        QCOMPARE(stat(QFile::encodeName(destination), &before), 0);
        const int root = open(QFile::encodeName(temporary.path()), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        QVERIFY(root >= 0);
        const uint16_t mask = minint ? WINPE_MININT : WINPE_I386;
        QCOMPARE(rufus::PrepareWinPeRootFiles(root, mask, minint, true), accepted ? 0 : rootSif ? ENOENT : EEXIST);
        QCOMPARE(close(root), 0);
        QByteArray expectedRoot = rootBytes;
        if (editedRootSif)
            QCOMPARE(InsertConfigSectionText(rootBytes, WINPE_SETUP_SECTION, WINPE_SETUP_SOURCE_DEVICE, expectedRoot), 0);
        if (editedRootLoader)
            QVERIFY(PatchWinPeLoader(expectedRoot.data(), uint32_t(expectedRoot.size()), 0, false));
        QCOMPARE(getFile(destination), expectedRoot);
        QCOMPARE(getFile(sourcePath), originalLoader);
        QCOMPARE(getFile(folder + QStringLiteral("/NTDETECT.COM")), originalDetector);
        QCOMPARE(getFile(folder + QStringLiteral("/TxtSetup.Sif")), originalSif);
        const QStringList outputs = QDir(temporary.path()).entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot);
        struct stat after;
        QCOMPARE(stat(QFile::encodeName(destination), &after), 0);
        if (editedRootSif || editedRootLoader)
        {
            QVERIFY(after.st_ino != before.st_ino);
            QCOMPARE(after.st_mode & 0777, before.st_mode & 0777);
        }
        else
        {
            QCOMPARE(after.st_ino, before.st_ino);
            QCOMPARE(after.st_mtim.tv_sec, before.st_mtim.tv_sec);
            QCOMPARE(after.st_mtim.tv_nsec, before.st_mtim.tv_nsec);
            QCOMPARE(after.st_ctim.tv_sec, before.st_ctim.tv_sec);
            QCOMPARE(after.st_ctim.tv_nsec, before.st_ctim.tv_nsec);
        }
        QStringList expected({rootName});
        if (accepted)
        {
            if (!sameLoader)
                expected.append(QStringLiteral("BOOTMGR"));
            if (!retainedDetector)
                expected.append(QStringLiteral("ntdetect.com"));
            if (!minint && !editedRootSif)
                expected.append(QStringLiteral("TXTSETUP.SIF"));
        }
        expected.sort(Qt::CaseInsensitive);
        QCOMPARE(outputs, expected);
        if (unusedRootSif)
        {
            QCOMPARE(getFile(temporary.path() + QStringLiteral("/BOOTMGR")), originalLoader);
            QCOMPARE(getFile(temporary.path() + QStringLiteral("/ntdetect.com")), originalDetector);
        }
    }

    void retainedDetector_data()
    {
        QTest::addColumn<QString>("issue");
        QTest::addColumn<int>("failure");
        QTest::addColumn<int>("error");
        QTest::addColumn<bool>("different");
        const struct { const char* issue; Fault failure; int error; } cases[] = {
            {"normal", NoFault, 0}, {"empty", NoFault, EBADMSG},
            {"large", NoFault, EFBIG}, {"directory", NoFault, ENOTSUP},
            {"symlink", NoFault, ELOOP}, {"hardlink", NoFault, EMLINK},
            {"duplicate", NoFault, EEXIST}, {"read", RootDetectorReadError, EIO},
            {"cancel", RootDetectorReadCancel, ECANCELED},
            {"change", RootDetectorChange, ESTALE}, {"replace", RootDetectorReplace, ESTALE},
            {"duplicate-during-write", RootDetectorDuplicate, EEXIST},
            {"remove", RootDetectorRemove, ESTALE},
            {"change-late", RootDetectorChangeLater, ESTALE},
            {"source-change", DetectorSourceChange, ESTALE}
        };
        for (const auto& test : cases)
        for (bool different : {false, true})
            QTest::newRow(qPrintable(QStringLiteral("%1-%2").arg(test.issue).arg(different)))
                << QString::fromLatin1(test.issue) << int(test.failure) << test.error << different;
        QTest::newRow("multiple-buffers") << QStringLiteral("multiple-buffers") << int(NoFault) << 0 << true;
    }
    void retainedDetector()
    {
        QFETCH(QString, issue); QFETCH(int, failure); QFETCH(int, error); QFETCH(bool, different);
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        makeTree(temporary.path(), "i386", "i386");
        const QString destination = temporary.path() + "/NtDetect.Com";
        QByteArray rootBytes = different ? QByteArray("different root detector") : QByteArray("detector bytes");
        if (issue == "multiple-buffers")
            rootBytes = QByteArray(2 * 64 * 1024 + 17, char(0xa5));
        const QString sourceDetector = temporary.path() + "/i386/NTDETECT.COM";
        if (issue == "directory") QVERIFY(QDir().mkdir(destination));
        else if (issue == "symlink") QVERIFY(QFile::link(sourceDetector, destination));
        else if (issue == "hardlink") QCOMPARE(link(QFile::encodeName(sourceDetector), QFile::encodeName(destination)), 0);
        else putFile(destination, issue == "empty" ? QByteArray() : issue == "large"
            ? QByteArray(CONFIG_PATCH_MAX_BYTES + 1, 'x') : rootBytes);
        if (issue == "duplicate") putFile(temporary.path() + "/NTDETECT.COM", "detector bytes");
        const QByteArray originalLoader = getFile(sourcePath);
        const QByteArray originalDetector = getFile(sourceDetector);
        struct stat before;
        QCOMPARE(lstat(QFile::encodeName(destination), &before), 0);
        const int root = open(QFile::encodeName(temporary.path()), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        QVERIFY(root >= 0);
        const int descriptors = QDir("/proc/self/fd").entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size();
        fault = Fault(failure);
        active = true;
        const int result = rufus::PrepareWinPeRootFiles(root, WINPE_I386, false, true);
        active = false;
        QCOMPARE(result, error);
        QCOMPARE(QDir("/proc/self/fd").entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size(), descriptors);
        QCOMPARE(close(root), 0);
        const bool sifPublished = error == 0 || failure == RootDetectorChangeLater;
        QCOMPARE(QFile::exists(temporary.path() + "/TXTSETUP.SIF"), sifPublished);
        QCOMPARE(QFile::exists(temporary.path() + "/BOOTMGR"), error == 0);
        QVERIFY(!QFile::exists(temporary.path() + "/ntdetect.com"));
        QCOMPARE(getFile(sourcePath), originalLoader);
        if (error == 0)
        {
            QCOMPARE(writes, 2); // Only SIF and loader; the detector is retained.
            QCOMPARE(getFile(destination), rootBytes);
            struct stat after;
            QCOMPARE(stat(QFile::encodeName(destination), &after), 0);
            QCOMPARE(after.st_ino, before.st_ino);
            QCOMPARE(after.st_mode, before.st_mode);
            QCOMPARE(after.st_uid, before.st_uid);
            QCOMPARE(after.st_gid, before.st_gid);
            QCOMPARE(after.st_mtim.tv_sec, before.st_mtim.tv_sec);
            QCOMPARE(after.st_mtim.tv_nsec, before.st_mtim.tv_nsec);
            QCOMPARE(after.st_ctim.tv_sec, before.st_ctim.tv_sec);
            QCOMPARE(after.st_ctim.tv_nsec, before.st_ctim.tv_nsec);
            QCOMPARE(getFile(temporary.path() + "/TXTSETUP.SIF"), QByteArray("[SetupData]\r\n") +
                WINPE_SETUP_SOURCE_DEVICE + "\r\nOsLoadOptions = \"\"\r\n");
            const QByteArray loader = getFile(temporary.path() + "/BOOTMGR");
            QCOMPARE(quint8(loader[loaderCrcOffset]), quint8(0xeb));
            QCOMPARE(quint8(loader[loaderCrcOffset + 1]), quint8(0x1a));
            QCOMPARE(loader.mid(70, 8), QByteArray("rdisk(1)"));
            const QByteArray expectedPath("\\i386\\txtsetup.sif");
            QCOMPARE(loader.mid(10, expectedPath.size()), expectedPath);
        }
        if (failure == RootDetectorChange || failure == RootDetectorChangeLater)
            QCOMPARE(getFile(destination), QByteArray("changed detector"));
        if (failure == RootDetectorReplace || failure == RootDetectorReadError || failure == RootDetectorReadCancel)
            QCOMPARE(getFile(destination), rootBytes);
        if (failure != DetectorSourceChange)
            QCOMPARE(getFile(sourceDetector), originalDetector);
        QVERIFY(QDir(temporary.path()).entryList({".rufus-winpe-*"}, QDir::Files | QDir::Hidden).isEmpty());
    }

    void existingLoaderContent_data()
    {
        QTest::addColumn<int>("mask");
        QTest::addColumn<QString>("setupDirectory");
        QTest::addColumn<bool>("minint");
        QTest::addColumn<bool>("large");
        QTest::addColumn<bool>("companions");
        QTest::addColumn<QString>("rootKind");
        const struct { const char* name; int mask; const char* directory; bool minint, large, companions; } cases[] = {
            {"i386", WINPE_I386, "i386", false, false, false},
            {"amd64", WINPE_AMD64, "amd64", false, false, false},
            {"mixed", WINPE_I386 | WINPE_AMD64, "amd64", false, false, false},
            {"keep-minint", WINPE_MININT, "minint", true, false, false},
            {"patch-i386-minint", WINPE_I386, "i386", true, false, false},
            {"multiple-buffers", WINPE_I386, "i386", false, true, false},
            {"all-existing-roots", WINPE_I386, "i386", false, false, true},
            {"all-retained-minint-roots", WINPE_MININT, "minint", true, false, true},
            {"i386-partial-minint-roots", WINPE_I386 | partialMinintMask, "i386", true, false, true},
            {"amd64-partial-minint-roots", WINPE_AMD64 | partialMinintMask, "amd64", true, false, true}
        };
        for (const auto& test : cases)
        for (const char* kind : {"identical", "prepatched", "partial"})
        {
            const bool keep = GetWinPeSetupPlan(uint16_t(test.mask), test.minint).action == WINPE_KEEP_LOADER;
            if (keep && strcmp(kind, "identical") != 0) continue;
            QTest::newRow(qPrintable(QStringLiteral("%1-%2").arg(test.name).arg(kind)))
                << test.mask << QString::fromLatin1(test.directory) << test.minint << test.large
                << test.companions << QString::fromLatin1(kind);
        }
    }
    void existingLoaderContent()
    {
        QFETCH(int, mask); QFETCH(QString, setupDirectory); QFETCH(bool, minint);
        QFETCH(bool, large); QFETCH(bool, companions); QFETCH(QString, rootKind);
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const WINPE_SETUP_PLAN plan = GetWinPeSetupPlan(uint16_t(mask), minint);
        makeTree(temporary.path(), plan.loader_directory, setupDirectory);
        QByteArray original = getFile(sourcePath);
        if (large)
        {
            constexpr int transferBytes = 64 * 1024, tailBytes = 123;
            original.append(QByteArray(transferBytes + tailBytes, 'z'));
            putFile(sourcePath, original);
        }
        QByteArray rootBytes = original;
        if (rootKind == "prepatched")
            QVERIFY(PatchWinPeLoader(rootBytes.data(), uint32_t(rootBytes.size()), plan.index, minint));
        if (rootKind == "partial")
        {
            rootBytes[loaderCrcOffset] = char(0xeb);
            rootBytes[loaderCrcOffset + 1] = 0x1a;
        }
        const QString destination = temporary.path() + "/BoOtMgR";
        putFile(destination, rootBytes);
        QCOMPARE(chmod(QFile::encodeName(destination), 0640), 0);
        const QString directorySif = temporary.path() + '/' + setupDirectory + "/TxtSetup.Sif";
        const QString directoryDetector = temporary.path() + '/' + plan.loader_directory + "/NTDETECT.COM";
        const QByteArray beforeSif = getFile(directorySif), beforeDetector = getFile(directoryDetector);
        if (companions)
        {
            putFile(temporary.path() + "/NtDetect.Com", beforeDetector);
            putFile(temporary.path() + "/TxTsEtUp.SiF", rootSifText);
        }
        struct stat before;
        QCOMPARE(stat(QFile::encodeName(destination), &before), 0);
        const int root = open(QFile::encodeName(temporary.path()), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        QVERIFY(root >= 0);
        const int descriptors = QDir("/proc/self/fd").entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size();
        active = true;
        const int error = rufus::PrepareWinPeRootFiles(root, uint16_t(mask), minint, true);
        active = false;
        QCOMPARE(error, 0);
        QCOMPARE(QDir("/proc/self/fd").entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size(), descriptors);
        QCOMPARE(close(root), 0);
        QByteArray expected = rootBytes;
        const bool patch = plan.action == WINPE_PATCH_LOADER;
        if (patch)
            QVERIFY(PatchWinPeLoader(expected.data(), uint32_t(expected.size()), plan.index, minint));
        QCOMPARE(getFile(destination), expected);
        QCOMPARE(getFile(sourcePath), original);
        QCOMPARE(getFile(directorySif), beforeSif);
        QCOMPARE(getFile(directoryDetector), beforeDetector);
        QVERIFY(!QFile::exists(temporary.path() + "/BOOTMGR"));
        struct stat after;
        QCOMPARE(stat(QFile::encodeName(destination), &after), 0);
        QCOMPARE(after.st_mode & 0777, before.st_mode & 0777);
        QCOMPARE(after.st_uid, before.st_uid);
        QCOMPARE(after.st_gid, before.st_gid);
        if (patch)
            QVERIFY(after.st_ino != before.st_ino);
        else
        {
            QCOMPARE(writes, companions ? 0 : 1); // No loader staging in the keep plan.
            QCOMPARE(after.st_ino, before.st_ino);
            QCOMPARE(after.st_mtim.tv_sec, before.st_mtim.tv_sec);
            QCOMPARE(after.st_mtim.tv_nsec, before.st_mtim.tv_nsec);
            QCOMPARE(after.st_ctim.tv_sec, before.st_ctim.tv_sec);
            QCOMPARE(after.st_ctim.tv_nsec, before.st_ctim.tv_nsec);
        }
        if (companions)
        {
            QCOMPARE(getFile(temporary.path() + "/NtDetect.Com"), beforeDetector);
            const QByteArray expectedSif = plan.copy_sif ? QByteArray(";root-only\r\n[SetupData]\r\n")
                + WINPE_SETUP_SOURCE_DEVICE + "\r\nRootValue=keep\r\n" : rootSifText;
            QCOMPARE(getFile(temporary.path() + "/TxTsEtUp.SiF"), expectedSif);
        }
        QVERIFY(QDir(temporary.path()).entryList({".rufus-winpe-*"}, QDir::Files | QDir::Hidden).isEmpty());
    }
    void existingLoaderFailure_data()
    {
        QTest::addColumn<QString>("issue");
        QTest::addColumn<int>("failure");
        QTest::addColumn<int>("error");
        QTest::addColumn<bool>("committed");
        QTest::addColumn<bool>("minint");
        QTest::addColumn<bool>("prepatched");
        const struct { const char* issue; Fault failure; int error; bool committed; } cases[] = {
            {"changed-byte", NoFault, EEXIST, false}, {"wrong-family", NoFault, EEXIST, false},
            {"prepatched-tail", NoFault, EEXIST, false}, {"second-pass", NoFault, EEXIST, false},
            {"wrong-drive", NoFault, EEXIST, false},
            {"opaque", NoFault, EEXIST, false}, {"empty", NoFault, EBADMSG, false},
            {"large", NoFault, EFBIG, false}, {"duplicate", NoFault, EEXIST, false},
            {"symlink", NoFault, ELOOP, false}, {"hardlink", NoFault, EMLINK, false},
            {"directory", NoFault, ENOTSUP, false}, {"fifo", NoFault, ENOTSUP, false},
            {"read", RootLoaderReadError, EIO, false}, {"cancel-read", RootLoaderReadCancel, ECANCELED, false},
            {"write", WriteLoaderError, ENOSPC, false}, {"verify", RootLoaderVerifyError, EBADMSG, false},
            {"file-sync", RootLoaderSyncError, EIO, false}, {"rename", RootLoaderRenameError, EIO, false},
            {"close", RootLoaderCloseError, EIO, false}, {"mode", RootLoaderModeError, EPERM, false},
            {"cancel-write", RootLoaderCancelWrite, ECANCELED, false},
            {"change", RootLoaderChange, ESTALE, false}, {"replace", RootLoaderReplace, ESTALE, false},
            {"ambiguous-during-write", RootLoaderDuplicate, EEXIST, false}, {"remove", RootLoaderRemove, ESTALE, false},
            {"change-during-loader-write", RootLoaderChangeDuringWrite, ESTALE, false},
            {"replace-during-loader-write", RootLoaderReplaceDuringWrite, ESTALE, false},
            {"directory-sync", RootLoaderDirectorySyncError, EIO, true},
            {"committed-loader-changed", RootLoaderCommittedChange, ESTALE, true},
            {"directory-source-changed", SourceChangeLater, ESTALE, false}
        };
        for (const auto& test : cases)
        for (bool prepatched : {false, true})
        {
            if (prepatched && test.failure == NoFault) continue;
            QTest::newRow(qPrintable(QStringLiteral("%1-%2").arg(test.issue).arg(prepatched)))
                << QString::fromLatin1(test.issue) << int(test.failure)
                << test.error << test.committed << false << prepatched;
        }
        QTest::newRow("keep-prepatched") << QStringLiteral("normal") << int(NoFault)
            << EEXIST << false << true << true;
        for (const auto& test : cases)
        {
            const bool retentionFault = test.failure == RootLoaderReadError || test.failure == RootLoaderReadCancel
                || test.failure == RootLoaderChange || test.failure == RootLoaderReplace
                || test.failure == RootLoaderDuplicate || test.failure == RootLoaderRemove;
            if (retentionFault)
                QTest::newRow((QByteArray("keep-") + test.issue).constData()) << QString::fromLatin1(test.issue)
                    << int(test.failure) << test.error << false << true << false;
        }
    }
    void existingLoaderFailure()
    {
        QFETCH(QString, issue); QFETCH(int, failure); QFETCH(int, error);
        QFETCH(bool, committed); QFETCH(bool, minint); QFETCH(bool, prepatched);
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString family = minint ? QStringLiteral("minint") : QStringLiteral("i386");
        makeTree(temporary.path(), family, family);
        QByteArray directoryLoader = getFile(sourcePath);
        if (issue == "second-pass")
        {
            const QByteArray path("\\minint\\txtsetup.sif");
            directoryLoader.replace(loaderPathOffset, path.size() * 2, path + path);
            putFile(sourcePath, directoryLoader);
        }
        QByteArray original = directoryLoader;
        if (prepatched || issue == "prepatched-tail" || issue == "second-pass")
            QVERIFY(PatchWinPeLoader(original.data(), uint32_t(original.size()), 0, false));
        if (issue == "prepatched-tail") original[original.size() - 1] ^= 1;
        if (issue == "wrong-family") QVERIFY(PatchWinPeLoader(original.data(), uint32_t(original.size()), 1, false));
        if (issue == "wrong-drive") original[loaderDriveOffset + rdiskNumberOffset] = '2';
        if (issue == "changed-byte") original[original.size() - 1] ^= 1;
        if (issue == "opaque") original = "unqualified root loader";
        if (issue == "empty") original.clear();
        if (issue == "large") original = QByteArray(CONFIG_PATCH_MAX_BYTES + 1, 'x');
        const QString destination = temporary.path() + "/BoOtMgR";
        const bool regularRoot = issue != "symlink" && issue != "hardlink" && issue != "directory" && issue != "fifo";
        if (issue == "symlink") QVERIFY(QFile::link(sourcePath, destination));
        else if (issue == "hardlink")
        {
            const QString other = temporary.path() + "/linked-loader";
            putFile(other, original);
            QCOMPARE(link(QFile::encodeName(other), QFile::encodeName(destination)), 0);
        }
        else if (issue == "directory") QVERIFY(QDir().mkdir(destination));
        else if (issue == "fifo") QCOMPARE(mkfifo(QFile::encodeName(destination), 0600), 0);
        else putFile(destination, original);
        if (regularRoot) QCOMPARE(chmod(QFile::encodeName(destination), 0640), 0);
        if (issue == "duplicate") putFile(temporary.path() + "/BOOTMGR", original);
        const int root = open(QFile::encodeName(temporary.path()), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        QVERIFY(root >= 0);
        const int descriptors = QDir("/proc/self/fd").entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size();
        fault = Fault(failure);
        active = true;
        const int result = rufus::PrepareWinPeRootFiles(root, minint ? WINPE_MININT : WINPE_I386, minint, true);
        active = false;
        QCOMPARE(result, error);
        if (failure == NoFault)
        {
            QCOMPARE(writes, 0); // Admission errors precede every publication.
            QVERIFY(!QFile::exists(temporary.path() + "/TXTSETUP.SIF"));
            QVERIFY(!QFile::exists(temporary.path() + "/ntdetect.com"));
        }
        QCOMPARE(QDir("/proc/self/fd").entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size(), descriptors);
        QCOMPARE(close(root), 0);
        if (failure != SourceChangeLater) QCOMPARE(getFile(sourcePath), directoryLoader);
        if (regularRoot && failure != RootLoaderRemove)
        {
            QByteArray expected = original;
            if (committed) QVERIFY(PatchWinPeLoader(expected.data(), uint32_t(expected.size()), 0, false));
            const bool changedRoot = failure == RootLoaderChange || failure == RootLoaderCommittedChange
                || failure == RootLoaderChangeDuringWrite;
            if (changedRoot) expected = "changed root loader";
            QCOMPARE(getFile(destination), expected);
        }
        if (failure == RootLoaderRemove) QVERIFY(!QFile::exists(destination));
        QVERIFY(QDir(temporary.path()).entryList({".rufus-winpe-*"}, QDir::Files | QDir::Hidden).isEmpty());
    }

    void existingSifContent_data()
    {
        QTest::addColumn<int>("mask");
        QTest::addColumn<QString>("setupDirectory");
        QTest::addColumn<int>("encoding");
        for (int encoding = 0; encoding < 3; ++encoding)
        {
            QTest::newRow(qPrintable(QStringLiteral("i386-%1").arg(encoding))) << int(WINPE_I386) << QStringLiteral("i386") << encoding;
            QTest::newRow(qPrintable(QStringLiteral("amd64-%1").arg(encoding))) << int(WINPE_AMD64) << QStringLiteral("amd64") << encoding;
            QTest::newRow(qPrintable(QStringLiteral("mixed-%1").arg(encoding))) << int(WINPE_AMD64 | WINPE_I386) << QStringLiteral("amd64") << encoding;
        }
    }
    void existingSifContent()
    {
        QFETCH(int, mask); QFETCH(QString, setupDirectory); QFETCH(int, encoding);
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        makeTree(temporary.path(), "i386", setupDirectory, "OsLoadOptions=/fastdetect\r\n");
        const QString destination = temporary.path() + "/TxTsEtUp.SiF";
        const QString originalText = QStringLiteral(";root-only\n[SetupData]\nRootValue=keep\n[Other]\nUnrelated=retain\n[SetupData]\nLast=retain\n");
        const QString expectedText = QStringLiteral(";root-only\r\n[SetupData]\r\n") + WINPE_SETUP_SOURCE_DEVICE +
            "\r\nRootValue=keep\r\n[Other]\r\nUnrelated=retain\r\n[SetupData]\r\n" + WINPE_SETUP_SOURCE_DEVICE + "\r\nLast=retain\r\n";
        putFile(destination, encoded(originalText, encoding));
        QCOMPARE(chmod(QFile::encodeName(destination), 0640), 0);
        const QString directorySif = temporary.path() + '/' + setupDirectory + "/TxtSetup.Sif";
        const QByteArray beforeSif = getFile(directorySif), beforeLoader = getFile(sourcePath);
        struct stat before;
        QCOMPARE(stat(QFile::encodeName(destination), &before), 0);
        const int root = open(QFile::encodeName(temporary.path()), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        QVERIFY(root >= 0);
        QCOMPARE(rufus::PrepareWinPeRootFiles(root, uint16_t(mask), false, true), 0);
        QCOMPARE(close(root), 0);
        QCOMPARE(getFile(destination), encoded(expectedText, encoding));
        QCOMPARE(getFile(directorySif), beforeSif);
        QCOMPARE(getFile(sourcePath), beforeLoader);
        QCOMPARE(getFile(temporary.path() + "/i386/NTDETECT.COM"), QByteArray("detector bytes"));
        QVERIFY(!QFile::exists(temporary.path() + "/TXTSETUP.SIF"));
        struct stat after;
        QCOMPARE(stat(QFile::encodeName(destination), &after), 0);
        QVERIFY(after.st_ino != before.st_ino);
        QCOMPARE(after.st_mode & 0777, before.st_mode & 0777);
        QCOMPARE(after.st_uid, before.st_uid);
        QCOMPARE(after.st_gid, before.st_gid);
        const QByteArray loader = getFile(temporary.path() + "/BOOTMGR");
        const QByteArray expectedPath = setupDirectory == "amd64" ? QByteArray("\\amd64\\txtsetup.sif") : QByteArray("\\i386\\txtsetup.sif");
        QCOMPARE(loader.mid(10, expectedPath.size()), expectedPath);
        QVERIFY(QDir(temporary.path()).entryList({".rufus-winpe-*"}, QDir::Files | QDir::Hidden).isEmpty());
    }
    void existingSifFailure_data()
    {
        QTest::addColumn<QString>("issue");
        QTest::addColumn<int>("failure");
        QTest::addColumn<int>("error");
        QTest::addColumn<bool>("committed");
        const struct { const char* issue; Fault failure; int error; bool committed; } cases[] = {
            {"no-section", NoFault, ENOENT, false}, {"localized", NoFault, ENOTSUP, false}, {"bad-utf8", NoFault, EILSEQ, false},
            {"nls-utf8", NoFault, ENOTSUP, false}, {"nls-1252", NoFault, EILSEQ, false},
            {"nls-932", NoFault, EILSEQ, false},
            {"empty", NoFault, EBADMSG, false}, {"large", NoFault, EFBIG, false},
            {"bad-utf16", NoFault, EILSEQ, false}, {"duplicate", NoFault, EEXIST, false},
            {"symlink", NoFault, ELOOP, false}, {"hardlink", NoFault, EMLINK, false},
            {"directory", NoFault, ENOTSUP, false},
            {"read", RootSifReadError, EIO, false}, {"cancel-read", RootSifReadCancel, ECANCELED, false},
            {"write", WriteSifError, ENOSPC, false}, {"verify", RootSifVerifyError, EBADMSG, false},
            {"file-sync", RootSifSyncError, EIO, false}, {"rename", RootSifRenameError, EIO, false},
            {"close", RootSifCloseError, EIO, false}, {"mode", RootSifModeError, EPERM, false},
            {"cancel-write", RootSifCancelWrite, ECANCELED, false},
            {"change", RootSifChange, ESTALE, false}, {"replace", RootSifReplace, ESTALE, false},
            {"ambiguous-during-write", RootSifDuplicate, EEXIST, false}, {"remove", RootSifRemove, ESTALE, false},
            {"directory-sync", RootSifDirectorySyncError, EIO, true},
            {"loader-write", WriteLoaderError, ENOSPC, true}, {"loader-cancel", CancelLoaderWrite, ECANCELED, true},
            {"committed-sif-changed", RootSifChangeLater, ESTALE, true},
            {"source-changed-late", SourceChangeLater, ESTALE, true}
        };
        for (const auto& test : cases)
            QTest::newRow(test.issue) << QString::fromLatin1(test.issue) << int(test.failure) << test.error << test.committed;
    }
    void existingSifFailure()
    {
        QFETCH(QString, issue); QFETCH(int, failure); QFETCH(int, error); QFETCH(bool, committed);
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        makeTree(temporary.path(), "i386", "i386");
        const QString destination = temporary.path() + "/TxTsEtUp.SiF";
        const QString directorySif = temporary.path() + "/i386/TxtSetup.Sif";
        QByteArray original = rootSifText;
        if (issue == "no-section") original = "[Other]\r\nKeep=yes\r\n";
        if (issue == "localized") original.append(QString::fromUtf8("é").toUtf8());
        if (issue == "bad-utf8") original.append(char(0xe9));
        if (issue == "nls-utf8") original += "[NLS]\r\nAnsiCodepage=c_1252.nls\r\n;\xc3\xa9\r\n";
        if (issue == "nls-1252") original += "[NLS]\r\nAnsiCodepage=c_1252.nls\r\n;\xe9\r\n";
        if (issue == "nls-932") original += "[NLS]\r\nAnsiCodepage=c_932.nls\r\n;\x82\xa0\r\n";
        if (issue == "empty") original.clear();
        if (issue == "large") original = QByteArray(CONFIG_PATCH_MAX_BYTES + 1, 'x');
        if (issue == "bad-utf16") original = QByteArray("\xff\xfe\x5b", 3);
        if (issue == "symlink") QVERIFY(QFile::link(directorySif, destination));
        else if (issue == "hardlink") QCOMPARE(link(QFile::encodeName(directorySif), QFile::encodeName(destination)), 0);
        else if (issue == "directory") QVERIFY(QDir().mkdir(destination));
        else putFile(destination, original);
        if (issue == "duplicate") putFile(temporary.path() + "/TXTSETUP.SIF", original);
        const QByteArray beforeDirectory = getFile(directorySif);
        const int root = open(QFile::encodeName(temporary.path()), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        QVERIFY(root >= 0);
        const int descriptors = QDir("/proc/self/fd").entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size();
        fault = Fault(failure);
        active = true;
        const int result = rufus::PrepareWinPeRootFiles(root, WINPE_I386, false, true);
        active = false;
        QCOMPARE(result, error);
        QCOMPARE(QDir("/proc/self/fd").entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size(), descriptors);
        QCOMPARE(close(root), 0);
        QCOMPARE(getFile(directorySif), beforeDirectory);
        QVERIFY(!QFile::exists(temporary.path() + "/BOOTMGR"));
        if (issue != "symlink" && issue != "hardlink" && issue != "directory" && failure != RootSifRemove)
        {
            QByteArray expected = original;
            if (committed)
                expected = ";root-only\r\n[SetupData]\r\n" + QByteArray(WINPE_SETUP_SOURCE_DEVICE) + "\r\nRootValue=keep\r\n";
            if (failure == RootSifChange || failure == RootSifChangeLater) expected = "changed root SIF";
            QCOMPARE(getFile(destination), expected);
        }
        QVERIFY(QDir(temporary.path()).entryList({".rufus-winpe-*"}, QDir::Files | QDir::Hidden).isEmpty());
    }

    void rejectedSource_data()
    {
        QTest::addColumn<int>("mode");
        QTest::addColumn<int>("error");
        QTest::newRow("unknown-options") << int(UnknownOptions) << ENOTSUP;
        QTest::newRow("missing-loader") << int(MissingLoader) << ENOENT;
        QTest::newRow("missing-sif") << int(MissingSif) << ENOENT;
        QTest::newRow("missing-ntdetect") << int(MissingDetector) << ENOENT;
        QTest::newRow("empty-loader") << int(EmptyLoader) << EBADMSG;
        QTest::newRow("short-loader") << int(ShortLoader) << EBADMSG;
        QTest::newRow("large-loader") << int(OversizedLoader) << EFBIG;
        QTest::newRow("ambiguous-directory") << int(AmbiguousDirectory) << EEXIST;
        QTest::newRow("ambiguous-file") << int(AmbiguousFile) << EEXIST;
        QTest::newRow("symlink-file") << int(SymlinkFile) << ELOOP;
        QTest::newRow("hardlink-file") << int(HardlinkFile) << EMLINK;
        QTest::newRow("directory-file") << int(SpecialFile) << ENOTSUP;
        QTest::newRow("existing-destination") << int(ExistingBootmgr) << EEXIST;
        QTest::newRow("bad-sif") << int(MissingSection) << ENOENT;
        QTest::newRow("symlink-directory") << int(SymlinkDirectory) << ENOTDIR;
        QTest::newRow("existing-destination-symlink") << int(SymlinkDestination) << ELOOP;
        QTest::newRow("existing-late-destination") << int(ExistingSif) << ENOENT;
    }
    void rejectedSource()
    {
        QFETCH(int, mode);
        QFETCH(int, error);
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        makeTree(temporary.path(), "i386", "i386");
        const QString sifPath = temporary.path() + "/i386/TxtSetup.Sif";
        const QString detector = temporary.path() + "/i386/NTDETECT.COM";
        if (mode == MissingLoader || (mode == SymlinkFile || mode == HardlinkFile || mode == SpecialFile)) QVERIFY(QFile::remove(sourcePath));
        if (mode == MissingSif) QVERIFY(QFile::remove(sifPath));
        if (mode == MissingDetector) QVERIFY(QFile::remove(detector));
        if (mode == EmptyLoader) putFile(sourcePath, {});
        if (mode == ShortLoader) putFile(sourcePath, QByteArray(32, 'x'));
        if (mode == OversizedLoader) putFile(sourcePath, QByteArray(CONFIG_PATCH_MAX_BYTES + 1, 'x'));
        if (mode == AmbiguousDirectory) QVERIFY(QDir().mkdir(temporary.path() + "/I386"));
        if (mode == AmbiguousFile) putFile(temporary.path() + "/i386/SETUPLDR.BIN", fixtureLoader());
        if (mode == SymlinkFile) QVERIFY(QFile::link(detector, sourcePath));
        if (mode == HardlinkFile) QCOMPARE(link(QFile::encodeName(detector), QFile::encodeName(sourcePath)), 0);
        if (mode == SpecialFile) QVERIFY(QDir().mkdir(sourcePath));
        if (mode == ExistingBootmgr) putFile(temporary.path() + "/BootMgr", "keep existing");
        if (mode == MissingSection) putFile(sifPath, "[Other]\n");
        if (mode == SymlinkDirectory)
        {
            QVERIFY(QDir().rename(temporary.path() + "/i386", temporary.path() + "/outside"));
            QVERIFY(QFile::link(temporary.path() + "/outside", temporary.path() + "/i386"));
        }
        if (mode == SymlinkDestination) QVERIFY(QFile::link(detector, temporary.path() + "/BOOTMGR"));
        if (mode == ExistingSif) putFile(temporary.path() + "/TXTSETUP.SIF", "keep existing sif");
        const int root = open(QFile::encodeName(temporary.path()), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        QVERIFY(root >= 0);
        QCOMPARE(rufus::PrepareWinPeRootFiles(root, WINPE_I386, false, mode != UnknownOptions), error);
        QCOMPARE(close(root), 0);
        QVERIFY(!QFile::exists(temporary.path() + "/ntdetect.com"));
        if (mode == ExistingBootmgr) QCOMPARE(getFile(temporary.path() + "/BootMgr"), QByteArray("keep existing"));
        if (mode == ExistingSif) QCOMPARE(getFile(temporary.path() + "/TXTSETUP.SIF"), QByteArray("keep existing sif"));
    }
    void io_data()
    {
        QTest::addColumn<int>("failure");
        QTest::addColumn<int>("error");
        QTest::newRow("short-io-and-eintr") << int(ShortIo) << 0;
        QTest::newRow("read") << int(ReadError) << EIO;
        QTest::newRow("write") << int(WriteError) << ENOSPC;
        QTest::newRow("zero-write") << int(ZeroWrite) << EIO;
        QTest::newRow("verify") << int(VerifyMismatch) << EBADMSG;
        QTest::newRow("file-sync") << int(FileSyncError) << EIO;
        QTest::newRow("directory-sync") << int(DirectorySyncError) << EIO;
        QTest::newRow("rename") << int(RenameError) << EIO;
        QTest::newRow("rename-flags-unavailable") << int(RenameInvalidFlags) << 0;
        QTest::newRow("rename-syscall-unavailable") << int(RenameMissingSyscall) << 0;
        QTest::newRow("rename-unsupported") << int(RenameUnsupported) << 0;
        QTest::newRow("link-failed") << int(LinkError) << ENOSPC;
        QTest::newRow("link-destination-race") << int(LinkCollision) << EEXIST;
        QTest::newRow("close") << int(CloseError) << EIO;
        QTest::newRow("cancel-write") << int(CancelWrite) << ECANCELED;
        QTest::newRow("source-changed") << int(SourceChange) << ESTALE;
        QTest::newRow("source-replaced") << int(SourcePathChange) << ESTALE;
        QTest::newRow("directory-replaced") << int(DirectoryPathChange) << ESTALE;
        QTest::newRow("case-collision-during-write") << int(DestinationCollision) << EEXIST;
        QTest::newRow("extra-output") << int(ExtraOutput) << EBADMSG;
    }
    void io()
    {
        QFETCH(int, failure);
        QFETCH(int, error);
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        makeTree(temporary.path(), "i386", "i386");
        const int root = open(QFile::encodeName(temporary.path()), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        QVERIFY(root >= 0);
        fault = Fault(failure);
        const int descriptors = QDir("/proc/self/fd").entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size();
        active = true;
        const int result = rufus::PrepareWinPeRootFiles(root, WINPE_I386, false, true);
        active = false;
        QCOMPARE(result, error);
        QCOMPARE(QDir("/proc/self/fd").entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size(), descriptors);
        QCOMPARE(close(root), 0);
        const bool collision = failure == LinkCollision;
        const bool published = error == 0 || failure == DirectorySyncError || collision;
        QCOMPARE(QFile::exists(temporary.path() + "/ntdetect.com"), published);
        if (published) QCOMPARE(getFile(temporary.path() + "/ntdetect.com"), collision
            ? QByteArray("keep existing destination") : QByteArray("detector bytes"));
        if (error == 0)
        {
            QVERIFY(QFile::exists(temporary.path() + "/BOOTMGR"));
            QVERIFY(getFile(temporary.path() + "/TXTSETUP.SIF").contains(WINPE_SETUP_SOURCE_DEVICE));
            struct stat detector;
            QVERIFY(stat(QFile::encodeName(temporary.path() + "/ntdetect.com").constData(), &detector) == 0);
            QCOMPARE(detector.st_nlink, nlink_t(1));
        }
        if (failure == DestinationCollision)
            QCOMPARE(getFile(temporary.path() + "/NtDetect.Com"), QByteArray("keep existing destination"));
        QVERIFY(QDir(temporary.path()).entryList({".rufus-winpe-*"}, QDir::Files | QDir::Hidden).isEmpty());
    }
    void lateFailure_data()
    {
        QTest::addColumn<int>("failure");
        QTest::addColumn<int>("error");
        QTest::addColumn<bool>("sifWritten");
        QTest::newRow("sif-write-failed") << int(WriteSifError) << ENOSPC << false;
        QTest::newRow("loader-write-failed") << int(WriteLoaderError) << ENOSPC << true;
        QTest::newRow("loader-cancelled") << int(CancelLoaderWrite) << ECANCELED << true;
        QTest::newRow("source-changed-late") << int(SourceChangeLater) << ESTALE << true;
    }
    void lateFailure()
    {
        QFETCH(int, failure);
        QFETCH(int, error);
        QFETCH(bool, sifWritten);
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        makeTree(temporary.path(), "i386", "i386");
        const int root = open(QFile::encodeName(temporary.path()), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        QVERIFY(root >= 0);
        fault = Fault(failure);
        active = true;
        const int result = rufus::PrepareWinPeRootFiles(root, WINPE_I386, false, true);
        active = false;
        QCOMPARE(result, error);
        QCOMPARE(close(root), 0);
        QCOMPARE(getFile(temporary.path() + "/ntdetect.com"), QByteArray("detector bytes"));
        QCOMPARE(QFile::exists(temporary.path() + "/TXTSETUP.SIF"), sifWritten);
        QVERIFY(!QFile::exists(temporary.path() + "/BOOTMGR"));
        QVERIFY(QDir(temporary.path()).entryList({".rufus-winpe-*"}, QDir::Files | QDir::Hidden).isEmpty());
    }
    void encodedRootFiles()
    {
        const QString text = QString::fromUtf8("[SetupData]\n;é😀\n");
        const QString expected = QStringLiteral("[SetupData]\r\n") + WINPE_SETUP_SOURCE_DEVICE +
            QString::fromUtf8("\r\n;é😀\r\n");
        for (int encoding = 1; encoding <= 2; ++encoding)
        {
            QTemporaryDir temporary;
            QVERIFY(temporary.isValid());
            makeTree(temporary.path(), "i386", "i386", encoded(text, encoding));
            const int root = open(QFile::encodeName(temporary.path()), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            QVERIFY(root >= 0);
            QCOMPARE(rufus::PrepareWinPeRootFiles(root, WINPE_I386, false, true), 0);
            QCOMPARE(close(root), 0);
            QCOMPARE(getFile(temporary.path() + "/TXTSETUP.SIF"), encoded(expected, encoding));
            QCOMPARE(getFile(temporary.path() + "/i386/TxtSetup.Sif"), encoded(text, encoding));
        }
    }
    void minintDoesNotCopySif_data()
    {
        QTest::addColumn<QByteArray>("sourceSif");
        QTest::newRow("missing") << QByteArray();
        QTest::newRow("ansi-1252") << QByteArray("[SetupData]\r\nOsLoadOptions=/minint\r\n;\xe9\r\n");
        QTest::newRow("ansi-932") << QByteArray("[SetupData]\r\nOsLoadOptions=/minint\r\n;\x82\xa0\r\n");
    }
    void minintDoesNotCopySif()
    {
        QFETCH(QByteArray, sourceSif);
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        makeTree(temporary.path(), "minint", "minint", sourceSif);
        const QString directorySif = temporary.path() + "/minint/TxtSetup.Sif";
        if (sourceSif.isEmpty())
            QVERIFY(QFile::remove(directorySif));
        const QByteArray rootSif(";\x82\xa0\r\n[SetupData]\r\n;\xe9\r\n");
        putFile(temporary.path() + "/TXTSETUP.SIF", rootSif);
        struct stat before;
        QCOMPARE(stat(QFile::encodeName(temporary.path() + "/TXTSETUP.SIF"), &before), 0);
        const int root = open(QFile::encodeName(temporary.path()), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        QVERIFY(root >= 0);
        QCOMPARE(rufus::PrepareWinPeRootFiles(root, WINPE_MININT, true, true), 0);
        QCOMPARE(close(root), 0);
        QCOMPARE(getFile(temporary.path() + "/BOOTMGR"), fixtureLoader());
        QCOMPARE(getFile(temporary.path() + "/TXTSETUP.SIF"), rootSif);
        struct stat after;
        QCOMPARE(stat(QFile::encodeName(temporary.path() + "/TXTSETUP.SIF"), &after), 0);
        QCOMPARE(after.st_ino, before.st_ino);
        QCOMPARE(after.st_mtim.tv_sec, before.st_mtim.tv_sec);
        QCOMPARE(after.st_mtim.tv_nsec, before.st_mtim.tv_nsec);
        if (!sourceSif.isEmpty())
            QCOMPARE(getFile(directorySif), sourceSif);
    }
    void cancelAndArguments()
    {
        QTemporaryDir temporary;
        makeTree(temporary.path(), "i386", "i386");
        const int root = open(QFile::encodeName(temporary.path()), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        QVERIFY(root >= 0);
        cancelled = true;
        QCOMPARE(rufus::PrepareWinPeRootFiles(root, WINPE_I386, false, true), ECANCELED);
        QByteArray output;
        QCOMPARE(InsertConfigSectionText("[SetupData]\n", "[SetupData]", "value", output), ECANCELED);
        cancelled = false;
        QCOMPARE(rufus::PrepareWinPeRootFiles(-1, WINPE_I386, false, true), EINVAL);
        QCOMPARE(InsertConfigSectionText({}, nullptr, "value", output), EINVAL);
        QCOMPARE(InsertConfigSectionText({}, "", "value", output), EINVAL);
        QCOMPARE(close(root), 0);
        QVERIFY(!QFile::exists(temporary.path() + "/ntdetect.com"));
    }
};

QTEST_GUILESS_MAIN(WinPePrepareTest)
#include "test_winpe_prepare.moc"
