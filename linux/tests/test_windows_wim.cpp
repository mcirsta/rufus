// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/windows_wim.h"
#include "platform/wim_read_file.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
#include <cerrno>
#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>

static bool cancelled, failSync;
static int cancelMessage, syncCalls;
static wimlib_progress_func_t progressFunction;
static void* progressContext;

extern "C" int IsRufusJobCancelled(void) { return cancelled; }
extern "C" int IsIsoScanCancelled(void) { return cancelled; }
extern "C" void uprintf(const char*, ...) {}
extern "C" int __real_fsync(int fd);
extern "C" void __real_wimlib_register_progress_function(WIMStruct*, wimlib_progress_func_t, void*);

extern "C" int __wrap_fsync(int fd)
{
    ++syncCalls;
    if (failSync)
    {
        errno = EIO;
        return -1;
    }
    return __real_fsync(fd);
}

static enum wimlib_progress_status TestProgress(enum wimlib_progress_msg message,
    union wimlib_progress_info* info, void*)
{
    if (message == cancelMessage)
        cancelled = true;
    return progressFunction(message, info, progressContext);
}

extern "C" void __wrap_wimlib_register_progress_function(WIMStruct* wim,
    wimlib_progress_func_t function, void* context)
{
    progressFunction = function;
    progressContext = context;
    __real_wimlib_register_progress_function(wim, function ? TestProgress : nullptr, nullptr);
}

class WindowsWimTest : public QObject
{
    Q_OBJECT
private:
    QTemporaryDir directory_;
    QByteArray wimPath_;
    QByteArray sourcePath_;

    bool writeFile(const QString& path, const QByteArray& bytes)
    {
        QFile file(path);
        return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.flush();
    }

    QByteArray diskHash()
    {
        QFile file(QString::fromUtf8(wimPath_));
        if (!file.open(QIODevice::ReadOnly))
            return {};
        return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256);
    }

    int makeFixture(int images = 2, int flags = 0)
    {
        WIMStruct* wim = nullptr;
        int error = wimlib_create_new_wim(WIMLIB_COMPRESSION_TYPE_LZX, &wim);
        for (int image = 1; image <= images && error == 0; ++image)
        {
            const QByteArray label = QByteArray::number(image);
            const QString tree = directory_.filePath("tree-" + QString::number(image));
            const bool written = QDir().mkpath(tree) && writeFile(tree + "/keep.txt", "keep-" + label) &&
                writeFile(tree + "/replace.txt", "original-" + label);
            if (!written)
                error = WIMLIB_ERR_WRITE;
            else
                error = wimlib_add_image(wim, tree.toUtf8().constData(), label.constData(), nullptr, 0);
        }
        struct wimlib_wim_info info = {};
        info.boot_index = 1; // Setup selection must not follow a different boot index.
        if (error == 0)
            error = wimlib_set_wim_info(wim, &info, WIMLIB_CHANGE_BOOT_INDEX);
        if (error == 0)
            error = wimlib_write(wim, wimPath_.constData(), WIMLIB_ALL_IMAGES, flags, 1);
        wimlib_free(wim);
        return error;
    }

    int applyUpdate()
    {
        WIMStruct* wim = nullptr;
        int image = 0;
        int error = OpenWindowsSetupWim(wimPath_.constData(), &wim, &image);
        struct wimlib_update_command commands[2] = {};
        commands[0].op = WIMLIB_UPDATE_OP_ADD;
        commands[0].add.fs_source_path = sourcePath_.data();
        char replacement[] = "/REPLACE.TXT";
        commands[0].add.wim_target_path = replacement;
        commands[1] = commands[0];
        char addition[] = "/added.txt";
        commands[1].add.wim_target_path = addition;
        if (error == 0)
            error = UpdateWindowsSetupWim(wim, image, commands, 2);
        wimlib_free(wim);
        return error;
    }

    void verifyFile(WIMStruct* wim, int image, const char* path, const QByteArray& expected)
    {
        uint8_t* bytes = nullptr;
        size_t size = 0;
        const int error = ReadWimFile(wim, image, path, &bytes, &size);
        const QByteArray actual(reinterpret_cast<const char*>(bytes), static_cast<qsizetype>(size));
        free(bytes);
        QCOMPARE(error, 0);
        QCOMPARE(actual, expected);
    }

    void verifyUpdate(int images)
    {
        WIMStruct* wim = nullptr;
        QCOMPARE(wimlib_open_wim(wimPath_.constData(), WIMLIB_OPEN_FLAG_CHECK_INTEGRITY, &wim), 0);
        struct wimlib_wim_info info = {};
        const int error = wimlib_get_wim_info(wim, &info);
        verifyFile(wim, images, "/replace.txt", QByteArray(256 * 1024, 'R'));
        verifyFile(wim, images, "/added.txt", QByteArray(256 * 1024, 'R'));
        verifyFile(wim, images, "/keep.txt", "keep-" + QByteArray::number(images));
        int missing = WIMLIB_ERR_PATH_DOES_NOT_EXIST;
        if (images == 2)
        {
            verifyFile(wim, 1, "/replace.txt", "original-1");
            verifyFile(wim, 1, "/keep.txt", "keep-1");
            uint8_t* bytes = nullptr;
            size_t size = 0;
            missing = ReadWimFile(wim, 1, "/added.txt", &bytes, &size);
            free(bytes);
        }
        const int verified = wimlib_verify_wim(wim, 0);
        wimlib_free(wim);
        QCOMPARE(error, 0);
        QCOMPARE(info.image_count, static_cast<uint32_t>(images));
        QCOMPARE(info.boot_index, 1u);
        QCOMPARE(verified, 0);
        QCOMPARE(missing, int(WIMLIB_ERR_PATH_DOES_NOT_EXIST));
    }

