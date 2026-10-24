// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/config_patch.h"
#include <QDir>
#include <QFile>
#include <QStringConverter>
#include <QTemporaryDir>
#include <QTest>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace
{
enum Failure { None, ShortIO, ReadFailure, WriteFailure, ZeroWrite, CancelWrite, FileSync,
    OutputClose, RenameFailure, DirectorySync, SourceChange, PathChange, CleanupFailure,
    MetadataUnsupported, DifferentOwner, DifferentGroup, MetadataStat };
Failure failure;
bool cancelled;
int reads, writes, syncs;
int ownerChanges, modeChanges;
QString inputPath;

void writeFixture(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
        qFatal("Could not write config fixture");
}

QByteArray utf16(const QByteArray& bytes)
{
    QStringEncoder encoder(QStringConverter::Utf16LE);
    QByteArray result("\xff\xfe", 2);
    result += encoder(QString::fromUtf8(bytes));
    return result;
}
}

extern "C" int IsIsoScanCancelled(void) { return cancelled; }
#if defined(__GLIBC__) && defined(__USE_FILE_OFFSET64)
#define WrappedPread __wrap_pread64
#define RealPread __real_pread64
#define WrappedStat __wrap_fstat64
#define RealStat __real_fstat64
#else
#define WrappedPread __wrap_pread
#define RealPread __real_pread
#define WrappedStat __wrap_fstat
#define RealStat __real_fstat
#endif
extern "C" ssize_t __real_write(int, const void*, size_t);
extern "C" ssize_t RealPread(int, void*, size_t, off_t);
extern "C" int __real_fsync(int);
extern "C" int __real_close(int);
extern "C" int __real_renameat(int, const char*, int, const char*);
extern "C" int __real_unlinkat(int, const char*, int);
extern "C" int RealStat(int, struct stat*);
extern "C" int __real_fchown(int, uid_t, gid_t);
extern "C" int __real_fchmod(int, mode_t);

extern "C" int WrappedStat(int fd, struct stat* info)
{
    const int result = RealStat(fd, info);
    if (result != 0 || (fcntl(fd, F_GETFL) & O_ACCMODE) == O_RDONLY)
        return result;
    if (failure == MetadataStat)
    {
        errno = EIO;
        return -1;
    }
    if (failure == DifferentOwner) ++info->st_uid;
    if (failure == DifferentGroup) ++info->st_gid;
    return result;
}

extern "C" int __wrap_fchown(int fd, uid_t owner, gid_t group)
{
    ++ownerChanges;
    const bool unsupported = failure == MetadataUnsupported || failure == DifferentOwner || failure == DifferentGroup;
    if (unsupported)
    {
        errno = EOPNOTSUPP;
        return -1;
    }
    return __real_fchown(fd, owner, group);
}

extern "C" int __wrap_fchmod(int fd, mode_t mode)
{
    ++modeChanges;
    if (failure == MetadataUnsupported)
    {
        errno = EOPNOTSUPP;
        return -1;
    }
    return __real_fchmod(fd, mode);
}

extern "C" ssize_t WrappedPread(int fd, void* data, size_t size, off_t offset)
{
    ++reads;
    if (failure == ReadFailure || (failure == ShortIO && reads == 1))
    {
        errno = failure == ReadFailure ? EIO : EINTR;
        return -1;
    }
    return RealPread(fd, data, failure == ShortIO ? qMin(size, size_t(7)) : size, offset);
}

extern "C" ssize_t __wrap_write(int fd, const void* data, size_t size)
{
    ++writes;
    if (failure == ShortIO && writes == 1)
    {
        errno = EINTR;
        return -1;
    }
    const bool failedWrite = failure == WriteFailure || failure == CleanupFailure;
    if (failedWrite && writes > 1)
    {
        errno = ENOSPC;
        return -1;
    }
    if (failure == ZeroWrite)
        return 0;
    const bool shortWrite = failedWrite || failure == ShortIO || failure == CancelWrite;
    const ssize_t count = __real_write(fd, data, shortWrite ? qMin(size, size_t(5)) : size);
    if (failure == CancelWrite)
        cancelled = true;
    if (failure == SourceChange && writes == 1)
        writeFixture(inputPath, "changed during edit\n");
    if (failure == PathChange && writes == 1)
    {
        if (!QFile::rename(inputPath, inputPath + QStringLiteral(".saved")))
            qFatal("Could not replace fixture path");
        writeFixture(inputPath, "new path contents\n");
    }
    return count;
}

