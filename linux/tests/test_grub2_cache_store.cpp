// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/grub2_cache.h"
#include "platform/download_signature.h"
#include "../../res/grub2/grub2_version.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QTest>
#include <cerrno>
#include <cstring>

namespace
{
constexpr quint64 partitionOffset = 1024 * 1024;
constexpr qsizetype chunkBytes = 64 * 1024;
enum Fault { None, OpenRejected, WriteRejected, ShortWrite, LaterWriteRejected,
    CommitRejected, CancelOpen, CancelFirstWrite, CancelLastWrite };
Fault fault = None;
bool active = false;
bool cancelled = false;
bool oldFileIntact = true;
bool directWrites = false;
int opens = 0;
int writes = 0;
int commits = 0;
qsizetype processed = 0;
qsizetype total = 0;
QSaveFile* saving = nullptr;
QString destination;
QByteArray previous;

QByteArray readFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}
}

extern "C" int IsIsoScanCancelled(void) { return cancelled; }
extern "C" void uprintf(const char*, ...) {}
// Wrap only calls originating in our adapter. Qt still creates/writes/cleans
// the real temporary file, and real commit tests its cancellation/error path.
extern "C" bool __real__ZN9QSaveFile4openE6QFlagsIN13QIODeviceBase12OpenModeFlagEE(QSaveFile*, QIODevice::OpenMode);
extern "C" qint64 __real__ZN9QIODevice5writeEPKcx(QIODevice*, const char*, qint64);
extern "C" bool __real__ZN9QSaveFile6commitEv(QSaveFile*);

extern "C" bool __wrap__ZN9QSaveFile4openE6QFlagsIN13QIODeviceBase12OpenModeFlagEE(QSaveFile* file, QIODevice::OpenMode mode)
{
    if (!active) return __real__ZN9QSaveFile4openE6QFlagsIN13QIODeviceBase12OpenModeFlagEE(file, mode);
    ++opens;
    saving = file;
    directWrites = file->directWriteFallback();
    if (fault == OpenRejected) return false;
    const bool opened = __real__ZN9QSaveFile4openE6QFlagsIN13QIODeviceBase12OpenModeFlagEE(file, mode);
    if (fault == CancelOpen) cancelled = true;
    return opened;
}

extern "C" qint64 __wrap__ZN9QIODevice5writeEPKcx(QIODevice* device, const char* data, qint64 size)
{
    if (!active || device != saving) return __real__ZN9QIODevice5writeEPKcx(device, data, size);
    ++writes;
    oldFileIntact = oldFileIntact && readFile(destination) == previous && size <= chunkBytes;
    const bool reject = fault == WriteRejected || (fault == LaterWriteRejected && writes > 1);
    if (reject) { saving->cancelWriting(); return -1; }
    const qint64 requested = fault == ShortWrite ? size - 1 : size;
    const qint64 count = __real__ZN9QIODevice5writeEPKcx(device, data, requested);
    if (count > 0) processed += count;
    const bool cancelNow = fault == CancelFirstWrite || (fault == CancelLastWrite && processed == total);
    if (cancelNow) cancelled = true;
    return count;
}

extern "C" bool __wrap__ZN9QSaveFile6commitEv(QSaveFile* file)
{
    if (!active) return __real__ZN9QSaveFile6commitEv(file);
    ++commits;
    oldFileIntact = oldFileIntact && readFile(destination) == previous;
    if (fault == CommitRejected) file->cancelWriting();
    return __real__ZN9QSaveFile6commitEv(file);
}

class TestGrub2CacheStore : public QObject
{
    Q_OBJECT
    RUFUS_IMG_REPORT report_ = {};
    QByteArray authentic_;
    bool grub4dos_ = false;
    QString relativeName_;