private slots:
    void initTestCase()
    {
        QVERIFY(directory_.isValid());
        wimPath_ = directory_.filePath("boot.wim").toUtf8();
        sourcePath_ = directory_.filePath("replacement.txt").toUtf8();
        QCOMPARE(wimlib_global_init(WIMLIB_INIT_FLAG_ASSUME_UTF8 |
            WIMLIB_INIT_FLAG_DEFAULT_CASE_INSENSITIVE), 0);
    }

    void cleanupTestCase() { wimlib_global_cleanup(); }

    void init()
    {
        cancelled = failSync = false;
        cancelMessage = syncCalls = 0;
        QVERIFY(writeFile(QString::fromUtf8(sourcePath_), QByteArray(256 * 1024, 'R')));
        QCOMPARE(makeFixture(), 0);
    }

    void updatesOnlySetupImage()
    {
        const QByteArray before = diskHash();
        QCOMPARE(applyUpdate(), 0);
        QVERIFY(syncCalls > 0);
        QVERIFY(diskHash() != before);
        verifyUpdate(2);
    }

    void singleImageFallback()
    {
        QCOMPARE(makeFixture(1), 0);
        QCOMPARE(applyUpdate(), 0);
        verifyUpdate(1);
    }

    void cancellation_data()
    {
        QTest::addColumn<int>("message");
        QTest::newRow("before-open") << -1;
        QTest::newRow("capture") << int(WIMLIB_PROGRESS_MSG_SCAN_BEGIN);
        QTest::newRow("streams") << int(WIMLIB_PROGRESS_MSG_WRITE_STREAMS);
        QTest::newRow("metadata") << int(WIMLIB_PROGRESS_MSG_WRITE_METADATA_BEGIN);
    }

    void cancellation()
    {
        QFETCH(int, message);
        const QByteArray before = diskHash();
        cancelled = message == -1;
        cancelMessage = message;
        QCOMPARE(applyUpdate(), int(WIMLIB_ERR_ABORTED_BY_PROGRESS));
        QVERIFY(cancelled);
        QCOMPARE(diskHash(), before);
    }

    void cancellationAfterCommit()
    {
        // Pipable WIMs use upstream's temporary-file/rebuild path.
        QCOMPARE(makeFixture(2, WIMLIB_WRITE_FLAG_PIPABLE), 0);
        cancelMessage = WIMLIB_PROGRESS_MSG_RENAME;
        QCOMPARE(applyUpdate(), 0);
        QVERIFY(cancelled);
        cancelled = false;
        cancelMessage = 0;
        verifyUpdate(2);
    }

    void missingSourceDoesNotCommit()
    {
        const QByteArray before = diskHash();
        QVERIFY(QFile::remove(QString::fromUtf8(sourcePath_)));
        QVERIFY(applyUpdate() != 0);
        QCOMPARE(syncCalls, 0);
        QCOMPARE(diskHash(), before);
    }

    void syncFailureIsReported()
    {
        failSync = true;
        QVERIFY(applyUpdate() != 0);
        QVERIFY(syncCalls > 0);
        // A storage failure is not a promise that on-disk bytes were rolled back.
    }

    void noCommandsPreserveBytes()
    {
        WIMStruct* wim = nullptr;
        int image = 0;
        const QByteArray before = diskHash();
        QCOMPARE(OpenWindowsSetupWim(wimPath_.constData(), &wim, &image), 0);
        const int error = UpdateWindowsSetupWim(wim, image, nullptr, 0);
        wimlib_free(wim);
        QCOMPARE(error, 0);
        QCOMPARE(syncCalls, 0);
        QCOMPARE(diskHash(), before);
    }

    void rejectsInvalidInputs()
    {
        WIMStruct* wim = nullptr;
        int image = 99;
        QCOMPARE(OpenWindowsSetupWim(nullptr, &wim, &image), int(WIMLIB_ERR_INVALID_PARAM));
        QVERIFY(wim == nullptr);
        QCOMPARE(image, 0);
        QCOMPARE(OpenWindowsSetupWim(wimPath_.constData(), nullptr, &image), int(WIMLIB_ERR_INVALID_PARAM));
        QCOMPARE(OpenWindowsSetupWim(wimPath_.constData(), &wim, nullptr), int(WIMLIB_ERR_INVALID_PARAM));
        QCOMPARE(UpdateWindowsSetupWim(nullptr, 1, nullptr, 0), int(WIMLIB_ERR_INVALID_PARAM));
        QCOMPARE(OpenWindowsSetupWim(wimPath_.constData(), &wim, &image), 0);
        const int badImage = UpdateWindowsSetupWim(wim, 0, nullptr, 0);
        const int badCommands = UpdateWindowsSetupWim(wim, image, nullptr, 1);
        wimlib_free(wim);
        QCOMPARE(badImage, int(WIMLIB_ERR_INVALID_PARAM));
        QCOMPARE(badCommands, int(WIMLIB_ERR_INVALID_PARAM));
    }

    void rejectsUnsafeTargets()
    {
        const QByteArray linkPath = directory_.filePath("link.wim").toUtf8();
        const QByteArray fifoPath = directory_.filePath("pipe.wim").toUtf8();
        WIMStruct* wim = nullptr;
        int image = 0;
        const QByteArray before = diskHash();
        QCOMPARE(symlink(wimPath_.constData(), linkPath.constData()), 0);
        QCOMPARE(OpenWindowsSetupWim(linkPath.constData(), &wim, &image), int(WIMLIB_ERR_NOT_A_REGULAR_FILE));
        QCOMPARE(unlink(linkPath.constData()), 0);
        QCOMPARE(link(wimPath_.constData(), linkPath.constData()), 0);
        QCOMPARE(OpenWindowsSetupWim(wimPath_.constData(), &wim, &image), int(WIMLIB_ERR_INVALID_PARAM));
        QCOMPARE(unlink(linkPath.constData()), 0);
        QCOMPARE(mkfifo(fifoPath.constData(), 0600), 0);
        QCOMPARE(OpenWindowsSetupWim(fifoPath.constData(), &wim, &image), int(WIMLIB_ERR_NOT_A_REGULAR_FILE));
        QCOMPARE(unlink(fifoPath.constData()), 0);
        QVERIFY(wim == nullptr);
        QCOMPARE(image, 0);
        QCOMPARE(diskHash(), before);
    }

    void rejectsInvalidArchives()
    {
        WIMStruct* wim = nullptr;
        int image = 0;
        QVERIFY(writeFile(QString::fromUtf8(wimPath_), "not a WIM"));
        QVERIFY(OpenWindowsSetupWim(wimPath_.constData(), &wim, &image) != 0);
        QVERIFY(wim == nullptr);
        QCOMPARE(image, 0);
        QCOMPARE(makeFixture(), 0);
        QCOMPARE(wimlib_open_wim(wimPath_.constData(), 0, &wim), 0);
        struct wimlib_wim_info info = {};
        info.is_marked_readonly = 1;
        const int marked = wimlib_set_wim_info(wim, &info, WIMLIB_CHANGE_READONLY_FLAG);
        const int written = wimlib_overwrite(wim, WIMLIB_WRITE_FLAG_IGNORE_READONLY_FLAG, 1);
        wimlib_free(wim);
        wim = nullptr;
        QCOMPARE(marked, 0);
        QCOMPARE(written, 0);
        const QByteArray before = diskHash();
        QCOMPARE(OpenWindowsSetupWim(wimPath_.constData(), &wim, &image), int(WIMLIB_ERR_WIM_IS_READONLY));
        QVERIFY(wim == nullptr);
        QCOMPARE(image, 0);
        QCOMPARE(diskHash(), before);
    }
};

QTEST_GUILESS_MAIN(WindowsWimTest)
#include "test_windows_wim.moc"