extern "C" int __wrap_fsync(int fd)
{
    ++syncs;
    struct stat info;
    if (fstat(fd, &info) != 0)
        qFatal("Could not stat descriptor to flush");
    const bool failFile = failure == FileSync && S_ISREG(info.st_mode);
    const bool failDirectory = failure == DirectorySync && S_ISDIR(info.st_mode);
    if (failFile || failDirectory || (failure == ShortIO && syncs == 1))
    {
        errno = failure == ShortIO ? EINTR : EIO;
        return -1;
    }
    return __real_fsync(fd);
}

extern "C" int __wrap_close(int fd)
{
    const bool writable = (fcntl(fd, F_GETFL) & O_ACCMODE) != O_RDONLY;
    const int result = __real_close(fd);
    if (failure == OutputClose && writable)
    {
        errno = EIO;
        return -1;
    }
    return result;
}

extern "C" int __wrap_renameat(int from, const char* source, int to, const char* destination)
{
    if (failure == RenameFailure)
    {
        errno = EACCES;
        return -1;
    }
    return __real_renameat(from, source, to, destination);
}

extern "C" int __wrap_unlinkat(int parent, const char* name, int flags)
{
    if (failure == CleanupFailure)
    {
        errno = EACCES;
        return -1;
    }
    return __real_unlinkat(parent, name, flags);
}

class TestConfigPatch : public QObject
{
    Q_OBJECT
    QTemporaryDir directory_;
    int parent_ = -1;
    qsizetype descriptors_ = 0;