    bool writeFile(const QString& path, const QByteArray& bytes)
    {
        if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
        QFile file(path);
        return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
    }
    int store(const QString& files, const QByteArray& bytes, quint64 offset = partitionOffset,
        bool mbr = true, bool bios = true)
    {
        RUFUS_IMG_REPORT before;
        memcpy(&before, &report_, sizeof(before));
        const QByteArray original = bytes;
        total = bytes.size();
        active = true;
        const int error = grub4dos_ ? rufus::StoreGrub4dosLoaderCache(files, bytes)
            : rufus::StoreGrub2CoreCache(files, report_, mbr, bios, offset, bytes);
        active = false;
        if (memcmp(&before, &report_, sizeof(before)) != 0 || bytes != original) return EPROTO;
        return error;
    }
    void checkDirectory(const QString& parent, bool exists)
    {
        const QStringList contents = QDir(parent).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot);
        QCOMPARE(contents, exists ? QStringList({grub4dos_ ? "grldr" : "core.img"}) : QStringList());
    }

private slots:
    void initTestCase()
    {
        const QString fixtures = QStringLiteral(RUFUS_SIGNED_FIXTURE);
        authentic_ = readFile(fixtures + "/core.img");
        const QByteArray signature = readFile(fixtures + "/core.img.sig");
        QVERIFY(!authentic_.isEmpty());
        QCOMPARE(VerifyRufusDownloadSignature(authentic_.constData(), size_t(authentic_.size()),
            signature.constData(), size_t(signature.size())), 0);
    }
    void init()
    {
        grub4dos_ = QByteArray(QTest::currentTestFunction()).startsWith("grub4dos");
        relativeName_ = grub4dos_ ? QStringLiteral("grub4dos-0.4.6a/grldr")
            : QStringLiteral("grub-2.12-nonstandard-demo/core.img");
        report_ = {};
        report_.has_grub2 = true;
        // The authenticated candidate is 2.12. The cache uses the full name.
        strcpy(report_.grub2_version, "2.12-nonstandard-demo");
        fault = None;
        active = cancelled = directWrites = false;
        oldFileIntact = true;
        opens = writes = commits = 0;
        processed = total = 0;
        saving = nullptr;
        destination.clear();
        previous.clear();
    }
    void cleanup() { active = cancelled = false; saving = nullptr; }

    void authenticRoundTrip()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString files = directory.filePath("files/nested");
        destination = files + "/" + relativeName_;
        QCOMPARE(store(files, authentic_), 0);
        QVERIFY(oldFileIntact && !directWrites);
        QCOMPARE(opens, 1);
        QCOMPARE(commits, 1);
        QCOMPARE(readFile(destination), authentic_);
        QVERIFY(!QDir(files + "/grub-2.12").exists());
        rufus::Grub2CachedCore core;
        QCOMPARE(grub4dos_ ? rufus::ReadGrub4dosLoaderCache(files, core)
            : rufus::ReadGrub2CoreCache(files, report_, true, true, partitionOffset, core), 0);
        QCOMPARE(core.state, rufus::Grub2CacheState::Ready);
        QCOMPARE(core.path, destination);
        QCOMPARE(core.bytes, authentic_);
        QCOMPARE(core.sha256, QCryptographicHash::hash(authentic_, QCryptographicHash::Sha256));
        checkDirectory(QFileInfo(destination).absolutePath(), true);
        QVERIFY(QFile::remove(destination));
        QCOMPARE(core.bytes, authentic_); // Later cache changes cannot alter the accepted snapshot.
    }

    void replacementAndFailures_data()
    {
        QTest::addColumn<int>("mode");
        QTest::addColumn<bool>("existing");
        const char* names[] = {"success", "open-error", "write-error", "short-write", "later-write-error",
            "commit-error", "cancel-open", "cancel-first-write", "cancel-last-write"};
        for (bool existing : {false, true})
            for (int mode = None; mode <= CancelLastWrite; ++mode)
            {
                const QByteArray row = QByteArray(existing ? "replace-" : "new-") + names[mode];
                QTest::newRow(row.constData()) << mode << existing;
            }
    }
    void replacementAndFailures()
    {
        QFETCH(int, mode);
        QFETCH(bool, existing);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        destination = directory.filePath(relativeName_);
        previous = existing ? QByteArray(512, 'o') : QByteArray();
        if (existing) QVERIFY(writeFile(destination, previous));
        // Synthetic bytes qualify storage, not authenticity or bootability.
        const QByteArray bytes(chunkBytes * 2 + 137, 'v');
        fault = Fault(mode);
        const bool cancel = fault == CancelOpen || fault == CancelFirstWrite || fault == CancelLastWrite;
        int expected = fault == None ? 0 : EIO;
        if (cancel) expected = ECANCELED;
        QCOMPARE(store(directory.path(), bytes), expected);
        QVERIFY(oldFileIntact && !directWrites);
        QCOMPARE(QFile::exists(destination), existing || expected == 0);
        QCOMPARE(readFile(destination), expected == 0 ? bytes : previous);
        QCOMPARE(commits, fault == None || fault == CommitRejected ? 1 : 0);
        checkDirectory(QFileInfo(destination).absolutePath(), existing || expected == 0);
        if (cancel)
        {
            fault = None;
            cancelled = false;
            QCOMPARE(store(directory.path(), bytes), 0); // Cancellation does not poison later saves.
            QCOMPARE(readFile(destination), bytes);
        }
    }

    void acceptedSizes_data()
    {
        QTest::addColumn<qsizetype>("size");
        QTest::addColumn<quint64>("offset");
        QTest::newRow("one-sector-gap") << qsizetype(512) << quint64(1024);
        QTest::newRow("full-maximum-gap") << qsizetype(partitionOffset - 512) << partitionOffset;
    }
    void acceptedSizes()
    {
        QFETCH(qsizetype, size);
        QFETCH(quint64, offset);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        destination = directory.filePath(relativeName_);
        const QByteArray bytes(size, 'v'); // Storage geometry test, not a signed candidate.
        QCOMPARE(store(directory.path(), bytes, offset), 0);
        QVERIFY(oldFileIntact && !directWrites);
        QCOMPARE(readFile(destination), bytes);
        rufus::Grub2CachedCore core;
        QCOMPARE(grub4dos_ ? rufus::ReadGrub4dosLoaderCache(directory.path(), core)
            : rufus::ReadGrub2CoreCache(directory.path(), report_, true, true, offset, core), 0);
        QCOMPARE(core.bytes, bytes);
        checkDirectory(QFileInfo(destination).absolutePath(), true);
    }

    void invalidInputs_data()
    {
        QTest::addColumn<QString>("reason");
        for (const char* reason : {"unknown", "embedded", "not-grub", "gpt", "uefi", "unterminated",
            "slash", "uppercase", "nul-directory", "relative-directory", "zero-offset", "no-gap",
            "misaligned", "huge-offset", "empty", "short", "oversized", "cancelled"})
            QTest::newRow(reason) << QString::fromLatin1(reason);
    }
    void invalidInputs()
    {
        QFETCH(QString, reason);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QString files = directory.filePath("must-not-exist");
        QByteArray bytes = authentic_;
        quint64 offset = partitionOffset;
        int expected = EINVAL;
        if (reason == "unknown") report_.grub2_version[0] = 0;
        if (reason == "embedded") strcpy(report_.grub2_version, GRUB2_PACKAGE_VERSION);
        if (reason == "not-grub") report_.has_grub2 = false;
        if (reason == "unterminated") memset(report_.grub2_version, 'x', sizeof(report_.grub2_version));
        if (reason == "slash") strcpy(report_.grub2_version, "2.12/../../escape");
        if (reason == "uppercase") strcpy(report_.grub2_version, "2.12-Upper");
        if (reason == "nul-directory") files += QChar(u'\0');
        if (reason == "relative-directory") files = "relative";
        if (reason == "zero-offset") offset = 0;
        if (reason == "no-gap") offset = 512;
        if (reason == "misaligned") offset = 1025;
        if (reason == "huge-offset") offset += 512;
        if (reason == "empty") { bytes.clear(); expected = EBADMSG; }
        if (reason == "short") { bytes = QByteArray(511, 'x'); expected = EBADMSG; }
        if (reason == "oversized") { offset = 63 * 512; expected = ENOSPC; }
        if (reason == "cancelled") { cancelled = true; expected = ECANCELED; }
        QCOMPARE(store(files, bytes, offset, reason != "gpt", reason != "uefi"), expected);
        QCOMPARE(opens, 0);
        QCOMPARE(writes, 0);
        QCOMPARE(commits, 0);
        QVERIFY(QDir(directory.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty());
    }

    void grub4dosReplacementAndFailures_data() { replacementAndFailures_data(); }
    void grub4dosReplacementAndFailures() { replacementAndFailures(); }
    void grub4dosSizes_data() { acceptedSizes_data(); }
    void grub4dosSizes() { acceptedSizes(); }
    void grub4dosFilesystemFailures_data() { filesystemFailures_data(); }
    void grub4dosFilesystemFailures() { filesystemFailures(); }

    void grub4dosInvalidInputs_data()
    {
        QTest::addColumn<QString>("reason");
        for (const char* reason : {"relative", "nul", "empty", "short", "oversized", "cancelled"})
            QTest::newRow(reason) << QString::fromLatin1(reason);
    }
    void grub4dosInvalidInputs()
    {
        QFETCH(QString, reason);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QString files = directory.filePath("must-not-exist");
        QByteArray bytes(512, 'v');
        int expected = EINVAL;
        if (reason == "relative") files = "relative";
        if (reason == "nul") files += QChar(u'\0');
        if (reason == "empty") { bytes.clear(); expected = EBADMSG; }
        if (reason == "short") { bytes.resize(511); expected = EBADMSG; }
        if (reason == "oversized") { bytes.resize(1024 * 1024 - 511); expected = ENOSPC; }
        if (reason == "cancelled") { cancelled = true; expected = ECANCELED; }
        QCOMPARE(store(files, bytes), expected);
        QCOMPARE(opens, 0); QCOMPARE(writes, 0); QCOMPARE(commits, 0);
        QVERIFY(QDir(directory.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty());
    }

    void filesystemFailures_data()
    {
        QTest::addColumn<QString>("reason");
        for (const char* reason : {"parent-file", "version-file", "target-directory", "readonly-directory"})
            QTest::newRow(reason) << QString::fromLatin1(reason);
    }
    void filesystemFailures()
    {
        QFETCH(QString, reason);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QString files = directory.filePath("files");
        destination = files + "/" + relativeName_;
        const QString parent = QFileInfo(destination).absolutePath();
        if (reason == "parent-file") QVERIFY(writeFile(files, "parent"));
        if (reason == "version-file") QVERIFY(writeFile(parent, "version"));
        if (reason == "target-directory") QVERIFY(QDir().mkpath(destination));
        if (reason == "readonly-directory")
        {
            previous = QByteArray(512, 'o');
            QVERIFY(writeFile(destination, previous));
            QVERIFY(QFile::setPermissions(parent, QFileDevice::ReadOwner | QFileDevice::ExeOwner));
        }
        const int error = store(files, authentic_);
        if (reason == "readonly-directory")
        {
            QVERIFY(QFile::setPermissions(parent, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
            QCOMPARE(readFile(destination), previous);
            checkDirectory(parent, true);
        }
        QVERIFY(error == EIO || error == EACCES);
        QCOMPARE(commits, 0);
        if (reason == "parent-file") QCOMPARE(readFile(files), QByteArray("parent"));
        if (reason == "version-file") QCOMPARE(readFile(parent), QByteArray("version"));
        if (reason == "target-directory") QVERIFY(QDir(destination).exists());
    }
};
QTEST_GUILESS_MAIN(TestGrub2CacheStore)
#include "test_grub2_cache_store.moc"
