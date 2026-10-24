// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/grub2_cache.h"
#include "file_timestamp_fixture.h"
#include "../../res/grub2/grub2_version.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>
#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace
{
enum Fault {
    None, DeniedOpen, InterruptedRead, PartialRead, FailedRead, FailedLaterRead,
    EarlyEnd, CancelRead, CancelWithError, ChangeBytes, TruncateFile, ReplaceFile,
    RemoveFile, RenameDirectory, RetargetLink
};
bool active = false;
bool cancelled = false;
Fault fault = None;
int opens = 0;
int reads = 0;
int mutationError = 0;
bool descriptorValid = true;
QByteArray cachePath;
QByteArray replacementPath;
constexpr quint64 partitionOffset = 1024 * 1024;

// Change a real file after the reader has taken its initial identity snapshot.
// Restoring mtime in ChangeBytes verifies that the existing ctime check matters.
void changeInput()
{
    if (fault == ChangeBytes || fault == TruncateFile)
    {
        int fd = ::open(cachePath.constData(), O_WRONLY | O_CLOEXEC);
        if (fd < 0) { mutationError = errno; return; }
        struct stat info;
        if (fstat(fd, &info) != 0)
            mutationError = errno;
        else if (fault == TruncateFile)
            mutationError = ftruncate(fd, 512) == 0 ? 0 : errno;
        else
        {
            if (!WaitForFileTimestampTick(&info.st_ctim)) {
                mutationError = ETIMEDOUT;
                close(fd);
                return;
            }
            const char changed = 'x';
            mutationError = pwrite(fd, &changed, 1, 0) == 1 ? 0 : EIO;
            const timespec times[] = {info.st_atim, info.st_mtim};
            if (futimens(fd, times) != 0) mutationError = errno;
        }
        close(fd);
    }
    else if (fault == ReplaceFile)
        mutationError = ::rename(replacementPath.constData(), cachePath.constData()) == 0 ? 0 : errno;
    else if (fault == RemoveFile || fault == RetargetLink)
    {
        mutationError = unlink(cachePath.constData()) == 0 ? 0 : errno;
        if (fault == RetargetLink && mutationError == 0)
            mutationError = symlink(replacementPath.constData(), cachePath.constData()) == 0 ? 0 : errno;
    }
    else if (fault == RenameDirectory)
    {
        const QByteArray parent = QFile::encodeName(QFileInfo(QString::fromLocal8Bit(cachePath)).absolutePath());
        mutationError = ::rename(parent.constData(), (parent + "-moved").constData()) == 0 ? 0 : errno;
    }
}
}

extern "C" int IsIsoScanCancelled(void) { return cancelled; }
extern "C" void uprintf(const char*, ...) {}
extern "C" int __real_open64(const char*, int, ...);
extern "C" ssize_t __real_pread64(int, void*, size_t, off64_t);

extern "C" int __wrap_open64(const char* path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & O_CREAT)
    {
        va_list arguments;
        va_start(arguments, flags);
        mode = va_arg(arguments, int);
        va_end(arguments);
    }
    const bool cacheOpen = active && cachePath == path && (flags & O_ACCMODE) == O_RDONLY;
    if (cacheOpen)
    {
        ++opens;
        if (fault == DeniedOpen) { errno = EACCES; return -1; }
    }
    return __real_open64(path, flags, mode);
}

extern "C" ssize_t __wrap_pread64(int fd, void* data, size_t size, off64_t offset)
{
    if (!active) return __real_pread64(fd, data, size, offset);
    ++reads;
    descriptorValid = descriptorValid && (fcntl(fd, F_GETFD) & FD_CLOEXEC)
        && (fcntl(fd, F_GETFL) & O_NONBLOCK) && lseek(fd, 0, SEEK_CUR) == 0;
    if (fault == InterruptedRead && reads == 1) { errno = EINTR; return -1; }
    const bool readError = fault == FailedRead || fault == CancelWithError
        || (fault == FailedLaterRead && reads > 1);
    if (readError)
    {
        if (fault == CancelWithError) cancelled = true;
        errno = EIO;
        return -1;
    }
    if (fault == EarlyEnd && reads > 1) return 0;
    if (fault == PartialRead || fault == EarlyEnd || fault == FailedLaterRead)
        size = std::min(size, size_t(137));
    const ssize_t result = __real_pread64(fd, data, size, offset);
    if (reads == 1 && result > 0)
    {
        if (fault == CancelRead) cancelled = true;
        changeInput();
    }
    return result;
}