    QByteArray contents(const QString& suffix = QString())
    {
        QFile file(inputPath + suffix);
        if (!file.open(QIODevice::ReadOnly))
            qFatal("Could not read fixture");
        return file.readAll();
    }

private slots:
    void init()
    {
        QVERIFY(directory_.isValid());
        inputPath = directory_.filePath(QStringLiteral("boot.cfg"));
        parent_ = open(QFile::encodeName(directory_.path()).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        QVERIFY(parent_ >= 0);
        failure = None;
        ownerChanges = modeChanges = 0;
        cancelled = false;
        reads = writes = syncs = 0;
        descriptors_ = QDir(QStringLiteral("/proc/self/fd")).entryList().size();
    }

    void cleanup()
    {
        failure = None;
        QCOMPARE(QDir(QStringLiteral("/proc/self/fd")).entryList().size(), descriptors_);
        __real_close(parent_);
        const QDir directory(directory_.path());
        const QStringList names = directory.entryList(QDir::Files | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
        for (const QString& name : names)
            QVERIFY(QFile::remove(directory.filePath(name)));
    }

    void edits_data()
    {
        QTest::addColumn<QByteArray>("input");
        QTest::addColumn<QByteArray>("expected");
        QTest::addColumn<int>("error");
        QTest::newRow("ascii") << QByteArray("# old\r\n ApPeNd old old old old old\r\n")
            << QByteArray("# old\n ApPeNd new new new new old\n") << 0;
        QTest::newRow("unchanged") << QByteArray("# append old\r\nappendix old\r\n")
            << QByteArray("# append old\r\nappendix old\r\n") << 0;
        QTest::newRow("utf8") << QByteArray("append čș🚀 old\r\n") << QByteArray("append čș🚀 new\n") << 0;
        QTest::newRow("utf8-bom") << QByteArray("\xef\xbb\xbf" "append old\r\n")
            << QByteArray("\xef\xbb\xbf" "append new\n") << 0;
        QTest::newRow("utf16") << utf16("append čș🚀 old\r\n") << utf16("append čș🚀 new\n") << 0;
        QTest::newRow("utf16-unchanged") << utf16("# append č old\r\n") << utf16("# append č old\r\n") << 0;
        QTest::newRow("continuation-token") << QByteArray(1023, 'x') + "append old\r\n"
            << QByteArray(1023, 'x') + "append new\n" << 0;
        QTest::newRow("split-surrogate") << QByteArray(1022, 'x') + "🚀\r\nappend old\r\n"
            << QByteArray(1022, 'x') + "🚀\nappend new\n" << 0;
        const QByteArray splitSource = "append " + QByteArray(1014, 'x') + "old\r\n";
        QTest::newRow("split-source") << splitSource << splitSource << 0;
        QTest::newRow("empty") << QByteArray() << QByteArray() << 0;
        QTest::newRow("no-final-newline") << QByteArray("append old") << QByteArray("append new") << 0;
        QTest::newRow("bare-cr") << QByteArray("# old\rX\nappend old\r") << QByteArray("# oldX\nappend new") << 0;
        QTest::newRow("invalid-utf8") << QByteArray("append old\n\xff") << QByteArray("append old\n\xff") << EILSEQ;
        QTest::newRow("incomplete-utf8") << QByteArray("append old\n\xc3") << QByteArray("append old\n\xc3") << EILSEQ;
        const QByteArray oddUtf16 = utf16("append old") + "x";
        QTest::newRow("incomplete-utf16") << oddUtf16 << oddUtf16 << EILSEQ;
        const QByteArray highSurrogate = utf16("append old") + QByteArray("\0\xd8", 2);
        const QByteArray lowSurrogate = utf16("append old") + QByteArray("\0\xdc", 2);
        QTest::newRow("unpaired-high-surrogate") << highSurrogate << highSurrogate << EILSEQ;
        QTest::newRow("unpaired-low-surrogate") << lowSurrogate << lowSurrogate << EILSEQ;
        QTest::newRow("utf16be") << QByteArray("\xfe\xff\0x", 4) << QByteArray("\xfe\xff\0x", 4) << ENOTSUP;
        QTest::newRow("nul") << QByteArray("append old\0tail", 15) << QByteArray("append old\0tail", 15) << ENOTSUP;
        QTest::newRow("eof-marker") << QByteArray("append old\x1a" "tail") << QByteArray("append old\x1a" "tail") << ENOTSUP;
    }

    void edits()
    {
        QFETCH(QByteArray, input);
        QFETCH(QByteArray, expected);
        QFETCH(int, error);
        writeFixture(inputPath, input);
        struct stat before, after;
        QCOMPARE(fchmodat(parent_, "boot.cfg", 0640, 0), 0);
        QCOMPARE(fstatat(parent_, "boot.cfg", &before, 0), 0);
        bool modified = true;
        QCOMPARE(ReplaceConfigTokenAt(parent_, "boot.cfg", "append", "old", "new", &modified), error);
        QCOMPARE(modified, input != expected);
        QCOMPARE(contents(), expected);
        QCOMPARE(fstatat(parent_, "boot.cfg", &after, 0), 0);
        QCOMPARE(after.st_uid, before.st_uid);
        QCOMPARE(after.st_gid, before.st_gid);
        QCOMPARE(after.st_mode, before.st_mode);
        if (!modified)
            QCOMPARE(after.st_ino, before.st_ino);
        QCOMPARE(QDir(directory_.path()).entryList(QDir::Files | QDir::Hidden).size(), 1);
    }

    void metadata_data()
    {
        QTest::addColumn<int>("fault");
        QTest::addColumn<int>("mode");
        QTest::addColumn<int>("error");
        QTest::addColumn<int>("owners");
        QTest::addColumn<int>("modes");
        QTest::newRow("already-matches") << int(MetadataUnsupported) << 0600 << 0 << 0 << 0;
        QTest::newRow("needs-mode") << int(MetadataUnsupported) << 0640 << EOPNOTSUPP << 0 << 1;
        QTest::newRow("needs-owner") << int(DifferentOwner) << 0600 << EOPNOTSUPP << 1 << 0;
        QTest::newRow("needs-group") << int(DifferentGroup) << 0600 << EOPNOTSUPP << 1 << 0;
        QTest::newRow("stat-failed") << int(MetadataStat) << 0600 << EIO << 0 << 0;
    }

    void metadata()
    {
        QFETCH(int, fault);
        QFETCH(int, mode);
        QFETCH(int, error);
        QFETCH(int, owners);
        QFETCH(int, modes);
        const QByteArray original("append old\n");
        writeFixture(inputPath, original);
        QCOMPARE(fchmodat(parent_, "boot.cfg", mode_t(mode), 0), 0);
        struct stat before, after;
        QCOMPARE(fstatat(parent_, "boot.cfg", &before, 0), 0);
        failure = Failure(fault);
        bool modified = false;
        QCOMPARE(ReplaceConfigTokenAt(parent_, "boot.cfg", "append", "old", "new", &modified), error);
        QCOMPARE(modified, error == 0);
        QCOMPARE(ownerChanges, owners);
        QCOMPARE(modeChanges, modes);
        QCOMPARE(contents(), error == 0 ? QByteArray("append new\n") : original);
        QCOMPARE(fstatat(parent_, "boot.cfg", &after, 0), 0);
        QCOMPARE(after.st_uid, before.st_uid);
        QCOMPARE(after.st_gid, before.st_gid);
        QCOMPARE(after.st_mode, before.st_mode);
        if (error != 0) QCOMPARE(after.st_ino, before.st_ino);
        QCOMPARE(QDir(directory_.path()).entryList(QDir::Files | QDir::Hidden).size(), 1);
    }

    void errors_data()
    {
        QTest::addColumn<int>("fault");
        QTest::addColumn<int>("error");
        QTest::newRow("short-io") << int(ShortIO) << 0;
        QTest::newRow("read") << int(ReadFailure) << EIO;
        QTest::newRow("partial-write") << int(WriteFailure) << ENOSPC;
        QTest::newRow("zero-write") << int(ZeroWrite) << EIO;
        QTest::newRow("cancel-write") << int(CancelWrite) << ECANCELED;
        QTest::newRow("file-sync") << int(FileSync) << EIO;
        QTest::newRow("output-close") << int(OutputClose) << EIO;
        QTest::newRow("rename") << int(RenameFailure) << EACCES;
        QTest::newRow("directory-sync") << int(DirectorySync) << EIO;
        QTest::newRow("source-change") << int(SourceChange) << ESTALE;
        QTest::newRow("path-change") << int(PathChange) << ESTALE;
        QTest::newRow("cleanup") << int(CleanupFailure) << EACCES;
    }

    void errors()
    {
        QFETCH(int, fault);
        QFETCH(int, error);
        const QByteArray original("append old old\r\n");
        writeFixture(inputPath, original);
        failure = Failure(fault);
        bool modified = false;
        QCOMPARE(ReplaceConfigTokenAt(parent_, "boot.cfg", "append", "old", "new", &modified), error);
        const bool committed = failure == ShortIO || failure == DirectorySync;
        QCOMPARE(modified, committed);
        if (failure == SourceChange)
            QCOMPARE(contents(), QByteArray("changed during edit\n"));
        else if (failure == PathChange)
        {
            QCOMPARE(contents(), QByteArray("new path contents\n"));
            QCOMPARE(contents(QStringLiteral(".saved")), original);
        }
        else
            QCOMPARE(contents(), committed ? QByteArray("append new new\n") : original);
        const qsizetype extraFile = failure == CleanupFailure || failure == PathChange ? 1 : 0;
        QCOMPARE(QDir(directory_.path()).entryList(QDir::Files | QDir::Hidden).size(), 1 + extraFile);
    }

    void boundsAndGuards()
    {
        writeFixture(inputPath, "append old\n");
        bool modified;
        QCOMPARE(ReplaceConfigTokenAt(parent_, "../boot.cfg", "append", "old", "new", &modified), EINVAL);
        QCOMPARE(ReplaceConfigTokenAt(parent_, "boot.cfg", "", "old", "new", &modified), EINVAL);
        QCOMPARE(ReplaceConfigTokenAt(parent_, "boot.cfg", "append", "old", "old", &modified), 0);
        QVERIFY(!modified);
        QCOMPARE(writes, 0);
        cancelled = true;
        QCOMPARE(ReplaceConfigTokenAt(parent_, "boot.cfg", "append", "old", "new", &modified), ECANCELED);
        cancelled = false;
        QCOMPARE(linkat(parent_, "boot.cfg", parent_, "link.cfg", 0), 0);
        QCOMPARE(ReplaceConfigTokenAt(parent_, "boot.cfg", "append", "old", "new", &modified), EMLINK);
        QVERIFY(QFile::remove(directory_.filePath(QStringLiteral("link.cfg"))));
        QCOMPARE(symlinkat("boot.cfg", parent_, "link.cfg"), 0);
        QCOMPARE(ReplaceConfigTokenAt(parent_, "link.cfg", "append", "old", "new", &modified), ELOOP);
        QVERIFY(QFile::remove(directory_.filePath(QStringLiteral("link.cfg"))));
        QCOMPARE(mkfifoat(parent_, "fifo.cfg", 0600), 0);
        QCOMPARE(ReplaceConfigTokenAt(parent_, "fifo.cfg", "append", "old", "new", &modified), ENOTSUP);
        QVERIFY(QFile::remove(directory_.filePath(QStringLiteral("fifo.cfg"))));
        QFile large(inputPath);
        QVERIFY(large.open(QIODevice::ReadWrite));
        QVERIFY(large.resize(8 * 1024 * 1024 + 1));
        large.close();
        QCOMPARE(ReplaceConfigTokenAt(parent_, "boot.cfg", "append", "old", "new", &modified), EFBIG);
        QCOMPARE(writes, 0);
        const QByteArray expanding = QByteArray("append old\n").repeated(10000);
        writeFixture(inputPath, expanding);
        const QByteArray replacement(2000, 'x');
        QCOMPARE(ReplaceConfigTokenAt(parent_, "boot.cfg", "append", "old", replacement.constData(), &modified), EFBIG);
        QVERIFY(!modified);
        QCOMPARE(contents(), expanding);
        QCOMPARE(writes, 0);
    }

    void persistence_data()
    {
        QTest::addColumn<QByteArray>("source");
        QTest::addColumn<QByteArray>("expected");
        QTest::addColumn<bool>("grub");
        QTest::newRow("ubuntu-preseed") << QByteArray("append file=/cdrom/preseed/ubuntu.seed boot=casper\n")
            << QByteArray("append persistent file=/cdrom/preseed/ubuntu.seed boot=casper\n") << false;
        QTest::newRow("ubuntu-grub-install") << QByteArray("linux /casper/vmlinuz file=/cdrom/preseed/ubuntu.seed maybe-ubiquity --\n")
            << QByteArray("linux /casper/vmlinuz persistent file=/cdrom/preseed/ubuntu.seed  --\n") << true;
        QTest::newRow("mint-casper") << QByteArray("append initrd=/casper/initrd boot=casper quiet\n")
            << QByteArray("append initrd=/casper/initrd boot=casper persistent quiet\n") << false;
        QTest::newRow("ubuntu-modern-grub") << QByteArray("linux /casper/vmlinuz quiet splash ---\n")
            << QByteArray("linux /casper/vmlinuz persistent quiet splash ---\n") << true;
        QTest::newRow("ubuntu-modern-kernel") << QByteArray("kernel /casper/vmlinuz quiet\n")
            << QByteArray("kernel /casper/vmlinuz persistent quiet\n") << false;
        QTest::newRow("debian-syslinux") << QByteArray("append boot=live components quiet\n")
            << QByteArray("append boot=live persistence components quiet\n") << false;
        QTest::newRow("debian-grub") << QByteArray("linux /live/vmlinuz boot=live components\n")
            << QByteArray("linux /live/vmlinuz boot=live persistence components\n") << true;
        QTest::newRow("first-file-pass") << QByteArray("append boot=casper\nappend boot=live\n")
            << QByteArray("append boot=casper persistent\nappend boot=live\n") << false;
        QTest::newRow("unmatched-comment") << QByteArray("# append boot=live\nsearch boot=live\n")
            << QByteArray("# append boot=live\nsearch boot=live\n") << false;
    }

    void persistence()
    {
        QFETCH(QByteArray, source);
        QFETCH(QByteArray, expected);
        QFETCH(bool, grub);
        for (bool wide : {false, true})
        {
            const QByteArray original = wide ? utf16(source) : source;
            const QByteArray output = wide ? utf16(expected) : expected;
            writeFixture(inputPath, original);
            size_t outputSize = 0;
            QCOMPARE(CheckIsoConfigText(original.constData(), size_t(original.size()), nullptr, 0,
                true, grub, &outputSize), 0);
            QCOMPARE(outputSize, size_t(output.size()));
            QCOMPARE(contents(), original);
            bool modified = false;
            QCOMPARE(ReplaceIsoConfigTokensAt(parent_, "boot.cfg", nullptr, 0, true, grub, &modified), 0);
            QCOMPARE(modified, original != output);
            QCOMPARE(contents(), output);
        }
    }

    void orderedPassesAndPreflight()
    {
        size_t outputSize = 0;
        const QByteArray original = "append OLD\r\nlinux OLD\r\n";
        const QByteArray expected = "append FINAL\nlinux FINAL\n";
        const CONFIG_TEXT_PATCH patches[] = {{"append", "OLD", "MID"}, {"linux", "OLD", "MID"},
            {"append", "MID", "FINAL"}, {"linux", "MID", "FINAL"}};
        writeFixture(inputPath, original);
        QCOMPARE(CheckConfigText(original.constData(), size_t(original.size()), patches, 4, &outputSize), 0);
        QCOMPARE(outputSize, size_t(expected.size()));
        QCOMPARE(writes, 0);
        QCOMPARE(contents(), original);
        bool modified = false;
        QCOMPARE(ReplaceConfigTokensAt(parent_, "boot.cfg", patches, 4, &modified), 0);
        QVERIFY(modified);
        QCOMPARE(contents(), expected);
        QCOMPARE(writes, 1); // One commit, despite four transformations.
        const CONFIG_TEXT_PATCH failing[] = {{"append", "OLD", "MID"}, {"linux", "MID", "\xff"}};
        writeFixture(inputPath, original);
        QCOMPARE(CheckConfigText(original.constData(), size_t(original.size()), failing, 2, &outputSize), EILSEQ);
        QCOMPARE(ReplaceConfigTokensAt(parent_, "boot.cfg", failing, 2, &modified), EILSEQ);
        QVERIFY(!modified);
        QCOMPARE(contents(), original);
        QCOMPARE(writes, 1);
        const CONFIG_TEXT_PATCH inactive[] = {{"append", "", "NEW"}, {"linux", "OLD", "OLD"}};
        QCOMPARE(CheckConfigText("\xff", 1, inactive, 2, &outputSize), 0);
        QCOMPARE(ReplaceConfigTokensAt(parent_, "boot.cfg", inactive, 2, &modified), 0);
        QVERIFY(!modified);
        QCOMPARE(writes, 1);
        QCOMPARE(CheckConfigText(nullptr, 1, patches, 4, &outputSize), EINVAL);
        QCOMPARE(CheckConfigText(nullptr, 0, nullptr, 1, &outputSize), EINVAL);
    }

    void labelPassChunkOrdering()
    {
        // Shrinking the first pass moves a later token off a 1023-unit boundary.
        // A combined single traversal would incorrectly replace the second OLD.
        const QByteArray padding(1012, 'x');
        writeFixture(inputPath, "options OLD" + padding + "linux OLD\n");
        CONFIG_TEXT_PATCH patches[ISO_CONFIG_MAX_PATCHES];
        const size_t count = GetIsoConfigPatches("OLD", "N", false, "dvd.iso", patches);
        bool modified = false;
        QCOMPARE(ReplaceConfigTokensAt(parent_, "boot.cfg", patches, count, &modified), 0);
        QVERIFY(modified);
        QCOMPARE(contents(), "options N" + padding + "linux OLD\n");
    }

    void unicodeParametersAndDeletion()
    {
        writeFixture(inputPath, utf16("append ș🚀 ș🚀\r\n"));
        bool modified = false;
        QCOMPARE(ReplaceConfigTokenAt(parent_, "boot.cfg", "append", "ș🚀", "č新", &modified), 0);
        QVERIFY(modified);
        QCOMPARE(contents(), utf16("append č新 č新\n"));
        QCOMPARE(ReplaceConfigTokenAt(parent_, "boot.cfg", "append", "č新", "", &modified), 0);
        QVERIFY(modified);
        QCOMPARE(contents(), utf16("append  \n"));
    }
};

QTEST_APPLESS_MAIN(TestConfigPatch)
#include "test_config_patch.moc"
