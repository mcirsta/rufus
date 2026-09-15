// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/udisks_target.h"
#include "platform/raw_write.h"

#include <QDBusArgument>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QFile>
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

extern "C" int IsIsoScanCancelled(void) { return 0; }
extern "C" int IsRufusJobCancelled(void) { return 0; }
extern "C" void _UpdateProgressWithInfo(int, int, uint64_t, uint64_t, int) {}

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
using State = rufus::UDisksTarget::State;

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

int descriptorCount()
{
    DIR* directory = opendir("/proc/self/fd");
    if (!directory) qFatal("Cannot count descriptors");
    int count = 0;
    while (readdir(directory)) ++count;
    closedir(directory);
    return count;
}
}

class TestUDisksTarget : public QObject
{
    Q_OBJECT
    QTemporaryDir directory_;
    QProcess daemon_, fixture_;
    std::unique_ptr<QDBusConnection> connection_;
    std::unique_ptr<rufus::RawWritePlan> plan_;
    std::unique_ptr<rufus::UDisksTarget> access_;
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

    void makeAccess(int timeout = 5000)
    {
        access_ = std::make_unique<rufus::UDisksTarget>(*connection_, *plan_, timeout);
        access_->check();
    }

private slots:
    void initTestCase()
    {
        QVERIFY(directory_.isValid());
        qDBusRegisterMetaType<rufus::UDisksInterfaces>();
        qDBusRegisterMetaType<rufus::UDisksObjects>();
        qDBusRegisterMetaType<QList<QByteArray>>();
    }