class TestGrub2Cache : public QObject
{
    Q_OBJECT
    QTemporaryDir directory_;
    RUFUS_IMG_REPORT report_ = {};
    rufus::Grub2CachedCore core_;
    QByteArray original_;
    bool grub4dos_ = false;

    bool writeFile(const QString& path, const QByteArray& bytes)
    {
        if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
        QFile file(path);
        return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size();
    }

    int readCache(quint64 offset = partitionOffset, const QString& files = QString())
    {
        RUFUS_IMG_REPORT before;
        memcpy(&before, &report_, sizeof(before));
        active = true;
        const QString directory = files.isNull() ? directory_.path() : files;
        const int error = grub4dos_ ? rufus::ReadGrub4dosLoaderCache(directory, core_)
            : rufus::ReadGrub2CoreCache(directory, report_, true, true, offset, core_);
        active = false;
        if (memcmp(&before, &report_, sizeof(report_)) != 0) mutationError = EPROTO;
        return error;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(directory_.isValid());
        QFile embedded(QString::fromUtf8(RUFUS_GRUB2_CORE));
        QVERIFY(embedded.open(QIODevice::ReadOnly));
        original_ = embedded.readAll();
        QCOMPARE(original_.size(), 42742);
        QCOMPARE(QCryptographicHash::hash(original_, QCryptographicHash::Sha256).toHex(),
            QByteArray("9a2c946704017fa8dc4e03a8a58d754d2d1607c2d2cd74f0e2920133f1192809"));
    }

    void init()
    {
        active = cancelled = false;
        opens = reads = mutationError = 0;
        descriptorValid = true;
        fault = None;
        report_ = {};
        report_.has_grub2 = true;
        strcpy(report_.grub2_version, "2.06-nonstandard-gdie-demo");
        core_ = {};
        grub4dos_ = QByteArray(QTest::currentTestFunction()).startsWith("grub4dos");
        const QString name = grub4dos_ ? QStringLiteral("grub4dos-0.4.6a/grldr")
            : QStringLiteral("grub-2.06-nonstandard-gdie-demo/core.img");
        cachePath = QFile::encodeName(directory_.filePath(name));
        replacementPath = QFile::encodeName(directory_.filePath(QStringLiteral("replacement.img")));
    }

    void embeddedCore()
    {
        const quint64 exactGap = quint64(original_.size()) + 512;
        for (quint64 offset : {quint64(0), quint64(511), quint64(512), exactGap - 1, exactGap, partitionOffset})
        {
            QByteArray core("stale output");
            const bool fits = offset >= exactGap;
            QCOMPARE(rufus::ReadEmbeddedGrub2Core(offset, core), fits ? 0 : EBADMSG);
            QCOMPARE(core, fits ? original_ : QByteArray());
            QCOMPARE(opens, 0); // Resource reads do not open a selected source/cache.
        }
    }

    void embeddedGrub4dos()
    {
        QFile original(QString::fromUtf8(RUFUS_GRUB4DOS_MBR));
        QVERIFY(original.open(QIODevice::ReadOnly));
        const QByteArray expected = original.readAll();
        QCOMPARE(expected.size(), qsizetype(8192));
        const quint64 exactGap = quint64(expected.size());
        for (quint64 offset : {quint64(0), quint64(511), quint64(512), exactGap - 1, exactGap, partitionOffset})
        {
            QByteArray resource("stale output");
            const bool fits = offset >= exactGap;
            QCOMPARE(rufus::ReadEmbeddedGrub4dosSbr(offset, resource), fits ? 0 : EBADMSG);
            QCOMPARE(resource, fits ? expected : QByteArray());
            QCOMPARE(opens, 0);
        }
    }

    void skipLookup_data()
    {
        QTest::addColumn<QString>("reason");
        for (const char* reason : {"unknown", "embedded", "not-grub", "gpt", "uefi", "unterminated"})
            QTest::newRow(reason) << QString::fromLatin1(reason);
    }

