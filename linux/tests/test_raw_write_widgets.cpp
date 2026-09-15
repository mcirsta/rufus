// SPDX-License-Identifier: GPL-3.0-or-later
#include "qt/main_window.h"
#include "qt/selection_controller.h"
#include "platform/raw_write.h"

#include <QDBusArgument>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QFile>
#include <QComboBox>
#include <QFileDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSemaphore>
#include <QToolButton>
#include <atomic>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <cerrno>
#include <cstdarg>
#include <dirent.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <memory>
#include <sys/ioctl.h>
#include <unistd.h>

// Only the private fixture's regular-file inode gets synthetic kernel facts.
// No block nodes are created or opened, and production has no test-mode branch.
static ino_t fixtureInode;
static dev_t fixtureDevice;
static bool emulateBlock;
static uint64_t nativeNumber, nativeCapacity;
static int nativeReadOnly, nativeSector, nativeError;


#if defined(__GLIBC__) && defined(__USE_FILE_OFFSET64)
#define WrappedStat __wrap_fstat64
#define RealStat __real_fstat64
#else
#define WrappedStat __wrap_fstat
#define RealStat __real_fstat
#endif
extern "C" int RealStat(int, struct stat*);
extern "C" int __real_ioctl(int, unsigned long, ...);

extern "C" int WrappedStat(int fd, struct stat* info)
{
    const int error = RealStat(fd, info);
    if (error == 0 && emulateBlock && info->st_ino == fixtureInode && info->st_dev == fixtureDevice)
    {
        info->st_mode = (info->st_mode & ~S_IFMT) | S_IFBLK;
        info->st_rdev = dev_t(nativeNumber);
    }
    return error;
}

extern "C" int __wrap_ioctl(int fd, unsigned long request, ...)
{
    va_list args;
    va_start(args, request);
    void* value = va_arg(args, void*);
    va_end(args);
    struct stat info;
    if (RealStat(fd, &info) == 0 && emulateBlock && info.st_ino == fixtureInode
        && info.st_dev == fixtureDevice)
    {
        if (nativeError) { errno = nativeError; return -1; }
        if (request == BLKGETSIZE64) *static_cast<uint64_t*>(value) = nativeCapacity;
        else if (request == BLKROGET) *static_cast<int*>(value) = nativeReadOnly;
        else if (request == BLKSSZGET) *static_cast<int*>(value) = nativeSector;
        else qFatal("Unexpected fixture ioctl");
        return 0;
    }
    return __real_ioctl(fd, request, value);
}

enum Fault { NoFault, PauseCopy, PauseSync, PauseVerify, WriteError, SyncError, CorruptVerify };
static std::atomic<int> faultKind{NoFault}, writeCalls{0}, syncCalls{0};
static std::atomic<uint64_t> actualWritten{0};
static QSemaphore pausedIo, resumeIo;

#if defined(__GLIBC__) && defined(__USE_FILE_OFFSET64)
#define WrappedRead __wrap_pread64
#define RealRead __real_pread64
#define WrappedWrite __wrap_pwrite64
#define RealWrite __real_pwrite64
#else
#define WrappedRead __wrap_pread
#define RealRead __real_pread
#define WrappedWrite __wrap_pwrite
#define RealWrite __real_pwrite
#endif
extern "C" ssize_t RealRead(int, void*, size_t, off_t);
extern "C" ssize_t RealWrite(int, const void*, size_t, off_t);
extern "C" int __real_fsync(int);

static bool fixtureTarget(int fd)
{
    struct stat info;
    return RealStat(fd, &info) == 0 && info.st_ino == fixtureInode && info.st_dev == fixtureDevice;
}

static void pauseIo()
{
    pausedIo.release();
    if (!resumeIo.tryAcquire(1, 5000)) qFatal("Test did not release paused worker I/O");
}

extern "C" ssize_t WrappedWrite(int fd, const void* data, size_t size, off_t offset)
{
    if (!fixtureTarget(fd)) return RealWrite(fd, data, size, offset);
    const int call = ++writeCalls;
    if (faultKind == PauseCopy && call == 2) pauseIo();
    if (faultKind == WriteError)
    {
        if (call > 1) { errno = ENOSPC; return -1; }
        size = qMin(size, size_t(4096));
    }
    const ssize_t written = RealWrite(fd, data, size, offset);
    if (written > 0) actualWritten += uint64_t(written);
    return written;
}