    void init()
    {
        emulateBlock = true;
        nativeNumber = 2048; nativeCapacity = 65536;
        nativeReadOnly = nativeError = 0; nativeSector = 512;
        sourcePath_ = directory_.filePath(QStringLiteral("source.img"));
        targetPath_ = directory_.filePath(QStringLiteral("target.bin"));
        sourceBytes_ = QByteArray(4096, '\x17');
        sourceBytes_[510] = '\x55'; sourceBytes_[511] = '\xaa';
        writeFile(sourcePath_, sourceBytes_);
        expectedTarget_ = QByteArray(65536, '\x5a');
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
        connection_ = std::make_unique<QDBusConnection>(QDBusConnection::connectToBus(address, QStringLiteral("raw-private-test")));
        QVERIFY(connection_->isConnected());
        fixture_.start(QCoreApplication::applicationDirPath() + QStringLiteral("/udisks_fixture"), {address});
        QVERIFY(fixture_.waitForStarted(2000));
        QVERIFY2(fixture_.waitForReadyRead(2000), fixture_.readAllStandardError().constData());
        QCOMPARE(fixture_.readLine(), QByteArray("ready\n"));
        objects_.clear();
        objects_[disk][blockInterface] = {
            {QStringLiteral("Drive"), QVariant::fromValue(drive)},
            {QStringLiteral("Device"), QByteArray("/dev/test-a\0", 12)},
            {QStringLiteral("Size"), QVariant::fromValue(quint64(65536))},
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
        plan_ = std::make_unique<rufus::RawWritePlan>();
        QVERIFY2(plan_->prepare(sourcePath_, device_).isEmpty(), "Fixture plan failed");
    }

    void cleanup()
    {
        access_.reset(); plan_.reset();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        const bool connected = connection_ && connection_->isConnected() && fixture_.state() == QProcess::Running;
        const int unexpected = connected ? stat(QStringLiteral("unexpected")) : 0;
        if (connected) control(QStringLiteral("Quit"));
        if (fixture_.state() != QProcess::NotRunning) fixture_.waitForFinished(2000);
        connection_.reset();
        QDBusConnection::disconnectFromBus(QStringLiteral("raw-private-test"));
        stopProcess(fixture_); stopProcess(daemon_);
        QCOMPARE(unexpected, 0);
        QCOMPARE(readFile(targetPath_), expectedTarget_);
    }

    void acquisitionAndOwnership()
    {
        const int before = descriptorCount();
        makeAccess();
        QTRY_VERIFY2_WITH_TIMEOUT(access_->state() == State::Prepared, qPrintable(access_->problem()), 2000);
        QCOMPARE(stat(QStringLiteral("opens")), 0); // Checking cannot prompt.
        QVERIFY(!access_->takeDescriptor().isValid());
        access_->authorize();
        QTRY_VERIFY2_WITH_TIMEOUT(access_->state() == State::Ready, qPrintable(access_->problem()), 2000);
        QVERIFY2(access_->problem().isEmpty(), qPrintable(access_->problem()));
        QDBusUnixFileDescriptor descriptor = access_->takeDescriptor();
        QVERIFY(descriptor.isValid());
        QVERIFY(!access_->takeDescriptor().isValid());
        QCOMPARE(stat(QStringLiteral("opens")), 1);
        QCOMPARE(stat(QStringLiteral("calls")), 3); // prepare, pre-auth, post-auth snapshots
        QVERIFY(fcntl(descriptor.fileDescriptor(), F_GETFD) & FD_CLOEXEC);
        access_.reset();
        QVERIFY(fcntl(descriptor.fileDescriptor(), F_GETFD) >= 0);
        QVERIFY(plan_->checkSource().isEmpty());
        QCOMPARE(readFile(sourcePath_), sourceBytes_);
        descriptor = {};
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QTRY_COMPARE_WITH_TIMEOUT(descriptorCount(), before, 2000);
    }

    void invalidInput_data()
    {
        QTest::addColumn<QString>("kind");
        for (const char* name : {"empty", "marker", "alignment", "suffix", "capacity", "synthetic"})
            QTest::newRow(name) << QString::fromLatin1(name);
    }

    void authorizedWriteAndVerify()
    {
        // An ordinary unmounted filesystem must remain eligible.
        objects_[partition][blockInterface] = {
            {QStringLiteral("Drive"), QVariant::fromValue(drive)},
            {QStringLiteral("DeviceNumber"), QVariant::fromValue(quint64(2049))}
        };
        objects_[partition][fsInterface][QStringLiteral("MountPoints")] = QVariant::fromValue(QList<QByteArray>{});
        setObjects();
        makeAccess();
        QTRY_VERIFY2_WITH_TIMEOUT(access_->state() == State::Prepared, qPrintable(access_->problem()), 2000);
        access_->authorize();
        QTRY_VERIFY2_WITH_TIMEOUT(access_->state() == State::Ready, qPrintable(access_->problem()), 2000);
        QDBusUnixFileDescriptor descriptor = access_->takeDescriptor();
        QVERIFY(descriptor.isValid());
        RAW_WRITE_RESULT result;
        QCOMPARE(WriteRawImageToDevice(&plan_->source(), QFile::encodeName(plan_->sourcePath()).constData(),
            descriptor.fileDescriptor(), plan_->target().number, plan_->target().size, &result), 0);
        QCOMPARE(result.phase, RAW_WRITE_DONE);
        QCOMPARE(result.bytes_written, uint64_t(sourceBytes_.size()));
        QCOMPARE(result.bytes_verified, result.bytes_written);
        expectedTarget_.replace(0, sourceBytes_.size(), sourceBytes_);
        QCOMPARE(readFile(targetPath_), expectedTarget_);
        QCOMPARE(readFile(sourcePath_), sourceBytes_);
    }

    void changedAfterAcquisition()
    {
        makeAccess();
        QTRY_VERIFY2_WITH_TIMEOUT(access_->state() == State::Prepared, qPrintable(access_->problem()), 2000);
        access_->authorize();
        QTRY_VERIFY2_WITH_TIMEOUT(access_->state() == State::Ready, qPrintable(access_->problem()), 2000);
        QDBusUnixFileDescriptor descriptor = access_->takeDescriptor();
        nativeNumber++;
        RAW_WRITE_RESULT result;
        QCOMPARE(WriteRawImageToDevice(&plan_->source(), QFile::encodeName(plan_->sourcePath()).constData(),
            descriptor.fileDescriptor(), plan_->target().number, plan_->target().size, &result), ESTALE);
        QCOMPARE(result.bytes_written, uint64_t(0));
        QCOMPARE(result.phase, RAW_WRITE_PREPARE);
    }

    void invalidInput()
    {
        QFETCH(QString, kind);
        QString path = sourcePath_;
        if (kind == QStringLiteral("empty")) writeFile(path, {});
        if (kind == QStringLiteral("marker")) writeFile(path, QByteArray(4096, '\0'));
        if (kind == QStringLiteral("alignment")) writeFile(path, sourceBytes_ + 'x');
        if (kind == QStringLiteral("suffix")) { path += QStringLiteral(".vhd"); writeFile(path, sourceBytes_); }
        if (kind == QStringLiteral("capacity")) device_.size = 512;
        if (kind == QStringLiteral("synthetic")) device_.identity.clear();
        QVERIFY(!plan_->prepare(path, device_).isEmpty());
        QCOMPARE(plan_->source().fd, -1);
        QCOMPARE(stat(QStringLiteral("opens")), 0);
    }

    void usedTarget_data()
    {
        QTest::addColumn<QString>("kind");
        for (const char* name : {"mounted", "mount-unknown", "mount-malformed", "source", "swap", "encrypted", "raid"})
            QTest::newRow(name) << QString::fromLatin1(name);
    }

    void usedTarget()
    {
        QFETCH(QString, kind);
        objects_[partition][blockInterface] = {
            {QStringLiteral("Drive"), QVariant::fromValue(drive)},
            {QStringLiteral("DeviceNumber"), QVariant::fromValue(quint64(2049))}
        };
        if (kind == QStringLiteral("mounted"))
            objects_[partition][fsInterface][QStringLiteral("MountPoints")] = QVariant::fromValue(QList<QByteArray>{QByteArray("/media/test\0", 12)});
        if (kind == QStringLiteral("mount-unknown")) objects_[partition][fsInterface] = {};
        if (kind == QStringLiteral("mount-malformed")) objects_[partition][fsInterface][QStringLiteral("MountPoints")] = 7;
        if (kind == QStringLiteral("source")) objects_[partition][blockInterface][QStringLiteral("DeviceNumber")] = QVariant::fromValue(quint64(plan_->source().info.st_dev));
        if (kind == QStringLiteral("swap")) objects_[partition][QStringLiteral("org.freedesktop.UDisks2.Swapspace")] = {};
        if (kind == QStringLiteral("encrypted")) objects_[partition][QStringLiteral("org.freedesktop.UDisks2.Encrypted")] = {};
        if (kind == QStringLiteral("raid")) objects_[partition][blockInterface][QStringLiteral("MDRaidMember")] = QVariant::fromValue(QDBusObjectPath(root + QStringLiteral("/mdraid/test")));
        setObjects();
        makeAccess();
        QTRY_COMPARE_WITH_TIMEOUT(access_->state(), State::Failed, 2000);
        QVERIFY(!access_->problem().isEmpty());
        QCOMPARE(stat(QStringLiteral("opens")), 0);
    }

    void openFailure_data()
    {
        QTest::addColumn<QString>("mode");
        for (const char* name : {"deny", "dismiss", "unsupported", "malformed", "readonly"})
            QTest::newRow(name) << QString::fromLatin1(name);
    }

    void openFailure()
    {
        QFETCH(QString, mode);
        control(QStringLiteral("OpenMode"), {mode});
        makeAccess();
        QTRY_VERIFY2_WITH_TIMEOUT(access_->state() == State::Prepared, qPrintable(access_->problem()), 2000);
        access_->authorize();
        QTRY_COMPARE_WITH_TIMEOUT(access_->state(), mode == QStringLiteral("dismiss") ? State::Cancelled : State::Failed, 2000);
        QVERIFY(!access_->takeDescriptor().isValid());
        QCOMPARE(stat(QStringLiteral("opens")), 1);
    }

    void invalidDescriptor_data()
    {
        QTest::addColumn<QString>("kind");
        for (const char* name : {"regular", "number", "capacity", "readonly", "sector", "ioctl"})
            QTest::newRow(name) << QString::fromLatin1(name);
    }

    void invalidDescriptor()
    {
        QFETCH(QString, kind);
        if (kind == QStringLiteral("regular")) emulateBlock = false;
        if (kind == QStringLiteral("number")) nativeNumber++;
        if (kind == QStringLiteral("capacity")) nativeCapacity--;
        if (kind == QStringLiteral("readonly")) nativeReadOnly = 1;
        if (kind == QStringLiteral("sector")) nativeSector = 8192;
        if (kind == QStringLiteral("ioctl")) nativeError = ENODEV;
        makeAccess();
        QTRY_VERIFY2_WITH_TIMEOUT(access_->state() == State::Prepared, qPrintable(access_->problem()), 2000);
        access_->authorize();
        QTRY_COMPARE_WITH_TIMEOUT(access_->state(), State::Failed, 2000);
        QVERIFY(!access_->takeDescriptor().isValid());
        QVERIFY(!access_->problem().isEmpty());
    }

    void changedBeforeAuthorization()
    {
        makeAccess();
        QTRY_VERIFY2_WITH_TIMEOUT(access_->state() == State::Prepared, qPrintable(access_->problem()), 2000);
        objects_[drive][driveInterface][QStringLiteral("Serial")] = QStringLiteral("replaced");
        setObjects();
        access_->authorize();
        QTRY_COMPARE_WITH_TIMEOUT(access_->state(), State::Failed, 2000);
        QCOMPARE(stat(QStringLiteral("opens")), 0);
    }

    void unavailableService()
    {
        control(QStringLiteral("ReleaseName"));
        makeAccess();
        QTRY_COMPARE_WITH_TIMEOUT(access_->state(), State::Failed, 2000);
        QVERIFY(!access_->takeDescriptor().isValid());
        QCOMPARE(stat(QStringLiteral("opens")), 0);
    }

    void disconnectedBus()
    {
        control(QStringLiteral("HoldOpen"), {true});
        makeAccess();
        QTRY_VERIFY2_WITH_TIMEOUT(access_->state() == State::Prepared, qPrintable(access_->problem()), 2000);
        access_->authorize();
        QTRY_COMPARE_WITH_TIMEOUT(stat(QStringLiteral("openHeld")), 1, 2000);
        stopProcess(daemon_);
        QTRY_COMPARE_WITH_TIMEOUT(access_->state(), State::Failed, 2000);
        QVERIFY(!access_->takeDescriptor().isValid());
    }

    void changeDuringAuthorization_data()
    {
        QTest::addColumn<QString>("kind");
        for (const char* name : {"device", "source", "mount", "remove", "service", "cancel", "timeout"})
            QTest::newRow(name) << QString::fromLatin1(name);
    }

    void changeDuringAuthorization()
    {
        QFETCH(QString, kind);
        const int before = descriptorCount();
        control(QStringLiteral("HoldOpen"), {true});
        makeAccess(kind == QStringLiteral("timeout") ? 100 : 5000);
        QTRY_VERIFY2_WITH_TIMEOUT(access_->state() == State::Prepared, qPrintable(access_->problem()), 2000);
        access_->authorize();
        QTRY_COMPARE_WITH_TIMEOUT(stat(QStringLiteral("openHeld")), 1, 2000);
        if (kind == QStringLiteral("device")) objects_[drive][driveInterface][QStringLiteral("TimeMediaDetected")] = QVariant::fromValue(quint64(99999));
        if (kind == QStringLiteral("source")) writeFile(sourcePath_, sourceBytes_);
        if (kind == QStringLiteral("mount")) objects_[disk][fsInterface][QStringLiteral("MountPoints")] = QVariant::fromValue(QList<QByteArray>{QByteArray("/media/test\0", 12)});
        if (kind == QStringLiteral("remove")) objects_.remove(disk);
        setObjects();
        if (kind == QStringLiteral("service")) control(QStringLiteral("ReleaseName"));
        if (kind == QStringLiteral("cancel")) access_->cancel();
        if (kind == QStringLiteral("timeout")) QTRY_COMPARE_WITH_TIMEOUT(access_->state(), State::Failed, 2000);
        control(QStringLiteral("ReplyOpen"));
        QTRY_COMPARE_WITH_TIMEOUT(access_->state(), kind == QStringLiteral("cancel") ? State::Cancelled : State::Failed, 2000);
        QVERIFY(!access_->takeDescriptor().isValid());
        QCOMPARE(stat(QStringLiteral("opens")), 1);
        // An ordinary bus round trip ensures the fixture sent the late reply.
        QCOMPARE(stat(QStringLiteral("openHeld")), 0);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(access_->state() != State::Ready);
        access_.reset();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QTRY_COMPARE_WITH_TIMEOUT(descriptorCount(), before, 2000);
    }
};

QTEST_GUILESS_MAIN(TestUDisksTarget)
#include "test_udisks_target.moc"