    void skipLookup()
    {
        QFETCH(QString, reason);
        if (reason == "unknown") report_.grub2_version[0] = 0;
        if (reason == "embedded") strcpy(report_.grub2_version, GRUB2_PACKAGE_VERSION);
        if (reason == "not-grub") report_.has_grub2 = false;
        if (reason == "unterminated") memset(report_.grub2_version, 'x', sizeof(report_.grub2_version));
        core_.bytes = core_.sha256 = QByteArray("old selection");
        core_.state = rufus::Grub2CacheState::Ready;
        core_.path = "old path";
        active = true;
        QCOMPARE(rufus::ReadGrub2CoreCache(directory_.path(), report_, reason != "gpt", reason != "uefi",
            partitionOffset, core_), 0);
        active = false;
        QCOMPARE(core_.state, rufus::Grub2CacheState::NotNeeded);
        QVERIFY(core_.bytes.isEmpty() && core_.sha256.isEmpty() && core_.path.isEmpty());
        QCOMPARE(opens, 0);
    }

    void missingFullName()
    {
        QVERIFY(writeFile(directory_.filePath("grub-2.06/core.img"), original_));
        QCOMPARE(readCache(), ENOENT);
        QCOMPARE(core_.state, rufus::Grub2CacheState::Missing);
        QCOMPARE(core_.path, QString::fromLocal8Bit(cachePath));
        QVERIFY(!QDir(directory_.filePath("grub-2.06-nonstandard-gdie-demo")).exists());
        QVERIFY(core_.bytes.isEmpty() && core_.sha256.isEmpty());
        QCOMPARE(opens, 1);
    }

    void ownsCompleteBytes()
    {
        QVERIFY(writeFile(QString::fromLocal8Bit(cachePath), original_));
        report_.has_syslinux = true; // Original lookup runs before SBR precedence.
        QCOMPARE(readCache(), 0);
        QCOMPARE(core_.state, rufus::Grub2CacheState::Ready);
        QCOMPARE(core_.bytes, original_);
        QCOMPARE(core_.sha256, QCryptographicHash::hash(original_, QCryptographicHash::Sha256));
        QCOMPARE(core_.path, QString::fromLocal8Bit(cachePath));
        QVERIFY(descriptorValid);
        QVERIFY(writeFile(QString::fromLocal8Bit(cachePath), QByteArray("replacement")));
        QVERIFY(QFile::remove(QString::fromLocal8Bit(cachePath)));
        QCOMPARE(core_.bytes, original_);
        QCOMPARE(core_.sha256, QCryptographicHash::hash(original_, QCryptographicHash::Sha256));
    }

    void sizeLimits_data()
    {
        QTest::addColumn<qint64>("size");
        QTest::addColumn<quint64>("offset");
        QTest::addColumn<int>("error");
        QTest::newRow("empty") << qint64(0) << partitionOffset << EBADMSG;
        QTest::newRow("short") << qint64(511) << partitionOffset << EBADMSG;
        QTest::newRow("one-sector") << qint64(512) << quint64(1024) << 0;
        QTest::newRow("full-gap") << qint64(partitionOffset - 512) << partitionOffset << 0;
        QTest::newRow("over-gap") << qint64(partitionOffset - 511) << partitionOffset << ENOSPC;
        QTest::newRow("old-small-gap") << qint64(42742) << quint64(63 * 512) << ENOSPC;
        QTest::newRow("huge-sparse") << qint64(8) * 1024 * 1024 * 1024 << partitionOffset << ENOSPC;
        QTest::newRow("no-gap") << qint64(512) << quint64(512) << EINVAL;
        QTest::newRow("zero-offset") << qint64(512) << quint64(0) << EINVAL;
        QTest::newRow("misaligned") << qint64(512) << quint64(1025) << EINVAL;
        QTest::newRow("too-large-offset") << qint64(512) << quint64(partitionOffset + 512) << EINVAL;
    }