extern "C" ssize_t WrappedRead(int fd, void* data, size_t size, off_t offset)
{
    const bool target = fixtureTarget(fd);
    if (target && faultKind == PauseVerify && offset == 0) pauseIo();
    const ssize_t read = RealRead(fd, data, size, offset);
    if (target && faultKind == CorruptVerify && read > 0) static_cast<char*>(data)[0] ^= 1;
    return read;
}

extern "C" int __wrap_fsync(int fd)
{
    if (!fixtureTarget(fd)) return __real_fsync(fd);
    ++syncCalls;
    if (faultKind == PauseSync) pauseIo();
    if (faultKind == SyncError) { errno = EIO; return -1; }
    return __real_fsync(fd);
}

namespace
{
const QString service = QStringLiteral("org.freedesktop.UDisks2");
const QString root = QStringLiteral("/org/freedesktop/UDisks2");
const QString controlInterface = QStringLiteral("org.rufus.TestUDisks");
const QString blockInterface = QStringLiteral("org.freedesktop.UDisks2.Block");
const QString driveInterface = QStringLiteral("org.freedesktop.UDisks2.Drive");
const QString fsInterface = QStringLiteral("org.freedesktop.UDisks2.Filesystem");
const QDBusObjectPath disk(root + QStringLiteral("/block_devices/test_a"));
const QDBusObjectPath partition(root + QStringLiteral("/block_devices/test_a1"));
const QDBusObjectPath drive(root + QStringLiteral("/drives/test_a"));
using State = rufus::WriteState;

void writeFile(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) qFatal("Fixture write failed");
}

QByteArray readFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) qFatal("Fixture read failed");
    return file.readAll();
}

void stopProcess(QProcess& process)
{
    if (process.state() == QProcess::NotRunning) return;
    process.terminate();
    if (!process.waitForFinished(2000)) { process.kill(); process.waitForFinished(2000); }
}

}

class TestRawWriteWidgets : public QObject
{
    Q_OBJECT
    QTemporaryDir directory_;
    QProcess daemon_, fixture_;
    std::unique_ptr<QDBusConnection> connection_;
    std::unique_ptr<rufus::SelectionController> controller_;
    std::unique_ptr<rufus::MainWindow> window_;
    QPushButton* start_ = nullptr;
    QPushButton* close_ = nullptr;
    QPushButton* select_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QList<int> phases_;
    bool guiDelivery_ = true;

    rufus::RawWriteController& writer() { return controller_->writer(); }
    State state() const { return controller_->writer().status().state; }
    rufus::UDisksObjects objects_;
    rufus::Device device_;
    QString sourcePath_, targetPath_;
    QByteArray sourceBytes_;
    QByteArray expectedTarget_;

    QDBusMessage control(const QString& method, const QList<QVariant>& args = {})
    {
        QDBusMessage request = QDBusMessage::createMethodCall(controlInterface, root, controlInterface, method);
        request.setArguments(args);
        const QDBusMessage reply = connection_->call(request, QDBus::Block, 2000);
        if (reply.type() != QDBusMessage::ReplyMessage) QTest::qFail(qPrintable(reply.errorMessage()), __FILE__, __LINE__);
        return reply;
    }

    int stat(const QString& name)
    {
        return qdbus_cast<QVariantMap>(control(QStringLiteral("Stats")).arguments().at(0)).value(name).toInt();
    }

    void setObjects()
    {
        control(QStringLiteral("SetObjects"), {QVariant::fromValue(objects_)});
    }

