// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/prepare_source.h"
#include <QCryptographicHash>
#include <QFile>
#include <QJsonDocument>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QTest>
#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace
{
enum Fault { None, InterruptedRead, PartialRead, FailedRead, FailedLaterRead, EarlyEnd,
    CancelRead, CancelFinalRead, ChangeFile, ReplaceFile, RemoveFile };
constexpr quint64 gapOffset = 1024 * 1024;
constexpr qsizetype fixtureBytes = 128 * 1024;
Fault fault = None;
bool active = false;
bool cancelled = false;
int reads = 0;
QByteArray sourcePath;
QByteArray replacementPath;
}

extern "C" int IsIsoScanCancelled(void) { return cancelled; }
extern "C" void uprintf(const char*, ...) {}
extern "C" ssize_t __real_pread64(int, void*, size_t, off64_t);
extern "C" ssize_t __wrap_pread64(int fd, void* data, size_t size, off64_t offset)
{
    if (!active)
        return __real_pread64(fd, data, size, offset);
    ++reads;
    if (fault == InterruptedRead && reads == 1) { errno = EINTR; return -1; }
    if (fault == FailedRead || (fault == FailedLaterRead && reads == 2)) { errno = EIO; return -1; }
    if (fault == EarlyEnd) return 0;
    if (fault == PartialRead) size = std::min(size, size_t(37));
    const ssize_t count = __real_pread64(fd, data, size, offset);
    if (reads == 1)
    {
        if (fault == CancelRead || fault == CancelFinalRead) cancelled = true;
        if (fault == ChangeFile)
        {
            const int writer = ::open(sourcePath.constData(), O_WRONLY | O_CLOEXEC);
            struct stat info;
            if (writer < 0 || fstat(writer, &info) != 0 || pwrite(writer, "X", 1, 0) != 1)
                qFatal("Cannot mutate core fixture");
            // Make the metadata change deterministic even when two writes
            // fall within the filesystem's timestamp resolution.
            timespec times[2] = {info.st_atim, info.st_mtim};
            ++times[1].tv_sec;
            if (futimens(writer, times) != 0 || close(writer) != 0)
                qFatal("Cannot timestamp core mutation");
        }
        if (fault == ReplaceFile && ::rename(replacementPath.constData(), sourcePath.constData()) != 0)
            qFatal("Cannot replace core fixture");
        if (fault == RemoveFile && unlink(sourcePath.constData()) != 0)
            qFatal("Cannot remove core fixture");
    }
    return count;
}

class TestPrepareGrub2Core : public QObject
{
    Q_OBJECT
    QTemporaryDir directory_;
    rufus::PrepareImageRequest request_;
    QByteArray expected_;
    bool loader_ = false;

    int load(uid_t caller, quint64 offset, QByteArray& bytes)
    {
        return loader_ ? rufus::LoadPrepareGrub4dosLoader(request_, caller, bytes)
            : rufus::LoadPrepareGrub2Core(request_, caller, offset, bytes);
    }

    void store(const QString& path, const QByteArray& data)
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(file.write(data), data.size());
        file.close();
    }