    void sizeLimits()
    {
        QFETCH(qint64, size);
        QFETCH(quint64, offset);
        QFETCH(int, error);
        QVERIFY(writeFile(QString::fromLocal8Bit(cachePath), {}));
        QFile file(QString::fromLocal8Bit(cachePath));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.resize(size));
        file.close();
        QCOMPARE(readCache(offset), error);
        if (error == 0)
        {
            QCOMPARE(core_.state, rufus::Grub2CacheState::Ready);
            QCOMPARE(core_.bytes, QByteArray(qsizetype(size), '\0'));
        }
        else
        {
            QCOMPARE(core_.state, rufus::Grub2CacheState::Unusable);
            QVERIFY(core_.bytes.isEmpty() && core_.sha256.isEmpty());
            QCOMPARE(reads, 0);
        }
    }

    void invalidNames_data()
    {
        QTest::addColumn<QString>("version");
        for (const char* name : {"../outside", "2.06/other", "2.06\\other", "2.06-UPPER", "2.06-é"})
            QTest::newRow(name) << QString::fromUtf8(name);
    }

    void invalidNames()
    {
        QFETCH(QString, version);
        const QByteArray bytes = version.toUtf8();
        memcpy(report_.grub2_version, bytes.constData(), size_t(bytes.size() + 1));
        QCOMPARE(readCache(), EINVAL);
        QCOMPARE(core_.state, rufus::Grub2CacheState::Unusable);
        QCOMPARE(opens, 0);
    }

    void invalidDirectories()
    {
        QCOMPARE(readCache(partitionOffset, "relative"), EINVAL);
        QCOMPARE(readCache(partitionOffset, QStringLiteral("")), EINVAL);
        QCOMPARE(readCache(partitionOffset, directory_.path() + QChar(u'\0') + "suffix"), EINVAL);
        QCOMPARE(opens, 0);
    }

    void nonregular_data()
    {
        QTest::addColumn<bool>("fifo");
        QTest::newRow("directory") << false;
        QTest::newRow("fifo") << true;
    }

    void nonregular()
    {
        QFETCH(bool, fifo);
        QVERIFY(QDir().mkpath(QFileInfo(QString::fromLocal8Bit(cachePath)).absolutePath()));
        if (fifo) QCOMPARE(mkfifo(cachePath.constData(), 0600), 0);
        else QVERIFY(QDir().mkpath(QString::fromLocal8Bit(cachePath)));
        QCOMPARE(readCache(), EINVAL);
        QCOMPARE(core_.state, rufus::Grub2CacheState::Unusable);
        QCOMPARE(reads, 0);
    }

    void followsStableSymlink()
    {
        QVERIFY(writeFile(QString::fromLocal8Bit(replacementPath), original_));
        QVERIFY(QDir().mkpath(QFileInfo(QString::fromLocal8Bit(cachePath)).absolutePath()));
        QCOMPARE(symlink(replacementPath.constData(), cachePath.constData()), 0);
        QCOMPARE(readCache(), 0);
        QCOMPARE(core_.bytes, original_);
        QCOMPARE(core_.state, rufus::Grub2CacheState::Ready);
    }

    void faults_data()
    {
        QTest::addColumn<int>("mode");
        QTest::addColumn<int>("error");
        QTest::addColumn<bool>("multiChunk");
        QTest::newRow("denied-open") << int(DeniedOpen) << EACCES << false;
        QTest::newRow("eintr-retry") << int(InterruptedRead) << 0 << false;
        QTest::newRow("partial-reads") << int(PartialRead) << 0 << false;
        QTest::newRow("failed-first-read") << int(FailedRead) << EIO << false;
        QTest::newRow("failed-later-read") << int(FailedLaterRead) << EIO << false;
        QTest::newRow("early-eof") << int(EarlyEnd) << EIO << false;
        QTest::newRow("cancel-on-last-read") << int(CancelRead) << ECANCELED << false;
        QTest::newRow("cancel-with-read-error") << int(CancelWithError) << ECANCELED << false;
        QTest::newRow("changed-bytes-restored-mtime") << int(ChangeBytes) << ESTALE << false;
        QTest::newRow("truncate-after-read") << int(TruncateFile) << ESTALE << false;
        QTest::newRow("replaced-file") << int(ReplaceFile) << ESTALE << false;
        QTest::newRow("removed-file") << int(RemoveFile) << ESTALE << false;
        QTest::newRow("renamed-directory") << int(RenameDirectory) << ESTALE << false;
        QTest::newRow("retargeted-link") << int(RetargetLink) << ESTALE << false;
        QTest::newRow("complete-multiple-chunks") << int(None) << 0 << true;
        QTest::newRow("truncate-between-chunks") << int(TruncateFile) << ESTALE << true;
        QTest::newRow("cancel-between-chunks") << int(CancelRead) << ECANCELED << true;
    }

    void faults()
    {
        QFETCH(int, mode);
        QFETCH(int, error);
        QFETCH(bool, multiChunk);
        const QByteArray payload = multiChunk ? original_.repeated(4) : original_;
        QVERIFY(writeFile(QString::fromLocal8Bit(cachePath), payload));
        QVERIFY(writeFile(QString::fromLocal8Bit(replacementPath), QByteArray(original_.size(), 'r')));
        if (mode == RetargetLink)
        {
            const QString target = directory_.filePath("first-target.img");
            QVERIFY(QFile::rename(QString::fromLocal8Bit(cachePath), target));
            QCOMPARE(symlink(QFile::encodeName(target).constData(), cachePath.constData()), 0);
        }
        fault = Fault(mode);
        const int descriptors = QDir("/proc/self/fd").entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size();
        QCOMPARE(readCache(), error);
        QCOMPARE(mutationError, 0);
        QCOMPARE(QDir("/proc/self/fd").entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size(), descriptors);
        QVERIFY(descriptorValid);
        if (error == 0)
        {
            QCOMPARE(core_.state, rufus::Grub2CacheState::Ready);
            QCOMPARE(core_.bytes, payload);
            QCOMPARE(core_.sha256, QCryptographicHash::hash(payload, QCryptographicHash::Sha256));
            QVERIFY(reads > 1);
        }
        else
        {
            QCOMPARE(core_.state, mode == DeniedOpen ? rufus::Grub2CacheState::Missing : rufus::Grub2CacheState::Unusable);
            QVERIFY(core_.bytes.isEmpty() && core_.sha256.isEmpty());
        }
    }

    void grub4dosFaults_data() { faults_data(); }
    void grub4dosFaults() { faults(); }
    void grub4dosNonregular_data() { nonregular_data(); }
    void grub4dosNonregular() { nonregular(); }
    void grub4dosDirectories() { invalidDirectories(); }
    void grub4dosOwnedBytes() { ownsCompleteBytes(); }
    void grub4dosStableSymlink() { followsStableSymlink(); }
    void grub4dosCancelledBeforeOpen() { cancelledBeforeOpen(); }

    void grub4dosSizes_data()
    {
        QTest::addColumn<qint64>("size"); QTest::addColumn<int>("error");
        QTest::newRow("empty") << qint64(0) << EBADMSG;
        QTest::newRow("short") << qint64(511) << EBADMSG;
        QTest::newRow("one-sector") << qint64(512) << 0;
        QTest::newRow("maximum") << qint64(1024 * 1024 - 512) << 0;
        QTest::newRow("oversized") << qint64(1024 * 1024 - 511) << ENOSPC;
        QTest::newRow("huge-sparse") << qint64(8) * 1024 * 1024 * 1024 << ENOSPC;
    }
    void grub4dosSizes()
    {
        QFETCH(qint64, size); QFETCH(int, error);
        QVERIFY(writeFile(QString::fromLocal8Bit(cachePath), {}));
        QFile file(QString::fromLocal8Bit(cachePath));
        QVERIFY(file.open(QIODevice::WriteOnly) && file.resize(size));
        file.close();
        QCOMPARE(readCache(), error);
        QCOMPARE(core_.bytes, error == 0 ? QByteArray(qsizetype(size), '\0') : QByteArray());
        if (error != 0) QCOMPARE(reads, 0);
    }
    void grub4dosFixedVersionMissing()
    {
        QVERIFY(writeFile(directory_.filePath("grub4dos-0.4.5/grldr"), original_));
        QVERIFY(writeFile(directory_.filePath("grub-0.4.6a/core.img"), original_));
        QCOMPARE(readCache(), ENOENT);
        QCOMPARE(core_.state, rufus::Grub2CacheState::Missing);
        QCOMPARE(core_.path, directory_.filePath("grub4dos-0.4.6a/grldr"));
        QVERIFY(core_.bytes.isEmpty() && core_.sha256.isEmpty());
        QCOMPARE(opens, 1);
    }

    void cancelledBeforeOpen()
    {
        cancelled = true;
        QCOMPARE(readCache(), ECANCELED);
        QCOMPARE(opens, 0);
        QVERIFY(core_.bytes.isEmpty() && core_.sha256.isEmpty());
    }

    void cleanup()
    {
        active = cancelled = false;
        for (const QString& entry : QDir(directory_.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot))
        {
            const QString path = directory_.filePath(entry);
            const QFileInfo info(path);
            if (info.isDir() && !info.isSymLink()) QVERIFY(QDir(path).removeRecursively());
            else QVERIFY(QFile::remove(path));
        }
        QCOMPARE(mutationError, 0);
    }
};
QTEST_GUILESS_MAIN(TestGrub2Cache)
#include "test_grub2_cache.moc"