    void startPlan()
    {
        const int opens = stat(QStringLiteral("opens"));
        QVERIFY(start_->isEnabled());
        QTest::mouseClick(start_, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(state(), State::AwaitingConfirmation, 2000);
        QCOMPARE(stat(QStringLiteral("opens")), opens);
    }

    void approvePlan()
    {
        QMessageBox* dialog = window_->findChild<QMessageBox*>(QStringLiteral("writeConfirmation"));
        QVERIFY(dialog && dialog->isVisible());
        QTest::mouseClick(dialog->button(QMessageBox::Ok), Qt::LeftButton);
    }

    void selectFixture()
    {
        QTest::mouseClick(select_, Qt::LeftButton);
        auto* dialog = window_->findChild<QFileDialog*>(QStringLiteral("imageDialog"));
        QVERIFY(dialog && dialog->isVisible());
        dialog->setDirectory(directory_.path());
        dialog->selectFile(sourcePath_);
        auto* buttons = dialog->findChild<QDialogButtonBox*>();
        QVERIFY(buttons);
        QTRY_VERIFY_WITH_TIMEOUT(buttons->button(QDialogButtonBox::Open)->isEnabled(), 1000);
        QTest::mouseClick(buttons->button(QDialogButtonBox::Open), Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(controller_->selection().rawImage, 1000);
        auto* devices = window_->findChild<QComboBox*>(QStringLiteral("deviceCombo"));
        QVERIFY(devices);
        devices->setFocus();
        QTest::keyClick(devices, Qt::Key_Home);
        QCOMPARE(controller_->selection().deviceId, device_.id);
    }

    void observeState()
    {
        guiDelivery_ = guiDelivery_ && QThread::currentThread() == window_->thread();
        const int phase = writer().status().outcome.io.phase;
        if (phases_.isEmpty() || phases_.last() != phase) phases_.append(phase);
    }

private slots:
    void initTestCase()
    {
        QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
        QVERIFY(directory_.isValid());
        qDBusRegisterMetaType<rufus::UDisksInterfaces>();
        qDBusRegisterMetaType<rufus::UDisksObjects>();
        qDBusRegisterMetaType<QList<QByteArray>>();
    }

    void init()
    {
        while (resumeIo.tryAcquire()) {}
        while (pausedIo.tryAcquire()) {}
        faultKind = NoFault;
        writeCalls = 0; actualWritten = 0; syncCalls = 0;
        emulateBlock = true;
        nativeNumber = 2048; nativeCapacity = 12 * 1024 * 1024;
        nativeReadOnly = nativeError = 0; nativeSector = 512;
        sourcePath_ = directory_.filePath(QStringLiteral("source.img"));
        targetPath_ = directory_.filePath(QStringLiteral("target.bin"));
        sourceBytes_ = QByteArray(8 * 1024 * 1024 + 512, '\x17');
        sourceBytes_[510] = '\x55'; sourceBytes_[511] = '\xaa';
        writeFile(sourcePath_, sourceBytes_);
        expectedTarget_ = QByteArray(qsizetype(nativeCapacity), '\x5a');
        writeFile(targetPath_, expectedTarget_);
        struct stat info;
        QVERIFY(::stat(QFile::encodeName(targetPath_).constData(), &info) == 0);
        fixtureInode = info.st_ino; fixtureDevice = info.st_dev;
        daemon_.start(QStringLiteral(RUFUS_DBUS_DAEMON), {QStringLiteral("--session"),
            QStringLiteral("--nofork"), QStringLiteral("--print-address=1"),
            QStringLiteral("--address=unix:tmpdir=%1").arg(directory_.path())});
        QVERIFY(daemon_.waitForStarted(2000));
        QVERIFY(daemon_.waitForReadyRead(2000));
        const QString address = QString::fromUtf8(daemon_.readLine()).trimmed();
        QVERIFY(address.startsWith(QStringLiteral("unix:")));
        connection_ = std::make_unique<QDBusConnection>(QDBusConnection::connectToBus(address, QStringLiteral("raw-widgets-test")));
        QVERIFY(connection_->isConnected());
        fixture_.start(QCoreApplication::applicationDirPath() + QStringLiteral("/udisks_fixture"), {address});
        QVERIFY(fixture_.waitForStarted(2000));
        QVERIFY2(fixture_.waitForReadyRead(2000), fixture_.readAllStandardError().constData());
        QCOMPARE(fixture_.readLine(), QByteArray("ready\n"));
        objects_.clear();
        objects_[disk][blockInterface] = {
            {QStringLiteral("Drive"), QVariant::fromValue(drive)},
            {QStringLiteral("Device"), QByteArray("/dev/test-a\0", 12)},
            {QStringLiteral("Size"), QVariant::fromValue(quint64(nativeCapacity))},
            {QStringLiteral("DeviceNumber"), QVariant::fromValue(quint64(2048))},
            {QStringLiteral("ReadOnly"), false}, {QStringLiteral("HintSystem"), false},
            {QStringLiteral("HintIgnore"), false}, {QStringLiteral("Id"), QStringLiteral("fixture-id")}
        };
        objects_[drive][driveInterface] = {
            {QStringLiteral("ConnectionBus"), QStringLiteral("usb")},
            {QStringLiteral("Removable"), true}, {QStringLiteral("MediaRemovable"), true},
            {QStringLiteral("MediaAvailable"), true}, {QStringLiteral("Optical"), false},
            {QStringLiteral("Serial"), QStringLiteral("fixture-serial")},
            {QStringLiteral("TimeDetected"), QVariant::fromValue(quint64(12345))},
            {QStringLiteral("TimeMediaDetected"), QVariant::fromValue(quint64(12346))}
        };
        setObjects();
        control(QStringLiteral("OpenFile"), {targetPath_});
        QVERIFY(rufus::DescribeUDisksDevice(objects_, disk.path(), device_));
        device_.serviceOwner = connection_->interface()->serviceOwner(service).value();
        device_.id = QStringLiteral("raw-device");
        controller_ = std::make_unique<rufus::SelectionController>();
        controller_->setDiscovery({device_}, {});
        writer().setConnection(*connection_);
        window_ = std::make_unique<rufus::MainWindow>(*controller_);
        start_ = window_->findChild<QPushButton*>(QStringLiteral("startButton"));
        close_ = window_->findChild<QPushButton*>(QStringLiteral("closeButton"));
        select_ = window_->findChild<QPushButton*>(QStringLiteral("selectImageButton"));
        progress_ = window_->findChild<QProgressBar*>(QStringLiteral("progressBar"));
        QVERIFY(start_ && close_ && select_ && progress_);
        phases_.clear(); guiDelivery_ = true;
        connect(&writer(), &rufus::RawWriteController::changed, this, &TestRawWriteWidgets::observeState);
        window_->show();
        selectFixture();
    }

    void cleanup()
    {
        resumeIo.release();
        window_.reset(); controller_.reset();
        const uint64_t written = actualWritten.load();
        QVERIFY(written <= uint64_t(sourceBytes_.size()));
        expectedTarget_.replace(0, qsizetype(written), sourceBytes_.first(qsizetype(written)));
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        const bool connected = connection_ && connection_->isConnected() && fixture_.state() == QProcess::Running;
        const int unexpected = connected ? stat(QStringLiteral("unexpected")) : 0;
        if (connected) control(QStringLiteral("Quit"));
        if (fixture_.state() != QProcess::NotRunning) fixture_.waitForFinished(2000);
        connection_.reset();
        QDBusConnection::disconnectFromBus(QStringLiteral("raw-widgets-test"));
        stopProcess(fixture_); stopProcess(daemon_);
        QCOMPARE(unexpected, 0);
        QCOMPARE(readFile(targetPath_), expectedTarget_);
    }

    void applicationRemainsGated()
    {
        rufus::SelectionController gated;
        gated.setDiscovery({device_}, {});
        gated.selectImage(sourcePath_);
        gated.selectDevice(device_.id);
        rufus::MainWindow preview(gated);
        QVERIFY(gated.selection().rawImage);
        QVERIFY(!gated.canStartWrite());
        QVERIFY(!preview.findChild<QPushButton*>(QStringLiteral("startButton"))->isEnabled());
        gated.startWrite();
        QCOMPARE(stat(QStringLiteral("opens")), 0);
        QCOMPARE(actualWritten.load(), uint64_t(0));
    }

    void confirmedImageIsWrittenAndVerified()
    {
        startPlan();
        auto* dialog = window_->findChild<QMessageBox*>(QStringLiteral("writeConfirmation"));
        QVERIFY(dialog);
        QVERIFY(dialog->text().contains(QStringLiteral("ALL DATA ON DEVICE")));
        QVERIFY(dialog->text().contains(device_.path));
        QVERIFY(dialog->informativeText().contains(sourcePath_));
        QCOMPARE(dialog->textFormat(), Qt::PlainText);
        QCOMPARE(dialog->defaultButton(), dialog->button(QMessageBox::Cancel));
        const QString screenshot = qEnvironmentVariable("RUFUS_TEST_WRITE_SCREENSHOT");
        if (!screenshot.isEmpty()) QVERIFY(dialog->grab().save(screenshot));
        QVERIFY(!select_->isEnabled());
        QVERIFY(!start_->isEnabled());
        approvePlan();
        QTRY_COMPARE_WITH_TIMEOUT(state(), State::Complete, 4000);
        QVERIFY(!writer().isActive());
        QVERIFY(!writer().isWriting());
        QVERIFY(!writer().plan());
        QCOMPARE(writer().status().outcome.error, 0);
        QCOMPARE(writer().status().outcome.io.bytes_written, uint64_t(sourceBytes_.size()));
        QCOMPARE(writer().status().outcome.io.bytes_verified, uint64_t(sourceBytes_.size()));
        QCOMPARE(actualWritten.load(), uint64_t(sourceBytes_.size()));
        QCOMPARE(syncCalls.load(), 1);
        QCOMPARE(progress_->value(), 100);
        QCOMPARE(progress_->format(), QStringLiteral("READY"));
        QVERIFY(phases_.contains(RAW_WRITE_COPY));
        QVERIFY(phases_.contains(RAW_WRITE_SYNC));
        QVERIFY(phases_.contains(RAW_WRITE_VERIFY));
        QVERIFY(guiDelivery_);
        QVERIFY(start_->isEnabled());
        QVERIFY(select_->isEnabled());
        QCOMPARE(close_->text(), QStringLiteral("CLOSE"));
        QCOMPARE(readFile(sourcePath_), sourceBytes_);
        const QByteArray target = readFile(targetPath_);
        QCOMPARE(target.first(sourceBytes_.size()), sourceBytes_);
        QCOMPARE(target.mid(sourceBytes_.size()), expectedTarget_.mid(sourceBytes_.size()));
    }

    void declinedConfirmationCanBeRetried()
    {
        startPlan();
        auto* dialog = window_->findChild<QMessageBox*>(QStringLiteral("writeConfirmation"));
        QTest::keyClick(dialog, Qt::Key_Escape);
        QTRY_COMPARE_WITH_TIMEOUT(state(), State::Cancelled, 1000);
        QCOMPARE(stat(QStringLiteral("opens")), 0);
        QCOMPARE(actualWritten.load(), uint64_t(0));
        startPlan();
        approvePlan();
        QTRY_COMPARE_WITH_TIMEOUT(state(), State::Complete, 4000);
        QCOMPARE(stat(QStringLiteral("opens")), 1);
    }

    void changedWhileConfirming_data()
    {
        QTest::addColumn<QString>("kind");
        for (const char* kind : {"device", "source", "mount"})
            QTest::newRow(kind) << QString::fromLatin1(kind);
    }

    void changedWhileConfirming()
    {
        QFETCH(QString, kind);
        startPlan();
        if (kind == QStringLiteral("device"))
        {
            rufus::Device changed = device_;
            changed.identity.append(QStringLiteral("replacement"));
            controller_->setDiscovery({changed}, {});
        }
        else
        {
            if (kind == QStringLiteral("source")) writeFile(sourcePath_, sourceBytes_);
            else
            {
                objects_[disk][fsInterface][QStringLiteral("MountPoints")] =
                    QVariant::fromValue(QList<QByteArray>{QByteArray("/test/mount\0", 12)});
                setObjects();
            }
            approvePlan();
        }
        QTRY_COMPARE_WITH_TIMEOUT(state(), State::Failed, 2000);
        QCOMPARE(stat(QStringLiteral("opens")), 0);
        QCOMPARE(actualWritten.load(), uint64_t(0));
        const auto dialogs = window_->findChildren<QMessageBox*>(QStringLiteral("writeConfirmation"));
        for (QMessageBox* dialog : dialogs) QVERIFY(!dialog->isVisible());
        writer().confirm(); // An obsolete confirmation cannot start a job.
        QCOMPARE(state(), State::Failed);
    }

    void authorizationFailure_data()
    {
        QTest::addColumn<QString>("mode");
        for (const char* mode : {"deny", "dismiss", "unsupported"})
            QTest::newRow(mode) << QString::fromLatin1(mode);
    }

    void authorizationFailure()
    {
        QFETCH(QString, mode);
        control(QStringLiteral("OpenMode"), {mode});
        startPlan(); approvePlan();
        QTRY_COMPARE_WITH_TIMEOUT(state(), mode == QStringLiteral("dismiss") ? State::Cancelled : State::Failed, 2000);
        QVERIFY(select_->isEnabled());
        QCOMPARE(actualWritten.load(), uint64_t(0));
        QVERIFY(progress_->value() < 100);
        QCOMPARE(stat(QStringLiteral("opens")), 1);
    }

    void cancelledAuthorizationIgnoresLateDescriptor()
    {
        control(QStringLiteral("HoldOpen"), {true});
        startPlan(); approvePlan();
        QTRY_COMPARE_WITH_TIMEOUT(stat(QStringLiteral("openHeld")), 1, 2000);
        QTest::mouseClick(close_, Qt::LeftButton);
        QCOMPARE(state(), State::Cancelled);
        QVERIFY(window_->isVisible());
        control(QStringLiteral("ReplyOpen"));
        QCOMPARE(stat(QStringLiteral("openHeld")), 0);
        QTest::qWait(30);
        QCOMPARE(state(), State::Cancelled);
        QCOMPARE(actualWritten.load(), uint64_t(0));
        QVERIFY(select_->isEnabled());
    }

    void cancelDuringIo_data()
    {
        QTest::addColumn<int>("fault");
        QTest::newRow("copy") << int(PauseCopy);
        QTest::newRow("flush") << int(PauseSync);
        QTest::newRow("verify") << int(PauseVerify);
    }

    void cancelDuringIo()
    {
        QFETCH(int, fault);
        faultKind = fault;
        startPlan(); approvePlan();
        QTRY_COMPARE_WITH_TIMEOUT(pausedIo.available(), 1, 2000);
        QVERIFY(writer().isWriting());
        QVERIFY(!select_->isEnabled());
        QVERIFY(!start_->isEnabled());
        const QString source = controller_->selection().imagePath;
        controller_->selectImage(QStringLiteral("/absent/replacement.img"));
        controller_->selectDevice(QString());
        QCOMPARE(controller_->selection().imagePath, source);
        QCOMPARE(controller_->selection().deviceId, device_.id);
        controller_->startWrite(); writer().confirm(); // No overlapping write.
        QCOMPARE(stat(QStringLiteral("opens")), 1);
        // The log remains usable while worker I/O is blocked.
        QTest::mouseClick(window_->findChild<QToolButton*>(QStringLiteral("logButton")), Qt::LeftButton);
        auto* log = window_->findChild<QDialog*>(QStringLiteral("logWindow"));
        QVERIFY(log && log->isVisible());
        log->close();
        QTest::mouseClick(close_, Qt::LeftButton);
        auto* dialog = window_->findChild<QMessageBox*>(QStringLiteral("cancelConfirmation"));
        QVERIFY(dialog && dialog->isVisible());
        QVERIFY(dialog->text().contains(QStringLiteral("UNUSABLE")));
        QCOMPARE(dialog->windowTitle(), QStringLiteral("Rufus - Cancellation"));
        QCOMPARE(dialog->defaultButton(), dialog->button(QMessageBox::No));
        QTest::mouseClick(dialog->button(QMessageBox::Yes), Qt::LeftButton);
        QCOMPARE(state(), State::Cancelling);
        QVERIFY(!close_->isEnabled());
        QVERIFY(window_->isVisible());
        QVERIFY(progress_->value() < 100);
        resumeIo.release();
        QTRY_COMPARE_WITH_TIMEOUT(state(), State::Cancelled, 3000);
        QCOMPARE(syncCalls.load(), 1); // A cancellation still flushes copied bytes.
        QCOMPARE(writer().status().outcome.io.bytes_written, actualWritten.load());
        QVERIFY(actualWritten.load() > 0);
        QVERIFY(progress_->value() < 100);
        QCOMPARE(progress_->format(), QStringLiteral("CANCELLED"));
        QVERIFY(guiDelivery_);
    }

    void decliningCancellationLetsWriteFinish()
    {
        faultKind = PauseCopy;
        startPlan(); approvePlan();
        QTRY_COMPARE_WITH_TIMEOUT(pausedIo.available(), 1, 2000);
        window_->close();
        auto* dialog = window_->findChild<QMessageBox*>(QStringLiteral("cancelConfirmation"));
        QVERIFY(dialog && dialog->isVisible());
        QTest::mouseClick(dialog->button(QMessageBox::No), Qt::LeftButton);
        QCOMPARE(state(), State::Writing);
        QVERIFY(window_->isVisible());
        resumeIo.release();
        QTRY_COMPARE_WITH_TIMEOUT(state(), State::Complete, 4000);
    }

    void cancelledWorkerCanBeRetried()
    {
        faultKind = PauseCopy;
        startPlan(); approvePlan();
        QTRY_COMPARE_WITH_TIMEOUT(pausedIo.available(), 1, 2000);
        writer().cancel();
        resumeIo.release();
        QTRY_COMPARE_WITH_TIMEOUT(state(), State::Cancelled, 3000);
        QVERIFY(actualWritten.load() > 0);
        faultKind = NoFault;
        actualWritten = 0; syncCalls = 0;
        startPlan(); approvePlan();
        QTRY_COMPARE_WITH_TIMEOUT(state(), State::Complete, 4000);
        QCOMPARE(actualWritten.load(), uint64_t(sourceBytes_.size()));
        QCOMPARE(stat(QStringLiteral("opens")), 2);
        QCOMPARE(syncCalls.load(), 1);
    }

    void deviceRemovalDuringWriteFails()
    {
        faultKind = PauseCopy;
        startPlan(); approvePlan();
        QTRY_COMPARE_WITH_TIMEOUT(pausedIo.available(), 1, 2000);
        controller_->setDiscovery({}, {});
        QCOMPARE(state(), State::Cancelling);
        resumeIo.release();
        QTRY_COMPARE_WITH_TIMEOUT(state(), State::Failed, 3000);
        QVERIFY(writer().statusText().contains(QStringLiteral("removed")));
        QCOMPARE(syncCalls.load(), 1);
        QVERIFY(!start_->isEnabled());
        QVERIFY(progress_->value() < 100);
    }

    void ioFailure_data()
    {
        QTest::addColumn<int>("fault");
        QTest::newRow("partial-write") << int(WriteError);
        QTest::newRow("flush") << int(SyncError);
        QTest::newRow("corrupt-readback") << int(CorruptVerify);
    }

    void ioFailure()
    {
        QFETCH(int, fault);
        faultKind = fault;
        startPlan(); approvePlan();
        QTRY_COMPARE_WITH_TIMEOUT(state(), State::Failed, 4000);
        QVERIFY(!writer().plan());
        QVERIFY(!writer().status().outcome.problem.isEmpty());
        QCOMPARE(writer().status().outcome.io.bytes_written, actualWritten.load());
        QCOMPARE(syncCalls.load(), 1);
        QVERIFY(writer().status().outcome.io.bytes_verified < uint64_t(sourceBytes_.size()));
        QCOMPARE(progress_->format(), QStringLiteral("FAILED"));
        QVERIFY(progress_->value() < 100);
        QVERIFY(select_->isEnabled());
        if (fault == WriteError) QCOMPARE(actualWritten.load(), uint64_t(4096));
        if (fault == SyncError) QVERIFY(writer().statusText().contains(QStringLiteral("Flushing failed")));
    }

    void teardownJoinsWorker()
    {
        faultKind = PauseCopy;
        startPlan(); approvePlan();
        QTRY_COMPARE_WITH_TIMEOUT(pausedIo.available(), 1, 2000);
        writer().cancel();
        resumeIo.release();
        window_.reset(); controller_.reset();
        QCOMPARE(syncCalls.load(), 1);
        QVERIFY(actualWritten.load() > 0);
    }
};

QTEST_MAIN(TestRawWriteWidgets)
#include "test_raw_write_widgets.moc"