private slots:
    void init()
    {
        QVERIFY(directory_.isValid());
        fault = None; active = cancelled = false; reads = 0;
        request_ = {};
        loader_ = QByteArray(QTest::currentTestFunction()).startsWith("loader");
        request_.grub2 = !loader_;
        request_.grub4dos = loader_;
        request_.mbr = request_.isDevice = true;
        request_.grub2CorePath = directory_.filePath(QStringLiteral("core.img"));
        QFile::remove(request_.grub2CorePath);
        expected_ = QByteArray(fixtureBytes, 'g');
        store(request_.grub2CorePath, expected_);
        request_.grub2CoreSize = quint64(expected_.size());
        request_.grub2CoreSha256 = QCryptographicHash::hash(expected_, QCryptographicHash::Sha256);
        sourcePath = QFile::encodeName(request_.grub2CorePath);
        replacementPath = QFile::encodeName(directory_.filePath(QStringLiteral("replacement")));
        store(QString::fromLocal8Bit(replacementPath), expected_);
        QFile::remove(directory_.filePath(QStringLiteral("link")));
    }

    void retainsOwnedSnapshot()
    {
        QByteArray core;
        QCOMPARE(load(getuid(), gapOffset, core), 0);
        QCOMPARE(core, expected_);
        store(request_.grub2CorePath, QByteArray(fixtureBytes, 'x'));
        QVERIFY(QFile::remove(request_.grub2CorePath));
        QCOMPARE(core, expected_);
    }

    void loaderSnapshot()
    {
        retainsOwnedSnapshot();
        QByteArray bytes("old result");
        QCOMPARE(rufus::LoadPrepareGrub2Core(request_, getuid(), gapOffset, bytes), EINVAL);
        QVERIFY(bytes.isEmpty());
    }

    void loaderFat16Snapshot()
    {
        request_.fileSystem = FS_FAT16;
        loaderSnapshot();
    }

    void loaderNtfsSnapshot()
    {
        request_.fileSystem = FS_NTFS;
        request_.volumeId = 0;
        loaderSnapshot();
    }

    void loaderRejectFilesystem()
    {
        request_.fileSystem = FS_EXFAT;
        QByteArray bytes("old result");
        QCOMPARE(load(getuid(), gapOffset, bytes), EINVAL);
        QVERIFY(bytes.isEmpty());
        QCOMPARE(reads, 0);
    }

    void loaderReject_data()
    {
        QTest::addColumn<QString>("mode"); QTest::addColumn<int>("error");
        const char* names[] = {"owner", "unreadable", "symlink", "hardlink", "directory", "fifo",
            "missing", "size", "empty", "partial", "operation", "cancelled"};
        const int errors[] = {EACCES, EACCES, ELOOP, EINVAL, EINVAL, EINVAL,
            ENOENT, ESTALE, EINVAL, EINVAL, EINVAL, ECANCELED};
        for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
            QTest::newRow(names[i]) << QString::fromLatin1(names[i]) << errors[i];
    }
    void loaderReject() { rejectsBeforeRead(); }
    void loaderChangedBytes() { rejectsChangedBytes(); }
    void loaderBoundary_data() { boundary_data(); }
    void loaderBoundary() { boundary(); }
    void loaderReadFault_data() { readFault_data(); }
    void loaderReadFault() { readFault(); }
    void loaderPreflight_data()
    {
        QTest::addColumn<QString>("mode"); QTest::addColumn<int>("error");
        QTest::newRow("valid") << QStringLiteral("valid") << ENOTSUP;
        QTest::newRow("hash") << QStringLiteral("hash") << ESTALE;
        QTest::newRow("size") << QStringLiteral("size") << ESTALE;
        QTest::newRow("missing") << QStringLiteral("missing") << ENOENT;
        QTest::newRow("owner") << QStringLiteral("owner") << EACCES;
        QTest::newRow("symlink") << QStringLiteral("symlink") << ELOOP;
    }
    void loaderPreflight() { helperPreflight(); }

    void windowsSbrSnapshot()
    {
        request_.grub2 = false;
        request_.windowsBios = request_.enhancedInstall = true;
        request_.fileSystem = FS_NTFS;
        QByteArray core;
        QCOMPARE(load(getuid(), gapOffset, core), 0);
        QCOMPARE(core, expected_);
        QVERIFY(QFile::remove(request_.grub2CorePath));
        QCOMPARE(core, expected_);
    }

    void rejectsBeforeRead_data()
    {
        QTest::addColumn<QString>("mode");
        QTest::addColumn<int>("error");
        QTest::newRow("different-owner") << QStringLiteral("owner") << EACCES;
        QTest::newRow("unreadable") << QStringLiteral("unreadable") << EACCES;
        QTest::newRow("symlink") << QStringLiteral("symlink") << ELOOP;
        QTest::newRow("hard-link") << QStringLiteral("hardlink") << EINVAL;
        QTest::newRow("directory") << QStringLiteral("directory") << EINVAL;
        QTest::newRow("fifo") << QStringLiteral("fifo") << EINVAL;
        QTest::newRow("missing") << QStringLiteral("missing") << ENOENT;
        QTest::newRow("changed-size") << QStringLiteral("size") << ESTALE;
        QTest::newRow("no-metadata") << QStringLiteral("empty") << EINVAL;
        QTest::newRow("partial-metadata") << QStringLiteral("partial") << EINVAL;
        QTest::newRow("wrong-operation") << QStringLiteral("operation") << EINVAL;
        QTest::newRow("no-gap") << QStringLiteral("gap") << EINVAL;
        QTest::newRow("unaligned-gap") << QStringLiteral("unaligned") << EINVAL;
        QTest::newRow("excessive-gap") << QStringLiteral("large-gap") << EINVAL;
        QTest::newRow("insufficient-gap") << QStringLiteral("small-gap") << ENOSPC;
        QTest::newRow("cancelled") << QStringLiteral("cancelled") << ECANCELED;
    }

    void rejectsBeforeRead()
    {
        QFETCH(QString, mode); QFETCH(int, error);
        uid_t caller = getuid();
        quint64 offset = gapOffset;
        if (mode == QStringLiteral("owner")) ++caller;
        if (mode == QStringLiteral("unreadable")) QVERIFY(QFile::setPermissions(request_.grub2CorePath, QFile::WriteOwner));
        if (mode == QStringLiteral("symlink"))
        {
            const QString link = directory_.filePath(QStringLiteral("link"));
            QVERIFY(QFile::link(request_.grub2CorePath, link));
            request_.grub2CorePath = link;
        }
        if (mode == QStringLiteral("hardlink"))
            QCOMPARE(::link(sourcePath.constData(), QFile::encodeName(directory_.filePath(QStringLiteral("link"))).constData()), 0);
        if (mode == QStringLiteral("directory")) request_.grub2CorePath = directory_.path();
        if (mode == QStringLiteral("fifo"))
        {
            QVERIFY(QFile::remove(request_.grub2CorePath));
            QCOMPARE(mkfifo(sourcePath.constData(), S_IRUSR | S_IWUSR), 0);
        }
        if (mode == QStringLiteral("missing")) QVERIFY(QFile::remove(request_.grub2CorePath));
        if (mode == QStringLiteral("size")) ++request_.grub2CoreSize;
        if (mode == QStringLiteral("empty")) request_ = {};
        if (mode == QStringLiteral("partial")) request_.grub2CoreSha256.clear();
        if (mode == QStringLiteral("operation")) request_.grub2 = request_.grub4dos = false;
        if (mode == QStringLiteral("gap")) offset = 512;
        if (mode == QStringLiteral("unaligned")) ++offset;
        if (mode == QStringLiteral("large-gap")) offset += 512;
        if (mode == QStringLiteral("small-gap")) offset = 1024;
        if (mode == QStringLiteral("cancelled")) cancelled = true;
        QByteArray core("old result");
        active = true;
        QCOMPARE(load(caller, offset, core), error);
        active = false;
        QVERIFY(core.isEmpty());
        QCOMPARE(reads, 0);
    }

    void rejectsChangedBytes()
    {
        // Same length and stable during open/read: the frozen hash must detect it.
        store(request_.grub2CorePath, QByteArray(fixtureBytes, 'x'));
        QByteArray core("old result");
        QCOMPARE(load(getuid(), gapOffset, core), ESTALE);
        QVERIFY(core.isEmpty());
    }

    void boundary_data()
    {
        QTest::addColumn<int>("bytes"); QTest::addColumn<int>("error");
        QTest::newRow("sector") << 512 << 0;
        QTest::newRow("non-sector-multiple") << 513 << 0;
        QTest::newRow("whole-gap") << 1048064 << 0;
        QTest::newRow("too-short") << 511 << EINVAL;
        QTest::newRow("too-large") << 1048065 << EINVAL;
    }

    void boundary()
    {
        QFETCH(int, bytes); QFETCH(int, error);
        const QByteArray input(bytes, 'b');
        store(request_.grub2CorePath, input);
        request_.grub2CoreSize = quint64(bytes);
        request_.grub2CoreSha256 = QCryptographicHash::hash(input, QCryptographicHash::Sha256);
        QByteArray core;
        QCOMPARE(load(getuid(), gapOffset, core), error);
        QCOMPARE(core, error == 0 ? input : QByteArray());
    }

    void readFault_data()
    {
        QTest::addColumn<int>("mode"); QTest::addColumn<int>("error");
        QTest::newRow("eintr") << int(InterruptedRead) << 0;
        QTest::newRow("partial") << int(PartialRead) << 0;
        QTest::newRow("failed-first") << int(FailedRead) << EIO;
        QTest::newRow("failed-later") << int(FailedLaterRead) << EIO;
        QTest::newRow("early-end") << int(EarlyEnd) << EIO;
        QTest::newRow("cancel") << int(CancelRead) << ECANCELED;
        QTest::newRow("cancel-final-chunk") << int(CancelFinalRead) << ECANCELED;
        QTest::newRow("mutate") << int(ChangeFile) << ESTALE;
        QTest::newRow("replace") << int(ReplaceFile) << ESTALE;
        QTest::newRow("unlink") << int(RemoveFile) << ESTALE;
    }

    void readFault()
    {
        QFETCH(int, mode); QFETCH(int, error);
        fault = Fault(mode);
        if (fault == CancelFinalRead)
        {
            expected_ = QByteArray(512, 'g');
            store(request_.grub2CorePath, expected_);
            request_.grub2CoreSize = quint64(expected_.size());
            request_.grub2CoreSha256 = QCryptographicHash::hash(expected_, QCryptographicHash::Sha256);
        }
        active = true;
        QByteArray core("old result");
        const int result = load(getuid(), gapOffset, core);
        active = false;
        QCOMPARE(result, error);
        QCOMPARE(core, error == 0 ? expected_ : QByteArray());
        QVERIFY(reads > 0);
    }

    void borrowedReader()
    {
        IMAGE_FILE_SOURCE source = {-1, {}, false, false, nullptr, 0};
        source.fd = ::open(sourcePath.constData(), O_RDONLY | O_CLOEXEC);
        QVERIFY(source.fd >= 0);
        QCOMPARE(fstat(source.fd, &source.info), 0);
        QCOMPARE(lseek(source.fd, 19, SEEK_SET), off_t(19));
        QByteArray core;
        QCOMPARE(rufus::ReadPrepareCoreSource(source, sourcePath, gapOffset - 512, core), 0);
        QCOMPARE(core, expected_);
        QCOMPARE(lseek(source.fd, 0, SEEK_CUR), off_t(19));
        QCOMPARE(rufus::ReadPrepareCoreSource(source, sourcePath, gapOffset, core), EINVAL);
        QVERIFY(core.isEmpty());
        CloseImageFileSource(&source);
    }

    void helperPreflight_data()
    {
        QTest::addColumn<QString>("mode"); QTest::addColumn<int>("error");
        QTest::newRow("valid-reaches-blocked-target-gate") << QStringLiteral("valid") << ENOTSUP;
        QTest::newRow("changed-hash-stops-before-target") << QStringLiteral("hash") << ESTALE;
        QTest::newRow("changed-size-stops-before-target") << QStringLiteral("size") << ESTALE;
        QTest::newRow("missing-stops-before-target") << QStringLiteral("missing") << ENOENT;
        QTest::newRow("foreign-owner-stops-before-target") << QStringLiteral("owner") << EACCES;
        QTest::newRow("symlink-stops-before-target") << QStringLiteral("symlink") << ELOOP;
        QTest::newRow("image-source-missing-before-target") << QStringLiteral("image") << ENOENT;
        QTest::newRow("image-core-missing-before-target") << QStringLiteral("image-missing-core") << ENOENT;
    }

    void helperPreflight()
    {
        QFETCH(QString, mode); QFETCH(int, error);
        request_.path = directory_.filePath(QStringLiteral("unused-target"));
        request_.device = 123;
        request_.size = 64 * gapOffset;
        request_.volumeId = 0x12345678;
        request_.diskSequence = 7;
        request_.objectPath = QStringLiteral("/org/freedesktop/UDisks2/block_devices/fixture");
        request_.serviceOwner = QStringLiteral(":1.2");
        request_.identity = {QStringLiteral("fixture")};
        const QByteArray target("Target must remain untouched");
        store(request_.path, target);
        uid_t caller = getuid();
        if (mode == QStringLiteral("hash")) request_.grub2CoreSha256[0] ^= 1;
        if (mode == QStringLiteral("size")) ++request_.grub2CoreSize;
        if (mode == QStringLiteral("missing")) QVERIFY(QFile::remove(request_.grub2CorePath));
        if (mode == QStringLiteral("owner")) ++caller;
        if (mode == QStringLiteral("symlink"))
        {
            const QString link = directory_.filePath(QStringLiteral("link"));
            QVERIFY(QFile::link(request_.grub2CorePath, link));
            request_.grub2CorePath = link;
        }
        if (mode.startsWith(QStringLiteral("image")))
        {
            request_.imagePath = directory_.filePath(QStringLiteral("unopened-source.iso"));
            request_.imageInode = request_.imageSize = 1;
            request_.imageModified = request_.imageChanged = QStringLiteral("1.000000000");
            if (mode == QStringLiteral("image-missing-core"))
                QVERIFY(QFile::remove(request_.grub2CorePath));
        }
        QProcess helper;
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("RUFUS_TEST_GRUB2_PREFLIGHT"), QStringLiteral("1"));
        environment.insert(QStringLiteral("PKEXEC_UID"), QString::number(caller));
        helper.setProcessEnvironment(environment);
        helper.start(QStringLiteral(RUFUS_PREPARE_PROBE), {QStringLiteral("--device")});
        QVERIFY(helper.waitForStarted());
        QByteArray output;
        while (!output.contains('\n'))
        {
            QVERIFY(helper.waitForReadyRead(5000));
            output += helper.readAllStandardOutput();
        }
        QCOMPARE(output, QByteArray("{\"event\":\"ready\"}\n"));
        const QJsonObject message = rufus::EncodePrepareRequest(request_);
        QVERIFY(!message.isEmpty());
        const QByteArray encoded = QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
        QCOMPARE(helper.write(encoded), encoded.size());
        QVERIFY(helper.waitForFinished(5000));
        QCOMPARE(helper.exitStatus(), QProcess::NormalExit);
        QCOMPARE(helper.exitCode(), 1);
        output = helper.readAllStandardOutput();
        QJsonObject result;
        for (const QByteArray& line : output.split('\n'))
        {
            const QJsonObject frame = QJsonDocument::fromJson(line).object();
            if (frame.value("event") == QStringLiteral("result")) result = frame;
            if (frame.value("event") == QStringLiteral("phase"))
                QCOMPARE(frame.value("phase"), QJsonValue(QStringLiteral("checking")));
        }
        rufus::PrepareImageOutcome outcome;
        QVERIFY(rufus::DecodePrepareOutcome(result, outcome));
        QCOMPARE(outcome.error, error);
        QVERIFY(!outcome.started);
        const QByteArray diagnostics = helper.readAllStandardError();
        QCOMPARE(diagnostics.contains("RUFUS_TEST_TARGET_GATE"), mode == QStringLiteral("valid"));
        QFile file(request_.path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), target);
    }
};

QTEST_GUILESS_MAIN(TestPrepareGrub2Core)
#include "test_prepare_grub2_core.moc"
