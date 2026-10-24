// SPDX-License-Identifier: GPL-3.0-or-later
#include "winpe_setup.h"
#include "format_policy.h"
#include "native_disk_fixture.h"
#include "qt/main_window.h"
#include "compressed_fixture.h"
#include "ffu_fixture.h"
#include "qt/selection_controller.h"
#include "secure_boot_fixture.h"
#include "platform/raw_write.h"
#include "platform/exfat_format.h"
#include "qt/image_mode_dialog.h"
#include "qt/windows_user_dialog.h"
#include "qt/windows_image_dialog.h"
#include "windows_to_go_fixture.h"
#include "qt/windows_region_dialog.h"
#include "qt/windows_silent_dialog.h"
#include "qt/freedos_locale_dialog.h"
#include "unattend.h"
#include "../../res/grub2/grub2_version.h"

#include <QApplication>
#include <QCryptographicHash>
#include <QDBusArgument>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QFile>
#include <QRandomGenerator>
#include <QComboBox>
#include <QCheckBox>
#include <QSettings>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QFileDialog>
#include <QInputDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QSemaphore>
#include <QStyle>
#include <QStyleOptionComboBox>
#include <QToolButton>
#include <atomic>
#include <QProcess>
#include <QPointer>
#include <QSignalSpy>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTest>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtEndian>

#include <cerrno>
#include <cstdarg>
#include <dirent.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <memory>
#include <sys/ioctl.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <zlib.h>

static constexpr int fixtureTimeoutMs = 2000;
static constexpr int uiTimeoutMs = 1000;
static constexpr int writeTimeoutMs = 4000;
static constexpr int cancelTimeoutMs = 3000;
static constexpr int resumeTimeoutMs = 5000;
static const quint64 fixtureDiskNumber = quint64(makedev(240, 42));

// Only the private fixture's regular-file inode gets synthetic kernel facts.
// No block nodes are created or opened, and production has no test-mode branch.
static ino_t fixtureInode;
static dev_t fixtureDevice;
static bool emulateBlock;
static uint64_t nativeNumber, nativeCapacity;
static int nativeReadOnly, nativeSector, nativeError;
static int helperStarts;
static QPointer<QProcess> helperProcess;
static int completionBeeps, completionAlerts;
static QString helperMode;
static QByteArray expectedGrubCore;
static quint64 helperCopiedBytes = 519; // EFI header plus ordinary payload.
static quint64 diskSequence = 123;
static QString missingSyslinuxTool;
static std::atomic<int> syslinuxToolChecks{0};
static std::atomic<bool> missingExfatTool{false};
static std::atomic<bool> missingFatTool{false};
static std::atomic<bool> missingNtfsTool{false};
static std::atomic<bool> missingExtTool{false};

extern "C" int __real_access(const char*, int);
extern "C" int __wrap_access(const char* path, int mode)
{
    const QString program = QString::fromUtf8(path);
    const bool missing = (program == QStringLiteral(RUFUS_MKFS_EXFAT_PATH) && missingExfatTool) ||
        (program == QStringLiteral(RUFUS_MKFS_FAT_PATH) && missingFatTool) ||
        (program == QStringLiteral(RUFUS_MKNTFS_PATH) && missingNtfsTool) ||
        (program == QStringLiteral(RUFUS_MKE2FS_PATH) && missingExtTool);
    if (mode == X_OK && missing)
    {
        errno = ENOENT;
        return -1;
    }
    const bool nativeTool = program == QStringLiteral(RUFUS_NATIVE_SYSLINUX_PATH)
        || program == QStringLiteral(RUFUS_NATIVE_SYSLINUX4_PATH)
        || program == QStringLiteral(RUFUS_NATIVE_EXTLINUX_PATH)
        || program == QStringLiteral(RUFUS_MTOOLS_DIRECTORY "/mcopy")
        || program == QStringLiteral(RUFUS_MTOOLS_DIRECTORY "/mattrib");
    if (nativeTool && mode == X_OK)
    {
        ++syslinuxToolChecks;
        if (program.endsWith(missingSyslinuxTool) && !missingSyslinuxTool.isEmpty())
        {
            errno = ENOENT;
            return -1;
        }
        return 0; // This protocol-only test never runs these programs.
    }
    return __real_access(path, mode);
}

// Keep offscreen tests silent and verify that only terminal success alerts.
extern "C" void __wrap__ZN12QApplication4beepEv() { ++completionBeeps; }
extern "C" void __wrap__ZN12QApplication5alertEP7QWidgeti(QWidget*, int) { ++completionAlerts; }

// Only this test executable redirects the fixed pkexec launch to a child that
// exercises the protocol without elevation or target I/O.
extern "C" void __real__ZN8QProcess5startERK7QStringRK5QListIS0_E6QFlagsIN13QIODeviceBase12OpenModeFlagEE(
    QProcess*, const QString&, const QStringList&, QIODevice::OpenMode);
extern "C" void __wrap__ZN8QProcess5startERK7QStringRK5QListIS0_E6QFlagsIN13QIODeviceBase12OpenModeFlagEE(
    QProcess* process, const QString& program, const QStringList& arguments, QIODevice::OpenMode mode)
{
    if (program == QStringLiteral("/usr/bin/pkexec"))
    {
        const QString fixture = QCoreApplication::applicationDirPath() + QStringLiteral("/iso_helper_fixture");
        const QStringList expected = {QStringLiteral("--disable-internal-agent"), fixture, QStringLiteral("--device")};
        if (arguments != expected)
            qFatal("Unexpected authorization request in Widgets fixture");
        ++helperStarts;
        helperProcess = process;
        __real__ZN8QProcess5startERK7QStringRK5QListIS0_E6QFlagsIN13QIODeviceBase12OpenModeFlagEE(process, fixture, {helperMode, QString::number(helperCopiedBytes), QString::fromLatin1(expectedGrubCore.toHex())}, mode);
        return;
    }
    __real__ZN8QProcess5startERK7QStringRK5QListIS0_E6QFlagsIN13QIODeviceBase12OpenModeFlagEE(process, program, arguments, mode);
}

extern "C" quint64 __wrap__ZN5rufus16ReadDiskSequenceEy(quint64 device)
{
    if (device != nativeNumber)
        qFatal("Unexpected device in Widgets fixture");
    return diskSequence;
}


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
    const bool fixtureFile = error == 0 && info->st_ino == fixtureInode && info->st_dev == fixtureDevice;
    if (emulateBlock && fixtureFile)
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
    const int error = RealStat(fd, &info);
    const bool fixtureFile = error == 0 && info.st_ino == fixtureInode && info.st_dev == fixtureDevice;
    if (emulateBlock && fixtureFile)
    {
        if (nativeError)
        {
            errno = nativeError;
            return -1;
        }
        if (request == BLKGETSIZE64)
            *static_cast<uint64_t*>(value) = nativeCapacity;
        else if (request == BLKROGET)
            *static_cast<int*>(value) = nativeReadOnly;
        else if (request == BLKSSZGET || request == BLKPBSZGET)
            *static_cast<int*>(value) = nativeSector;
        else
            qFatal("Unexpected fixture ioctl");
        return 0;
    }
    return __real_ioctl(fd, request, value);
}

enum Fault { NoFault, PauseCopy, PauseSync, PauseVerify, WriteError, SyncError, CorruptVerify, PauseIsoCheck, LargeWindowsWim, LargeLinuxFile };
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
    if (!resumeIo.tryAcquire(1, resumeTimeoutMs))
        qFatal("Test did not release paused worker I/O");
}

extern "C" int __real_AnalyzeIsoImageWithSource(const char*, uint64_t, const ISO_ANALYSIS_OPTIONS*, ISO_ANALYSIS*, ISO_ANALYSIS_SOURCE*);
extern "C" int __wrap_AnalyzeIsoImageWithSource(const char* path, uint64_t size,
    const ISO_ANALYSIS_OPTIONS* options, ISO_ANALYSIS* result, ISO_ANALYSIS_SOURCE* source)
{
    if (faultKind == PauseIsoCheck)
        pauseIo();
    const int status = __real_AnalyzeIsoImageWithSource(path, size, options, result, source);
    if (faultKind == LargeWindowsWim && result->tree_complete)
        result->report.has_4GB_file = 0x11;
    if (faultKind == LargeLinuxFile && result->tree_complete)
        result->report.has_4GB_file = 1;
    return status;
}

extern "C" ssize_t WrappedWrite(int fd, const void* data, size_t size, off_t offset)
{
    if (!fixtureTarget(fd))
        return RealWrite(fd, data, size, offset);
    const int call = ++writeCalls;
    if (faultKind == PauseCopy && call == 2)
        pauseIo();
    if (faultKind == WriteError)
    {
        if (call > 1)
        {
            errno = ENOSPC;
            return -1;
        }
        size = qMin(size, size_t(4096));
    }
    const ssize_t written = RealWrite(fd, data, size, offset);
    if (written > 0)
        actualWritten += uint64_t(written);
    return written;
}

extern "C" ssize_t WrappedRead(int fd, void* data, size_t size, off_t offset)
{
    const bool target = fixtureTarget(fd);
    if (target && faultKind == PauseVerify && offset == 0)
        pauseIo();
    const ssize_t read = RealRead(fd, data, size, offset);
    if (target && faultKind == CorruptVerify && read > 0)
        static_cast<char*>(data)[0] ^= 1;
    return read;
}

extern "C" int __wrap_fsync(int fd)
{
    if (!fixtureTarget(fd))
        return __real_fsync(fd);
    ++syncCalls;
    if (faultKind == PauseSync)
        pauseIo();
    if (faultKind == SyncError)
    {
        errno = EIO;
        return -1;
    }
    return __real_fsync(fd);
}

namespace
{

QString compressedSuffix(int compression)
{
    return compression == BLED_COMPRESSION_XZ ? QStringLiteral("xz")
        : compression == BLED_COMPRESSION_BZIP2 ? QStringLiteral("bz2")
        : compression == BLED_COMPRESSION_LZMA ? QStringLiteral("lzma")
        : compression == BLED_COMPRESSION_LZW ? QStringLiteral("Z")
        : compression == BLED_COMPRESSION_ZIP ? QStringLiteral("zip")
        : compression == BLED_COMPRESSION_VTSI ? QStringLiteral("vtsi")
        : compression == IMG_COMPRESSION_VHD ? QStringLiteral("vhd")
        : compression == BLED_COMPRESSION_ZSTD ? QStringLiteral("zst") : QStringLiteral("gz");
}

QByteArray compressedLabel(int compression)
{
    return compression == BLED_COMPRESSION_XZ ? QByteArray("xz")
        : compression == BLED_COMPRESSION_BZIP2 ? QByteArray("bzip2")
        : compression == BLED_COMPRESSION_LZMA ? QByteArray("lzma")
        : compression == BLED_COMPRESSION_LZW ? QByteArray("lzw")
        : compression == BLED_COMPRESSION_ZIP ? QByteArray("zip")
        : compression == BLED_COMPRESSION_VTSI ? QByteArray("vtsi")
        : compression == IMG_COMPRESSION_VHD ? QByteArray("vhd")
        : compression == BLED_COMPRESSION_ZSTD ? QByteArray("zstd")
        : compression == LzwGzipFixture ? QByteArray("lzw-gzip") : QByteArray("gzip");
}
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
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
        qFatal("Fixture write failed");
}

QByteArray readFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        qFatal("Fixture read failed");
    return file.readAll();
}

void stopProcess(QProcess& process)
{
    if (process.state() == QProcess::NotRunning)
        return;
    process.terminate();
    if (!process.waitForFinished(fixtureTimeoutMs))
    {
        process.kill();
        process.waitForFinished(fixtureTimeoutMs);
    }
}

}

// Local signed-download peer. It never connects to the public Rufus server.
class GrubDownloadServer : public QTcpServer
{
    Q_OBJECT
public:
    QByteArray core, signature;
    QStringList requests;
    QString mode;
    QPointer<QTcpSocket> held;

    explicit GrubDownloadServer(QObject* parent = nullptr) : QTcpServer(parent)
    {
        connect(this, &QTcpServer::newConnection, this, &GrubDownloadServer::accept);
    }
    QUrl root() const { return QUrl(QStringLiteral("http://127.0.0.1:%1/files").arg(serverPort())); }
    void releaseSignature()
    {
        if (!held) return;
        held->write(signature);
        held->disconnectFromHost();
        held.clear();
    }
private:
    void accept()
    {
        while (hasPendingConnections())
        {
            QTcpSocket* socket = nextPendingConnection();
            connect(socket, &QTcpSocket::readyRead, this, &GrubDownloadServer::read);
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        }
    }
    void read()
    {
        auto* socket = qobject_cast<QTcpSocket*>(sender());
        if (!socket || !socket->peek(socket->bytesAvailable()).contains("\r\n\r\n")) return;
        const QByteArray path = socket->readAll().split(' ').value(1);
        requests.append(QString::fromLatin1(path));
        const bool detached = path.endsWith(".sig");
        QByteArray body = detached ? signature : core;
        const bool missing = mode == QStringLiteral("not-found");
        if (missing) body = "Not found";
        if (mode == QStringLiteral("bad-signature") && detached) body.fill('x');
        const QByteArray response = missing ? "HTTP/1.1 404 Not Found\r\n" : "HTTP/1.1 200 OK\r\n";
        socket->write(response + "Connection: close\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n\r\n");
        if (mode == QStringLiteral("hold-signature") && detached)
            held = socket;
        else if (mode == QStringLiteral("hold-payload") && !detached)
        {
            socket->write(body.first(body.size() / 2));
            held = socket;
        }
        else
        {
            socket->write(body);
            socket->disconnectFromHost();
        }
    }
};

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
    bool chooseEsp_ = false;

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
        const QDBusMessage reply = connection_->call(request, QDBus::Block, fixtureTimeoutMs);
        if (reply.type() != QDBusMessage::ReplyMessage)
            QTest::qFail(qPrintable(reply.errorMessage()), __FILE__, __LINE__);
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
        QTRY_COMPARE_WITH_TIMEOUT(state(), State::AwaitingConfirmation, fixtureTimeoutMs);
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
        QTRY_VERIFY_WITH_TIMEOUT(buttons->button(QDialogButtonBox::Open)->isEnabled(), uiTimeoutMs);
        QTest::mouseClick(buttons->button(QDialogButtonBox::Open), Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(controller_->selection().rawImage, uiTimeoutMs);
        auto* devices = window_->findChild<QComboBox*>(QStringLiteral("deviceCombo"));
        QVERIFY(devices);
        QCOMPARE(devices->currentIndex(), 0);
        QCOMPARE(controller_->selection().deviceId, device_.id);
        QCOMPARE(stat(QStringLiteral("opens")), 0);
        QCOMPARE(helperStarts, 0);
        QCOMPARE(actualWritten.load(), uint64_t(0));
    }

    void selectCompressedFixture(int compression, unsigned sector = 512, int decodedBytes = 65536 + 37,
        bool throughPicker = false)
    {
        sourceBytes_ = QByteArray(decodedBytes, 'g');
        if (compression != BLED_COMPRESSION_GZIP)
        {
            QRandomGenerator generator(25);
            for (qsizetype i = 0; i < sourceBytes_.size(); i++) sourceBytes_[i] = char(generator.generate());
        }
        if (compression == BLED_COMPRESSION_VTSI)
        {
            sourceBytes_.fill('\0');
            sourceBytes_.replace(0, 65536, QByteArray(65536, 'V'));
            sourceBytes_.replace(192 * 1024, 65536, QByteArray(65536, 'S'));
        }
        sourceBytes_[510] = char(0x55);
        sourceBytes_[511] = char(0xaa);
        sourcePath_ = directory_.filePath(QStringLiteral("source.") + compressedSuffix(compression));
        if (compression == BLED_COMPRESSION_VTSI)
        {
            QFile output(sourcePath_);
            QByteArray layoutHash;
            const QByteArray packed = VtsiSparseFixture(sourceBytes_, 65536, 192 * 1024, 65536, layoutHash);
            QVERIFY(!packed.isEmpty() && output.open(QIODevice::WriteOnly));
            QCOMPARE(output.write(packed), qint64(packed.size()));
            output.close();
        }
        else if (compression == IMG_COMPRESSION_VHD)
        {
            QFile output(sourcePath_);
            const QByteArray packed = VhdFixedFixture(sourceBytes_);
            QVERIFY(!packed.isEmpty() && output.open(QIODevice::WriteOnly));
            QCOMPARE(output.write(packed), qint64(packed.size()));
            output.close();
        }
        else if (compression == BLED_COMPRESSION_ZIP)
        {
            QFile output(sourcePath_);
            QVERIFY(output.open(QIODevice::WriteOnly));
            const QByteArray archive = ZipDeflateFixture(sourceBytes_);
            QVERIFY(!archive.isEmpty());
            QCOMPARE(output.write(archive), qint64(archive.size()));
            output.close();
        }
        else if (compression == BLED_COMPRESSION_LZW || compression == LzwGzipFixture)
        {
            QFile output(sourcePath_);
            QVERIFY(output.open(QIODevice::WriteOnly));
            const QByteArray packed = LzwLiteralFixture(sourceBytes_);
            QCOMPARE(output.write(packed), qint64(packed.size()));
            output.close();
        }
        else if (compression == BLED_COMPRESSION_GZIP)
        {
            gzFile file = gzopen(QFile::encodeName(sourcePath_).constData(), "wb0");
            QVERIFY(file != nullptr);
            const int written = gzwrite(file, sourceBytes_.constData(), unsigned(sourceBytes_.size()));
            QCOMPARE(gzclose(file), Z_OK);
            QCOMPARE(written, sourceBytes_.size());
        }
        else
        {
            QProcess encoder;
            encoder.setStandardOutputFile(sourcePath_);
            if (compression == BLED_COMPRESSION_BZIP2)
                encoder.start(QStringLiteral("bzip2"), {QStringLiteral("--stdout")});
            else if (compression == BLED_COMPRESSION_ZSTD)
                encoder.start(QStringLiteral("zstd"), {QStringLiteral("--stdout"), QStringLiteral("--quiet")});
            else if (compression == BLED_COMPRESSION_LZMA)
                encoder.start(QStringLiteral("xz"), {QStringLiteral("--stdout"), QStringLiteral("--format=lzma"),
                    QStringLiteral("--lzma1=dict=64KiB")});
            else
                encoder.start(QStringLiteral("xz"), {QStringLiteral("--stdout"), QStringLiteral("--check=crc64"),
                    QStringLiteral("--lzma2=dict=64KiB")});
            QVERIFY(encoder.waitForStarted(fixtureTimeoutMs));
            QCOMPARE(encoder.write(sourceBytes_), qint64(sourceBytes_.size()));
            encoder.closeWriteChannel();
            QVERIFY(encoder.waitForFinished(fixtureTimeoutMs));
            QCOMPARE(encoder.exitStatus(), QProcess::NormalExit);
            QCOMPARE(encoder.exitCode(), 0);
        }
        auto& facts = TestNativeDisk(nativeNumber);
        facts.logicalSectorSize = sector;
        facts.diskSequence = diskSequence;
        const QString owner = device_.serviceOwner;
        const QString id = device_.id;
        QVERIFY(rufus::DescribeUDisksDevice(objects_, disk.path(), device_));
        device_.serviceOwner = owner;
        device_.id = id;
        controller_->setDiscovery({device_}, {});
        controller_->selectDevice(device_.id);
        if (throughPicker) selectFixture();
        else controller_->selectImage(sourcePath_);
        QTRY_VERIFY_WITH_TIMEOUT(!controller_->isAnalyzing(), writeTimeoutMs);
        QCOMPARE(controller_->selection().imageState, rufus::ImageState::Complete);
        QVERIFY(controller_->selection().rawImage);
        QCOMPARE(controller_->selection().imageSource.decodedImageSize, quint64(sourceBytes_.size()));
        QCOMPARE(controller_->selection().imageSource.imageCompression, CompressedFixtureAssociation(compression));
    }

    void selectFixedUsb()
    {
        objects_[drive][driveInterface][QStringLiteral("Removable")] = false;
        objects_[drive][driveInterface][QStringLiteral("MediaRemovable")] = false;
        setObjects();
        const QString owner = device_.serviceOwner;
        const QString id = device_.id;
        QVERIFY(rufus::DescribeUDisksDevice(objects_, disk.path(), device_, true));
        device_.serviceOwner = owner;
        device_.id = id;
        controller_->setAllowFixedUsb(true);
        controller_->setDiscovery({device_}, {});
        controller_->selectDevice(device_.id);
        helperMode = QStringLiteral("fixed-success");
    }

    void existingPartitions(int count)
    {
        const QString tableInterface = QStringLiteral("org.freedesktop.UDisks2.PartitionTable");
        const QString partitionInterface = QStringLiteral("org.freedesktop.UDisks2.Partition");
        objects_[disk][tableInterface][QStringLiteral("Type")] = QStringLiteral("gpt");
        for (int i = 1; i <= count; ++i)
        {
            const QDBusObjectPath part(disk.path() + QString::number(i));
            objects_[part][partitionInterface] = {{QStringLiteral("Table"), QVariant::fromValue(disk)},
                {QStringLiteral("Type"), QStringLiteral("ebd0a0a2-b9e5-4433-87c0-68b6b72699c7")}};
            objects_[part][blockInterface] = {{QStringLiteral("Drive"), QVariant::fromValue(drive)},
                {QStringLiteral("DeviceNumber"), quint64(nativeNumber + i)}, {QStringLiteral("HintSystem"), false},
                {QStringLiteral("HintIgnore"), false}, {QStringLiteral("ReadOnly"), false}};
        }
        setObjects();
    }

    void observeState()
    {
        guiDelivery_ = guiDelivery_ && QThread::currentThread() == window_->thread();
        const int phase = writer().status().outcome.io.phase;
        if (phases_.isEmpty() || phases_.last() != phase)
            phases_.append(phase);
    }

    void selectFormat(int filesystem = FS_EXFAT, quint64 capacity = 64 * 1024 * 1024, int bootType = BT_NON_BOOTABLE)
    {
        nativeCapacity = capacity; // Simulated block capacity; no large file allocation.
        objects_[disk][blockInterface][QStringLiteral("Size")] = QVariant::fromValue(quint64(nativeCapacity));
        setObjects();
        const QString owner = device_.serviceOwner;
        const QString id = device_.id;
        QVERIFY(rufus::DescribeUDisksDevice(objects_, disk.path(), device_, true));
        // Large SFD capacity cases now need Rufus's hard-drive listing option.
        if (!device_.defaultVisible) controller_->setAllowFixedUsb(true);
        device_.serviceOwner = owner;
        device_.id = id;
        controller_->setDiscovery({device_}, {});
        controller_->selectDevice(device_.id);
        controller_->isoWriter().configure(*connection_,
            QCoreApplication::applicationDirPath() + QStringLiteral("/iso_helper_fixture"));
        auto* boot = window_->findChild<QComboBox*>(QStringLiteral("bootCombo"));
        if (bootType >= BT_SYSLINUX_V4 && boot->findText(QStringLiteral("UEFI:NTFS")) < 0)
            QTest::mouseClick(window_->findChild<QToolButton*>(QStringLiteral("advancedDriveButton")), Qt::LeftButton);
        const QString bootName = bootType == BT_SYSLINUX_V4 ? QStringLiteral("Syslinux 4.07")
            : bootType == BT_SYSLINUX_V6 ? QStringLiteral("Syslinux 6.04")
            : bootType == BT_GRUB2 ? QStringLiteral("Grub 2.14")
            : bootType == BT_GRUB4DOS ? QStringLiteral("Grub4DOS 0.4.6a")
            : bootType == BT_MSDOS ? QStringLiteral("MS-DOS")
            : bootType == BT_REACTOS ? QStringLiteral("ReactOS")
            : bootType == BT_FREEDOS ? QStringLiteral("FreeDOS") : QStringLiteral("UEFI:NTFS");
        boot->setCurrentIndex(bootType == BT_NON_BOOTABLE ? 0 : boot->findText(bootName));
        QVERIFY(QMetaObject::invokeMethod(boot, "activated", Q_ARG(int, boot->currentIndex())));
        QCOMPARE(controller_->selection().bootType, bootType);
        if (filesystem == FS_EXT2 || filesystem == FS_EXT3)
            QTest::mouseClick(window_->findChild<QToolButton*>(QStringLiteral("advancedFormatButton")), Qt::LeftButton);
        controller_->setFileSystem(filesystem);
        QCOMPARE(controller_->selection().fileSystem, filesystem);
        QVERIFY(start_->isEnabled());
    }

    void acceptOrdinaryEspQuestion()
    {
        const auto& modes = controller_->isoWriter().check().choices;
        if (modes.count != 2 || modes.mode[1] != IMAGE_WRITE_ESP) return;
        // Existing ISO-copy scenarios accept the original recommended row.
        auto* dialog = window_->findChild<rufus::ImageModeDialog*>(QStringLiteral("imageModeDialog"));
        QVERIFY(dialog != nullptr);
        if (chooseEsp_) dialog->findChild<QRadioButton*>(QStringLiteral("imageModeChoice2"))->setChecked(true);
        dialog->accept();
    }

private slots:
    void initTestCase()
    {
        qputenv("XDG_DATA_HOME", directory_.filePath(QStringLiteral("application-data")).toUtf8());
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory_.filePath(QStringLiteral("settings")));
        QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
        QVERIFY(directory_.isValid());
        qDBusRegisterMetaType<rufus::UDisksInterfaces>();
        qDBusRegisterMetaType<rufus::UDisksObjects>();
        qDBusRegisterMetaType<QList<QByteArray>>();
    }

    void init()
    {
        chooseEsp_ = false;
        ResetTestNativeDisks();
        QSettings(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("Rufus"), QStringLiteral("Linux")).clear();
        helperStarts = 0;
        expectedGrubCore.clear();
        syslinuxToolChecks = 0;
        missingSyslinuxTool.clear();
        missingExfatTool = false;
        missingFatTool = false;
        missingNtfsTool = false;
        missingExtTool = false;
        helperProcess.clear();
        completionBeeps = completionAlerts = 0;
        helperMode.clear();
        helperCopiedBytes = 519;
        diskSequence = 123;
        while (resumeIo.tryAcquire()) {}
        while (pausedIo.tryAcquire()) {}
        faultKind = NoFault;
        writeCalls = 0; actualWritten = 0; syncCalls = 0;
        emulateBlock = true;
        nativeNumber = fixtureDiskNumber; nativeCapacity = 12 * 1024 * 1024;
        nativeReadOnly = nativeError = 0; nativeSector = 512;
        // Qt 6.4 does not call cleanup() when init() skips a row.
        // Gate optional providers before acquiring any per-row resources.
        if (QByteArray(QTest::currentTestFunction()).startsWith("exfat") ||
            QByteArray(QTest::currentDataTag()).startsWith("exfat-"))
        {
            QString problem;
            if (rufus::CheckExfatFormatTools(QStringLiteral(RUFUS_MKFS_EXFAT_PATH), problem) != 0)
                QSKIP(qPrintable(problem));
        }
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
        QVERIFY(daemon_.waitForStarted(fixtureTimeoutMs));
        QVERIFY2(daemon_.canReadLine() || daemon_.waitForReadyRead(fixtureTimeoutMs),
            daemon_.readAllStandardError().constData());
        const QString address = QString::fromUtf8(daemon_.readLine()).trimmed();
        QVERIFY(address.startsWith(QStringLiteral("unix:")));
        connection_ = std::make_unique<QDBusConnection>(QDBusConnection::connectToBus(address, QStringLiteral("raw-widgets-test")));
        QVERIFY(connection_->isConnected());
        fixture_.start(QCoreApplication::applicationDirPath() + QStringLiteral("/udisks_fixture"), {address});
        QVERIFY(fixture_.waitForStarted(fixtureTimeoutMs));
        QVERIFY2(fixture_.waitForReadyRead(fixtureTimeoutMs), fixture_.readAllStandardError().constData());
        QCOMPARE(fixture_.readLine(), QByteArray("ready\n"));
        objects_.clear();
        objects_[disk][blockInterface] = {
            {QStringLiteral("Drive"), QVariant::fromValue(drive)},
            {QStringLiteral("Device"), QByteArray("/dev/test-a\0", 12)},
            {QStringLiteral("Size"), QVariant::fromValue(quint64(nativeCapacity))},
            {QStringLiteral("DeviceNumber"), QVariant::fromValue(fixtureDiskNumber)},
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
        // Existing scenarios exercise creation without optional appearance files.
        // The dedicated appearance cases enable and qualify the default option.
        controller_->setExtendedLabel(false);
        controller_->setDiscovery({device_}, {});
        writer().setConnection(*connection_);
        writer().setHelper(QCoreApplication::applicationDirPath() + QStringLiteral("/iso_helper_fixture"));
        window_ = std::make_unique<rufus::MainWindow>(*controller_);
        connect(&controller_->isoWriter(), &rufus::IsoWriteController::modeRequired,
            this, &TestRawWriteWidgets::acceptOrdinaryEspQuestion);
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
        if (connected)
            control(QStringLiteral("Quit"));
        if (fixture_.state() != QProcess::NotRunning)
            fixture_.waitForFinished(fixtureTimeoutMs);
        connection_.reset();
        QDBusConnection::disconnectFromBus(QStringLiteral("raw-widgets-test"));
        stopProcess(fixture_); stopProcess(daemon_);
        QCOMPARE(unexpected, 0);
        QCOMPARE(readFile(targetPath_), expectedTarget_);
        QDir isoTree(directory_.filePath(QStringLiteral("iso-tree")));
        if (isoTree.exists())
            QVERIFY(isoTree.removeRecursively());
    }

    void compressedPlanGeometry_data()
    {
        QTest::addColumn<unsigned>("sector");
        QTest::addColumn<int>("compression");
        for (int compression : CompressedFixtureKinds)
            for (unsigned sector : {512U, 4096U, 65536U})
            {
                const QByteArray row = compressedLabel(compression) + "-" + QByteArray::number(sector);
                QTest::newRow(row.constData()) << sector << compression;
            }
    }

    void zeroShortcuts_data()
    {
        QTest::addColumn<bool>("fast");
        QTest::newRow("full") << false;
        QTest::newRow("fast-empty") << true;
    }

    void zeroShortcuts()
    {
        QFETCH(bool, fast);
        const QString selected = controller_->selection().imagePath;
        TestNativeDisk(nativeNumber).diskSequence = diskSequence;
        const QString owner = device_.serviceOwner, id = device_.id;
        QVERIFY(rufus::DescribeUDisksDevice(objects_, disk.path(), device_));
        device_.serviceOwner = owner; device_.id = id;
        controller_->setDiscovery({device_}, {});
        controller_->selectDevice(device_.id);
        helperMode = fast ? QStringLiteral("zero-empty-steps") : QStringLiteral("raw-steps");
        window_->activateWindow();
        QTRY_VERIFY_WITH_TIMEOUT(window_->isActiveWindow(), uiTimeoutMs);
        QTest::keyClick(window_.get(), Qt::Key_Z, fast ? Qt::ControlModifier | Qt::AltModifier : Qt::AltModifier);
        QTRY_VERIFY2_WITH_TIMEOUT(state() == State::AwaitingConfirmation, qPrintable(writer().statusText()), fixtureTimeoutMs);
        QVERIFY(writer().plan()->sourcePath().isEmpty());
        QCOMPARE(writer().plan()->imageMetadata().zeroMode, fast ? ZERO_FAST : ZERO_FULL);
        QCOMPARE(controller_->selection().imagePath, selected);
        QCOMPARE(helperStarts, 0);
        auto* confirmation = window_->findChild<QMessageBox*>(QStringLiteral("writeConfirmation"));
        QVERIFY(confirmation && confirmation->informativeText().contains(QStringLiteral("No filesystem")));
        approvePlan();
        QTRY_VERIFY2_WITH_TIMEOUT(helperProcess && writer().percent() == 25, qPrintable(writer().statusText()), fixtureTimeoutMs);
        QCOMPARE(helperStarts, 1);
        helperProcess->write("resume\n");
        QTRY_COMPARE_WITH_TIMEOUT(writer().status().outcome.io.phase, RAW_WRITE_SYNC, fixtureTimeoutMs);
        helperProcess->write("resume\n");
        QTRY_VERIFY_WITH_TIMEOUT(writer().status().outcome.io.bytes_verified != 0, fixtureTimeoutMs);
        helperProcess->write("resume\n");
        QTRY_COMPARE_WITH_TIMEOUT(writer().percent(), 99, fixtureTimeoutMs);
        QVERIFY(writer().isActive());
        helperProcess->write("resume\n");
        QTRY_COMPARE_WITH_TIMEOUT(state(), State::Complete, fixtureTimeoutMs);
        QCOMPARE(writer().percent(), 100);
        if (fast)
            QCOMPARE(writer().status().outcome.io.bytes_written, quint64(0));
        QCOMPARE(controller_->selection().imagePath, selected);
    }

    void imageEntryWorkflows_data()
    {
        QTest::addColumn<QString>("suffix");
        QTest::newRow("raw-wic") << QStringLiteral("WIC");
        QTest::newRow("raw-usb") << QStringLiteral("USB");
        QTest::newRow("raw-unknown") << QStringLiteral("bin");
        QTest::newRow("raw-iso") << QStringLiteral("iso");
        QTest::newRow("gzip") << QStringLiteral("gz");
        QTest::newRow("bzip2-alias") << QStringLiteral("BZIP2");
    }

    void imageEntryWorkflows()
    {
        QFETCH(QString, suffix);
        QByteArray bytes = sourceBytes_;
        bytes[510] = bytes[511] = '\0';
        const QString path = directory_.filePath(QStringLiteral("update with spaces-é.") + suffix);
        if (suffix == QStringLiteral("gz"))
        {
            gzFile file = gzopen(QFile::encodeName(path).constData(), "wb0");
            QVERIFY(file);
            QCOMPARE(gzwrite(file, bytes.constData(), unsigned(bytes.size())), int(bytes.size()));
            QCOMPARE(gzclose(file), Z_OK);
        }
        else if (suffix == QStringLiteral("BZIP2"))
        {
            QProcess encoder;
            encoder.setStandardOutputFile(path);
            encoder.start(QStringLiteral("bzip2"), {QStringLiteral("--stdout")});
            QVERIFY(encoder.waitForStarted());
            QCOMPARE(encoder.write(bytes), qint64(bytes.size()));
            encoder.closeWriteChannel();
            QVERIFY(encoder.waitForFinished() && encoder.exitCode() == 0);
        }
        else
            writeFile(path, bytes);
        QVERIFY(!controller_->ignoreBootMarker());
        window_->activateWindow();
        QTRY_VERIFY_WITH_TIMEOUT(window_->isActiveWindow(), uiTimeoutMs);
        QTest::keyClick(window_.get(), Qt::Key_M, Qt::AltModifier);
        QTRY_VERIFY_WITH_TIMEOUT(controller_->ignoreBootMarker(), uiTimeoutMs);
        rufus::SelectionController restored;
        QVERIFY(restored.ignoreBootMarker());
        QMimeData mime;
        mime.setUrls({QUrl::fromLocalFile(path), QUrl::fromLocalFile(sourcePath_)});
        QDragEnterEvent enter(QPoint(10, 10), Qt::CopyAction | Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(window_.get(), &enter);
        QVERIFY(enter.isAccepted());
        QDropEvent drop(QPointF(10, 10), Qt::CopyAction | Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(window_.get(), &drop);
        QVERIFY(drop.isAccepted());
        QCOMPARE(drop.dropAction(), Qt::CopyAction);
        QTRY_VERIFY_WITH_TIMEOUT(!controller_->isAnalyzing(), fixtureTimeoutMs);
        QCOMPARE(controller_->selection().imagePath, path);
        QVERIFY(controller_->selection().rawImage);
        QVERIFY(controller_->selection().imageSource.ignoreBootMarker);
        const bool compressed = suffix == QStringLiteral("gz") || suffix == QStringLiteral("BZIP2");
        QCOMPARE(controller_->selection().imageSource.hasCompressedImage(), compressed);
        controller_->toggleBootMarker();
        QVERIFY(!controller_->ignoreBootMarker());
        // An already accepted image retains its explicit analysis choice.
        startPlan();
        QVERIFY(writer().plan()->imageMetadata().ignoreBootMarker);
        QCOMPARE(helperStarts, 0);
        const QString selected = controller_->selection().imagePath;
        QDropEvent busyDrop(QPointF(10, 10), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(window_.get(), &busyDrop);
        QVERIFY(!busyDrop.isAccepted());
        QCOMPARE(controller_->selection().imagePath, selected);
        auto* confirm = window_->findChild<QMessageBox*>(QStringLiteral("writeConfirmation"));
        QVERIFY(confirm);
        QTest::mouseClick(confirm->button(QMessageBox::Cancel), Qt::LeftButton);
        if (suffix == QStringLiteral("WIC"))
        {
            QDropEvent moveOnly(QPointF(10, 10), Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(window_.get(), &moveOnly);
            QVERIFY(!moveOnly.isAccepted());
            QMimeData remote;
            remote.setUrls({QUrl(QStringLiteral("https://example.invalid/image.iso"))});
            QDropEvent remoteDrop(QPointF(10, 10), Qt::CopyAction, &remote, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(window_.get(), &remoteDrop);
            QVERIFY(!remoteDrop.isAccepted());
            QCOMPARE(controller_->selection().imagePath, selected);
            QVERIFY(QFileInfo::exists(path));
        }
    }

    void zipPickerSelectsCompressedImage()
    {
        selectCompressedFixture(BLED_COMPRESSION_ZIP, 512, 65536 + 37, true);
        QCOMPARE(controller_->selection().imageSource.imageCompression, int(BLED_COMPRESSION_ZIP));
        QCOMPARE(controller_->selection().imageSource.decodedImageSha256,
            QCryptographicHash::hash(sourceBytes_, QCryptographicHash::Sha256));
        QCOMPARE(helperStarts, 0);
    }

    void ffuStoreSelection_data()
    {
        QTest::addColumn<QString>("action");
        for (const char* action : {"first", "second", "cancel", "changed"})
            QTest::newRow(action) << QString::fromLatin1(action);
    }

    void ffuStoreSelection()
    {
        QFETCH(QString, action);
        const QByteArray first(256 * 1024, 'A'), second(384 * 1024, 'B');
        sourcePath_ = directory_.filePath(QStringLiteral("stores.ffu"));
        writeFile(sourcePath_, FfuMultiStoreFixture({first, second}));
        controller_->selectImage(sourcePath_);
        QTRY_VERIFY_WITH_TIMEOUT(!controller_->isAnalyzing(), fixtureTimeoutMs);
        QCOMPARE(controller_->selection().imageState, rufus::ImageState::ChoosingStore);
        QVERIFY(!start_->isEnabled());
        QCOMPARE(helperStarts, 0);
        auto* dialog = window_->findChild<QInputDialog*>(QStringLiteral("ffuStoreDialog"));
        QVERIFY(dialog && dialog->isVisible());
        const QStringList stores = controller_->selection().ffuStores;
        QCOMPARE(stores.size(), 2);
        QVERIFY(stores[1].contains(QStringLiteral("VenHw(Store2)")));
        if (action == QStringLiteral("cancel"))
        {
            dialog->reject();
            QCOMPARE(controller_->selection().imageState, rufus::ImageState::Cancelled);
            QVERIFY(!start_->isEnabled());
            QCOMPARE(helperStarts, 0);
            return;
        }
        const unsigned selected = action == QStringLiteral("first") ? 1 : 2;
        if (action == QStringLiteral("changed")) writeFile(sourcePath_, FfuMultiStoreFixture({second, first}));
        dialog->setTextValue(stores[int(selected - 1)]);
        dialog->accept();
        QTRY_VERIFY_WITH_TIMEOUT(!controller_->isAnalyzing(), fixtureTimeoutMs);
        if (action == QStringLiteral("changed"))
        {
            QCOMPARE(controller_->selection().imageState, rufus::ImageState::Failed);
            QVERIFY(!start_->isEnabled());
            QCOMPARE(helperStarts, 0);
            return;
        }
        QCOMPARE(controller_->selection().imageState, rufus::ImageState::Complete);
        const auto& source = controller_->selection().imageSource;
        QCOMPARE(source.ffuStore, selected);
        QCOMPARE(source.decodedImageSha256, QCryptographicHash::hash(selected == 1 ? first : second, QCryptographicHash::Sha256));
        startPlan();
        QCOMPARE(writer().plan()->imageMetadata().ffuStore, selected);
        QCOMPARE(writer().plan()->source().ffu_store, selected);
        auto* confirm = window_->findChild<QMessageBox*>(QStringLiteral("writeConfirmation"));
        QVERIFY(confirm->informativeText().contains(QStringLiteral("FFU store: %1").arg(selected)));
        QCOMPARE(helperStarts, 0);
        QTest::mouseClick(confirm->button(QMessageBox::Cancel), Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(state(), State::Cancelled, cancelTimeoutMs);
    }

    void zstdPickerSelectsCompressedImage()
    {
        selectCompressedFixture(BLED_COMPRESSION_ZSTD, 512, 65536 + 37, true);
        QCOMPARE(controller_->selection().imageSource.imageCompression, int(BLED_COMPRESSION_ZSTD));
        QCOMPARE(controller_->selection().imageSource.decodedImageSha256,
            QCryptographicHash::hash(sourceBytes_, QCryptographicHash::Sha256));
        QCOMPARE(helperStarts, 0);
    }

    void vtsiPickerFreezesSparsePlan()
    {
        selectCompressedFixture(BLED_COMPRESSION_VTSI, 512, 512 * 512, true);
        QByteArray layoutHash;
        QVERIFY(!VtsiSparseFixture(sourceBytes_, 65536, 192 * 1024, 65536, layoutHash).isEmpty());
        QCOMPARE(controller_->selection().imageSource.decodedImageSha256,
            QCryptographicHash::hash(sourceBytes_, QCryptographicHash::Sha256));
        QCOMPARE(controller_->selection().imageSource.imageLayoutSha256, layoutHash);
        QCOMPARE(helperStarts, 0);
        QVERIFY(start_->isEnabled());
        startPlan();
        const auto* plan = writer().plan();
        QVERIFY(plan);
        QCOMPARE(plan->writeBytes(), quint64(sourceBytes_.size()));
        QCOMPARE(plan->imageMetadata().imageLayoutSha256, layoutHash);
        QCOMPARE(plan->imageMetadata().imageCompression, int(BLED_COMPRESSION_VTSI));
        QCOMPARE(plan->imageMetadata().imageSectorSize, 512U);
        auto* confirm = window_->findChild<QMessageBox*>(QStringLiteral("writeConfirmation"));
        QVERIFY(confirm);
        QTest::mouseClick(confirm->button(QMessageBox::Cancel), Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(state(), State::Cancelled, cancelTimeoutMs);
        QCOMPARE(helperStarts, 0);
    }

    void vhdPickerFreezesLogicalPlan()
    {
        selectCompressedFixture(IMG_COMPRESSION_VHD, 512, 512 * 512, true);
        QCOMPARE(controller_->selection().imageSource.decodedImageSha256,
            QCryptographicHash::hash(sourceBytes_, QCryptographicHash::Sha256));
        QCOMPARE(controller_->selection().imageSource.imageCompression, int(IMG_COMPRESSION_VHD));
        QCOMPARE(helperStarts, 0);
        QVERIFY(start_->isEnabled());
        startPlan();
        const auto* plan = writer().plan();
        QVERIFY(plan);
        QCOMPARE(plan->writeBytes(), quint64(sourceBytes_.size()));
        QCOMPARE(plan->imageMetadata().imageCompression, int(IMG_COMPRESSION_VHD));
        QCOMPARE(plan->imageMetadata().imageSectorSize, 512U);
        auto* confirm = window_->findChild<QMessageBox*>(QStringLiteral("writeConfirmation"));
        QVERIFY(confirm);
        QTest::mouseClick(confirm->button(QMessageBox::Cancel), Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(state(), State::Cancelled, cancelTimeoutMs);
        QCOMPARE(helperStarts, 0);
    }

    void compressedPlanGeometry()
    {
        QFETCH(unsigned, sector);
        QFETCH(int, compression);
        selectCompressedFixture(compression, sector);
        QVERIFY(start_->isEnabled());
        startPlan();
        const auto* plan = writer().plan();
        QVERIFY(plan);
        const quint64 padded = (quint64(sourceBytes_.size()) + sector - 1) / sector * sector;
        QCOMPARE(plan->writeBytes(), padded);
        QCOMPARE(writer().status().size, padded);
        QCOMPARE(plan->targetDiskSequence(), diskSequence);
        QCOMPARE(plan->imageMetadata().imageSectorSize, sector);
        QCOMPARE(plan->imageMetadata().imageCompression, CompressedFixtureAssociation(compression));
        QCOMPARE(plan->imageMetadata().decodedImageSha256, QCryptographicHash::hash(sourceBytes_, QCryptographicHash::Sha256));
        QCOMPARE(plan->imageMetadata().decodedImageSize, quint64(sourceBytes_.size()));
        auto* confirm = window_->findChild<QMessageBox*>(QStringLiteral("writeConfirmation"));
        QVERIFY(confirm && confirm->informativeText().contains(QStringLiteral("Compressed file:")));
        QTest::mouseClick(confirm->button(QMessageBox::Cancel), Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(state(), State::Cancelled, cancelTimeoutMs);
        QCOMPARE(helperStarts, 0);
    }

    void compressedCapacity_data()
    {
        QTest::addColumn<unsigned>("sector");
        QTest::addColumn<int>("decoded");
        QTest::addColumn<quint64>("capacity");
        QTest::addColumn<bool>("fits");
        QTest::addColumn<int>("compression");
        struct Case { const char* name; unsigned sector; int decoded; quint64 capacity; bool fits; };
        const Case cases[] = {{"stored-source-larger-than-target", 512U, 512, 512, true},
            {"final-sector-does-not-fit", 512U, 513, 512, false},
            {"4k-padding", 4096U, 4097, 8192, true},
            {"64k-padding", 65536U, 65537, 131072, true},
            {"capacity-not-sector-aligned", 4096U, 512, 4608, false},
            {"unknown-sector", 0U, 512, 4096, false}};
        for (int compression : CompressedFixtureKinds)
            for (const Case& test : cases)
            {
                const QByteArray row = compressedLabel(compression) + "-" + test.name;
                QTest::newRow(row.constData()) << test.sector << test.decoded << test.capacity << test.fits << compression;
            }
    }

    void compressedCapacity()
    {
        QFETCH(unsigned, sector);
        QFETCH(int, decoded);
        QFETCH(quint64, capacity);
        QFETCH(bool, fits);
        QFETCH(int, compression);
        selectCompressedFixture(compression, sector, decoded);
        if (fits && capacity == 512)
            QVERIFY(QFileInfo(sourcePath_).size() > qint64(capacity));
        auto target = device_;
        target.size = capacity;
        rufus::RawWritePlan plan;
        const QString problem = plan.prepare(sourcePath_, target, &controller_->selection().imageSource);
        QCOMPARE(problem.isEmpty(), fits);
        if (fits)
        {
            const quint64 padded = (quint64(decoded) + sector - 1) / sector * sector;
            QCOMPARE(plan.writeBytes(), padded);
        }
        QCOMPARE(helperStarts, 0);
    }

    void compressedInvalidatedBeforeAuthorization_data()
    {
        QTest::addColumn<QString>("change");
        QTest::addColumn<int>("compression");
        for (int compression : CompressedFixtureKinds)
            for (const char* change : {"before-start", "source", "sector", "sequence"})
            {
                const QByteArray row = compressedLabel(compression) + "-" + change;
                QTest::newRow(row.constData()) << QString::fromLatin1(change) << compression;
            }
    }

    void compressedInvalidatedBeforeAuthorization()
    {
        QFETCH(QString, change);
        QFETCH(int, compression);
        selectCompressedFixture(compression);
        if (change != "before-start") startPlan();
        if (change == "sector") TestNativeDisk(nativeNumber).logicalSectorSize = 4096;
        else if (change == "sequence") TestNativeDisk(nativeNumber).diskSequence++;
        else
        {
            QFile file(sourcePath_);
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Append));
            QCOMPARE(file.write("x", 1), qint64(1));
        }
        if (change == "before-start") controller_->startWrite();
        else approvePlan();
        QTRY_COMPARE_WITH_TIMEOUT(state(), State::Failed, writeTimeoutMs);
        QCOMPARE(helperStarts, 0);
        QCOMPARE(actualWritten.load(), uint64_t(0));
        QCOMPARE(stat(QStringLiteral("opens")), 0);
    }

    void compressedReadyWaitsForJoinedHelper_data()
    {
        QTest::addColumn<int>("compression");
        QTest::newRow("gzip") << int(BLED_COMPRESSION_GZIP);
        QTest::newRow("xz") << int(BLED_COMPRESSION_XZ);
        QTest::newRow("bzip2") << int(BLED_COMPRESSION_BZIP2);
        QTest::newRow("lzma") << int(BLED_COMPRESSION_LZMA);
        QTest::newRow("lzw") << int(BLED_COMPRESSION_LZW);
        QTest::newRow("zip") << int(BLED_COMPRESSION_ZIP);
        QTest::newRow("zstd") << int(BLED_COMPRESSION_ZSTD);
        QTest::newRow("lzw-gzip") << LzwGzipFixture;
    }

    void compressedReadyWaitsForJoinedHelper()
    {
        QFETCH(int, compression);
        selectCompressedFixture(compression, 4096);
        helperMode = QStringLiteral("raw-steps");
        startPlan();
        const quint64 padded = writer().status().size;
        approvePlan();
        QTRY_COMPARE_WITH_TIMEOUT(progress_->value(), 25, writeTimeoutMs);
        QCOMPARE(helperStarts, 1);
        helperProcess->write("resume\n");
        QTRY_COMPARE_WITH_TIMEOUT(writer().status().outcome.io.phase, RAW_WRITE_SYNC, writeTimeoutMs);
        QCOMPARE(progress_->maximum(), 0);
        helperProcess->write("resume\n");
        QTRY_COMPARE_WITH_TIMEOUT(progress_->value(), 75, writeTimeoutMs);
        helperProcess->write("resume\n");
        QTRY_COMPARE_WITH_TIMEOUT(progress_->value(), 99, writeTimeoutMs);
        QCOMPARE(state(), State::Writing);
        QCOMPARE(helperProcess->state(), QProcess::Running);
        QCOMPARE(completionBeeps, 0);
        helperProcess->closeWriteChannel();
        QTRY_COMPARE_WITH_TIMEOUT(state(), State::Complete, writeTimeoutMs);
        QCOMPARE(writer().status().outcome.io.bytes_written, padded);
        QCOMPARE(writer().status().outcome.io.bytes_verified, padded);
        QCOMPARE(progress_->value(), 100);
        QCOMPARE(progress_->format(), QStringLiteral("READY"));
        QCOMPARE(completionBeeps, 1);
        QCOMPARE(completionAlerts, 1);
        controller_->setDiscovery({}, {});
        QCOMPARE(state(), State::Complete);
        QCOMPARE(completionBeeps, 1);
        QCOMPARE(actualWritten.load(), uint64_t(0));
    }

    void compressedHelperFailure_data()
    {
        QTest::addColumn<QString>("mode");
        QTest::addColumn<int>("compression");
        for (int compression : CompressedFixtureKinds)
            for (const char* mode : {"write-error", "sync-error", "verify-error", "raw-hold-copy"})
            {
                const QByteArray row = compressedLabel(compression) + "-" + mode;
                QTest::newRow(row.constData()) << QString::fromLatin1(mode) << compression;
            }
    }

    void compressedHelperFailure()
    {
        QFETCH(QString, mode);
        QFETCH(int, compression);
        selectCompressedFixture(compression);
        helperMode = mode;
        startPlan(); approvePlan();
        if (mode == "raw-hold-copy")
        {
            QTRY_COMPARE_WITH_TIMEOUT(progress_->value(), 25, writeTimeoutMs);
            writer().cancel();
        }
        const State expected = mode == "raw-hold-copy" ? State::Cancelled : State::Failed;
        QTRY_COMPARE_WITH_TIMEOUT(state(), expected, writeTimeoutMs);
        QCOMPARE(helperStarts, 1);
        QCOMPARE(completionBeeps, 0);
        QCOMPARE(completionAlerts, 0);
        QVERIFY(progress_->format() != QStringLiteral("READY"));
        QCOMPARE(actualWritten.load(), uint64_t(0));
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

    void firstTargetAfterIsoScanGetsFormatDefaults()
    {
        selectIsoFixture();
        rufus::SelectionController lateTarget;
        lateTarget.writer().setConnection(*connection_);
        lateTarget.writer().setHelper(QCoreApplication::applicationDirPath() + QStringLiteral("/iso_helper_fixture"));
        lateTarget.isoWriter().configure(*connection_,
            QCoreApplication::applicationDirPath() + QStringLiteral("/iso_helper_fixture"));
        rufus::MainWindow preview(lateTarget);
        preview.show();
        lateTarget.selectImage(controller_->selection().imagePath);
        QTRY_VERIFY_WITH_TIMEOUT(!lateTarget.isAnalyzing(), fixtureTimeoutMs);
        QCOMPARE(lateTarget.selection().imageState, rufus::ImageState::Partial);
        QVERIFY(lateTarget.selection().imageAnalysis.tree_complete);
        QVERIFY(lateTarget.selection().deviceId.isEmpty());
        QVERIFY(!lateTarget.canStartWrite());

        lateTarget.setDiscovery({device_}, {});
        QCOMPARE(lateTarget.selection().deviceId, device_.id);
        QCOMPARE(lateTarget.selection().fileSystem, int(FS_FAT32));
        QCOMPARE(lateTarget.selection().clusterSize, quint32(512));
        QCOMPARE(preview.findChild<QComboBox*>(QStringLiteral("partitionCombo"))->currentText(), QStringLiteral("GPT"));
        QCOMPARE(lateTarget.selection().volumeLabel, QStringLiteral("SOURCE"));
        QVERIFY(preview.findChild<QPushButton*>(QStringLiteral("startButton"))->isEnabled());
        QVERIFY(!lateTarget.isWritingActive());
        for (QMessageBox* dialog : preview.findChildren<QMessageBox*>())
            QVERIFY(!dialog->isVisible());
        QCOMPARE(stat(QStringLiteral("opens")), 0);
        QCOMPARE(helperStarts, 0);
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
        if (!screenshot.isEmpty())
            QVERIFY(dialog->grab().save(screenshot));
        QVERIFY(!select_->isEnabled());
        QVERIFY(!start_->isEnabled());
        approvePlan();
        QTRY_COMPARE_WITH_TIMEOUT(state(), State::Complete, writeTimeoutMs);
        QVERIFY(!writer().isActive());
        QVERIFY(!writer().isWriting());
        QVERIFY(!writer().plan());
        QCOMPARE(writer().status().outcome.error, 0);
        QCOMPARE(writer().status().outcome.io.bytes_written, uint64_t(sourceBytes_.size()));
        QCOMPARE(writer().status().outcome.io.bytes_verified, uint64_t(sourceBytes_.size()));
        QCOMPARE(actualWritten.load(), uint64_t(0));
        QCOMPARE(helperStarts, 1);
        QCOMPARE(stat(QStringLiteral("opens")), 0);
        QCOMPARE(syncCalls.load(), 0);
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
        QCOMPARE(target, expectedTarget_);
        QCOMPARE(target.mid(sourceBytes_.size()), expectedTarget_.mid(sourceBytes_.size()));
    }

    void declinedConfirmationCanBeRetried()
    {
        startPlan();
        auto* dialog = window_->findChild<QMessageBox*>(QStringLiteral("writeConfirmation"));
        QTest::keyClick(dialog, Qt::Key_Escape);
        QTRY_COMPARE_WITH_TIMEOUT(state(), State::Cancelled, uiTimeoutMs);
        QCOMPARE(progress_->format(), QStringLiteral("READY"));
        QCOMPARE(progress_->value(), 0);
        QCOMPARE(stat(QStringLiteral("opens")), 0);
        QCOMPARE(actualWritten.load(), uint64_t(0));
        startPlan();
        approvePlan();
        QTRY_COMPARE_WITH_TIMEOUT(state(), State::Complete, writeTimeoutMs);
        QCOMPARE(stat(QStringLiteral("opens")), 0);
        QCOMPARE(helperStarts, 1);
    }

    void multiplePartitionConfirmation_data()
    {
        QTest::addColumn<QString>("route");
        QTest::addColumn<QString>("action");
        for (const char* route : {"raw", "mounted-raw", "iso", "format", "sfd", "fixed-raw", "fixed-iso", "fixed-format", "fixed-sfd"})
            for (const char* action : {"accept", "cancel", "removed"})
            {
                const QByteArray row = QByteArray(route) + '-' + action;
                QTest::newRow(row.constData()) << QString::fromLatin1(route) << QString::fromLatin1(action);
            }
    }

    void multiplePartitionConfirmation()
    {
        QFETCH(QString, route); QFETCH(QString, action);
        const bool fixedUsb = route.startsWith(QStringLiteral("fixed-"));
        if (fixedUsb) route.remove(0, 6);
        const bool raw = route == QStringLiteral("raw") || route == QStringLiteral("mounted-raw");
        if (route == QStringLiteral("iso")) selectIsoFixture();
        else if (!raw) selectFormat(FS_FAT32);
        if (route == QStringLiteral("sfd")) controller_->setPartitionScheme(rufus::PartitionScheme::Sfd);
        if (fixedUsb)
        {
            selectFixedUsb();
            writer().setHelper(QCoreApplication::applicationDirPath() + QStringLiteral("/iso_helper_fixture"));
        }
        if (route == QStringLiteral("mounted-raw"))
        {
            writer().setHelper(QCoreApplication::applicationDirPath() + QStringLiteral("/iso_helper_fixture"));
            objects_[disk][fsInterface][QStringLiteral("MountPoints")] =
                QVariant::fromValue(QList<QByteArray>{QByteArray("/run/media/user/USB") + '\0'});
        }
        // The selection has stale zero-partition metadata: the fresh preflight must win.
        existingPartitions(2);
        if (raw) startPlan();
        else
        {
            QTest::mouseClick(start_, Qt::LeftButton);
            QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::AwaitingConfirmation, writeTimeoutMs);
        }
        QVERIFY(!window_->findChild<QMessageBox*>(QStringLiteral("partitionConfirmation")));
        if (fixedUsb)
        {
            QVERIFY(!window_->findChild<QCheckBox*>(QStringLiteral("listUsbHardDrives"))->isEnabled());
            controller_->setAllowFixedUsb(false);
            QVERIFY(controller_->selection().allowFixedUsb);
            if (!raw) QVERIFY(controller_->isoWriter().request().allowFixedUsb);
        }
        if (raw) approvePlan(); else approveIsoPlan();
        auto* dialog = window_->findChild<QMessageBox*>(QStringLiteral("partitionConfirmation"));
        QVERIFY(dialog && dialog->isVisible());
        QCOMPARE(dialog->windowTitle(), QStringLiteral("Multiple partitions detected"));
        QVERIFY(dialog->text().contains(QStringLiteral("THIS DRIVE CONTAINS MULTIPLE PARTITIONS!!")));
        QCOMPARE(dialog->textFormat(), Qt::PlainText);
        QCOMPARE(dialog->defaultButton(), dialog->button(QMessageBox::Cancel));
        QCOMPARE(helperStarts, 0);
        QCOMPARE(stat(QStringLiteral("opens")), 0);
        QCOMPARE(actualWritten.load(), uint64_t(0));
        if (action == QStringLiteral("removed")) controller_->setDiscovery({}, {});
        else if (action == QStringLiteral("cancel")) QTest::keyClick(dialog, Qt::Key_Escape);
        else QTest::mouseClick(dialog->button(QMessageBox::Ok), Qt::LeftButton);
        if (raw)
        {
            const State expected = action == QStringLiteral("accept") ? State::Complete :
                action == QStringLiteral("cancel") ? State::Cancelled : State::Failed;
            QTRY_COMPARE_WITH_TIMEOUT(state(), expected, writeTimeoutMs);
        }
        else
        {
            const auto expected = action == QStringLiteral("accept") ? rufus::IsoWriteState::Complete :
                action == QStringLiteral("cancel") ? rufus::IsoWriteState::Cancelled : rufus::IsoWriteState::Failed;
            QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), expected, writeTimeoutMs);
        }
        const bool accepted = action == QStringLiteral("accept");
        QCOMPARE(helperStarts, accepted ? 1 : 0);
        QCOMPARE(stat(QStringLiteral("opens")), 0);
        QCOMPARE(completionBeeps, accepted ? 1 : 0);
        if (!accepted) QCOMPARE(actualWritten.load(), uint64_t(0));
        QTRY_VERIFY_WITH_TIMEOUT(window_->findChildren<QMessageBox*>(QStringLiteral("partitionConfirmation")).isEmpty(), uiTimeoutMs);
    }

    void partitionsAddedDuringConfirmation_data()
    {
        QTest::addColumn<bool>("raw");
        QTest::newRow("raw") << true;
        QTest::newRow("format") << false;
    }

    void partitionsAddedDuringConfirmation()
    {
        QFETCH(bool, raw);
        if (!raw) selectFormat(FS_FAT32);
        existingPartitions(1);
        if (raw) startPlan();
        else
        {
            QTest::mouseClick(start_, Qt::LeftButton);
            QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::AwaitingConfirmation, writeTimeoutMs);
        }
        existingPartitions(2);
        if (raw) approvePlan(); else approveIsoPlan();
        if (raw) QTRY_COMPARE_WITH_TIMEOUT(state(), State::Failed, writeTimeoutMs);
        else QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::Failed, writeTimeoutMs);
        QCOMPARE(helperStarts, 0);
        QCOMPARE(stat(QStringLiteral("opens")), 0);
        QCOMPARE(actualWritten.load(), uint64_t(0));
        const QString problem = raw ? writer().statusText() : controller_->isoWriter().statusText();
        QVERIFY(problem.contains(QStringLiteral("multiple partitions")));
    }

    void mountedRawRouting_data()
    {
        QTest::addColumn<QString>("action");
        QTest::addColumn<bool>("fixedUsb");
        for (const char* action : {"success", "appears", "disappears", "cancel", "deny", "missing-helper",
             "protected-before", "protected-after", "source-change", "disk-change", "sync-error"})
        {
            QTest::newRow(action) << QString::fromLatin1(action) << false;
            QTest::newRow(qPrintable(QStringLiteral("fixed-") + QLatin1String(action))) << QString::fromLatin1(action) << true;
        }
    }

    void mountedRawRouting()
    {
        QFETCH(QString, action); QFETCH(bool, fixedUsb);
        if (fixedUsb) selectFixedUsb();
        if (action == QStringLiteral("missing-helper"))
        {
            writer().setHelper({});
            QVERIFY(!start_->isEnabled());
            QCOMPARE(helperStarts, 0);
            return;
        }
        else
            writer().setHelper(QCoreApplication::applicationDirPath() + QStringLiteral("/iso_helper_fixture"));
        if (action != QStringLiteral("appears"))
            objects_[disk][fsInterface][QStringLiteral("MountPoints")] =
                QVariant::fromValue(QList<QByteArray>{QByteArray("/run/media/user/USB") + '\0'});
        if (action == QStringLiteral("protected-before"))
            objects_[disk][blockInterface][QStringLiteral("HintSystem")] = true;
        setObjects();
        const bool initialFailure = action == QStringLiteral("missing-helper") || action == QStringLiteral("protected-before");
        if (initialFailure)
        {
            QTest::mouseClick(start_, Qt::LeftButton);
            QTRY_COMPARE_WITH_TIMEOUT(state(), State::Failed, fixtureTimeoutMs);
            QCOMPARE(helperStarts, 0);
            QCOMPARE(stat(QStringLiteral("opens")), 0);
            return;
        }
        startPlan();
        auto* dialog = window_->findChild<QMessageBox*>(QStringLiteral("writeConfirmation"));
        QCOMPARE(dialog->defaultButton(), dialog->button(QMessageBox::Cancel));
        QCOMPARE(helperStarts, 0);
        if (action == QStringLiteral("cancel"))
        {
            QTest::keyClick(dialog, Qt::Key_Escape);
            QTRY_COMPARE_WITH_TIMEOUT(state(), State::Cancelled, uiTimeoutMs);
            QCOMPARE(helperStarts, 0);
            QCOMPARE(stat(QStringLiteral("opens")), 0);
            QCOMPARE(progress_->format(), QStringLiteral("READY"));
            QCOMPARE(progress_->value(), 0);
            return;
        }
        if (action == QStringLiteral("appears"))
            objects_[disk][fsInterface][QStringLiteral("MountPoints")] =
                QVariant::fromValue(QList<QByteArray>{QByteArray("/run/media/user/USB") + '\0'});
        if (action == QStringLiteral("disappears")) objects_[disk].remove(fsInterface);
        if (action == QStringLiteral("protected-after"))
            objects_[disk][blockInterface][QStringLiteral("HintSystem")] = true;
        setObjects();
        if (action == QStringLiteral("source-change")) writeFile(sourcePath_, sourceBytes_);
        if (action == QStringLiteral("disk-change")) ++diskSequence;
        helperMode = fixedUsb ? QStringLiteral("fixed-") + action : action;
        approvePlan();
        const bool success = action == QStringLiteral("success") || action == QStringLiteral("appears")
            || action == QStringLiteral("disappears");
        QTRY_COMPARE_WITH_TIMEOUT(state(), success ? State::Complete : State::Failed, writeTimeoutMs);
        const bool helperExpected = action == QStringLiteral("disappears")
            || action == QStringLiteral("success") || action == QStringLiteral("appears")
            || action == QStringLiteral("deny") || action == QStringLiteral("sync-error");
        QCOMPARE(helperStarts, helperExpected ? 1 : 0);
        QCOMPARE(stat(QStringLiteral("opens")), 0);
        QCOMPARE(progress_->format(), success ? QStringLiteral("READY") : QStringLiteral("FAILED"));
        QCOMPARE(completionBeeps, success ? 1 : 0);
        if (action == QStringLiteral("sync-error")) QCOMPARE(writer().status().outcome.io.sync_error, EIO);
    }

    void mountAfterAuthorizationDoesNotPromptAgain()
    {
        helperMode = QStringLiteral("raw-hold-copy");
        startPlan(); approvePlan();
        QTRY_COMPARE_WITH_TIMEOUT(progress_->value(), 25, writeTimeoutMs);
        objects_[disk][fsInterface][QStringLiteral("MountPoints")] =
            QVariant::fromValue(QList<QByteArray>{QByteArray("/run/media/user/USB") + '\0'});
        setObjects();
        helperProcess->write("continue\n");
        QTRY_COMPARE_WITH_TIMEOUT(state(), State::Complete, writeTimeoutMs);
        QCOMPARE(helperStarts, 1);
        QCOMPARE(stat(QStringLiteral("opens")), 0);
    }

    void mountedRawProgressWaitsForHelperExit_data()
    {
        QTest::addColumn<bool>("fixedUsb");
        QTest::newRow("mounted-removable") << false;
        QTest::newRow("unmounted-fixed") << true;
    }

    void mountedRawProgressWaitsForHelperExit()
    {
        QFETCH(bool, fixedUsb);
        writer().setHelper(QCoreApplication::applicationDirPath() + QStringLiteral("/iso_helper_fixture"));
        objects_[disk][fsInterface][QStringLiteral("MountPoints")] =
            QVariant::fromValue(QList<QByteArray>{QByteArray("/run/media/user/USB") + '\0'});
        setObjects();
        helperMode = QStringLiteral("raw-steps");
        if (fixedUsb)
        {
            selectFixedUsb();
            objects_[disk].remove(fsInterface);
            setObjects();
            helperMode = QStringLiteral("fixed-raw-steps");
        }
        startPlan(); approvePlan();
        QTRY_COMPARE_WITH_TIMEOUT(progress_->value(), 25, writeTimeoutMs);
        QCOMPARE(progress_->format(), QStringLiteral("Writing image: %p%"));
        QVERIFY(writer().isWriting());
        QVERIFY(!select_->isEnabled());
        QVERIFY(!start_->isEnabled());
        QCOMPARE(helperStarts, 1);
        QCOMPARE(stat(QStringLiteral("opens")), 0);
        helperProcess->write("resume\n");
        QTRY_COMPARE_WITH_TIMEOUT(writer().status().outcome.io.phase, RAW_WRITE_SYNC, writeTimeoutMs);
        QCOMPARE(progress_->maximum(), 0);
        QCOMPARE(completionBeeps, 0);
        helperProcess->write("resume\n");
        QTRY_COMPARE_WITH_TIMEOUT(progress_->value(), 75, writeTimeoutMs);
        QCOMPARE(progress_->format(), QStringLiteral("Verifying image: %p%"));
        helperProcess->write("resume\n");
        QTRY_COMPARE_WITH_TIMEOUT(progress_->value(), 99, writeTimeoutMs);
        QCOMPARE(state(), State::Writing);
        QCOMPARE(helperProcess->state(), QProcess::Running);
        QCOMPARE(completionBeeps, 0);
        helperProcess->closeWriteChannel();
        QTRY_COMPARE_WITH_TIMEOUT(state(), State::Complete, writeTimeoutMs);
        QCOMPARE(helperProcess->state(), QProcess::NotRunning);
        QCOMPARE(progress_->value(), 100);
        QCOMPARE(progress_->format(), QStringLiteral("READY"));
        QCOMPARE(progress_->palette().color(QPalette::Highlight), QColor(0x06, 0xB0, 0x25));
        QCOMPARE(completionBeeps, 1);
        QCOMPARE(completionAlerts, 1);
        controller_->setDiscovery({}, {});
        QCOMPARE(state(), State::Complete);
        QCOMPARE(progress_->value(), 100);
        QCOMPARE(completionBeeps, 1);
        QCOMPARE(actualWritten.load(), uint64_t(0)); // Protocol fixture never opens the target.
    }

    void mountedRawInterrupted_data()
    {
        QTest::addColumn<QString>("reason");
        for (const char* reason : {"cancel", "removed", "service"})
            QTest::newRow(reason) << QString::fromLatin1(reason);
    }

    void mountedRawInterrupted()
    {
        QFETCH(QString, reason);
        const bool invalidated = reason != QStringLiteral("cancel");
        writer().setHelper(QCoreApplication::applicationDirPath() + QStringLiteral("/iso_helper_fixture"));
        objects_[disk][fsInterface][QStringLiteral("MountPoints")] =
            QVariant::fromValue(QList<QByteArray>{QByteArray("/run/media/user/USB") + '\0'});
        setObjects();
        helperMode = QStringLiteral("raw-hold-copy");
        startPlan(); approvePlan();
        QTRY_COMPARE_WITH_TIMEOUT(progress_->value(), 25, writeTimeoutMs);
        if (reason == QStringLiteral("removed")) controller_->setDiscovery({}, {});
        else if (reason == QStringLiteral("service")) control(QStringLiteral("ReleaseName"));
        else
        {
            QTest::mouseClick(close_, Qt::LeftButton);
            auto* dialog = window_->findChild<QMessageBox*>(QStringLiteral("cancelConfirmation"));
            QVERIFY(dialog && dialog->isVisible());
            QCOMPARE(dialog->defaultButton(), dialog->button(QMessageBox::No));
            QTest::mouseClick(dialog->button(QMessageBox::Yes), Qt::LeftButton);
        }
        QTRY_COMPARE_WITH_TIMEOUT(state(), invalidated ? State::Failed : State::Cancelled, cancelTimeoutMs);
        QCOMPARE(helperProcess->state(), QProcess::NotRunning);
        QCOMPARE(helperStarts, 1);
        QCOMPARE(stat(QStringLiteral("opens")), 0);
        QCOMPARE(completionBeeps, 0);
        QVERIFY(writer().status().outcome.started);
        QVERIFY(progress_->value() < 100);
        QCOMPARE(progress_->format(), invalidated ? QStringLiteral("FAILED") : QStringLiteral("CANCELLED"));
    }

    void changedWhileConfirming_data()
    {
        QTest::addColumn<QString>("kind");
        for (const char* kind : {"device", "readonly", "source", "disk-sequence"})
            QTest::newRow(kind) << QString::fromLatin1(kind);
    }

    void changedWhileConfirming()
    {
        QFETCH(QString, kind);
        startPlan();
        if (kind == QStringLiteral("device") || kind == QStringLiteral("readonly"))
        {
            rufus::Device changed = device_;
            if (kind == QStringLiteral("readonly")) changed.readOnly = true;
            else changed.identity.append(QStringLiteral("replacement"));
            controller_->setDiscovery({changed}, {});
            if (kind == QStringLiteral("readonly")) {
                QCOMPARE(controller_->selection().deviceId, device_.id);
                QVERIFY(controller_->canSaveDevice() && !controller_->canStartWrite());
            }
        }
        else
        {
            if (kind == QStringLiteral("source"))
                writeFile(sourcePath_, sourceBytes_);
            else
                ++diskSequence;
            approvePlan();
        }
        QTRY_COMPARE_WITH_TIMEOUT(state(), State::Failed, fixtureTimeoutMs);
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
        helperMode = mode;
        startPlan(); approvePlan();
        QTRY_COMPARE_WITH_TIMEOUT(state(), mode == QStringLiteral("dismiss") ? State::Cancelled : State::Failed, fixtureTimeoutMs);
        QVERIFY(select_->isEnabled());
        QCOMPARE(actualWritten.load(), uint64_t(0));
        QVERIFY(progress_->value() < 100);
        QCOMPARE(stat(QStringLiteral("opens")), 0);
        QCOMPARE(helperStarts, 1);
    }

    void cancelledAuthorizationIgnoresLateDescriptor()
    {
        helperMode = QStringLiteral("hold-authorization");
        startPlan(); approvePlan();
        QTRY_COMPARE_WITH_TIMEOUT(helperStarts, 1, fixtureTimeoutMs);
        QTRY_COMPARE_WITH_TIMEOUT(state(), State::Authorizing, fixtureTimeoutMs);
        writer().cancel();
        QTRY_COMPARE_WITH_TIMEOUT(state(), State::Cancelled, cancelTimeoutMs);
        QCOMPARE(helperProcess->state(), QProcess::NotRunning);
        QCOMPARE(stat(QStringLiteral("opens")), 0);
        QCOMPARE(completionBeeps, 0);
    }

    void cancelDuringIo_data()
    {
        QTest::addColumn<QString>("mode");
        for (const char* mode : {"raw-hold-copy", "raw-hold-sync", "raw-hold-verify"})
            QTest::newRow(mode) << QString::fromLatin1(mode);
    }

    void cancelDuringIo()
    {
        QFETCH(QString, mode);
        helperMode = mode;
        startPlan(); approvePlan();
        const int expected = mode == "raw-hold-copy" ? 25 : mode == "raw-hold-sync" ? 50 : 75;
        QTRY_COMPARE_WITH_TIMEOUT(progress_->value(), expected, writeTimeoutMs);
        QTest::mouseClick(close_, Qt::LeftButton);
        auto* dialog = window_->findChild<QMessageBox*>(QStringLiteral("cancelConfirmation"));
        QVERIFY(dialog && dialog->isVisible());
        QCOMPARE(dialog->defaultButton(), dialog->button(QMessageBox::No));
        QTest::mouseClick(dialog->button(QMessageBox::Yes), Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(state(), State::Cancelled, cancelTimeoutMs);
        QCOMPARE(helperStarts, 1);
        QCOMPARE(helperProcess->state(), QProcess::NotRunning);
        QVERIFY(progress_->value() < 100);
        QCOMPARE(progress_->format(), QStringLiteral("CANCELLED"));
        QVERIFY(writer().status().outcome.started);
        QCOMPARE(completionBeeps, 0);
    }

    void decliningCancellationLetsWriteFinish()
    {
        helperMode = QStringLiteral("raw-hold-copy");
        startPlan(); approvePlan();
        QTRY_COMPARE_WITH_TIMEOUT(progress_->value(), 25, fixtureTimeoutMs);
        window_->close();
        auto* dialog = window_->findChild<QMessageBox*>(QStringLiteral("cancelConfirmation"));
        QVERIFY(dialog && dialog->isVisible());
        QTest::mouseClick(dialog->button(QMessageBox::No), Qt::LeftButton);
        QCOMPARE(state(), State::Writing);
        QVERIFY(window_->isVisible());
        helperProcess->write("continue\n");
        QTRY_COMPARE_WITH_TIMEOUT(state(), State::Complete, writeTimeoutMs);
    }

    void rawReadyWaitsForSync()
    {
        helperMode = QStringLiteral("raw-hold-sync");
        startPlan();
        approvePlan();
        QTRY_COMPARE_WITH_TIMEOUT(progress_->value(), 50, fixtureTimeoutMs);
        QVERIFY(writer().isWriting());
        QVERIFY(progress_->format() != QStringLiteral("READY"));
        QVERIFY(progress_->value() < 100);
        QCOMPARE(completionBeeps, 0);
        QCOMPARE(completionAlerts, 0);
        helperProcess->write("continue\n");
        QTRY_COMPARE_WITH_TIMEOUT(state(), State::Complete, writeTimeoutMs);
        QCOMPARE(helperStarts, 1);
        QCOMPARE(helperProcess->state(), QProcess::NotRunning);
        QCOMPARE(progress_->value(), 100);
        QCOMPARE(progress_->format(), QStringLiteral("READY"));
        QCOMPARE(progress_->palette().color(QPalette::Highlight), QColor(0x06, 0xB0, 0x25));
        QCOMPARE(completionBeeps, 1);
        QCOMPARE(completionAlerts, 1);
        controller_->setDiscovery({}, {});
        QCOMPARE(state(), State::Complete);
        controller_->setDiscovery({device_}, {});
        QVERIFY(controller_->selection().deviceId.isEmpty());
        QCOMPARE(state(), State::Complete);
        QCOMPARE(progress_->value(), 100);
        QCOMPARE(progress_->format(), QStringLiteral("READY"));
        QCOMPARE(completionBeeps, 1);
        QCOMPARE(completionAlerts, 1);
    }

    void cancelledWorkerCanBeRetried()
    {
        helperMode = QStringLiteral("raw-hold-copy");
        startPlan(); approvePlan();
        QTRY_COMPARE_WITH_TIMEOUT(progress_->value(), 25, fixtureTimeoutMs);
        writer().cancel();
        QTRY_COMPARE_WITH_TIMEOUT(state(), State::Cancelled, cancelTimeoutMs);
        helperMode.clear();
        startPlan(); approvePlan();
        QTRY_COMPARE_WITH_TIMEOUT(state(), State::Complete, writeTimeoutMs);
        QCOMPARE(helperStarts, 2);
        QCOMPARE(stat(QStringLiteral("opens")), 0);
    }

    void deviceRemovalDuringWriteFails()
    {
        helperMode = QStringLiteral("raw-hold-copy");
        startPlan(); approvePlan();
        QTRY_COMPARE_WITH_TIMEOUT(progress_->value(), 25, fixtureTimeoutMs);
        controller_->setDiscovery({}, {});
        controller_->setDiscovery({device_}, {});
        QVERIFY(controller_->selection().deviceId.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(state(), State::Failed, cancelTimeoutMs);
        QVERIFY(writer().statusText().contains(QStringLiteral("removed")));
        QCOMPARE(helperProcess->state(), QProcess::NotRunning);
        QVERIFY(!start_->isEnabled());
        QVERIFY(progress_->value() < 100);
    }

    void ioFailure_data()
    {
        QTest::addColumn<QString>("mode");
        for (const char* mode : {"write-error", "sync-error", "verify-error"})
            QTest::newRow(mode) << QString::fromLatin1(mode);
    }

    void ioFailure()
    {
        QFETCH(QString, mode);
        helperMode = mode;
        startPlan(); approvePlan();
        QTRY_COMPARE_WITH_TIMEOUT(state(), State::Failed, writeTimeoutMs);
        QVERIFY(!writer().plan());
        QVERIFY(writer().status().outcome.error != 0);
        QVERIFY(writer().status().outcome.io.bytes_verified < uint64_t(sourceBytes_.size()));
        QCOMPARE(progress_->format(), QStringLiteral("FAILED"));
        QVERIFY(progress_->value() < 100);
        QVERIFY(select_->isEnabled());
        QCOMPARE(helperStarts, 1);
        QCOMPARE(stat(QStringLiteral("opens")), 0);
    }

    void teardownJoinsWorker()
    {
        helperMode = QStringLiteral("raw-hold-copy");
        startPlan(); approvePlan();
        QTRY_COMPARE_WITH_TIMEOUT(progress_->value(), 25, fixtureTimeoutMs);
        window_.reset(); controller_.reset();
        QVERIFY(helperProcess.isNull()); // Process destruction waited for child exit.
        QCOMPARE(helperStarts, 1);
    }

    void isoCreationUsesOneHelper_data()
    {
        QTest::addColumn<int>("filesystem");
        QTest::newRow("FAT16") << int(FS_FAT16);
        QTest::newRow("FAT32") << int(FS_FAT32);
    }

    void isoCreationUsesOneHelper()
    {
        QFETCH(int, filesystem);
        selectIsoFixture();
        controller_->setFileSystem(filesystem);
        startIsoPlan();
        QVERIFY(!select_->isEnabled());
        auto* label = window_->findChild<QLineEdit*>(QStringLiteral("volumeEdit"));
        QVERIFY(label && !label->isEnabled());
        const auto& request = controller_->isoWriter().request();
        QCOMPARE(request.label, QStringLiteral("NEW LABEL"));
        QCOMPARE(request.fileSystem, filesystem);
        QCOMPARE(request.clusterSize, filesystem == FS_FAT16 ? quint32(2048) : quint32(512));
        QCOMPARE(helperStarts, 0);
        approveIsoPlan();
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::Complete, writeTimeoutMs);
        QCOMPARE(helperStarts, 1);
        QCOMPARE(stat(QStringLiteral("opens")), 0);
        QCOMPARE(progress_->value(), MAX_PROGRESS);
        QCOMPARE(progress_->format(), QStringLiteral("READY"));
        QVERIFY(label->isEnabled());
        QCOMPARE(readFile(sourcePath_), sourceBytes_);
    }

    void mountedPreparation_data()
    {
        QTest::addColumn<QString>("route");
        QTest::addColumn<QString>("action");
        for (const char* route : {"iso", "format", "sfd"})
            for (const char* action : {"success", "cancel", "protected-before", "protected-after", "deny"})
            {
                const QByteArray row = QByteArray(route) + '-' + action;
                QTest::newRow(row.constData()) << QString::fromLatin1(route) << QString::fromLatin1(action);
            }
    }

    void mountedPreparation()
    {
        QFETCH(QString, route);
        QFETCH(QString, action);
        if (route == QStringLiteral("iso")) selectIsoFixture();
        else selectFormat(FS_FAT32);
        if (route == QStringLiteral("sfd")) controller_->setPartitionScheme(rufus::PartitionScheme::Sfd);
        const QDBusObjectPath member(root + QStringLiteral("/block_devices/mounted"));
        objects_[member][blockInterface] = {{QStringLiteral("Drive"), QVariant::fromValue(drive)},
            {QStringLiteral("DeviceNumber"), quint64(nativeNumber + 1)}, {QStringLiteral("HintSystem"), false},
            {QStringLiteral("HintIgnore"), false}, {QStringLiteral("ReadOnly"), false}};
        objects_[member][QStringLiteral("org.freedesktop.UDisks2.Partition")][QStringLiteral("Table")] = QVariant::fromValue(disk);
        objects_[member][QStringLiteral("org.freedesktop.UDisks2.Filesystem")][QStringLiteral("MountPoints")] =
            QVariant::fromValue(QList<QByteArray>{QByteArray("/run/media/user/USB") + '\0'});
        if (action == QStringLiteral("protected-before"))
            objects_[member][blockInterface][QStringLiteral("HintSystem")] = true;
        setObjects();
        helperMode = action == QStringLiteral("deny") ? QStringLiteral("deny") : QString();
        auto& writer = controller_->isoWriter();
        QTest::mouseClick(start_, Qt::LeftButton);
        if (action == QStringLiteral("protected-before"))
        {
            QTRY_COMPARE_WITH_TIMEOUT(writer.state(), rufus::IsoWriteState::Failed, writeTimeoutMs);
            QCOMPARE(helperStarts, 0);
            return;
        }
        QTRY_COMPARE_WITH_TIMEOUT(writer.state(), rufus::IsoWriteState::AwaitingConfirmation, writeTimeoutMs);
        auto* dialog = window_->findChild<QMessageBox*>(QStringLiteral("isoWriteConfirmation"));
        QVERIFY(dialog && dialog->isVisible());
        QCOMPARE(dialog->defaultButton(), dialog->button(QMessageBox::Cancel));
        QCOMPARE(helperStarts, 0);
        if (action == QStringLiteral("cancel"))
        {
            QTest::mouseClick(dialog->button(QMessageBox::Cancel), Qt::LeftButton);
            QTRY_COMPARE_WITH_TIMEOUT(writer.state(), rufus::IsoWriteState::Cancelled, cancelTimeoutMs);
            QCOMPARE(helperStarts, 0);
        }
        else
        {
            const bool changed = action == QStringLiteral("protected-after");
            if (changed)
            {
                objects_[member][blockInterface][QStringLiteral("HintSystem")] = true;
                setObjects();
            }
            approveIsoPlan();
            const bool success = action == QStringLiteral("success");
            QTRY_COMPARE_WITH_TIMEOUT(writer.state(), success ? rufus::IsoWriteState::Complete : rufus::IsoWriteState::Failed, writeTimeoutMs);
            QCOMPARE(helperStarts, changed ? 0 : 1);
            QCOMPARE(progress_->format(), success ? QStringLiteral("READY") : QStringLiteral("FAILED"));
            QCOMPARE(completionBeeps, success ? 1 : 0);
            if (success) QCOMPARE(progress_->value(), MAX_PROGRESS);
        }
        QCOMPARE(stat(QStringLiteral("opens")), 0);
        QCOMPARE(stat(QStringLiteral("unexpected")), 0);
        QCOMPARE(actualWritten.load(), uint64_t(0)); // Protocol-only fixture, no device writes.
    }

    void fat16SelectionAndReload()
    {
        selectIsoFixture();
        auto* filesystems = window_->findChild<QComboBox*>(QStringLiteral("fileSystemCombo"));
        auto* clusters = window_->findChild<QComboBox*>(QStringLiteral("clusterCombo"));
        auto* label = window_->findChild<QLineEdit*>(QStringLiteral("volumeEdit"));
        QVERIFY(filesystems && clusters && label);
        QCOMPARE(filesystems->itemText(0), QStringLiteral("FAT16 (Default)"));
        QCOMPARE(filesystems->currentText(), QStringLiteral("FAT32"));
        QTest::keyClick(filesystems, Qt::Key_Home);
        QCOMPARE(controller_->selection().fileSystem, int(FS_FAT16));
        QCOMPARE(clusters->currentText(), QStringLiteral("2048 bytes (Default)"));
        QVERIFY(label->toolTip().contains(QStringLiteral("FAT16")));
        clusters->setCurrentIndex(clusters->findData(4096));
        QVERIFY(QMetaObject::invokeMethod(clusters, "activated", Q_ARG(int, clusters->currentIndex())));
        QCOMPARE(controller_->selection().clusterSize, quint32(4096));
        controller_->setDiscovery({device_}, {});
        QCOMPARE(controller_->selection().fileSystem, int(FS_FAT16));
        QCOMPARE(controller_->selection().clusterSize, quint32(4096));
        // Loading another ISO re-applies Rufus's image preference.
        controller_->selectImage(sourcePath_);
        QTRY_VERIFY_WITH_TIMEOUT(!controller_->isAnalyzing(), writeTimeoutMs);
        QCOMPARE(filesystems->currentText(), QStringLiteral("FAT32"));
        QCOMPARE(clusters->currentData().toUInt(), quint32(512));
        QCOMPARE(helperStarts, 0);
        QCOMPARE(stat(QStringLiteral("opens")), 0);
    }

    void fat16CapacitySelection_data()
    {
        QTest::addColumn<quint64>("capacity");
        QTest::addColumn<int>("expected");
        QTest::addColumn<bool>("hasFat16");
        constexpr quint64 MiB = 1024 * 1024;
        QTest::newRow("FAT16-only-small-target") << 16 * MiB << int(FS_FAT16) << true;
        QTest::newRow("both-FAT-formats") << 64 * MiB << int(FS_FAT32) << true;
        QTest::newRow("last-FAT16-sector") << 4096 * MiB - 512 << int(FS_FAT32) << true;
        QTest::newRow("FAT16-limit") << 4096 * MiB << int(FS_FAT32) << false;
    }

    void fat16CapacitySelection()
    {
        QFETCH(quint64, capacity);
        QFETCH(int, expected);
        QFETCH(bool, hasFat16);
        selectIsoFixture();
        rufus::Device target = device_;
        target.id = QStringLiteral("fat16-capacity-target");
        target.size = capacity; // Discovery only; no image allocation or writes.
        controller_->setDiscovery({device_, target}, {});
        controller_->selectDevice(target.id);
        auto* filesystems = window_->findChild<QComboBox*>(QStringLiteral("fileSystemCombo"));
        QCOMPARE(filesystems->findData(FS_FAT16) >= 0, hasFat16);
        QCOMPARE(controller_->selection().fileSystem, expected);
        if (hasFat16)
        {
            QCOMPARE(filesystems->itemText(0), QStringLiteral("FAT16 (Default)"));
            const bool manualChoice = filesystems->count() > 1;
            if (manualChoice)
            {
                controller_->setFileSystem(FS_NTFS);
                controller_->setFileSystem(FS_FAT16);
            }
            controller_->selectDevice(device_.id);
            QCOMPARE(controller_->selection().fileSystem, manualChoice ? int(FS_FAT16) : int(FS_FAT32));
            QCOMPARE(controller_->selection().clusterSize, manualChoice ? quint32(2048) : quint32(512));
        }
        else
        {
            controller_->setFileSystem(FS_FAT16);
            QCOMPARE(controller_->selection().fileSystem, expected);
        }
        QCOMPARE(helperStarts, 0);
        QCOMPARE(stat(QStringLiteral("opens")), 0);
    }

    void ntfsSelectionUsesOneHelper()
    {
        selectIsoFixture();
        auto* filesystems = window_->findChild<QComboBox*>(QStringLiteral("fileSystemCombo"));
        QVERIFY(filesystems && filesystems->isEnabled());
        QCOMPARE(filesystems->count(), 3);
        QCOMPARE(filesystems->currentData().toInt(), int(FS_FAT32));
        QTest::keyClick(filesystems, Qt::Key_End);
        QCOMPARE(controller_->selection().fileSystem, int(FS_NTFS));
        QCOMPARE(controller_->selection().clusterSize, quint32(4096));
        const QString label = QString::fromUtf8("Windows 11 ș😀.USB");
        controller_->setVolumeLabel(label);
        startIsoPlan();
        const auto& request = controller_->isoWriter().request();
        QVERIFY(request.fileSystem == FS_NTFS);
        QCOMPARE(request.volumeId, quint32(0));
        QCOMPARE(request.label, QString::fromUtf8("Windows 11 ș😀_USB"));
        QVERIFY(!filesystems->isEnabled());
        controller_->setFileSystem(FS_FAT32);
        QCOMPARE(controller_->selection().fileSystem, int(FS_NTFS));
        approveIsoPlan();
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::Complete, writeTimeoutMs);
        QCOMPARE(helperStarts, 1);
        QCOMPARE(stat(QStringLiteral("opens")), 0);
        QCOMPARE(readFile(targetPath_), expectedTarget_);
    }

    void windowsCustomizationProtocol_data()
    {
        QTest::addColumn<QString>("mode");
        QTest::addColumn<int>("expectedError");
        QTest::newRow("complete") << QString() << 0;
        QTest::newRow("missing-phase") << QStringLiteral("skip-customization") << EIO;
        QTest::newRow("failed-extraction") << QStringLiteral("extract-error") << EIO;
        QTest::newRow("failed-customization") << QStringLiteral("customization-error") << ENOSPC;
        QTest::newRow("cancelled-customization") << QStringLiteral("customization-cancel") << ECANCELED;
    }

    void windowsCustomizationProtocol()
    {
        QFETCH(QString, mode);
        QFETCH(int, expectedError);
        rufus::PrepareImageRequest request;
        request.isDevice = true;
        request.path = QStringLiteral("/dev/fixture");
        request.device = 123;
        request.size = 64 * 1024 * 1024;
        request.diskSequence = 7;
        request.label = QStringLiteral("TEST");
        request.objectPath = QStringLiteral("/org/freedesktop/UDisks2/block_devices/fixture");
        request.serviceOwner = QStringLiteral(":1.2");
        request.identity = {QStringLiteral("fixture")};
        request.imagePath = sourcePath_;
        request.imageDevice = 1;
        request.imageInode = 2;
        request.imageSize = quint64(sourceBytes_.size());
        request.imageModified = request.imageChanged = QStringLiteral("1:0");
        request.windowsFlags = UNATTEND_NO_DATA_COLLECTION;
        helperMode = mode;
        const QString fixture = QCoreApplication::applicationDirPath() + QStringLiteral("/iso_helper_fixture");
        rufus::PrepareImageProcess process(fixture);
        QSignalSpy finished(&process, &rufus::PrepareImageProcess::finished);
        QVERIFY(process.start(request));
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, writeTimeoutMs);
        QCOMPARE(process.outcome().error, expectedError);
        if (expectedError == 0)
            QCOMPARE(process.state(), rufus::PrepareImageProcess::State::Complete);
        else if (expectedError == ECANCELED)
            QCOMPARE(process.state(), rufus::PrepareImageProcess::State::Cancelled);
        else
            QCOMPARE(process.state(), rufus::PrepareImageProcess::State::Failed);
        QCOMPARE(helperStarts, 1);
        QCOMPARE(readFile(targetPath_), expectedTarget_);
    }

    void windowsDialogCancellationAndDeviceRemoval()
    {
        selectIsoFixture(false, true);
        QTest::mouseClick(start_, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::ChoosingWindowsOptions, writeTimeoutMs);
        auto* dialog = window_->findChild<rufus::WindowsUserDialog*>();
        QVERIFY(dialog && dialog->isVisible());
        auto* editions = dialog->findChild<QComboBox*>(QStringLiteral("windowsEdition"));
        QVERIFY(editions && !editions->isEnabled());
        QCOMPARE(editions->count(), 1);
        QCOMPARE(editions->itemData(0).toInt(), 1);
        QCOMPARE(editions->itemText(0), controller_->isoWriter().check().windowsInstall.editions[0].name);
        QCOMPARE(editions->currentIndex(), -1);
        QVERIFY(!window_->findChild<QMessageBox*>(QStringLiteral("isoWriteConfirmation")));
        QCOMPARE(helperStarts, 0);
        QTest::keyClick(dialog, Qt::Key_Escape);
        QCOMPARE(controller_->isoWriter().state(), rufus::IsoWriteState::Cancelled);
        QCOMPARE(readFile(targetPath_), expectedTarget_);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QTest::mouseClick(start_, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::ChoosingWindowsOptions, writeTimeoutMs);
        controller_->setDiscovery({}, {});
        QCOMPARE(controller_->isoWriter().state(), rufus::IsoWriteState::Failed);
        dialog = window_->findChild<rufus::WindowsUserDialog*>();
        QVERIFY(!dialog || !dialog->isVisible());
        QCOMPARE(helperStarts, 0);
    }

    void expertShortcutPersistsWithoutSelectingSMode()
    {
        QVERIFY(!controller_->isoWriter().expertMode());
        QVERIFY(!controller_->enhancedInstall());
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        window_->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(window_.get()));
        QTRY_COMPARE(QApplication::activeWindow(), window_.get());
        QTest::keyClick(window_.get(), Qt::Key_E, Qt::AltModifier);
        QVERIFY(!controller_->isoWriter().expertMode());
        QVERIFY(controller_->enhancedInstall());
        QCOMPARE(window_->statusBar()->currentMessage(), QStringLiteral("Dual UEFI/BIOS mode enabled"));
        rufus::SelectionController enhancedSession;
        QVERIFY(enhancedSession.enhancedInstall());
        QTest::keyClick(window_.get(), Qt::Key_E, Qt::ControlModifier | Qt::AltModifier);
        QVERIFY(controller_->isoWriter().expertMode());
        QVERIFY(controller_->enhancedInstall());
        QCOMPARE(window_->statusBar()->currentMessage(), QStringLiteral("Expert Mode enabled"));
        rufus::IsoWriteController nextSession;
        QVERIFY(nextSession.expertMode());
        QVERIFY(!(nextSession.windowsOptions() & UNATTEND_FORCE_S_MODE));
        QTest::keyClick(window_.get(), Qt::Key_E, Qt::ControlModifier | Qt::AltModifier);
        QVERIFY(!controller_->isoWriter().expertMode());
        QCOMPARE(window_->statusBar()->currentMessage(), QStringLiteral("Expert Mode disabled"));
        rufus::IsoWriteController disabledSession;
        QVERIFY(!disabledSession.expertMode());
        QVERIFY(controller_->enhancedInstall());
        QTest::keyClick(window_.get(), Qt::Key_E, Qt::AltModifier);
        QVERIFY(!controller_->enhancedInstall());
        QCOMPARE(window_->statusBar()->currentMessage(), QStringLiteral("Dual UEFI/BIOS mode disabled"));
        rufus::SelectionController disabledEnhancedSession;
        QVERIFY(!disabledEnhancedSession.enhancedInstall());
        QCOMPARE(helperStarts, 0);
        QCOMPARE(readFile(targetPath_), expectedTarget_);
    }

    void windowsBiosSelectionAndRequest_data()
    {
        QTest::addColumn<QString>("version");
        QTest::addColumn<bool>("efi");
        QTest::addColumn<bool>("rootEfiOnly");
        QTest::addColumn<bool>("dual");
        QTest::addColumn<bool>("rufusMbr");
        QTest::addColumn<int>("filesystem");
        QTest::addColumn<bool>("largeWim");
        for (const char* version : {"win7", "win8", "win8.1", "win10", "win11"})
        for (const char* layout : {"bios", "root-efi", "fallback-efi"})
        for (bool dual : {false, true})
        for (bool rufusMbr : {false, true})
        {
            const bool efi = QByteArray(layout) != "bios";
            const bool rootEfiOnly = QByteArray(layout) == "root-efi";
            const QByteArray name = QByteArray(version) + '-' + layout
                + (dual ? "-enhanced-" : "-ordinary-") + (rufusMbr ? "rufus" : "win7-mbr");
            QTest::newRow(name.constData()) << QString::fromLatin1(version) << efi << rootEfiOnly
                << dual << rufusMbr << int(FS_NTFS) << false;
            if (dual)
                for (bool large : {false, true})
                    QTest::newRow((name + (large ? "-FAT32-large-WIM" : "-FAT32-small-WIM")).constData())
                        << QString::fromLatin1(version) << efi << rootEfiOnly << dual << rufusMbr << int(FS_FAT32) << large;
        }
    }

    void windowsBiosSelectionAndRequest()
    {
        QFETCH(QString, version); QFETCH(bool, efi); QFETCH(bool, rootEfiOnly);
        QFETCH(bool, dual); QFETCH(bool, rufusMbr);
        QFETCH(int, filesystem); QFETCH(bool, largeWim);
        faultKind = largeWim ? LargeWindowsWim : NoFault;
        const QByteArray versionBytes = version.toLatin1();
        selectIsoFixture(false, true, versionBytes.constData(), nullptr, efi, false, nullptr, true, rootEfiOnly);
        QVERIFY(controller_->canSelectWindowsBios());
        QVERIFY(!controller_->canSelectGrub() && !controller_->canSelectSyslinux());
        auto* partition = window_->findChild<QComboBox*>(QStringLiteral("partitionCombo"));
        auto* target = window_->findChild<QComboBox*>(QStringLiteral("targetCombo"));
        auto* filesystems = window_->findChild<QComboBox*>(QStringLiteral("fileSystemCombo"));
        QVERIFY(partition && target && filesystems);
        QCOMPARE(partition->count(), efi ? 2 : 1);
        QCOMPARE(partition->currentText(), efi ? QStringLiteral("GPT") : QStringLiteral("MBR"));
        QCOMPARE(target->currentText(), efi ? QStringLiteral("UEFI (non CSM)") : QStringLiteral("BIOS (or UEFI-CSM)"));
        activateMainWindow();
        if (dual)
        {
            QTest::keyClick(window_.get(), Qt::Key_E, Qt::AltModifier);
            QVERIFY(controller_->enhancedInstall());
            QCOMPARE(partition->currentText(), QStringLiteral("MBR"));
            QCOMPARE(window_->statusBar()->currentMessage(), QStringLiteral("Dual UEFI/BIOS mode enabled"));
        }
        else if (efi)
        {
            partition->setCurrentIndex(partition->findData(int(rufus::PartitionScheme::Mbr)));
            QVERIFY(QMetaObject::invokeMethod(partition, "activated", Q_ARG(int, partition->currentIndex())));
        }
        if (!rufusMbr)
        {
            QTest::keyClick(window_.get(), Qt::Key_A, Qt::AltModifier);
            QCOMPARE(window_->statusBar()->currentMessage(), QStringLiteral("Use Rufus MBR disabled"));
        }
        QCOMPARE(controller_->windowsRufusMbr(), rufusMbr);
        rufus::SelectionController nextSession;
        QCOMPARE(nextSession.windowsRufusMbr(), rufusMbr);
        QCOMPARE(target->currentText(), dual && efi ? QStringLiteral("BIOS or UEFI") : QStringLiteral("BIOS (or UEFI-CSM)"));
        QCOMPARE(filesystems->count(), dual ? 3 : 1);
        QCOMPARE(filesystems->findData(FS_FAT16) >= 0, dual);
        controller_->setFileSystem(filesystem);
        const bool fat = filesystem == FS_FAT32;
        QCOMPARE(filesystems->currentText(), fat ? QStringLiteral("FAT32") : QStringLiteral("NTFS"));
        QCOMPARE(controller_->selection().clusterSize, fat ? quint32(512) : quint32(4096));
        helperMode = QStringLiteral("ok");
        startUncustomizedWindowsPlan();
        const auto& request = controller_->isoWriter().request();
        QVERIFY(request.windowsBios && request.mbr && !request.grub2 && !request.syslinux);
        QCOMPARE(request.windowsRufusMbr, rufusMbr);
        QCOMPARE(request.enhancedInstall, dual);
        QCOMPARE(request.fileSystem, filesystem);
        QCOMPARE(request.usesUefiNtfs(), dual && efi && !fat);
        QCOMPARE(controller_->isoWriter().check().splitInstallWim, fat && largeWim);
        QCOMPARE(!controller_->isoWriter().check().windowsEfiRepair.isEmpty(), rootEfiOnly && dual);
        const bool older = version != QStringLiteral("win10") && version != QStringLiteral("win11");
        if (older)
        {
            QVERIFY(!window_->findChild<rufus::WindowsUserDialog*>());
            QVERIFY(controller_->isoWriter().check().windowsInstall.editions.isEmpty());
        }
        if (fat)
        {
            QCOMPARE(controller_->isoWriter().check().partition.uefi_ntfs_size, quint64(0));
            QCOMPARE(controller_->isoWriter().check().partition.uefi_ntfs_offset, quint64(0));
        }
        QCOMPARE(request.windowsFlags, 0);
        const auto encoded = rufus::EncodePrepareRequest(request);
        const QString operation = fat ? QStringLiteral("prepare-windows-fat32-bios-device-v1")
            : dual && efi ? QStringLiteral("prepare-windows-ntfs-dual-device-v1")
            : QStringLiteral("prepare-windows-ntfs-bios-device-v1");
        QCOMPARE(encoded.value("operation").toString(), operation);
        auto* confirmation = window_->findChild<QMessageBox*>(QStringLiteral("isoWriteConfirmation"));
        QVERIFY(confirmation && confirmation->isVisible());
        const QString layout = (dual && efi ? QStringLiteral("MBR, %1, BIOS or UEFI") : QStringLiteral("MBR, %1, BIOS"))
            .arg(fat ? QStringLiteral("FAT32") : QStringLiteral("NTFS"));
        QVERIFY(confirmation->informativeText().contains(layout));
        QCOMPARE(helperStarts, 0);
        controller_->toggleWindowsMbr();
        controller_->toggleEnhancedInstall();
        QCOMPARE(controller_->windowsRufusMbr(), rufusMbr);
        QCOMPARE(controller_->enhancedInstall(), dual);
        approveIsoPlan();
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::Complete, writeTimeoutMs);
        QCOMPARE(helperStarts, 1);
        QCOMPARE(syslinuxToolChecks.load(), 0);
        QCOMPARE(stat(QStringLiteral("opens")), 0);
        QCOMPARE(progress_->value(), MAX_PROGRESS);
        QCOMPARE(progress_->format(), QStringLiteral("READY"));
        QCOMPARE(completionBeeps, 1);
        QCOMPARE(readFile(targetPath_), expectedTarget_);
    }

    void legacyWinPeSelectionAndRequest_data()
    {
        QTest::addColumn<QString>("family");
        QTest::addColumn<bool>("udf");
        QTest::addColumn<bool>("enhanced");
        QTest::addColumn<int>("filesystem");
        QTest::addColumn<QString>("rootKind");
        QTest::addColumn<bool>("syslinuxData");
        QTest::addColumn<bool>("grubData");
        for (const char* family : {"i386", "amd64", "minint", "i386-partial-minint", "amd64-partial-minint"})
            for (bool udf : {false, true})
                for (bool enhanced : {false, true})
                    for (int filesystem : {FS_NTFS, FS_FAT32})
                    for (const char* rootKind : {"absent", "identical", "prepatched"})
                    for (bool syslinuxData : {false, true})
                    {
                        if (!enhanced && filesystem == FS_FAT32) continue;
                        if (syslinuxData && !enhanced) continue;
                        const bool keepLoader = strcmp(family, "minint") == 0 || strstr(family, "-partial-minint") != nullptr;
                        if (keepLoader && strcmp(rootKind, "prepatched") == 0) continue;
                        QTest::newRow(qPrintable(QStringLiteral("%1-%2-%3-%4-%5-%6").arg(family).arg(udf).arg(enhanced).arg(filesystem).arg(rootKind).arg(syslinuxData)))
                            << QString::fromLatin1(family) << udf << enhanced << filesystem << QString::fromLatin1(rootKind) << syslinuxData << false;
                    }
        for (const char* family : {"i386", "minint"})
        for (bool udf : {false, true})
        for (int filesystem : {FS_FAT32, FS_NTFS})
        for (const char* rootKind : {"absent", "identical"})
            QTest::newRow(qPrintable(QStringLiteral("grub-%1-%2-%3-%4").arg(QString::fromLatin1(family)).arg(udf).arg(filesystem).arg(QString::fromLatin1(rootKind))))
                << QString::fromLatin1(family) << udf << true << filesystem << QString::fromLatin1(rootKind) << true << true;
    }

    void legacyWinPeSelectionAndRequest()
    {
        QFETCH(QString, family); QFETCH(bool, udf); QFETCH(bool, enhanced); QFETCH(int, filesystem);
        QFETCH(QString, rootKind);
        QFETCH(bool, syslinuxData);
        QFETCH(bool, grubData);
        const bool rootLoader = rootKind != QStringLiteral("absent");
        const bool fat32 = filesystem == FS_FAT32;
        if (syslinuxData) controller_->toggleEnhancedInstall();
        selectWinPeFixture(family, udf, false, rootLoader ? QStringLiteral("root-loader-") + rootKind : QString(), syslinuxData, grubData);
        const QString rootPath = directory_.filePath(QStringLiteral("iso-tree/BoOtMgR"));
        const QByteArray rootBefore = rootLoader ? readFile(rootPath) : QByteArray();
        if (rootKind == QStringLiteral("prepatched"))
            QVERIFY(rootBefore != readFile(directory_.filePath(QStringLiteral("iso-tree/i386/setupldr.bin"))));
        QCOMPARE(bool(HAS_BOOTMGR_BIOS(controller_->selection().imageAnalysis.report)), rootLoader);
        QCOMPARE(bool(controller_->selection().imageAnalysis.pending & ISO_PENDING_WINDOWS), rootLoader);
        const bool partialMinint = family.endsWith(QStringLiteral("-partial-minint"));
        if (partialMinint)
        {
            constexpr uint16_t minintSifMask = 0x100;
            const uint16_t familyMask = family.startsWith(QStringLiteral("amd64")) ? WINPE_AMD64 : WINPE_I386;
            const auto& report = controller_->selection().imageAnalysis.report;
            QCOMPARE(report.winpe, uint16_t(familyMask | minintSifMask));
            const WINPE_SETUP_PLAN plan = GetWinPeSetupPlan(report.winpe, report.uses_minint);
            QCOMPARE(plan.action, WINPE_KEEP_LOADER);
            QVERIFY(!plan.copy_sif && !plan.masquerade_disk);
        }
        QVERIFY(controller_->canSelectWindowsBios());
        QVERIFY(!controller_->canSelectGrub() && !controller_->canSelectSyslinux());
        if (enhanced && !syslinuxData) controller_->toggleEnhancedInstall();
        // Toggling retains NTFS; loading with Alt+E uses the shared FAT32 preference.
        QCOMPARE(controller_->enhancedInstall(), enhanced);
        if (enhanced) controller_->toggleWindowsMbr();
        auto* partitions = window_->findChild<QComboBox*>(QStringLiteral("partitionCombo"));
        auto* filesystems = window_->findChild<QComboBox*>(QStringLiteral("fileSystemCombo"));
        QCOMPARE(partitions->count(), 1);
        QCOMPARE(partitions->currentText(), QStringLiteral("MBR"));
        QCOMPARE(window_->findChild<QComboBox*>(QStringLiteral("targetCombo"))->currentText(), QStringLiteral("BIOS (or UEFI-CSM)"));
        QCOMPARE(filesystems->count(), enhanced ? 3 : 1);
        QCOMPARE(filesystems->currentData().toInt(), int(syslinuxData ? FS_FAT32 : FS_NTFS));
        QCOMPARE(filesystems->findData(FS_FAT16) >= 0, enhanced);
        QCOMPARE(controller_->selection().clusterSize, quint32(syslinuxData ? 512 : 4096));
        QCOMPARE(controller_->selection().volumeLabel, QStringLiteral("WINPE"));
        QCOMPARE(progress_->format(), QStringLiteral("READY"));
        QCOMPARE(progress_->value(), 0); // Idle READY is empty, as on Win32.
        QCOMPARE(controller_->selection().imageAnalysis.report.win_version.major, uint32_t(0));
        QVERIFY(!window_->findChild<QWidget*>(QStringLiteral("imageOptions"))->isVisible());
        controller_->setFileSystem(FS_FAT16);
        controller_->setFileSystem(filesystem);
        controller_->setPartitionScheme(rufus::PartitionScheme::Gpt);
        QCOMPARE(controller_->selection().fileSystem, filesystem);
        QCOMPARE(controller_->selection().clusterSize, quint32(fat32 ? 512 : 4096));
        QCOMPARE(controller_->selection().partition, rufus::PartitionScheme::Mbr);
        const QString screenshot = qEnvironmentVariable("RUFUS_TEST_WINPE_SCREENSHOT");
        const bool capture = !screenshot.isEmpty() && family == QStringLiteral("i386") && !udf && (fat32 || !enhanced);
        const QString screenshotPrefix = screenshot + (fat32 ? QStringLiteral("-fat32") : QString());
        if (capture) QVERIFY(window_->grab().save(screenshotPrefix + QStringLiteral("-selection.png")));
        helperMode = QStringLiteral("ok");
        QTest::mouseClick(start_, Qt::LeftButton);
        if (grubData)
        {
            QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::SelectingGrubCore, writeTimeoutMs);
            auto* download = window_->findChild<QMessageBox*>(QStringLiteral("grubDownloadDialog"));
            QVERIFY(download && download->isVisible());
            QVERIFY(download->text().contains(QStringLiteral("This image uses Grub 2.12")));
            QCOMPARE(helperStarts, 0);
            QTest::mouseClick(download->button(QMessageBox::No), Qt::LeftButton);
        }
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::AwaitingConfirmation, writeTimeoutMs);
        const auto& writer = controller_->isoWriter();
        const auto& request = writer.request();
        QVERIFY(writer.check().legacyWinPe);
        QCOMPARE(bool(HAS_BOOTMGR_BIOS(writer.check().analysis.report)), rootLoader);
        if (rootLoader)
        {
            QCOMPARE(readFile(rootPath), rootBefore);
            QCOMPARE(readFile(directory_.filePath(QStringLiteral("iso-tree/NtDetect.Com"))),
                QByteArray("different existing root detector"));
        }
        QCOMPARE(writer.check().windows.state, WINDOWS_IMAGE_UNKNOWN);
        QVERIFY(request.windowsBios && request.mbr && !request.usesUefiNtfs());
        QCOMPARE(request.enhancedInstall, enhanced);
        QCOMPARE(request.fileSystem, filesystem);
        QVERIFY(!request.syslinux && !request.grub2 && !request.hasWindowsInstallSettings());
        QCOMPARE(request.windowsFlags, 0);
        QCOMPARE(request.windowsRufusMbr, !enhanced);
        const QString operation = fat32 ? QStringLiteral("prepare-windows-fat32-bios-device-v1")
            : QStringLiteral("prepare-windows-ntfs-bios-device-v1");
        QCOMPARE(rufus::EncodePrepareRequest(request).value("operation").toString(), operation);
        QVERIFY(!window_->findChild<rufus::WindowsUserDialog*>());
        auto* confirmation = window_->findChild<QMessageBox*>(QStringLiteral("isoWriteConfirmation"));
        const QString detail = fat32 ? QStringLiteral("MBR, FAT32, BIOS") : QStringLiteral("MBR, NTFS, BIOS");
        QVERIFY(confirmation && confirmation->informativeText().contains(detail));
        QVERIFY(!confirmation->informativeText().contains(QStringLiteral("BIOS or UEFI")));
        QCOMPARE(helperStarts, 0);
        if (syslinuxData)
        {
            QVERIFY(HAS_SYSLINUX(writer.check().analysis.report));
            QCOMPARE(bool(writer.check().analysis.report.has_grub2), grubData);
            QCOMPARE(bool(writer.check().analysis.report.has_grub4dos), grubData);
            QVERIFY(!request.hasGrub2Core());
            QCOMPARE(writer.check().analysis.report.sl_detected_version, quint16(0));
            QCOMPARE(writer.check().analysis.report.sl_version, quint16(0));
            QVERIFY(!writer.check().analysis.report.sl_version_inferred);
            helperCopiedBytes = writer.check().tree.bytes_written; // Protocol fixture emits the fresh copy denominator.
        }
        approveIsoPlan();
        QTRY_COMPARE_WITH_TIMEOUT(writer.state(), rufus::IsoWriteState::Complete, writeTimeoutMs);
        QCOMPARE(helperStarts, 1);
        QCOMPARE(progress_->format(), QStringLiteral("READY"));
        QCOMPARE(progress_->value(), MAX_PROGRESS);
        QCOMPARE(completionBeeps, 1);
        if (capture) QVERIFY(window_->grab().save(screenshotPrefix + QStringLiteral("-ready.png")));
        QCOMPARE(readFile(targetPath_), expectedTarget_);
        if (syslinuxData) QCOMPARE(syslinuxToolChecks.load(), 0);
    }

    void legacySyslinuxChoiceRefresh_data()
    {
        QTest::addColumn<bool>("udf");
        QTest::addColumn<bool>("hybrid");
        QTest::addColumn<bool>("grubData");
        for (bool udf : {false, true})
        for (bool hybrid : {false, true})
        for (bool grubData : {false, true})
            QTest::newRow(qPrintable(QStringLiteral("%1-%2-%3").arg(udf).arg(hybrid).arg(grubData))) << udf << hybrid << grubData;
    }

    void legacySyslinuxChoiceRefresh()
    {
        QFETCH(bool, udf); QFETCH(bool, hybrid); QFETCH(bool, grubData);
        selectWinPeFixture(QStringLiteral("i386"), udf, hybrid, QStringLiteral("root-loader-prepatched"), true, grubData);
        const auto report = controller_->selection().imageAnalysis.report;
        QVERIFY(HAS_SYSLINUX(report) && HAS_WINPE(report) && HAS_BOOTMGR_BIOS(report));
        QCOMPARE(bool(HAS_GRUB(report)), grubData);
        QCOMPARE(report.sl_detected_version, quint16(0));
        QVERIFY(!controller_->canSelectWindowsBios());
        QVERIFY(controller_->canSelectSyslinux());
        const auto choices = controller_->isoWriteChoices(0, false, false);
        QCOMPARE(choices.count, hybrid ? 2 : 1);
        QCOMPARE(choices.mode[0], IMAGE_WRITE_ISO);
        activateMainWindow();
        QTest::keyClick(window_.get(), Qt::Key_E, Qt::AltModifier);
        QVERIFY(controller_->enhancedInstall());
        QVERIFY(controller_->canSelectWindowsBios());
        QCOMPARE(controller_->selection().partition, rufus::PartitionScheme::Mbr);
        QCOMPARE(controller_->selection().fileSystem, int(FS_NTFS));
        auto* filesystem = window_->findChild<QComboBox*>(QStringLiteral("fileSystemCombo"));
        QCOMPARE(filesystem->count(), 3);
        QVERIFY(filesystem->findData(FS_FAT16) >= 0);
        filesystem->setCurrentIndex(filesystem->findData(FS_NTFS));
        QVERIFY(QMetaObject::invokeMethod(filesystem, "activated", Q_ARG(int, filesystem->currentIndex())));
        QCOMPARE(controller_->selection().fileSystem, int(FS_NTFS));
        QTest::keyClick(window_.get(), Qt::Key_E, Qt::AltModifier);
        QVERIFY(!controller_->canSelectWindowsBios());
        QVERIFY(controller_->canSelectSyslinux());
        QCOMPARE(controller_->isoWriteChoices(0, false, false).count, hybrid ? 2 : 1);
        QTest::keyClick(window_.get(), Qt::Key_E, Qt::AltModifier);
        QVERIFY(controller_->canSelectWindowsBios());
        QCOMPARE(controller_->selection().fileSystem, int(FS_NTFS));
        QCOMPARE(controller_->selection().clusterSize, quint32(4096));
        controller_->selectImage(sourcePath_);
        QTRY_VERIFY_WITH_TIMEOUT(!controller_->isAnalyzing(), writeTimeoutMs);
        QCOMPARE(controller_->selection().fileSystem, int(FS_FAT32));
        QVERIFY(HAS_SYSLINUX(controller_->selection().imageAnalysis.report));
        rufus::SelectionController nextSession;
        QVERIFY(nextSession.enhancedInstall());
        QVERIFY(!window_->findChild<QWidget*>(QStringLiteral("imageOptions"))->isVisible());
        QCOMPARE(helperStarts, 0);
        QCOMPARE(syslinuxToolChecks.load(), 0);
    }

    void legacyGrub2SbrFlow_data()
    {
        QTest::addColumn<QString>("mode");
        QTest::addColumn<QString>("family");
        QTest::addColumn<bool>("udf");
        QTest::addColumn<bool>("rufusMbr");
        QTest::addColumn<int>("filesystem");
        for (const char* mode : {"no", "cache", "download", "cancel", "bad-signature",
            "source-change", "unusable-cache", "embedded", "unknown"})
        for (int filesystem : {FS_NTFS, FS_FAT32})
        for (bool udf : {false, true})
        for (bool rufusMbr : {false, true})
            QTest::newRow(qPrintable(QStringLiteral("%1-%2-%3-%4").arg(QString::fromLatin1(mode)).arg(udf).arg(rufusMbr).arg(filesystem)))
                << QString::fromLatin1(mode) << QStringLiteral("minint") << udf << rufusMbr << filesystem;
        for (const char* family : {"i386", "amd64"})
        for (const char* mode : {"no", "cancel", "bad-signature"})
        for (int filesystem : {FS_NTFS, FS_FAT32})
        for (bool udf : {false, true})
            QTest::newRow(qPrintable(QStringLiteral("patch-%1-%2-%3-%4").arg(QString::fromLatin1(family))
                .arg(QString::fromLatin1(mode)).arg(udf).arg(filesystem)))
                << QString::fromLatin1(mode) << QString::fromLatin1(family) << udf << false << filesystem;
        for (const char* family : {"i386-minint-options", "amd64-minint-options"})
        for (int filesystem : {FS_NTFS, FS_FAT32})
        for (bool udf : {false, true})
            QTest::newRow(qPrintable(QStringLiteral("patch-%1-no-%2-%3").arg(QString::fromLatin1(family)).arg(udf).arg(filesystem)))
                << QStringLiteral("no") << QString::fromLatin1(family) << udf << false << filesystem;
        for (const char* family : {"i386-partial-minint", "amd64-partial-minint"})
        for (int filesystem : {FS_NTFS, FS_FAT32})
        for (bool udf : {false, true})
            QTest::newRow(qPrintable(QStringLiteral("%1-%2-%3").arg(QString::fromLatin1(family)).arg(udf).arg(filesystem)))
                << QStringLiteral("no") << QString::fromLatin1(family) << udf << true << filesystem;
    }

    void legacyGrub2SbrFlow()
    {
        QFETCH(QString, mode); QFETCH(QString, family); QFETCH(bool, udf); QFETCH(bool, rufusMbr); QFETCH(int, filesystem);
        controller_->toggleEnhancedInstall();
        const QByteArray version = mode == QStringLiteral("unknown") ? QByteArray()
            : mode == QStringLiteral("embedded") ? QByteArray(GRUB2_PACKAGE_VERSION) : QByteArray("2.12");
        selectWinPeFixture(family, udf, false, {}, false, true, version.constData());
        if (mode == QStringLiteral("unknown"))
        {
            const auto& report = controller_->selection().imageAnalysis.report;
            QCOMPARE(report.grub2_version[0], '\0');
            // Original scanning clears an unrecognized directory-only GRUB2
            // provider. The remaining GRUB4DOS provider uses bundled boot records.
            QCOMPARE(GetSecondaryBootType(BT_IMAGE, &report, false), int(BT_GRUB4DOS));
            QVERIFY(controller_->canSelectWindowsBios() && start_->isEnabled());
            if (!rufusMbr) controller_->toggleWindowsMbr();
            controller_->setFileSystem(filesystem);
            QSignalSpy warnings(&controller_->isoWriter(), &rufus::IsoWriteController::grubDownloadRequired);
            QTest::mouseClick(start_, Qt::LeftButton);
            auto& writer = controller_->isoWriter();
            QTRY_COMPARE_WITH_TIMEOUT(writer.state(), rufus::IsoWriteState::AwaitingConfirmation, writeTimeoutMs);
            QCOMPARE(warnings.size(), 0);
            QCOMPARE(writer.check().analysis.report.grub2_version[0], '\0');
            QCOMPARE(GetSecondaryBootType(BT_IMAGE, &writer.check().analysis.report, false), int(BT_GRUB4DOS));
            QVERIFY(writer.grubCoreResult().bytes.isEmpty() && !writer.request().hasGrub2Core());
            QCOMPARE(writer.request().windowsRufusMbr, rufusMbr);
            QCOMPARE(writer.request().fileSystem, filesystem);
            helperCopiedBytes = writer.check().tree.bytes_written;
            approveIsoPlan();
            QTRY_COMPARE_WITH_TIMEOUT(writer.state(), rufus::IsoWriteState::Complete, writeTimeoutMs);
            QCOMPARE(helperStarts, 1);
            QCOMPARE(completionBeeps, 1);
            QCOMPARE(progress_->format(), QStringLiteral("READY"));
            QCOMPARE(progress_->value(), MAX_PROGRESS);
            return;
        }
        QVERIFY(controller_->canSelectWindowsBios());
        QVERIFY(!controller_->canSelectGrub() && !controller_->canSelectSyslinux());
        QCOMPARE(controller_->selection().partition, rufus::PartitionScheme::Mbr);
        QCOMPARE(controller_->selection().fileSystem, int(FS_FAT32));
        auto* filesystems = window_->findChild<QComboBox*>(QStringLiteral("fileSystemCombo"));
        QCOMPARE(filesystems->count(), 3);
        QVERIFY(filesystems->findData(FS_FAT32) >= 0 && filesystems->findData(FS_NTFS) >= 0);
        QVERIFY(filesystems->findData(FS_FAT16) >= 0);
        if (!rufusMbr) controller_->toggleWindowsMbr();
        controller_->setFileSystem(filesystem);
        QCOMPARE(controller_->selection().fileSystem, filesystem);
        GrubDownloadServer server;
        server.core = readFile(QStringLiteral(RUFUS_SIGNED_FIXTURE "/core.img"));
        server.signature = readFile(QStringLiteral(RUFUS_SIGNED_FIXTURE "/core.img.sig"));
        server.mode = mode == QStringLiteral("bad-signature") ? mode : QStringLiteral("download");
        QVERIFY(server.listen(QHostAddress::LocalHost));
        const QString files = directory_.filePath(QStringLiteral("windows-grub-files"));
        QVERIFY(!QFileInfo::exists(files) || QDir(files).removeRecursively());
        const QString cached = files + QStringLiteral("/grub-2.12/core.img");
        const bool cache = mode == QStringLiteral("cache") || mode == QStringLiteral("unusable-cache");
        if (cache)
        {
            QVERIFY(QDir().mkpath(QFileInfo(cached).absolutePath()));
            writeFile(cached, mode == QStringLiteral("cache") ? server.core : QByteArray("short"));
        }
        controller_->isoWriter().configure(*connection_, QCoreApplication::applicationDirPath() + QStringLiteral("/iso_helper_fixture"),
            files, server.root());
        helperMode = QStringLiteral("ok");
        QSignalSpy warnings(&controller_->isoWriter(), &rufus::IsoWriteController::grubDownloadRequired);
        QTest::mouseClick(start_, Qt::LeftButton);
        const bool prompt = !cache && mode != QStringLiteral("embedded") && mode != QStringLiteral("unknown");
        if (prompt)
        {
            QTRY_COMPARE_WITH_TIMEOUT(warnings.size(), 1, writeTimeoutMs);
            auto* dialog = window_->findChild<QMessageBox*>(QStringLiteral("grubDownloadDialog"));
            QVERIFY(dialog && dialog->isVisible());
            QCOMPARE(helperStarts, 0);
            if (mode == QStringLiteral("source-change"))
                writeFile(sourcePath_, sourceBytes_ + "changed");
            const auto choice = mode == QStringLiteral("cancel") ? QMessageBox::Cancel
                : mode == QStringLiteral("download") || mode == QStringLiteral("bad-signature") ? QMessageBox::Yes : QMessageBox::No;
            QTest::mouseClick(dialog->button(choice), Qt::LeftButton);
        }
        const auto expected = mode == QStringLiteral("cancel") ? rufus::IsoWriteState::Cancelled
            : mode == QStringLiteral("bad-signature") || mode == QStringLiteral("source-change") ? rufus::IsoWriteState::Failed
            : rufus::IsoWriteState::AwaitingConfirmation;
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), expected, writeTimeoutMs);
        QCOMPARE(warnings.size(), int(prompt));
        QCOMPARE(helperStarts, 0);
        QCOMPARE(stat(QStringLiteral("opens")), 0);
        if (expected != rufus::IsoWriteState::AwaitingConfirmation) return;
        const auto& writer = controller_->isoWriter();
        const auto& request = writer.request();
        const auto& report = writer.check().analysis.report;
        QVERIFY(request.windowsBios && request.enhancedInstall && request.mbr && !request.grub2 && !request.syslinux);
        QCOMPARE(request.windowsRufusMbr, rufusMbr);
        QCOMPARE(request.fileSystem, filesystem);
        QCOMPARE(request.clusterSize, quint32(filesystem == FS_FAT32 ? 512 : 4096));
        QCOMPARE(GetSecondaryBootType(BT_IMAGE, &report, false), int(BT_GRUB2));
        const bool minintOptions = family.endsWith(QStringLiteral("-minint-options"));
        const bool patch = family == QStringLiteral("i386") || family == QStringLiteral("amd64") || minintOptions;
        const WINPE_SETUP_PLAN setup = GetWinPeSetupPlan(report.winpe, report.uses_minint);
        QCOMPARE(setup.action, patch ? WINPE_PATCH_LOADER : WINPE_KEEP_LOADER);
        QCOMPARE(bool(setup.masquerade_disk), patch && !minintOptions);
        QCOMPARE(QString::fromLatin1(report.grub2_version), QString::fromLatin1(version));
        QCOMPARE(writer.check().windows.state, WINDOWS_IMAGE_UNKNOWN);
        const auto& selected = writer.grubCoreResult();
        const bool external = mode == QStringLiteral("cache") || mode == QStringLiteral("download");
        if (external)
        {
            QCOMPARE(selected.bytes, server.core);
            QCOMPARE(selected.origin, mode == QStringLiteral("cache") ? rufus::Grub2CoreOrigin::Cache : rufus::Grub2CoreOrigin::Download);
            writeFile(cached, QByteArray(server.core.size(), 'x'));
        }
        else
        {
            QCOMPARE(selected.origin, rufus::Grub2CoreOrigin::Embedded);
            QCOMPARE(selected.bytes.size(), 42742);
            QCOMPARE(selected.sha256.toHex(), QByteArray("9a2c946704017fa8dc4e03a8a58d754d2d1607c2d2cd74f0e2920133f1192809"));
        }
        QCOMPARE(selected.sha256, QCryptographicHash::hash(selected.bytes, QCryptographicHash::Sha256));
        QVERIFY(!request.hasGrub2Core()); // Existing process staging owns the temporary path.
        expectedGrubCore = selected.sha256;
        helperCopiedBytes = writer.check().tree.bytes_written;
        approveIsoPlan();
        QTRY_VERIFY_WITH_TIMEOUT(!controller_->isoWriter().isActive(), writeTimeoutMs);
        QVERIFY2(controller_->isoWriter().state() == rufus::IsoWriteState::Complete,
            qPrintable(controller_->isoWriter().statusText()));
        QCOMPARE(helperStarts, 1);
        QCOMPARE(progress_->value(), MAX_PROGRESS);
        QCOMPARE(progress_->format(), QStringLiteral("READY"));
        QCOMPARE(readFile(sourcePath_), sourceBytes_);
        if (mode != QStringLiteral("download")) QVERIFY(server.requests.isEmpty());
    }

    void legacyGrub4dosSbrFlow_data()
    {
        QTest::addColumn<QString>("family"); QTest::addColumn<bool>("udf");
        QTest::addColumn<int>("filesystem"); QTest::addColumn<bool>("rufusMbr");
        QTest::addColumn<QString>("mode");
        for (const char* family : {"minint", "i386", "amd64", "i386-minint-options"})
        for (bool udf : {false, true})
        for (int filesystem : {FS_FAT32, FS_NTFS})
        for (bool rufusMbr : {false, true})
        {
            const QByteArray name = QByteArray(family) + (udf ? "-udf-" : "-iso-")
                + QByteArray::number(filesystem) + (rufusMbr ? "-rufus" : "-win7");
            QTest::newRow(name.constData()) << QString::fromLatin1(family) << udf << filesystem << rufusMbr << QStringLiteral("ok");
        }
        for (const char* mode : {"confirmation-cancel", "source-change", "sync-error", "cleanup-error", "hold-after-result", "hold-bootloader"})
        for (int filesystem : {FS_FAT32, FS_NTFS})
        {
            const QByteArray name = QByteArray(mode) + '-' + QByteArray::number(filesystem);
            QTest::newRow(name.constData()) << QStringLiteral("amd64") << false << filesystem << true << QString::fromLatin1(mode);
        }
    }

    void legacyGrub4dosSbrFlow()
    {
        QFETCH(QString, family); QFETCH(bool, udf); QFETCH(int, filesystem);
        QFETCH(bool, rufusMbr); QFETCH(QString, mode);
        controller_->toggleEnhancedInstall();
        selectWinPeFixture(family, udf, false, {}, false, false, "2.12", true);
        QVERIFY(controller_->canSelectWindowsBios() && start_->isEnabled());
        QVERIFY(!controller_->canSelectGrub() && !controller_->canSelectSyslinux());
        QCOMPARE(controller_->selection().partition, rufus::PartitionScheme::Mbr);
        QCOMPARE(controller_->selection().fileSystem, int(FS_FAT32));
        auto* filesystems = window_->findChild<QComboBox*>(QStringLiteral("fileSystemCombo"));
        QCOMPARE(filesystems->count(), 3);
        QVERIFY(filesystems->findData(FS_FAT32) >= 0 && filesystems->findData(FS_NTFS) >= 0);
        QVERIFY(filesystems->findData(FS_FAT16) >= 0);
        QCOMPARE(window_->findChild<QComboBox*>(QStringLiteral("targetCombo"))->currentText(), QStringLiteral("BIOS (or UEFI-CSM)"));
        QVERIFY(window_->findChild<QWidget*>(QStringLiteral("imageOptions"))->isHidden());
        if (!rufusMbr) controller_->toggleWindowsMbr();
        controller_->setFileSystem(filesystem);
        helperMode = mode;
        auto& writer = controller_->isoWriter();
        QSignalSpy warnings(&writer, &rufus::IsoWriteController::grubDownloadRequired);
        QTest::mouseClick(start_, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(writer.state(), rufus::IsoWriteState::AwaitingConfirmation, writeTimeoutMs);
        QCOMPARE(warnings.size(), 0);
        QCOMPARE(helperStarts, 0);
        QCOMPARE(stat(QStringLiteral("opens")), 0);
        const auto& request = writer.request();
        const auto& report = writer.check().analysis.report;
        QVERIFY(request.windowsBios && request.enhancedInstall && request.mbr && !request.grub2 && !request.syslinux);
        QCOMPARE(request.windowsRufusMbr, rufusMbr);
        QCOMPARE(request.fileSystem, filesystem);
        QCOMPARE(request.clusterSize, quint32(filesystem == FS_FAT32 ? 512 : 4096));
        QVERIFY(report.has_grub4dos && !report.has_grub2 && !HAS_SYSLINUX(report));
        QCOMPARE(report.grub2_version[0], '\0');
        QCOMPARE(GetSecondaryBootType(BT_IMAGE, &report, false), int(BT_GRUB4DOS));
        QCOMPARE(writer.check().windows.state, WINDOWS_IMAGE_UNKNOWN);
        QVERIFY(writer.grubCoreResult().bytes.isEmpty() && !request.hasGrub2Core());
        const bool minintOptions = family.endsWith(QStringLiteral("-minint-options"));
        const bool patch = family != QStringLiteral("minint");
        const WINPE_SETUP_PLAN setup = GetWinPeSetupPlan(report.winpe, report.uses_minint);
        QCOMPARE(setup.action, patch ? WINPE_PATCH_LOADER : WINPE_KEEP_LOADER);
        QCOMPARE(bool(setup.masquerade_disk), patch && !minintOptions);
        auto* confirmation = window_->findChild<QMessageBox*>(QStringLiteral("isoWriteConfirmation"));
        QVERIFY(confirmation && confirmation->isVisible());
        QCOMPARE(confirmation->defaultButton(), confirmation->button(QMessageBox::Cancel));
        QVERIFY(confirmation->informativeText().contains(filesystem == FS_FAT32
            ? QStringLiteral("MBR, FAT32, BIOS") : QStringLiteral("MBR, NTFS, BIOS")));
        if (mode == QStringLiteral("confirmation-cancel"))
        {
            QTest::mouseClick(confirmation->button(QMessageBox::Cancel), Qt::LeftButton);
            QTRY_COMPARE_WITH_TIMEOUT(writer.state(), rufus::IsoWriteState::Cancelled, writeTimeoutMs);
            QCOMPARE(helperStarts, 0);
            QCOMPARE(completionBeeps, 0);
            return;
        }
        if (mode == QStringLiteral("source-change")) writeFile(sourcePath_, sourceBytes_ + "changed");
        helperCopiedBytes = writer.check().tree.bytes_written;
        approveIsoPlan();
        if (mode == QStringLiteral("hold-after-result"))
        {
            QTRY_COMPARE_WITH_TIMEOUT(writer.outcome().copiedBytes, helperCopiedBytes, writeTimeoutMs);
            QCOMPARE(writer.state(), rufus::IsoWriteState::Writing);
            QVERIFY(progress_->format() != QStringLiteral("READY") && progress_->value() < MAX_PROGRESS);
            QCOMPARE(completionBeeps, 0);
            QVERIFY(helperProcess);
            helperProcess->write("exit\n");
        }
        if (mode == QStringLiteral("hold-bootloader"))
        {
            QTRY_COMPARE_WITH_TIMEOUT(writer.statusText(), QStringLiteral("Writing partition boot record..."), writeTimeoutMs);
            writer.cancel();
            QTRY_COMPARE_WITH_TIMEOUT(writer.state(), rufus::IsoWriteState::Cancelled, cancelTimeoutMs);
        }
        else
        {
            const bool failure = mode == QStringLiteral("source-change") || mode == QStringLiteral("sync-error")
                || mode == QStringLiteral("cleanup-error");
            QTRY_COMPARE_WITH_TIMEOUT(writer.state(), failure ? rufus::IsoWriteState::Failed : rufus::IsoWriteState::Complete, writeTimeoutMs);
            if (mode == QStringLiteral("sync-error") || mode == QStringLiteral("cleanup-error"))
                QCOMPARE(writer.outcome().error, mode == QStringLiteral("sync-error") ? EIO : EBUSY);
        }
        const bool complete = writer.state() == rufus::IsoWriteState::Complete;
        QCOMPARE(helperStarts, mode == QStringLiteral("source-change") ? 0 : 1);
        QCOMPARE(completionBeeps, int(complete));
        if (complete)
        {
            QCOMPARE(progress_->format(), QStringLiteral("READY"));
            QCOMPARE(progress_->value(), MAX_PROGRESS);
            controller_->setDiscovery({}, {});
            QCOMPARE(progress_->format(), QStringLiteral("READY"));
            QCOMPARE(completionBeeps, 1);
        }
        else QVERIFY(progress_->format() != QStringLiteral("READY"));
        QCOMPARE(warnings.size(), 0);
        QCOMPARE(readFile(targetPath_), expectedTarget_);
        if (mode != QStringLiteral("source-change")) QCOMPARE(readFile(sourcePath_), sourceBytes_);
    }

    void legacyGrub4dosChoiceRefresh_data()
    {
        QTest::addColumn<bool>("hybrid"); QTest::addColumn<QString>("family");
        for (bool hybrid : {false, true})
        for (const char* family : {"minint", "i386", "amd64", "i386-minint-options"})
        {
            const QByteArray name = QByteArray(family) + (hybrid ? "-hybrid" : "-iso");
            QTest::newRow(name.constData()) << hybrid << QString::fromLatin1(family);
        }
    }

    void legacyGrub4dosChoiceRefresh()
    {
        QFETCH(bool, hybrid); QFETCH(QString, family);
        selectWinPeFixture(family, false, hybrid, {}, false, false, "2.12", true);
        QVERIFY(!controller_->canSelectWindowsBios());
        QVERIFY(controller_->canSelectGrub() && start_->isEnabled());
        controller_->toggleEnhancedInstall();
        QVERIFY(controller_->canSelectWindowsBios() && start_->isEnabled());
        QCOMPARE(controller_->selection().fileSystem, int(FS_NTFS));
        controller_->setFileSystem(FS_FAT32);
        controller_->toggleEnhancedInstall();
        QVERIFY(!controller_->canSelectWindowsBios());
        QVERIFY(controller_->canSelectGrub() && start_->isEnabled());
        const auto choices = controller_->isoWriteChoices(0, false, false);
        QCOMPARE(choices.count, hybrid ? 2 : 1);
        QCOMPARE(choices.mode[0], IMAGE_WRITE_ISO);
        controller_->toggleEnhancedInstall();
        QVERIFY(controller_->canSelectWindowsBios() && start_->isEnabled());
        QCOMPARE(controller_->selection().partition, rufus::PartitionScheme::Mbr);
        QCOMPARE(controller_->selection().fileSystem, int(FS_NTFS));
        QCOMPARE(controller_->selection().fileSystems, quint32(FS_MASK(FS_FAT16) | FS_MASK(FS_FAT32) | FS_MASK(FS_NTFS)));
        QCOMPARE(helperStarts, 0);
    }

    void legacyGrub2ChoiceRefresh_data()
    {
        QTest::addColumn<bool>("hybrid");
        QTest::newRow("iso") << false;
        QTest::newRow("hybrid") << true;
    }

    void legacyGrub2ChoiceRefresh()
    {
        QFETCH(bool, hybrid);
        controller_->toggleEnhancedInstall();
        selectWinPeFixture(QStringLiteral("minint"), false, hybrid, {}, false, true);
        QVERIFY(controller_->canSelectWindowsBios());
        QCOMPARE(controller_->selection().fileSystem, int(FS_FAT32));
        controller_->setFileSystem(FS_NTFS);
        controller_->toggleEnhancedInstall();
        QVERIFY(!controller_->canSelectWindowsBios());
        QVERIFY(controller_->canSelectGrub() && start_->isEnabled());
        const auto choices = controller_->isoWriteChoices(0, false, false);
        QCOMPARE(choices.count, hybrid ? 2 : 1);
        QCOMPARE(choices.mode[0], IMAGE_WRITE_ISO);
        controller_->toggleEnhancedInstall();
        QVERIFY(controller_->canSelectWindowsBios() && start_->isEnabled());
        QCOMPARE(controller_->selection().partition, rufus::PartitionScheme::Mbr);
        QCOMPARE(controller_->selection().fileSystem, int(FS_NTFS));
        QCOMPARE(controller_->selection().fileSystems, quint32(FS_MASK(FS_FAT16) | FS_MASK(FS_FAT32) | FS_MASK(FS_NTFS)));
        QCOMPARE(helperStarts, 0);
    }

    void legacyGrubDataPromptCancel()
    {
        controller_->toggleEnhancedInstall();
        selectWinPeFixture(QStringLiteral("i386"), false, false, QStringLiteral("root-loader-prepatched"), true, true);
        QTest::mouseClick(start_, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::SelectingGrubCore, writeTimeoutMs);
        auto* download = window_->findChild<QMessageBox*>(QStringLiteral("grubDownloadDialog"));
        QVERIFY(download && download->isVisible());
        QTest::mouseClick(download->button(QMessageBox::Cancel), Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::Cancelled, writeTimeoutMs);
        QCOMPARE(helperStarts, 0);
        QCOMPARE(completionBeeps, 0);
        QVERIFY(!window_->findChild<QMessageBox*>(QStringLiteral("isoWriteConfirmation")));
        QCOMPARE(readFile(targetPath_), expectedTarget_);
    }

    void legacyWinPeAdvancedChoiceRefresh()
    {
        selectWinPeFixture(QStringLiteral("i386"), false);
        activateMainWindow();
        QTest::keyClick(window_.get(), Qt::Key_E, Qt::AltModifier);
        QVERIFY(controller_->enhancedInstall());
        QCOMPARE(controller_->selection().fileSystem, int(FS_NTFS));
        auto* filesystem = window_->findChild<QComboBox*>(QStringLiteral("fileSystemCombo"));
        filesystem->setCurrentIndex(filesystem->findData(FS_FAT32));
        QVERIFY(QMetaObject::invokeMethod(filesystem, "activated", Q_ARG(int, filesystem->currentIndex())));
        QCOMPARE(controller_->selection().fileSystem, int(FS_FAT32));
        QTest::keyClick(window_.get(), Qt::Key_E, Qt::AltModifier);
        QVERIFY(!controller_->enhancedInstall());
        QCOMPARE(controller_->selection().fileSystem, int(FS_NTFS));
        QCOMPARE(filesystem->count(), 1);
        QTest::keyClick(window_.get(), Qt::Key_E, Qt::AltModifier);
        QCOMPARE(controller_->selection().fileSystem, int(FS_NTFS));
        QCOMPARE(filesystem->count(), 3);
        // A saved Alt+E preference on the next image load follows Rufus's
        // existing advanced FAT32 preference; ordinary mode defaults to NTFS.
        controller_->selectImage(sourcePath_);
        QTRY_VERIFY_WITH_TIMEOUT(!controller_->isAnalyzing(), writeTimeoutMs);
        QCOMPARE(controller_->selection().fileSystem, int(FS_FAT32));
        QCOMPARE(window_->findChild<QComboBox*>(QStringLiteral("targetCombo"))->currentText(), QStringLiteral("BIOS (or UEFI-CSM)"));
        rufus::SelectionController nextSession;
        QVERIFY(nextSession.enhancedInstall());
        QCOMPARE(helperStarts, 0);
    }

    void legacyWinPeFatRootLoaderPreserved_data()
    {
        QTest::addColumn<QString>("loader");
        QTest::addColumn<bool>("udf");
        QTest::addColumn<bool>("grubData");
        for (const char* loader : {"ntldr", "io.sys", "msdos.sys"})
            for (bool grubData : {false, true})
            for (bool udf : {false, true})
                QTest::newRow(qPrintable(QStringLiteral("%1-%2-%3").arg(loader).arg(udf).arg(grubData))) << QString::fromLatin1(loader) << udf << grubData;
    }

    void legacyWinPeFatRootLoaderPreserved()
    {
        QFETCH(QString, loader); QFETCH(bool, udf); QFETCH(bool, grubData);
        selectWinPeFixture(grubData ? QStringLiteral("minint") : QStringLiteral("i386"), udf, false, loader, false, grubData);
        controller_->toggleEnhancedInstall();
        controller_->setFileSystem(FS_FAT32);
        QCOMPARE(controller_->selection().fileSystem, int(FS_FAT32));
        const QString rootLoader = directory_.filePath(QStringLiteral("iso-tree/"))+loader;
        const QByteArray rootBytes = readFile(rootLoader);
        QCOMPARE(rootBytes,QByteArray("existing root loader"));
        helperMode = QStringLiteral("ok");
        QTest::mouseClick(start_, Qt::LeftButton);
        auto& writer = controller_->isoWriter();
        if (grubData)
        {
            QTRY_COMPARE_WITH_TIMEOUT(writer.state(),rufus::IsoWriteState::SelectingGrubCore,writeTimeoutMs);
            auto* download = window_->findChild<QMessageBox*>(QStringLiteral("grubDownloadDialog"));
            QVERIFY(download && download->isVisible());
            QCOMPARE(helperStarts,0);
            QTest::mouseClick(download->button(QMessageBox::No),Qt::LeftButton);
        }
        QTRY_COMPARE_WITH_TIMEOUT(writer.state(),rufus::IsoWriteState::AwaitingConfirmation,writeTimeoutMs);
        QVERIFY(writer.check().legacyWinPe && writer.request().windowsBios && writer.request().mbr);
        QVERIFY(writer.request().enhancedInstall && !writer.request().syslinux && !writer.request().grub2);
        QCOMPARE(writer.request().fileSystem,int(FS_FAT32));
        QCOMPARE(writer.request().imagePath,sourcePath_);
        QCOMPARE(readFile(rootLoader),rootBytes);
        helperCopiedBytes = writer.check().tree.bytes_written;
        QCOMPARE(helperStarts, 0);
        QCOMPARE(completionBeeps, 0);
        approveIsoPlan();
        QTRY_COMPARE_WITH_TIMEOUT(writer.state(),rufus::IsoWriteState::Complete,writeTimeoutMs);
        QCOMPARE(helperStarts,1); QCOMPARE(completionBeeps,1);
        QCOMPARE(progress_->format(),QStringLiteral("READY")); QCOMPARE(progress_->value(),MAX_PROGRESS);
        QCOMPARE(readFile(rootLoader),rootBytes);
        QCOMPARE(readFile(sourcePath_),sourceBytes_);
        QCOMPARE(readFile(targetPath_), expectedTarget_);
    }

    void legacyWinPeHybridUsesRufusModeDialog_data()
    {
        QTest::addColumn<bool>("fat32");
        QTest::newRow("ordinary-ntfs") << false;
        QTest::newRow("advanced-fat32") << true;
    }

    void legacyWinPeHybridUsesRufusModeDialog()
    {
        QFETCH(bool, fat32);
        selectWinPeFixture(QStringLiteral("i386"), false, true);
        if (fat32)
        {
            controller_->toggleEnhancedInstall();
            controller_->setFileSystem(FS_FAT32);
        }
        const auto choices = controller_->isoWriteChoices(0, false, fat32);
        QCOMPARE(choices.count, 2);
        QCOMPARE(choices.mode[0], IMAGE_WRITE_ISO);
        QCOMPARE(choices.mode[1], IMAGE_WRITE_DD);
        QTest::mouseClick(start_, Qt::LeftButton);
        auto* dialog = window_->findChild<rufus::ImageModeDialog*>();
        QVERIFY(dialog && dialog->isVisible());
        QVERIFY(dialog->findChild<QRadioButton*>(QStringLiteral("imageModeChoice1"))->isChecked());
        chooseImageMode(IMAGE_WRITE_DD);
        QTRY_COMPARE_WITH_TIMEOUT(controller_->writer().status().state, rufus::WriteState::AwaitingConfirmation, writeTimeoutMs);
        QVERIFY(!window_->findChild<rufus::WindowsUserDialog*>());
        controller_->writer().cancel();
        QCOMPARE(helperStarts, 0);
        QCOMPARE(readFile(targetPath_), expectedTarget_);
    }

    void legacyWinPeInvalidRootStopsBeforeAuthorization_data()
    {
        QTest::addColumn<QString>("problem");
        QTest::addColumn<bool>("syslinuxData");
        QTest::addColumn<bool>("grubData");
        for (const char* problem : {"short-loader", "root-detector-empty", "root-detector-directory",
            "root-detector-duplicate", "root-sif-missing-section", "root-sif-no-bom", "root-loader-different",
            "root-loader-empty", "root-loader-duplicate", "root-loader-directory"})
        for (bool syslinuxData : {false, true})
        for (bool grubData : {false, true})
        {
            if (grubData && !syslinuxData) continue;
            QTest::newRow(qPrintable(QStringLiteral("%1-%2-%3").arg(QString::fromLatin1(problem)).arg(syslinuxData).arg(grubData)))
                << QString::fromLatin1(problem) << syslinuxData << grubData;
        }
    }

    void legacyWinPeKeepsDdWhenNtfsMbrIsUnavailable()
    {
        selectWinPeFixture(QStringLiteral("i386"), false, true);
        rufus::Device large = device_;
        large.size = 3ULL << 40; // Beyond Rufus's 512-sector MBR range; no disk allocation.
        controller_->setDiscovery({large}, {});
        controller_->selectDevice(large.id);
        QVERIFY(!controller_->canSelectWindowsBios());
        const auto choices = controller_->isoWriteChoices(0, true, false);
        QCOMPARE(choices.count, 1);
        QCOMPARE(choices.mode[0], IMAGE_WRITE_DD);
        QVERIFY(start_->isEnabled());
        QVERIFY(!window_->findChild<QComboBox*>(QStringLiteral("fileSystemCombo"))->isEnabled());
        QCOMPARE(helperStarts, 0);
        QCOMPARE(readFile(targetPath_), expectedTarget_);
    }

    void legacyWinPeInvalidRootStopsBeforeAuthorization()
    {
        QFETCH(QString, problem); QFETCH(bool, syslinuxData); QFETCH(bool, grubData);
        if (syslinuxData) controller_->toggleEnhancedInstall();
        selectWinPeFixture(QStringLiteral("i386"), false, false, problem, syslinuxData, grubData);
        if (syslinuxData) controller_->setFileSystem(FS_NTFS);
        QTest::mouseClick(start_, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::Failed, writeTimeoutMs);
        QVERIFY(!window_->findChild<QMessageBox*>(QStringLiteral("isoWriteConfirmation")));
        QVERIFY(progress_->format() != QStringLiteral("READY"));
        QCOMPARE(helperStarts, 0);
        QCOMPARE(completionBeeps, 0);
        QCOMPARE(readFile(targetPath_), expectedTarget_);
    }

    void olderWindowsBiosRefresh_data()
    {
        QTest::addColumn<QString>("version");
        for (const char* version : {"win7", "win8", "win8.1"})
            QTest::newRow(version) << QString::fromLatin1(version);
    }

    void olderWindowsBiosRefresh()
    {
        QFETCH(QString, version);
        controller_->toggleEnhancedInstall();
        controller_->setPartitionScheme(rufus::PartitionScheme::Gpt); // Alt+E takes precedence on image load.
        const QByteArray versionBytes = version.toLatin1();
        selectIsoFixture(false, true, versionBytes.constData(), nullptr, true, false, nullptr, true);
        activateMainWindow();
        QTest::keyClick(window_.get(), Qt::Key_E, Qt::AltModifier);
        QVERIFY(!controller_->enhancedInstall());
        QCOMPARE(window_->statusBar()->currentMessage(), QStringLiteral("Dual UEFI/BIOS mode disabled"));
        rufus::SelectionController nextSession;
        QVERIFY(!nextSession.enhancedInstall());
        controller_->toggleWindowsMbr();
        controller_->setDiscovery({}, {});
        QVERIFY(!start_->isEnabled());
        controller_->setDiscovery({device_}, {});
        controller_->selectDevice(device_.id);
        QCOMPARE(controller_->selection().partition, rufus::PartitionScheme::Gpt);
        QCOMPARE(controller_->selection().fileSystems, quint32(FS_MASK(FS_FAT16) | FS_MASK(FS_NTFS) | FS_MASK(FS_FAT32)));
        QVERIFY(!controller_->windowsRufusMbr() && !controller_->enhancedInstall());
        QVERIFY(start_->isEnabled());
        controller_->setBootType(BT_NON_BOOTABLE);
        QVERIFY(!controller_->enhancedInstall());
        QVERIFY(controller_->selection().fileSystems & FS_MASK(FS_FAT16));
        controller_->setBootType(BT_SYSLINUX_V4);
        QVERIFY(controller_->selection().fileSystems & FS_MASK(FS_FAT16));
        controller_->setBootType(BT_IMAGE);
        QVERIFY(!controller_->enhancedInstall());
        QCOMPARE(controller_->selection().partition, rufus::PartitionScheme::Gpt);
        QCOMPARE(controller_->selection().fileSystem, int(FS_FAT32));
        constexpr quint64 beyondMbrBytes = (quint64(1) << 41) + 512;
        auto oversized = device_;
        oversized.size = beyondMbrBytes;
        controller_->setDiscovery({oversized}, {});
        controller_->selectDevice(oversized.id);
        QVERIFY(!controller_->canSelectWindowsBios());
        QVERIFY(start_->isEnabled());
        QCOMPARE(controller_->selection().partition, rufus::PartitionScheme::Gpt);
        QVERIFY(controller_->isoWriteChoices(0, true, false).count != 0);
        controller_->setDiscovery({device_}, {});
        controller_->selectDevice(device_.id);
        QVERIFY(start_->isEnabled());
        controller_->toggleEnhancedInstall();
        rufus::SelectionController enhancedSession;
        QVERIFY(enhancedSession.enhancedInstall() && !enhancedSession.windowsRufusMbr());
        QCOMPARE(controller_->selection().partition, rufus::PartitionScheme::Mbr);
        selectIsoFixture(false, true, "win11", nullptr, true, false, nullptr, true);
        QVERIFY(controller_->enhancedInstall());
        QCOMPARE(controller_->selection().partition, rufus::PartitionScheme::Mbr);
        QCOMPARE(controller_->selection().fileSystems, quint32(FS_MASK(FS_FAT16) | FS_MASK(FS_NTFS) | FS_MASK(FS_FAT32)));
        QCOMPARE(helperStarts, 0);
        QCOMPARE(readFile(targetPath_), expectedTarget_);
    }

    void windowsBiosCompletion_data()
    {
        QTest::addColumn<bool>("dual");
        QTest::addColumn<QString>("mode");
        QTest::addColumn<int>("filesystem");
        QTest::addColumn<QString>("version");
        for (bool dual : {false, true})
        for (const char* mode : {"sync-error", "cleanup-error", "hold-after-result", "hold-bootloader"})
        {
            const QByteArray name = QByteArray(dual ? "dual-" : "bios-") + mode;
            QTest::newRow(name.constData()) << dual << QString::fromLatin1(mode) << int(FS_NTFS) << QStringLiteral("win11");
            if (dual)
                QTest::newRow((name + "-FAT32").constData()) << dual << QString::fromLatin1(mode) << int(FS_FAT32) << QStringLiteral("win11");
        }
        for (const char* version : {"win7", "win8", "win8.1"})
        for (bool dual : {false, true})
        for (int filesystem : {FS_NTFS, FS_FAT32})
        for (const char* mode : {"sync-error", "cleanup-error", "hold-after-result", "hold-bootloader"})
        {
            if (!dual && filesystem == FS_FAT32)
                continue;
            const QByteArray name = QByteArray(version) + (dual ? "-dual-" : "-bios-")
                + (filesystem == FS_NTFS ? "NTFS-" : "FAT32-") + mode;
            QTest::newRow(name.constData()) << dual << QString::fromLatin1(mode) << filesystem << QString::fromLatin1(version);
        }
    }

    void windowsBiosCompletion()
    {
        QFETCH(bool, dual); QFETCH(QString, mode);
        QFETCH(int, filesystem);
        QFETCH(QString, version);
        const QByteArray versionBytes = version.toLatin1();
        selectIsoFixture(false, true, versionBytes.constData(), nullptr, true, false, nullptr, true);
        controller_->setPartitionScheme(rufus::PartitionScheme::Mbr);
        if (dual) controller_->toggleEnhancedInstall();
        controller_->setFileSystem(filesystem);
        helperMode = mode;
        startUncustomizedWindowsPlan();
        approveIsoPlan();
        auto& writer = controller_->isoWriter();
        if (mode == QStringLiteral("hold-after-result"))
        {
            QTRY_COMPARE_WITH_TIMEOUT(writer.outcome().copiedBytes, helperCopiedBytes, writeTimeoutMs);
            QCOMPARE(writer.state(), rufus::IsoWriteState::Writing);
            QVERIFY(progress_->format() != QStringLiteral("READY"));
            QCOMPARE(completionBeeps, 0);
            controller_->toggleWindowsMbr();
            controller_->toggleEnhancedInstall();
            QVERIFY(controller_->windowsRufusMbr());
            QCOMPARE(controller_->enhancedInstall(), dual);
            QVERIFY(helperProcess);
            helperProcess->write("exit\n");
            QTRY_COMPARE_WITH_TIMEOUT(writer.state(), rufus::IsoWriteState::Complete, writeTimeoutMs);
            QCOMPARE(progress_->format(), QStringLiteral("READY"));
            QCOMPARE(progress_->value(), MAX_PROGRESS);
            QCOMPARE(completionBeeps, 1);
        }
        else if (mode == QStringLiteral("hold-bootloader"))
        {
            QTRY_COMPARE_WITH_TIMEOUT(writer.statusText(), QStringLiteral("Writing partition boot record..."), writeTimeoutMs);
            writer.cancel();
            QTRY_COMPARE_WITH_TIMEOUT(writer.state(), rufus::IsoWriteState::Cancelled, cancelTimeoutMs);
            QVERIFY(progress_->format() != QStringLiteral("READY"));
            QCOMPARE(completionBeeps, 0);
        }
        else
        {
            QTRY_COMPARE_WITH_TIMEOUT(writer.state(), rufus::IsoWriteState::Failed, writeTimeoutMs);
            QCOMPARE(writer.outcome().error, mode == QStringLiteral("sync-error") ? EIO : EBUSY);
            QVERIFY(progress_->format() != QStringLiteral("READY"));
            QCOMPARE(completionBeeps, 0);
        }
        QCOMPARE(helperStarts, 1);
        QCOMPARE(readFile(targetPath_), expectedTarget_);
    }

    void windowsBiosUnsupportedImagesStayGated_data()
    {
        QTest::addColumn<QString>("version");
        QTest::addColumn<bool>("bios");
        QTest::addColumn<bool>("mixed");
        QTest::newRow("no-BOOTMGR") << QStringLiteral("win11") << false << false;
        for (const char* version : {"win7", "win8", "win8.1"})
        {
            QTest::newRow((QByteArray(version) + "-no-BOOTMGR").constData()) << QString::fromLatin1(version) << false << false;
        }
    }

    void windowsBiosUnsupportedImagesStayGated()
    {
        QFETCH(QString, version); QFETCH(bool, bios); QFETCH(bool, mixed);
        const QByteArray versionBytes = version.toLatin1();
        selectIsoFixture(false, true, versionBytes.constData(), mixed ? "6.04" : nullptr, true, false, nullptr, bios);
        QVERIFY(!controller_->canSelectWindowsBios());
        controller_->toggleEnhancedInstall();
        auto* partition = window_->findChild<QComboBox*>(QStringLiteral("partitionCombo"));
        QVERIFY(partition);
        QCOMPARE(partition->findData(int(rufus::PartitionScheme::Mbr)), -1);
        QCOMPARE(partition->count(), 1);
        controller_->setPartitionScheme(rufus::PartitionScheme::Mbr);
        QCOMPARE(controller_->selection().partition, rufus::PartitionScheme::Gpt);
        QCOMPARE(helperStarts, 0);
        QCOMPARE(readFile(targetPath_), expectedTarget_);
    }

    void b4LegacyIsoRoutes_data()
    {
        QTest::addColumn<QString>("route");
        for (const char* route : {"unknown-installer", "bootmgr-pe", "windows-fat16", "legacy-pe-fat16", "reactos", "kolibri"})
            QTest::newRow(route) << QString::fromLatin1(route);
    }

    void b4LegacyIsoRoutes()
    {
        QFETCH(QString, route);
        const bool reactos = route == QStringLiteral("reactos"), kolibri = route == QStringLiteral("kolibri");
        const bool fat16 = route.endsWith(QStringLiteral("fat16"));
        if (route == QStringLiteral("legacy-pe-fat16")) selectWinPeFixture(QStringLiteral("i386"), false);
        else if (route == QStringLiteral("unknown-installer")) selectIsoFixture(false,true,"unknown",nullptr,true,false,nullptr,true);
        else if (route == QStringLiteral("windows-fat16")) selectIsoFixture(false,true,"win7",nullptr,true,false,nullptr,true);
        else selectIsoFixture(false,false,"win11",nullptr,false,false,nullptr,false,false,false,reactos ? 1 : kolibri ? 2 : 3);
        if (fat16) controller_->toggleEnhancedInstall();
        controller_->setPartitionScheme(rufus::PartitionScheme::Mbr);
        controller_->setFileSystem(fat16 ? FS_FAT16 : reactos || kolibri ? FS_FAT32 : FS_NTFS);
        if (reactos) controller_->toggleWindowsMbr();
        QCOMPARE(controller_->selection().partition,rufus::PartitionScheme::Mbr);
        QCOMPARE(controller_->selection().syslinux,reactos);
        QCOMPARE(controller_->canSelectWindowsBios(),!reactos && !kolibri);
        QCOMPARE(controller_->canSelectKolibri(),kolibri);
        helperMode = QStringLiteral("hold-after-result");
        startUncustomizedWindowsPlan();
        auto& writer = controller_->isoWriter();
        const auto request = writer.request();
        QCOMPARE(request.windowsBios,!reactos && !kolibri);
        QCOMPARE(request.syslinux,reactos); QCOMPARE(request.kolibriOs,kolibri);
        QCOMPARE(request.mbr,!reactos); QCOMPARE(request.enhancedInstall,fat16);
        QCOMPARE(request.fileSystem,int(fat16 ? FS_FAT16 : reactos || kolibri ? FS_FAT32 : FS_NTFS));
        QVERIFY(!request.hasWindowsInstallSettings() && request.windowsFlags == 0);
        const QJsonObject encoded = rufus::EncodePrepareRequest(request);
        if (reactos) QVERIFY(encoded.value("rufusMbr").isBool() && !encoded.value("rufusMbr").toBool());
        QCOMPARE(request.windowsRufusMbr,!reactos);
        const QString operation = encoded.value("operation").toString();
        QCOMPARE(operation,reactos ? QStringLiteral("prepare-syslinux-device-v1")
            : kolibri ? QStringLiteral("prepare-kolibri-fat32-device-v1")
            : fat16 ? QStringLiteral("prepare-windows-fat16-bios-device-v1") : QStringLiteral("prepare-windows-ntfs-bios-device-v1"));
        rufus::PrepareImageRequest decoded;
        QVERIFY(rufus::DecodePrepareRequest(rufus::EncodePrepareRequest(request),decoded));
        QCOMPARE(decoded.kolibriOs,kolibri); QCOMPARE(decoded.windowsBios,request.windowsBios);
        QCOMPARE(decoded.windowsRufusMbr,request.windowsRufusMbr);
        helperCopiedBytes = writer.check().tree.bytes_written;
        QCOMPARE(helperStarts,0);
        approveIsoPlan();
        QTRY_COMPARE_WITH_TIMEOUT(writer.outcome().copiedBytes,helperCopiedBytes,writeTimeoutMs);
        QCOMPARE(helperStarts,1); QVERIFY(writer.isActive() && writer.state() != rufus::IsoWriteState::Complete);
        QVERIFY(progress_->value() < MAX_PROGRESS && completionBeeps == 0);
        helperProcess->closeWriteChannel();
        QTRY_COMPARE_WITH_TIMEOUT(writer.state(),rufus::IsoWriteState::Complete,writeTimeoutMs);
        QCOMPARE(progress_->value(),MAX_PROGRESS); QCOMPARE(progress_->format(),QStringLiteral("READY"));
        QCOMPARE(completionBeeps,1); QCOMPARE(helperStarts,1);
        QCOMPARE(readFile(sourcePath_),sourceBytes_); QCOMPARE(readFile(targetPath_),expectedTarget_);
    }

    void windowsBiosEnhancedRefreshAndGptDispatch_data()
    {
        QTest::addColumn<QString>("version");
        QTest::addColumn<bool>("rootEfiOnly");
        for (const char* version : {"win7", "win8", "win8.1", "win11"})
        for (bool rootEfiOnly : {false, true})
            QTest::newRow((QByteArray(version) + (rootEfiOnly ? "-repair" : "-regular")).constData())
                << QString::fromLatin1(version) << rootEfiOnly;
    }

    void windowsBiosEnhancedRefreshAndGptDispatch()
    {
        QFETCH(QString, version); QFETCH(bool, rootEfiOnly);
        const QByteArray versionBytes = version.toLatin1();
        selectIsoFixture(false, true, versionBytes.constData(), nullptr, true, false, nullptr, true, rootEfiOnly);
        controller_->setFileSystem(FS_FAT32);
        controller_->toggleEnhancedInstall();
        QCOMPARE(controller_->selection().partition, rufus::PartitionScheme::Mbr);
        QCOMPARE(controller_->selection().fileSystem, int(FS_FAT32));
        controller_->setPartitionScheme(rufus::PartitionScheme::Gpt);
        QCOMPARE(controller_->selection().partition, rufus::PartitionScheme::Gpt);
        controller_->setFileSystem(FS_FAT32);
        controller_->toggleWindowsMbr();
        helperMode = QStringLiteral("enhanced-fat32");
        startUncustomizedWindowsPlan();
        const auto& request = controller_->isoWriter().request();
        QVERIFY(!request.windowsBios && !request.grub2 && !request.mbr && request.windowsRufusMbr);
        QCOMPARE(request.fileSystem, int(FS_FAT32));
        approveIsoPlan();
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::Complete, writeTimeoutMs);
        QCOMPARE(helperStarts, 1);
        controller_->toggleEnhancedInstall();
        QCOMPARE(controller_->selection().partition, rufus::PartitionScheme::Gpt);
        QCOMPARE(controller_->selection().fileSystem, int(FS_FAT32));
    }

    void windowsBiosFatRefresh_data()
    {
        QTest::addColumn<bool>("largeWim");
        QTest::newRow("small-WIM") << false;
        QTest::newRow("large-WIM") << true;
    }

    void windowsBiosFatRefresh()
    {
        QFETCH(bool, largeWim);
        faultKind = largeWim ? LargeWindowsWim : NoFault;
        selectIsoFixture(false, true, "win11", nullptr, true, false, nullptr, true);
        QCOMPARE(controller_->selection().fileSystem, largeWim ? int(FS_NTFS) : int(FS_FAT32));
        activateMainWindow();
        QTest::keyClick(window_.get(), Qt::Key_E, Qt::AltModifier);
        QCOMPARE(controller_->selection().partition, rufus::PartitionScheme::Mbr);
        QCOMPARE(controller_->selection().fileSystems, quint32(FS_MASK(FS_FAT16) | FS_MASK(FS_FAT32) | FS_MASK(FS_NTFS)));
        QCOMPARE(controller_->selection().fileSystem, largeWim ? int(FS_NTFS) : int(FS_FAT32));
        QTest::keyClick(window_.get(), Qt::Key_E, Qt::AltModifier);
        QCOMPARE(controller_->selection().partition, rufus::PartitionScheme::Gpt);
        QCOMPARE(controller_->selection().fileSystem, largeWim ? int(FS_NTFS) : int(FS_FAT32));
        QTest::keyClick(window_.get(), Qt::Key_E, Qt::AltModifier);
        controller_->setPartitionScheme(rufus::PartitionScheme::Mbr); // Keep an explicit BIOS choice through later refresh.
        controller_->setFileSystem(FS_FAT32);
        controller_->setClusterSize(1024); // Too few FAT32 clusters on this 64-MiB target.
        QCOMPARE(controller_->selection().clusterSize, quint32(512));
        QTest::keyClick(window_.get(), Qt::Key_A, Qt::AltModifier);
        QCOMPARE(controller_->selection().fileSystem, int(FS_FAT32));
        QVERIFY(!controller_->windowsRufusMbr());
        QTest::keyClick(window_.get(), Qt::Key_E, Qt::AltModifier);
        QCOMPARE(controller_->selection().fileSystems, quint32(FS_MASK(FS_NTFS)));
        QCOMPARE(controller_->selection().fileSystem, int(FS_NTFS));
        QTest::keyClick(window_.get(), Qt::Key_E, Qt::AltModifier);
        QCOMPARE(controller_->selection().fileSystem, int(FS_NTFS));
        controller_->setFileSystem(FS_FAT32);
        controller_->setClusterSize(512);
        helperMode = QStringLiteral("enhanced-fat32");
        startUncustomizedWindowsPlan();
        const auto& request = controller_->isoWriter().request();
        QVERIFY(request.windowsBios && request.enhancedInstall && !request.windowsRufusMbr);
        QCOMPARE(request.clusterSize, quint32(512));
        QCOMPARE(request.fileSystem, int(FS_FAT32));
        QCOMPARE(controller_->isoWriter().check().splitInstallWim, largeWim);
        controller_->setFileSystem(FS_NTFS); // Frozen at confirmation, including its native cluster choice.
        QCOMPARE(controller_->selection().fileSystem, int(FS_FAT32));
        approveIsoPlan();
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::Complete, writeTimeoutMs);
        QCOMPARE(helperStarts, 1);
        QCOMPARE(progress_->format(), QStringLiteral("READY"));
        QCOMPARE(progress_->value(), MAX_PROGRESS);
        QCOMPARE(readFile(targetPath_), expectedTarget_);
    }

    void enhancedSelectionKeepsNtfsDefault()
    {
        faultKind = LargeWindowsWim;
        selectIsoFixture(false, true);
        auto* filesystems = window_->findChild<QComboBox*>(QStringLiteral("fileSystemCombo"));
        auto* clusters = window_->findChild<QComboBox*>(QStringLiteral("clusterCombo"));
        QVERIFY(filesystems && clusters);
        QCOMPARE(filesystems->count(), 1);
        QCOMPARE(filesystems->currentData().toInt(), int(FS_NTFS));
        QCOMPARE(filesystems->currentText(), QStringLiteral("NTFS"));
        activateMainWindow();
        QTest::keyClick(window_.get(), Qt::Key_E, Qt::AltModifier);
        QVERIFY(controller_->enhancedInstall());
        QCOMPARE(filesystems->count(), 3);
        QCOMPARE(filesystems->currentData().toInt(), int(FS_NTFS));
        QCOMPARE(filesystems->itemText(0), QStringLiteral("FAT16 (Default)"));
        QCOMPARE(filesystems->currentText(), QStringLiteral("NTFS"));
        filesystems->setFocus();
        filesystems->setCurrentIndex(filesystems->findData(FS_FAT32));
        QVERIFY(QMetaObject::invokeMethod(filesystems, "activated", Q_ARG(int, filesystems->currentIndex())));
        QCOMPARE(controller_->selection().fileSystem, int(FS_FAT32));
        QCOMPARE(clusters->currentData().toUInt(), quint32(512));
        QTest::keyClick(window_.get(), Qt::Key_E, Qt::AltModifier);
        QVERIFY(!controller_->enhancedInstall());
        QCOMPARE(filesystems->count(), 1);
        QCOMPARE(filesystems->currentData().toInt(), int(FS_NTFS));
        QCOMPARE(clusters->currentData().toUInt(), quint32(4096));
        QCOMPARE(controller_->selection().volumeLabel, QStringLiteral("NEW LABEL"));
        // The saved preference also applies to a subsequently selected image.
        QTest::keyClick(window_.get(), Qt::Key_E, Qt::AltModifier);
        controller_->selectImage(sourcePath_);
        QTRY_VERIFY_WITH_TIMEOUT(!controller_->isAnalyzing(), writeTimeoutMs);
        QCOMPARE(filesystems->count(), 3);
        QCOMPARE(filesystems->currentData().toInt(), int(FS_NTFS));
        QCOMPARE(helperStarts, 0);
        QCOMPARE(stat(QStringLiteral("opens")), 0);
    }

    void enhancedSelectionPreservesValidFilesystem_data()
    {
        QTest::addColumn<int>("filesystem");
        QTest::newRow("selected-NTFS") << int(FS_NTFS);
        QTest::newRow("selected-FAT32") << int(FS_FAT32);
    }

    void enhancedSelectionPreservesValidFilesystem()
    {
        QFETCH(int, filesystem);
        selectIsoFixture(false, true);
        controller_->setFileSystem(filesystem);
        controller_->toggleEnhancedInstall();
        QVERIFY(controller_->enhancedInstall());
        QCOMPARE(controller_->selection().fileSystem, filesystem);
        controller_->toggleEnhancedInstall();
        QVERIFY(!controller_->enhancedInstall());
        QCOMPARE(controller_->selection().fileSystem, filesystem);
        QCOMPARE(helperStarts, 0);
    }

    void enhancedRequestReachesHelper_data()
    {
        QTest::addColumn<bool>("largeWim");
        QTest::addColumn<bool>("ntfs");
        QTest::newRow("large-WIM-FAT32") << true << false;
        QTest::newRow("large-WIM-NTFS") << true << true;
        QTest::newRow("small-WIM-FAT32") << false << false;
        QTest::newRow("small-WIM-NTFS") << false << true;
    }

    void enhancedRequestReachesHelper()
    {
        QFETCH(bool, largeWim);
        QFETCH(bool, ntfs);
        faultKind = largeWim ? LargeWindowsWim : NoFault;
        controller_->toggleEnhancedInstall();
        selectIsoFixture(false, true);
        controller_->setFileSystem(ntfs ? FS_NTFS : FS_FAT32);
        helperMode = ntfs ? QStringLiteral("enhanced-ntfs") : QStringLiteral("enhanced-fat32");
        startUncustomizedWindowsPlan();
        const auto& writer = controller_->isoWriter();
        QVERIFY(writer.request().enhancedInstall);
        QCOMPARE(writer.request().fileSystem == FS_NTFS, ntfs);
        QCOMPARE(writer.request().windowsFlags, 0);
        QCOMPARE(writer.check().splitInstallWim, largeWim && !ntfs);
        QCOMPARE(helperStarts, 0);
        approveIsoPlan();
        QTRY_COMPARE_WITH_TIMEOUT(writer.state(), rufus::IsoWriteState::Complete, writeTimeoutMs);
        QCOMPARE(helperStarts, 1);
        QCOMPARE(stat(QStringLiteral("opens")), 0);
        QCOMPARE(progress_->format(), QStringLiteral("READY"));
        QCOMPARE(progress_->value(), MAX_PROGRESS);
        QCOMPARE(readFile(targetPath_), expectedTarget_);
    }

    void enhancedToggleIsBlockedDuringChecks()
    {
        selectIsoFixture(false, true);
        faultKind = PauseIsoCheck;
        controller_->selectImage(sourcePath_);
        QTRY_COMPARE_WITH_TIMEOUT(pausedIo.available(), 1, fixtureTimeoutMs);
        QVERIFY(controller_->isAnalyzing());
        activateMainWindow();
        QTest::keyClick(window_.get(), Qt::Key_E, Qt::AltModifier);
        controller_->toggleEnhancedInstall();
        QTest::keyClick(window_.get(), Qt::Key_A, Qt::AltModifier);
        controller_->toggleWindowsMbr();
        QVERIFY(controller_->windowsRufusMbr());
        QVERIFY(!controller_->enhancedInstall());
        resumeIo.release();
        QTRY_VERIFY_WITH_TIMEOUT(!controller_->isAnalyzing(), writeTimeoutMs);
        pausedIo.acquire();
        QTest::mouseClick(start_, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(pausedIo.available(), 1, fixtureTimeoutMs);
        QCOMPARE(controller_->isoWriter().state(), rufus::IsoWriteState::Checking);
        QTest::keyClick(window_.get(), Qt::Key_E, Qt::AltModifier);
        controller_->toggleEnhancedInstall();
        QVERIFY(!controller_->enhancedInstall());
        QVERIFY(!controller_->isoWriter().request().enhancedInstall);
        QTest::keyClick(window_.get(), Qt::Key_A, Qt::AltModifier);
        controller_->toggleWindowsMbr();
        QVERIFY(controller_->windowsRufusMbr());
        controller_->isoWriter().cancel();
        resumeIo.release();
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::Cancelled, cancelTimeoutMs);
        rufus::SelectionController nextSession;
        QVERIFY(!nextSession.enhancedInstall());
        QVERIFY(nextSession.windowsRufusMbr());
        QCOMPARE(helperStarts, 0);
    }

    void enhancedToggleKeepsActiveRequest()
    {
        controller_->toggleEnhancedInstall();
        selectIsoFixture(false, true);
        helperMode = QStringLiteral("hold");
        startUncustomizedWindowsPlan();
        controller_->toggleEnhancedInstall();
        QVERIFY(controller_->enhancedInstall());
        QVERIFY(controller_->isoWriter().request().enhancedInstall);
        approveIsoPlan();
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::Writing, writeTimeoutMs);
        activateMainWindow();
        QTest::keyClick(window_.get(), Qt::Key_E, Qt::AltModifier);
        controller_->toggleEnhancedInstall();
        QVERIFY(controller_->enhancedInstall());
        QVERIFY(controller_->isoWriter().request().enhancedInstall);
        controller_->isoWriter().cancel();
        controller_->toggleEnhancedInstall();
        QVERIFY(controller_->enhancedInstall());
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::Cancelled, cancelTimeoutMs);
        controller_->toggleEnhancedInstall();
        QVERIFY(!controller_->enhancedInstall());
        QCOMPARE(helperStarts, 1);
        QCOMPARE(readFile(targetPath_), expectedTarget_);
    }

    void olderWindowsUefiSelectionAndRequest_data()
    {
        QTest::addColumn<QString>("version");
        QTest::addColumn<bool>("enhanced");
        QTest::addColumn<bool>("rootEfiOnly");
        QTest::addColumn<int>("filesystem");
        QTest::addColumn<bool>("largeWim");
        for (const char* version : {"win7", "win8", "win8.1"})
        for (bool enhanced : {false, true})
        for (bool rootEfiOnly : {false, true})
        for (int filesystem : {FS_FAT32, FS_NTFS})
        {
            const QByteArray name = QByteArray(version) + (enhanced ? "-enhanced" : "-plain")
                + (rootEfiOnly ? "-repair" : "-regular") + (filesystem == FS_NTFS ? "-NTFS" : "-FAT32");
            QTest::newRow(name.constData()) << QString::fromLatin1(version) << enhanced << rootEfiOnly << filesystem << false;
            if (enhanced && filesystem == FS_FAT32)
                QTest::newRow((name + "-large-WIM").constData())
                    << QString::fromLatin1(version) << enhanced << rootEfiOnly << filesystem << true;
        }
    }

    void olderWindowsUefiSelectionAndRequest()
    {
        QFETCH(QString, version); QFETCH(bool, enhanced); QFETCH(bool, rootEfiOnly);
        QFETCH(int, filesystem); QFETCH(bool, largeWim);
        faultKind = largeWim ? LargeWindowsWim : NoFault;
        if (enhanced) controller_->toggleEnhancedInstall();
        const QByteArray kind = version.toLatin1();
        selectIsoFixture(false, true, kind.constData(), nullptr, true, false, nullptr, false, rootEfiOnly);
        QVERIFY(!controller_->canSelectWindowsBios()); // No BIOS BOOTMGR in this source.
        auto* partition = window_->findChild<QComboBox*>(QStringLiteral("partitionCombo"));
        auto* target = window_->findChild<QComboBox*>(QStringLiteral("targetCombo"));
        auto* filesystems = window_->findChild<QComboBox*>(QStringLiteral("fileSystemCombo"));
        QCOMPARE(partition->count(), 1);
        QCOMPARE(partition->currentText(), QStringLiteral("GPT"));
        QCOMPARE(target->currentText(), QStringLiteral("UEFI (non CSM)"));
        QVERIFY(filesystems->findData(FS_FAT16) >= 0);
        controller_->setFileSystem(filesystem);
        helperMode = enhanced ? (filesystem == FS_NTFS ? QStringLiteral("enhanced-ntfs") : QStringLiteral("enhanced-fat32"))
            : QStringLiteral("ok");
        QTest::mouseClick(start_, Qt::LeftButton);
        auto& writer = controller_->isoWriter();
        QTRY_COMPARE_WITH_TIMEOUT(writer.state(), rufus::IsoWriteState::AwaitingConfirmation, writeTimeoutMs);
        QVERIFY(!window_->findChild<rufus::WindowsUserDialog*>());
        QVERIFY(!writer.request().windowsBios && !writer.request().mbr && !writer.request().grub2 && !writer.request().syslinux);
        QCOMPARE(writer.request().fileSystem, filesystem);
        QCOMPARE(writer.request().enhancedInstall, enhanced);
        QCOMPARE(writer.request().usesUefiNtfs(), filesystem == FS_NTFS);
        QCOMPARE(writer.check().splitInstallWim, largeWim);
        QCOMPARE(!writer.check().windowsEfiRepair.isEmpty(), rootEfiOnly);
        QCOMPARE(writer.request().windowsFlags, 0);
        QVERIFY(!writer.request().hasWindowsInstallSettings());
        QVERIFY(writer.check().windowsInstall.editions.isEmpty());
        auto* confirmation = window_->findChild<QMessageBox*>(QStringLiteral("isoWriteConfirmation"));
        const QString layout = filesystem == FS_NTFS ? QStringLiteral("GPT, NTFS, UEFI") : QStringLiteral("GPT, FAT32, UEFI");
        QVERIFY(confirmation && confirmation->informativeText().contains(layout));
        QCOMPARE(helperStarts, 0);
        approveIsoPlan();
        QTRY_COMPARE_WITH_TIMEOUT(writer.state(), rufus::IsoWriteState::Complete, writeTimeoutMs);
        QCOMPARE(helperStarts, 1);
        QCOMPARE(progress_->format(), QStringLiteral("READY"));
        QCOMPARE(progress_->value(), MAX_PROGRESS);
        QCOMPARE(completionBeeps, 1);
        QCOMPARE(readFile(targetPath_), expectedTarget_);
    }

    void hiddenSModeRejectedBeforeAuthorization()
    {
        selectIsoFixture(false, true, "win11");
        QTest::mouseClick(start_, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::ChoosingWindowsOptions, writeTimeoutMs);
        rufus::PrepareImageRequest options;
        options.windowsFlags = UNATTEND_FORCE_S_MODE;
        controller_->isoWriter().chooseWindowsOptions(options);
        QCOMPARE(controller_->isoWriter().state(), rufus::IsoWriteState::Failed);
        QCOMPARE(helperStarts, 0);
        QCOMPARE(readFile(targetPath_), expectedTarget_);
    }

    void windowsChoicesReachHelperAndPersist_data()
    {
        QTest::addColumn<int>("flags");
        QTest::newRow("local-account") << int(UNATTEND_SET_USER);
        QTest::newRow("hardware-and-local-account") << int(UNATTEND_SET_USER | UNATTEND_SECUREBOOT_TPM_MINRAM);
        QTest::newRow("ca-2023-and-local-account") << int(UNATTEND_SET_USER | UNATTEND_USE_MS2023_BOOTLOADERS);
        QTest::newRow("ca-2023-hardware-and-local-account") << int(UNATTEND_SET_USER |
            UNATTEND_SECUREBOOT_TPM_MINRAM | UNATTEND_USE_MS2023_BOOTLOADERS);
        QTest::newRow("policy-only") << int(UNATTEND_APPLY_SKUSIPOLICY);
        QTest::newRow("s-mode-only") << int(UNATTEND_FORCE_S_MODE);
        QTest::newRow("policy-ca-2023-hardware-and-local-account") << int(UNATTEND_SET_USER |
            UNATTEND_SECUREBOOT_TPM_MINRAM | UNATTEND_USE_MS2023_BOOTLOADERS | UNATTEND_APPLY_SKUSIPOLICY);
    }

    void windowsChoicesReachHelperAndPersist()
    {
        QFETCH(int, flags);
        const bool needs25h2 = flags & (UNATTEND_USE_MS2023_BOOTLOADERS | UNATTEND_APPLY_SKUSIPOLICY);
        const char* version = needs25h2 ? "win11-25h2" : "win11";
        selectIsoFixture(false, true, version);
        const bool sMode = flags & UNATTEND_FORCE_S_MODE;
        if (sMode)
        {
            controller_->isoWriter().toggleExpertMode();
            helperMode = QStringLiteral("s-mode-settings");
        }
        QTest::mouseClick(start_, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::ChoosingWindowsOptions, writeTimeoutMs);
        auto* dialog = window_->findChild<rufus::WindowsUserDialog*>();
        QVERIFY(dialog);
        for (QCheckBox* box : dialog->findChildren<QCheckBox*>())
            box->setChecked((box->property("unattendFlag").toInt() & flags) != 0);
        dialog->findChild<QLineEdit*>()->setText(QStringLiteral("  Rufus.Test&User  "));
        QTest::mouseClick(dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok), Qt::LeftButton);
        QCOMPARE(controller_->isoWriter().state(), rufus::IsoWriteState::AwaitingConfirmation);
        QCOMPARE(controller_->isoWriter().request().windowsFlags, flags);
        const bool localAccount = flags & UNATTEND_SET_USER;
        const QString username = localAccount ? QStringLiteral("Rufus_Test_User") : QString();
        QCOMPARE(controller_->isoWriter().request().windowsUsername, username);
        QCOMPARE(helperStarts, 0);
        rufus::IsoWriteController nextSession;
        QCOMPARE(nextSession.expertMode(), sMode);
        QVERIFY(!(nextSession.windowsOptions() & UNATTEND_FORCE_S_MODE));
        QCOMPARE(bool(controller_->isoWriter().windowsOptions() & UNATTEND_FORCE_S_MODE), sMode);
        QCOMPARE(nextSession.windowsOptions() & UNATTEND_SET_USER, flags & UNATTEND_SET_USER);
        QVERIFY(!(nextSession.windowsOptions() & UNATTEND_NO_ONLINE_ACCOUNT));
        QCOMPARE(nextSession.windowsOptions() & UNATTEND_SECUREBOOT_TPM_MINRAM,
            flags & UNATTEND_SECUREBOOT_TPM_MINRAM);
        QCOMPARE(nextSession.windowsOptions() & UNATTEND_USE_MS2023_BOOTLOADERS,
            flags & UNATTEND_USE_MS2023_BOOTLOADERS);
        QCOMPARE(nextSession.windowsOptions() & UNATTEND_APPLY_SKUSIPOLICY,
            flags & UNATTEND_APPLY_SKUSIPOLICY);
        approveIsoPlan();
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::Complete, writeTimeoutMs);
        QCOMPARE(helperStarts, 1);
        QCOMPARE(readFile(targetPath_), expectedTarget_);
        if (sMode)
        {
            // Hiding the expert choice must not clear its in-session preference.
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            controller_->isoWriter().toggleExpertMode();
            QTest::mouseClick(start_, Qt::LeftButton);
            QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::ChoosingWindowsOptions, writeTimeoutMs);
            dialog = window_->findChild<rufus::WindowsUserDialog*>();
            QVERIFY(dialog);
            const QString optionName = QStringLiteral("windowsOption%1").arg(UNATTEND_FORCE_S_MODE);
            QVERIFY(!dialog->findChild<QCheckBox*>(optionName));
            dialog->accept();
            QCOMPARE(controller_->isoWriter().state(), rufus::IsoWriteState::AwaitingConfirmation);
            QCOMPARE(controller_->isoWriter().request().windowsFlags, 0);
            QVERIFY(controller_->isoWriter().windowsOptions() & UNATTEND_FORCE_S_MODE);
            controller_->isoWriter().cancel();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            controller_->isoWriter().toggleExpertMode();
            QTest::mouseClick(start_, Qt::LeftButton);
            QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::ChoosingWindowsOptions, writeTimeoutMs);
            dialog = window_->findChild<rufus::WindowsUserDialog*>();
            QVERIFY(dialog);
            auto* choice = dialog->findChild<QCheckBox*>(optionName);
            QVERIFY(choice && choice->isChecked());
            dialog->reject();
            QCOMPARE(helperStarts, 1);
        }
    }

    void silentCreationUsesOneHelperAndFreshChoices()
    {
        startSilentPlan();
        approveSilentWarning();
        QCOMPARE(controller_->isoWriter().state(), rufus::IsoWriteState::AwaitingConfirmation);
        const auto request = controller_->isoWriter().request();
        QCOMPARE(request.windowsFlags, UNATTEND_SET_USER | UNATTEND_NO_DATA_COLLECTION |
            UNATTEND_DUPLICATE_LOCALE | UNATTEND_SILENT_INSTALL);
        QCOMPARE(request.windowsUsername, QStringLiteral("Măriuș_Test"));
        QCOMPARE(request.windowsInstallPath, QStringLiteral("/sources/install.wim"));
        QCOMPARE(request.windowsEdition, 1);
        QCOMPARE(request.windowsRegion.inputLocale, QStringLiteral("0409:00020409;ro-RO"));
        QCOMPARE(request.windowsRegion.timezone, QStringLiteral("GTB Standard Time"));
        QVERIFY(request.windowsSilentConfirmed);
        QCOMPARE(request.imagePath, sourcePath_);
        QCOMPARE(request.path, device_.path);
        QCOMPARE(helperStarts, 0);
        helperMode = QStringLiteral("silent-settings"); // Fixture independently checks received choices.
        approveIsoPlan();
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::Complete, writeTimeoutMs);
        QCOMPARE(helperStarts, 1);
        QCOMPARE(readFile(targetPath_), expectedTarget_);
        rufus::IsoWriteController nextSession;
        QVERIFY(!(nextSession.windowsOptions() & (UNATTEND_SILENT_INSTALL | UNATTEND_DUPLICATE_LOCALE)));
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QTest::mouseClick(start_, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::ChoosingWindowsOptions, writeTimeoutMs);
        auto* dialog = window_->findChild<rufus::WindowsUserDialog*>();
        QVERIFY(dialog);
        auto* silent = dialog->findChild<QCheckBox*>(QStringLiteral("windowsOption%1").arg(UNATTEND_SILENT_INSTALL));
        QVERIFY(silent->isHidden() && !silent->isChecked());
        QCOMPARE(dialog->findChild<QComboBox*>(QStringLiteral("windowsEdition"))->currentIndex(), -1);
        QVERIFY(!controller_->isoWriter().request().hasWindowsInstallSettings());
        QTest::mouseClick(dialog->findChild<QPushButton*>(QStringLiteral("windowsRegionButton")), Qt::LeftButton);
        auto* region = dialog->findChild<rufus::WindowsRegionDialog*>();
        QVERIFY(region && region->options().uiLanguage.isEmpty() && region->options().timezone.isEmpty());
        dialog->reject();
        QCOMPARE(controller_->isoWriter().state(), rufus::IsoWriteState::Cancelled);
        QCOMPARE(helperStarts, 1);
    }

    void silentDeviceRemovalClosesChildren_data()
    {
        QTest::addColumn<bool>("regionalEditor");
        QTest::newRow("regional-editor") << true;
        QTest::newRow("acknowledgements") << false;
    }

    void silentDeviceRemovalClosesChildren()
    {
        QFETCH(bool, regionalEditor);
        startSilentPlan();
        auto* dialog = window_->findChild<rufus::WindowsUserDialog*>();
        QPointer<QDialog> child;
        if (regionalEditor)
        {
            QTest::mouseClick(dialog->findChild<QPushButton*>(QStringLiteral("windowsRegionButton")), Qt::LeftButton);
            child = dialog->findChild<rufus::WindowsRegionDialog*>();
        }
        else
        {
            dialog->accept();
            child = dialog->findChild<rufus::WindowsSilentDialog*>();
        }
        QVERIFY(child && child->isVisible());
        controller_->setDiscovery({}, {});
        QCOMPARE(controller_->isoWriter().state(), rufus::IsoWriteState::Failed);
        QVERIFY(!child || !child->isVisible());
        QCOMPARE(helperStarts, 0);
        QCOMPARE(readFile(targetPath_), expectedTarget_);
    }

    void silentSourceChangeStillFailsBeforeAuthorization()
    {
        startSilentPlan();
        auto* dialog = window_->findChild<rufus::WindowsUserDialog*>();
        dialog->accept();
        QVERIFY(dialog->findChild<rufus::WindowsSilentDialog*>());
        writeFile(sourcePath_, sourceBytes_);
        approveSilentWarning();
        QCOMPARE(controller_->isoWriter().state(), rufus::IsoWriteState::AwaitingConfirmation);
        approveIsoPlan();
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::Failed, writeTimeoutMs);
        QCOMPARE(helperStarts, 0);
        QCOMPARE(readFile(targetPath_), expectedTarget_);
    }

    void controllerRejectsUnconfirmedSilentSelection()
    {
        startSilentPlan();
        rufus::PrepareImageRequest options;
        options.windowsFlags = UNATTEND_SILENT_INSTALL | UNATTEND_SET_USER |
            UNATTEND_NO_DATA_COLLECTION | UNATTEND_DUPLICATE_LOCALE;
        options.windowsUsername = QStringLiteral("TestUser");
        options.windowsInstallPath = QStringLiteral("/sources/install.wim");
        options.windowsEdition = 1;
        options.windowsRegion = {QStringLiteral("ro-RO"), QStringLiteral("ro-RO"), QStringLiteral("en-GB"),
            QStringLiteral("en-US"), {}, QStringLiteral("GTB Standard Time")};
        controller_->isoWriter().chooseWindowsOptions(options);
        QCOMPARE(controller_->isoWriter().state(), rufus::IsoWriteState::Failed);
        QCOMPARE(helperStarts, 0);
    }

    void windowsLargeWimSelectsNtfs()
    {
        // Only the size classification is synthetic here. Metadata is read
        // from a real generated WIM; CP11g covers actual files beyond 4 GiB.
        faultKind = LargeWindowsWim;
        selectIsoFixture(false, true);
        QCOMPARE(controller_->selection().windowsAnalysis.state, WINDOWS_IMAGE_KNOWN);
        QCOMPARE(controller_->selection().imageAnalysis.report.win_version.major, quint16(11));
        auto* filesystems = window_->findChild<QComboBox*>(QStringLiteral("fileSystemCombo"));
        QCOMPARE(filesystems->count(), 1);
        QCOMPARE(filesystems->currentText(), QStringLiteral("NTFS"));
        QCOMPARE(controller_->selection().clusterSize, quint32(4096));
        controller_->setFileSystem(FS_FAT32);
        QCOMPARE(controller_->selection().fileSystem, int(FS_NTFS));
        startIsoPlan();
        const auto& install = controller_->isoWriter().check();
        QCOMPARE(install.windowsInstallPath, QStringLiteral("/sources/install.wim"));
        QCOMPARE(install.windowsInstallError, 0);
        QCOMPARE(install.windowsInstall.editions.size(), 1);
        QCOMPARE(install.windowsInstall.editions[0].image, 1);
        QCOMPARE(install.windowsInstall.uiLanguage, QStringLiteral("en-US"));
        QCOMPARE(controller_->isoWriter().check().extraPartitions, quint8(4));
        approveIsoPlan();
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::Complete, writeTimeoutMs);
        QCOMPARE(helperStarts, 1);
        QCOMPARE(controller_->isoWriter().outcome().copiedBytes, helperCopiedBytes);
        QCOMPARE(readFile(targetPath_), expectedTarget_);
    }

    void ntfsHybridKeepsIsoAndDdChoices()
    {
        selectIsoFixture(true);
        controller_->setFileSystem(FS_NTFS);
        QTest::mouseClick(start_, Qt::LeftButton);
        auto* dialog = window_->findChild<rufus::ImageModeDialog*>();
        QVERIFY(dialog && dialog->isVisible());
        const auto radios = dialog->findChildren<QRadioButton*>();
        QCOMPARE(radios.size(), 2);
        QVERIFY(radios.first()->isChecked());
        QTest::keyClick(dialog, Qt::Key_Escape);
        QCOMPARE(helperStarts, 0);
        QCOMPARE(stat(QStringLiteral("opens")), 0);
    }

    void isoConfirmationCanBeDeclined()
    {
        selectIsoFixture();
        startIsoPlan();
        auto* dialog = window_->findChild<QMessageBox*>(QStringLiteral("isoWriteConfirmation"));
        QTest::keyClick(dialog, Qt::Key_Escape);
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::Cancelled, uiTimeoutMs);
        QCOMPARE(progress_->format(), QStringLiteral("READY"));
        QCOMPARE(progress_->value(), 0);
        QCOMPARE(helperStarts, 0);
        startIsoPlan();
        approveIsoPlan();
        QTRY_VERIFY_WITH_TIMEOUT(!controller_->isoWriter().isActive(), writeTimeoutMs);
        QVERIFY2(controller_->isoWriter().state() == rufus::IsoWriteState::Complete,
            qPrintable(controller_->isoWriter().statusText()));
        QCOMPARE(helperStarts, 1);
    }

    void isoLabelNormalization_data()
    {
        QTest::addColumn<QString>("input");
        QTest::addColumn<QString>("expected");
        QTest::newRow("lowercase") << QStringLiteral("rufus test") << QStringLiteral("RUFUS TEST");
        QTest::newRow("punctuation") << QStringLiteral("usb.name*:long") << QStringLiteral("USB_NAMELON");
        QTest::newRow("unicode") << QString::fromUtf8("ABCD😀ș") << QStringLiteral("ABCD___");
        QTest::newRow("size-fallback") << QString::fromUtf8("😀") << QStringLiteral("64 MB");
    }

    void isoLabelNormalization()
    {
        QFETCH(QString, input);
        QFETCH(QString, expected);
        selectIsoFixture();
        controller_->setVolumeLabel(input);
        startIsoPlan();
        QCOMPARE(controller_->isoWriter().request().label, expected);
        QCOMPARE(controller_->selection().volumeLabel, input);
        auto* confirmation = window_->findChild<QMessageBox*>(QStringLiteral("isoWriteConfirmation"));
        QVERIFY(confirmation->informativeText().contains(QStringLiteral("Volume label: ") + expected));
        QTest::mouseClick(confirmation->button(QMessageBox::Cancel), Qt::LeftButton);
        QCOMPARE(helperStarts, 0);
    }

    void isoClusterChoicesFollowTarget()
    {
        selectIsoFixture();
        auto* clusters = window_->findChild<QComboBox*>(QStringLiteral("clusterCombo"));
        QVERIFY(clusters && clusters->isEnabled());
        QCOMPARE(clusters->count(), 1);
        QCOMPARE(clusters->currentText(), QStringLiteral("512 bytes (Default)"));
        controller_->setClusterSize(4096);
        QCOMPARE(controller_->selection().clusterSize, quint32(512));
        // These large sizes are discovery metadata only; this test never starts
        // a write to the fixture's smaller backing file.
        rufus::Device large = device_;
        large.id = QStringLiteral("large-cluster-target");
        large.size = 16ULL * 1024 * 1024 * 1024;
        controller_->setDiscovery({device_, large}, {});
        controller_->selectDevice(large.id);
        QCOMPARE(clusters->count(), 5);
        QCOMPARE(clusters->currentText(), QStringLiteral("8192 bytes (Default)"));
        QCOMPARE(clusters->itemText(3), QStringLiteral("32 kilobytes"));
        QCOMPARE(clusters->itemText(4), QStringLiteral("64 kilobytes"));
        controller_->setClusterSize(32768);
        QCOMPARE(controller_->selection().clusterSize, quint32(32768));
        controller_->setClusterSize(65536);
        QCOMPARE(controller_->selection().clusterSize, quint32(65536));
        QCOMPARE(clusters->currentData().toUInt(), quint32(65536));
        controller_->setDiscovery({device_, large}, {});
        QCOMPARE(controller_->selection().clusterSize, quint32(65536));
        QCOMPARE(clusters->currentData().toUInt(), quint32(65536));
        controller_->selectDevice(device_.id);
        QCOMPARE(clusters->currentText(), QStringLiteral("512 bytes (Default)"));
    }

    void filesystemLabelsFollowTargetCapacity_data()
    {
        QTest::addColumn<quint64>("capacity");
        QTest::addColumn<QString>("fatLabel");
        constexpr quint64 GiB = 1024ULL * 1024 * 1024;
        QTest::newRow("small-native-target") << quint64(64 * 1024 * 1024) << QStringLiteral("FAT32");
        QTest::newRow("below-large-threshold") << 32 * GiB - 512 << QStringLiteral("FAT32 (Default)");
        QTest::newRow("at-large-threshold") << 32 * GiB << QStringLiteral("FAT32 (Default)");
        QTest::newRow("above-large-threshold") << 32 * GiB + 512 << QStringLiteral("FAT32 (Default)");
        QTest::newRow("fat32-limit") << 2048 * GiB << QString();
    }

    void rejectedImageRestoresDeviceFormats_data()
    {
        QTest::addColumn<bool>("ntfs");
        QTest::addColumn<bool>("corruptIso");
        QTest::newRow("missing-default-fat32") << false << false;
        QTest::newRow("missing-selected-ntfs") << true << false;
        QTest::newRow("corrupt-default-fat32") << false << true;
        QTest::newRow("corrupt-selected-ntfs") << true << true;
    }

    void rejectedImageRestoresDeviceFormats()
    {
        QFETCH(bool, ntfs);
        QFETCH(bool, corruptIso);
        selectIsoFixture();
        auto* filesystems = window_->findChild<QComboBox*>(QStringLiteral("fileSystemCombo"));
        auto* clusters = window_->findChild<QComboBox*>(QStringLiteral("clusterCombo"));
        QVERIFY(filesystems && clusters && filesystems->isEnabled());
        if (ntfs)
            QTest::keyClick(filesystems, Qt::Key_End);
        const QString path = directory_.filePath(QStringLiteral("rejected.iso"));
        QFile::remove(path);
        if (corruptIso)
            writeFile(path, QByteArray("Invalid ISO"));
        controller_->selectImage(path);
        QTRY_VERIFY_WITH_TIMEOUT(!controller_->isAnalyzing(), fixtureTimeoutMs);
        QCOMPARE(controller_->selection().imageState, rufus::ImageState::Failed);
        QVERIFY(controller_->selection().imagePath.isEmpty());
        QCOMPARE(filesystems->count(), 3);
        QCOMPARE(filesystems->itemText(0), QStringLiteral("FAT16 (Default)"));
        QCOMPARE(filesystems->currentText(), ntfs ? QStringLiteral("NTFS") : QStringLiteral("FAT16 (Default)"));
        QCOMPARE(clusters->currentData().toUInt(), ntfs ? quint32(4096) : quint32(2048));
        QVERIFY(!filesystems->isEnabled());
        QVERIFY(!clusters->isEnabled());
        QVERIFY(!start_->isEnabled());
        QCOMPARE(progress_->format(), QStringLiteral("READY"));
        QCOMPARE(progress_->value(), 0);
        QCOMPARE(helperStarts, 0);
        QCOMPARE(stat(QStringLiteral("opens")), 0);
    }

    void filesystemLabelsFollowTargetCapacity()
    {
        QFETCH(quint64, capacity);
        QFETCH(QString, fatLabel);
        selectIsoFixture();
        auto* filesystems = window_->findChild<QComboBox*>(QStringLiteral("fileSystemCombo"));
        QVERIFY(filesystems && filesystems->isEnabled());
        // Capacity is discovery metadata only. No sparse disks or writes.
        rufus::Device target = device_;
        target.id = QStringLiteral("filesystem-label-target");
        target.size = capacity;
        controller_->setDiscovery({device_, target}, {});
        controller_->selectDevice(target.id);
        const bool hasFat = !fatLabel.isEmpty();
        const bool fat16Available = capacity < 4ULL * 1024 * 1024 * 1024;
        QCOMPARE(filesystems->count(), hasFat ? (fat16Available ? 3 : 2) : 1);
        if (hasFat)
        {
            QCOMPARE(filesystems->itemText(filesystems->findData(FS_FAT32)), fatLabel);
            QCoreApplication::processEvents();
            QStyleOptionComboBox option;
            option.initFrom(filesystems);
            const QRect textArea = filesystems->style()->subControlRect(QStyle::CC_ComboBox,
                &option, QStyle::SC_ComboBoxEditField, filesystems);
            QVERIFY(filesystems->fontMetrics().horizontalAdvance(fatLabel) <= textArea.width());
        }
        else
            QCOMPARE(filesystems->findData(FS_FAT32), -1);
        // Leave an automatic fallback untouched; clicking it now records a preference.
        if (hasFat)
            controller_->setFileSystem(FS_NTFS);
        QCOMPARE(filesystems->currentData().toInt(), int(FS_NTFS));
        QCOMPARE(filesystems->currentText(), hasFat ? QStringLiteral("NTFS") : QStringLiteral("NTFS (Default)"));
        if (hasFat)
            QCOMPARE(filesystems->itemText(filesystems->findData(FS_FAT32)), fatLabel);
        controller_->selectDevice(device_.id);
        QCOMPARE(filesystems->itemText(filesystems->findData(FS_FAT32)), QStringLiteral("FAT32"));
        // A manual change is retained; the automatic NTFS choice for a
        // FAT32-ineligible target does not create a user preference.
        const QString selectedAfterReturn = hasFat ? QStringLiteral("NTFS") : QStringLiteral("FAT32");
        QCOMPARE(filesystems->currentText(), selectedAfterReturn);
        QCOMPARE(helperStarts, 0);
        QCOMPARE(stat(QStringLiteral("opens")), 0);
        controller_->setDiscovery({}, {});
        QCOMPARE(filesystems->count(), 0);
        QCOMPARE(controller_->selection().defaultFileSystem, int(FS_UNKNOWN));
    }

    void isoChangesBeforeConfirmation_data()
    {
        QTest::addColumn<QString>("change");
        for (const char* change : {"source", "device", "mount", "diskseq"})
            QTest::newRow(change) << QString::fromLatin1(change);
    }

    void isoChangesBeforeConfirmation()
    {
        QFETCH(QString, change);
        selectIsoFixture();
        startIsoPlan();
        if (change == QStringLiteral("device"))
            controller_->setDiscovery({}, {});
        else
        {
            if (change == QStringLiteral("source"))
                writeFile(sourcePath_, sourceBytes_);
            else if (change == QStringLiteral("diskseq"))
                ++diskSequence;
            else
            {
                objects_[disk][fsInterface][QStringLiteral("MountPoints")] =
                    QVariant::fromValue(QList<QByteArray>{QByteArray("/test/mount\0", 12)});
                setObjects();
            }
            approveIsoPlan();
        }
        // An eligible desktop mount appearing during confirmation is handled by
        // the same authorized helper. Source/device identity changes still fail.
        const bool mounted = change == QStringLiteral("mount");
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), mounted ? rufus::IsoWriteState::Complete :
            rufus::IsoWriteState::Failed, writeTimeoutMs);
        QCOMPARE(helperStarts, mounted ? 1 : 0);
        QCOMPARE(stat(QStringLiteral("opens")), 0);
    }

    void isoHelperFailures_data()
    {
        QTest::addColumn<QString>("mode");
        QTest::addColumn<int>("filesystem");
        for (int filesystem : {FS_FAT16, FS_FAT32})
            for (const char* mode : {"deny", "cleanup-error", "sync-error", "short-copy", "wrong-layout",
                 "progress-backwards", "progress-changed-total", "progress-overrun", "progress-wrong-phase"})
            {
                const QByteArray row = QByteArray::number(filesystem) + '-' + mode;
                QTest::newRow(row.constData()) << QString::fromLatin1(mode) << filesystem;
            }
    }

    void isoHelperFailures()
    {
        QFETCH(QString, mode);
        QFETCH(int, filesystem);
        selectIsoFixture();
        controller_->setFileSystem(filesystem);
        helperMode = mode;
        startIsoPlan();
        approveIsoPlan();
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::Failed, writeTimeoutMs);
        QCOMPARE(helperStarts, 1);
        QCOMPARE(progress_->format(), QStringLiteral("FAILED"));
        QVERIFY(progress_->value() < MAX_PROGRESS);
        QCOMPARE(completionBeeps, 0);
        QCOMPARE(completionAlerts, 0);
        QCOMPARE(progress_->palette().color(QPalette::Highlight), QColor(0xDA, 0x26, 0x26));
        QVERIFY(!controller_->isoWriter().statusText().isEmpty());
    }

    void isoCancellationWaitsForHelperCleanup_data()
    {
        QTest::addColumn<int>("filesystem");
        QTest::newRow("FAT16") << int(FS_FAT16);
        QTest::newRow("FAT32") << int(FS_FAT32);
    }

    void isoCancellationWaitsForHelperCleanup()
    {
        QFETCH(int, filesystem);
        selectIsoFixture();
        controller_->setFileSystem(filesystem);
        helperMode = QStringLiteral("hold");
        startIsoPlan();
        approveIsoPlan();
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().statusText(), QStringLiteral("Copying ISO files: 50.0%"), writeTimeoutMs);
        QCOMPARE(progress_->maximum(), MAX_PROGRESS);
        QCOMPARE(progress_->value(), 35061); // FAT32: 9% preparation + half of its 89% copy span.
        QCOMPARE(progress_->format(), QStringLiteral("Copying ISO files: 50.0%"));
        QTest::mouseClick(close_, Qt::LeftButton);
        auto* dialog = window_->findChild<QMessageBox*>(QStringLiteral("cancelConfirmation"));
        QVERIFY(dialog && dialog->isVisible());
        QCOMPARE(dialog->defaultButton(), dialog->button(QMessageBox::No));
        QTest::mouseClick(dialog->button(QMessageBox::Yes), Qt::LeftButton);
        QVERIFY(window_->isVisible());
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::Cancelled, writeTimeoutMs);
        QCOMPARE(helperStarts, 1);
        QCOMPARE(progress_->format(), QStringLiteral("CANCELLED"));
        QVERIFY(controller_->isoWriter().statusText().contains(QStringLiteral("incomplete")));
        auto* status = window_->findChild<QLabel*>(QStringLiteral("statusLabel"));
        QCOMPARE(status->text(), QStringLiteral("Operation cancelled by the user"));
        QCOMPARE(status->toolTip(), controller_->isoWriter().statusText());
        QVERIFY(status->fontMetrics().horizontalAdvance(status->text()) <= status->width());
        const QString screenshot = qEnvironmentVariable("RUFUS_TEST_CANCEL_SCREENSHOT");
        if (!screenshot.isEmpty())
            QVERIFY(window_->grab().save(screenshot));
    }

    void isoCopyCompleteStillWaitsForCleanup()
    {
        selectIsoFixture();
        helperMode = QStringLiteral("hold-finalizing");
        startIsoPlan();
        approveIsoPlan();
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().statusText(), QStringLiteral("Finalizing, please wait..."), writeTimeoutMs);
        QCOMPARE(progress_->maximum(), MAX_PROGRESS);
        QVERIFY(progress_->value() < MAX_PROGRESS);
        QVERIFY(progress_->format() != QStringLiteral("READY"));
        QVERIFY(controller_->isoWriter().isWriting());
        QCOMPARE(completionBeeps, 0);
        QCOMPARE(completionAlerts, 0);
        controller_->isoWriter().cancel();
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::Cancelled, writeTimeoutMs);
        QCOMPARE(helperStarts, 1);
    }

    void isoReadyWaitsForHelperExit_data()
    {
        QTest::addColumn<int>("filesystem");
        QTest::newRow("FAT16") << int(FS_FAT16);
        QTest::newRow("FAT32") << int(FS_FAT32);
    }

    void isoReadyWaitsForHelperExit()
    {
        QFETCH(int, filesystem);
        selectIsoFixture();
        controller_->setFileSystem(filesystem);
        helperMode = QStringLiteral("hold-after-result");
        startIsoPlan();
        approveIsoPlan();
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().outcome().copiedBytes, helperCopiedBytes, writeTimeoutMs);
        QVERIFY(helperProcess);
        QCOMPARE(helperProcess->state(), QProcess::Running);
        QCOMPARE(controller_->isoWriter().state(), rufus::IsoWriteState::Writing);
        QCOMPARE(progress_->format(), QStringLiteral("Finalizing, please wait..."));
        QVERIFY(progress_->value() < MAX_PROGRESS);
        QVERIFY(!start_->isEnabled());
        QCOMPARE(close_->text(), QStringLiteral("CANCEL"));
        QCOMPARE(completionBeeps, 0);
        QCOMPARE(completionAlerts, 0);
        auto* elapsed = window_->findChild<QLabel*>(QStringLiteral("elapsedLabel"));
        QVERIFY(elapsed && elapsed->isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(elapsed->text() != QStringLiteral("00:00:00"), uiTimeoutMs * 2);
        activateMainWindow();
        QTRY_VERIFY_WITH_TIMEOUT(close_->hasFocus(), uiTimeoutMs);
        helperProcess->closeWriteChannel();
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::Complete, writeTimeoutMs);
        QCOMPARE(helperProcess->state(), QProcess::NotRunning);
        QCOMPARE(progress_->value(), MAX_PROGRESS);
        QCOMPARE(progress_->format(), QStringLiteral("READY"));
        QCOMPARE(progress_->palette().color(QPalette::Highlight), QColor(0x06, 0xB0, 0x25));
        QVERIFY(progress_->isTextVisible());
        QCOMPARE(close_->text(), QStringLiteral("CLOSE"));
        QVERIFY(close_->isEnabled());
        QCOMPARE(completionBeeps, 1);
        QCOMPARE(completionAlerts, 1);
        const QString completedTime = elapsed->text();
        QTest::qWait(1100); // A stopped clock must remain unchanged past the next tick.
        QCOMPARE(elapsed->text(), completedTime);
        // Unplugging after READY is expected and must not retroactively fail.
        controller_->setDiscovery({}, {});
        QCOMPARE(controller_->isoWriter().state(), rufus::IsoWriteState::Complete);
        QCOMPARE(progress_->format(), QStringLiteral("READY"));
        QVERIFY(!start_->isEnabled());
        QCOMPARE(completionBeeps, 1);
        QCOMPARE(completionAlerts, 1);
        QCOMPARE(helperStarts, 1);
    }

    void isoPreflightCancellationKeepsGuiResponsive()
    {
        selectIsoFixture();
        faultKind = PauseIsoCheck;
        QTest::mouseClick(start_, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(pausedIo.available(), 1, fixtureTimeoutMs);
        QVERIFY(!select_->isEnabled());
        QTest::mouseClick(close_, Qt::LeftButton);
        QCOMPARE(controller_->isoWriter().state(), rufus::IsoWriteState::Cancelling);
        QVERIFY(!close_->isEnabled());
        QVERIFY(window_->isVisible());
        resumeIo.release();
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::Cancelled, writeTimeoutMs);
        QCOMPARE(helperStarts, 0);
        QCOMPARE(stat(QStringLiteral("opens")), 0);
    }

    void isoRemovalWaitsForHelperCleanup()
    {
        selectIsoFixture();
        helperMode = QStringLiteral("hold");
        startIsoPlan();
        approveIsoPlan();
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().statusText(), QStringLiteral("Copying ISO files: 50.0%"), writeTimeoutMs);
        QCOMPARE(progress_->maximum(), MAX_PROGRESS);
        QCOMPARE(progress_->value(), 35061); // FAT32: 9% preparation + half of its 89% copy span.
        QCOMPARE(progress_->format(), QStringLiteral("Copying ISO files: 50.0%"));
        controller_->setDiscovery({}, {});
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::Failed, writeTimeoutMs);
        QCOMPARE(helperStarts, 1);
        QCOMPARE(progress_->format(), QStringLiteral("FAILED"));
        QVERIFY(controller_->isoWriter().statusText().contains(QStringLiteral("removed")));
        QVERIFY(controller_->selection().deviceId.isEmpty());
        QVERIFY(!start_->isEnabled());
    }

    void isoRejectsChangesSinceSelect()
    {
        selectIsoFixture();
        writeFile(sourcePath_, sourceBytes_);
        QTest::mouseClick(start_, Qt::LeftButton);
        QCOMPARE(controller_->isoWriter().state(), rufus::IsoWriteState::Failed);
        QCOMPARE(helperStarts, 0);
    }

    void isoHybridKeepsModesAndWritesExplicitDd_data()
    {
        QTest::addColumn<bool>("mounted");
        QTest::addColumn<bool>("fixedUsb");
        QTest::newRow("unmounted") << false << false;
        QTest::newRow("mounted") << true << false;
        QTest::newRow("unmounted-fixed") << false << true;
        QTest::newRow("mounted-fixed") << true << true;
    }

    void isoHybridKeepsModesAndWritesExplicitDd()
    {
        QFETCH(bool, mounted); QFETCH(bool, fixedUsb);
        selectIsoFixture(true);
        if (fixedUsb)
        {
            selectFixedUsb();
            writer().setHelper(QCoreApplication::applicationDirPath() + QStringLiteral("/iso_helper_fixture"));
        }
        if (mounted)
        {
            writer().setHelper(QCoreApplication::applicationDirPath() + QStringLiteral("/iso_helper_fixture"));
            objects_[disk][fsInterface][QStringLiteral("MountPoints")] =
                QVariant::fromValue(QList<QByteArray>{QByteArray("/run/media/user/USB") + '\0'});
            setObjects();
        }
        QTest::mouseClick(start_, Qt::LeftButton);
        auto* dialog = window_->findChild<rufus::ImageModeDialog*>();
        QVERIFY(dialog && dialog->isVisible());
        const auto radios = dialog->findChildren<QRadioButton*>();
        QCOMPARE(radios.size(), 3);
        QVERIFY(radios[0]->isChecked());
        QVERIFY(radios[1]->isEnabled());
        QVERIFY(radios[2]->isEnabled());
        QTest::keyClick(dialog, Qt::Key_Escape);
        QCOMPARE(helperStarts, 0);
        QCOMPARE(stat(QStringLiteral("opens")), 0);
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::Cancelled, uiTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(window_->findChildren<rufus::ImageModeDialog*>().isEmpty(), uiTimeoutMs);
        // DD does not format: invalid ISO formatting options cannot veto it.
        controller_->setVolumeLabel(QStringLiteral("bad\nlabel"));
        QTest::mouseClick(start_, Qt::LeftButton);
        dialog = window_->findChild<rufus::ImageModeDialog*>();
        QVERIFY(dialog);
        const auto choices = dialog->findChildren<QRadioButton*>();
        choices.last()->setChecked(true);
        QTest::mouseClick(dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok), Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(state(), State::AwaitingConfirmation, fixtureTimeoutMs);
        approvePlan();
        QTRY_COMPARE_WITH_TIMEOUT(state(), State::Complete, writeTimeoutMs);
        QCOMPARE(helperStarts, 1);
        QCOMPARE(stat(QStringLiteral("opens")), 0);
        QCOMPARE(readFile(targetPath_), expectedTarget_); // Protocol-only helper.
    }

    void windowsToGoFlow_data()
    {
        QTest::addColumn<QString>("kind");
        QTest::newRow("standalone-WIM") << QStringLiteral("standalone");
        QTest::newRow("ISO-WIM") << QStringLiteral("iso");
        QTest::newRow("archive-and-edition-cancel") << QStringLiteral("cancel");
        QTest::newRow("missing-completion-marker") << QStringLiteral("missing-marker");
    }

    void windowsToGoFlow()
    {
        QFETCH(QString, kind);
        selectIsoFixture(false, true);
        nativeCapacity = 1024ULL * 1024 * 1024;
        objects_[disk][blockInterface][QStringLiteral("Size")] = QVariant::fromValue(quint64(nativeCapacity));
        setObjects();
        const QString owner = device_.serviceOwner;
        QVERIFY(rufus::DescribeUDisksDevice(objects_, disk.path(), device_));
        device_.id = QStringLiteral("iso-device");
        device_.serviceOwner = owner;
        controller_->setDiscovery({device_}, {});
        controller_->selectDevice(device_.id);
        const bool iso = kind == "iso" || kind == "cancel";
        if (iso)
        {
            const QString tree = directory_.filePath(QStringLiteral("iso-tree"));
            const QString archive = tree + "/sources/install.wim";
            QVERIFY(QFile::remove(archive));
            QVERIFY(CreateWindowsToGoFixture(archive));
            if (kind == "cancel")
            {
                QVERIFY(QDir().mkpath(tree + "/x64/sources"));
                QVERIFY(QFile::copy(archive, tree + "/x64/sources/install.wim"));
            }
            QProcess creator;
            creator.start(QStringLiteral(RUFUS_MKISOFS), {"-quiet", "-R", "-J", "-o", sourcePath_, tree});
            QVERIFY(creator.waitForFinished(writeTimeoutMs));
            QCOMPARE(creator.exitCode(), 0);
        }
        else
        {
            sourcePath_ = directory_.filePath(QStringLiteral("windows-%1.wim").arg(kind));
            QVERIFY(CreateWindowsToGoFixture(sourcePath_));
        }
        controller_->selectImage(sourcePath_);
        QTRY_VERIFY_WITH_TIMEOUT(!controller_->isAnalyzing(), writeTimeoutMs);
        QVERIFY(controller_->canSelectWindowsToGo());
        if (iso) controller_->setWindowsToGo(true);
        QVERIFY(controller_->selection().windowsToGo);
        QCOMPARE(controller_->selection().fileSystems, FS_MASK(FS_NTFS));
        auto* imageOption = window_->findChild<QComboBox*>(QStringLiteral("imageOptionCombo"));
        QCOMPARE(imageOption->count(), iso ? 2 : 1);
        QCOMPARE(imageOption->currentText(), QStringLiteral("Windows To Go"));
        QVERIFY(start_->isEnabled());
        QCOMPARE(helperStarts, 0);
        QTest::mouseClick(start_, Qt::LeftButton);
        if (kind == "cancel")
        {
            QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::ChoosingWindowsArchive, writeTimeoutMs);
            auto* dialog = window_->findChild<rufus::WindowsImageDialog*>(QStringLiteral("windowsArchiveDialog"));
            QVERIFY(dialog);
            dialog->findChild<QComboBox*>()->setCurrentIndex(1);
            dialog->accept();
        }
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::ChoosingWindowsEdition, writeTimeoutMs);
        auto* editions = window_->findChild<rufus::WindowsImageDialog*>(QStringLiteral("windowsEditionDialog"));
        QVERIFY(editions);
        QCOMPARE(helperStarts, 0);
        if (kind == "cancel")
        {
            editions->reject();
            QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::Cancelled, writeTimeoutMs);
            QCOMPARE(helperStarts, 0);
            QCOMPARE(readFile(targetPath_), expectedTarget_);
            return;
        }
        editions->findChild<QComboBox*>()->setCurrentIndex(1);
        editions->accept();
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::ChoosingWindowsOptions, writeTimeoutMs);
        auto* options = window_->findChild<rufus::WindowsUserDialog*>();
        QVERIFY(options);
        options->accept();
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::AwaitingConfirmation, writeTimeoutMs);
        QCOMPARE(controller_->isoWriter().request().windowsEdition, 2);
        QCOMPARE(controller_->isoWriter().request().windowsInstallPath, iso ? QStringLiteral("/sources/install.wim") : QString());
        QCOMPARE(helperStarts, 0);
        helperCopiedBytes = controller_->isoWriter().check().windowsApplyBytes;
        QVERIFY(helperCopiedBytes > 0);
        helperMode = kind == "missing-marker" ? QStringLiteral("missing-togo") : QStringLiteral("hold-after-result");
        auto* confirmation = window_->findChild<QMessageBox*>(QStringLiteral("isoWriteConfirmation"));
        QVERIFY(confirmation && confirmation->informativeText().contains(QStringLiteral("Windows To Go")));
        QVERIFY(confirmation->informativeText().contains(QStringLiteral("Windows edition 2")));
        approveIsoPlan();
        if (kind != "missing-marker")
        {
            QTRY_VERIFY_WITH_TIMEOUT(controller_->isoWriter().outcome().windowsToGoDone, writeTimeoutMs);
            QCOMPARE(controller_->isoWriter().state(), rufus::IsoWriteState::Writing);
            QVERIFY(progress_->value() < MAX_PROGRESS && progress_->format() != QStringLiteral("READY"));
            QCOMPARE(completionBeeps, 0);
            QVERIFY(helperProcess);
            helperProcess->write("exit\n");
        }
        const auto expected = kind == "missing-marker" ? rufus::IsoWriteState::Failed : rufus::IsoWriteState::Complete;
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), expected, writeTimeoutMs);
        QCOMPARE(helperStarts, 1);
        QCOMPARE(readFile(targetPath_), expectedTarget_); // Protocol child never opens the target.
        QCOMPARE(progress_->format() == QStringLiteral("READY"), kind != "missing-marker");
    }

    void isoLoadRestoresWindowsControlsAndFocus()
    {
        controller_->selectImage(directory_.filePath(QStringLiteral("missing.iso")));
        QCOMPARE(window_->findChild<QComboBox*>(QStringLiteral("partitionCombo"))->currentText(), QStringLiteral("MBR"));
        QVERIFY(!window_->findChild<QComboBox*>(QStringLiteral("partitionCombo"))->isEnabled());
        selectIsoFixture(false, true);
        activateMainWindow();
        auto* options = window_->findChild<QWidget*>(QStringLiteral("imageOptions"));
        auto* installation = window_->findChild<QComboBox*>(QStringLiteral("imageOptionCombo"));
        QVERIFY(options && installation && options->isVisible() && options->isEnabled());
        QCOMPARE(installation->currentText(), QStringLiteral("Standard Windows installation"));
        QCOMPARE(installation->itemText(1), QStringLiteral("Windows To Go"));
        QVERIFY(installation->model()->flags(installation->model()->index(1, 0)) & Qt::ItemIsEnabled);
        QVERIFY(!window_->findChild<QWidget*>(QStringLiteral("persistenceSlider"))->isVisible());
        auto* partition = window_->findChild<QComboBox*>(QStringLiteral("partitionCombo"));
        auto* target = window_->findChild<QComboBox*>(QStringLiteral("targetCombo"));
        QCOMPARE(partition->count(), 1);
        QCOMPARE(target->count(), 1);
        QVERIFY(partition->isEnabled() && target->isEnabled());
        QCOMPARE(partition->currentText(), QStringLiteral("GPT"));
        QCOMPARE(target->currentText(), QStringLiteral("UEFI (non CSM)"));
        controller_->selectImage(sourcePath_);
        QVERIFY(!select_->isEnabled() && !partition->isEnabled() && !target->isEnabled());
        QVERIFY(!options->isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(!controller_->isAnalyzing(), writeTimeoutMs);
        QTRY_VERIFY_WITH_TIMEOUT(start_->hasFocus(), uiTimeoutMs);
        QCOMPARE(window_->findChild<QLineEdit*>(QStringLiteral("volumeEdit"))->text(), QStringLiteral("SOURCE"));
        QCOMPARE(progress_->format(), QStringLiteral("READY"));
        QCOMPARE(progress_->value(), 0);
        QCOMPARE(window_->findChild<QLabel*>(QStringLiteral("statusLabel"))->text(), QStringLiteral("Using image: source.iso"));
        QCOMPARE(window_->findChild<QLabel*>(QStringLiteral("elapsedLabel"))->text(), QStringLiteral("00:00:00"));
        const QString screenshot = qEnvironmentVariable("RUFUS_TEST_WINDOWS_SELECTION_SCREENSHOT");
        if (!screenshot.isEmpty())
            QVERIFY(window_->grab().save(screenshot));
        controller_->setDiscovery({}, {});
        QVERIFY(!start_->isEnabled() && !options->isEnabled());
        QVERIFY(!partition->isEnabled() && !target->isEnabled());
        QVERIFY(!window_->findChild<QLineEdit*>(QStringLiteral("volumeEdit"))->isEnabled());
    }

    void replacingWindowsWithRawImageHidesInstallationOptions()
    {
        selectIsoFixture(false, true);
        activateMainWindow();
        const int windowsHeight = window_->height();
        const QString raw = directory_.filePath(QStringLiteral("replacement.img"));
        QByteArray bytes(512, '\0');
        bytes[510] = '\x55';
        bytes[511] = '\xaa';
        writeFile(raw, bytes);
        controller_->selectImage(raw);
        QTRY_VERIFY_WITH_TIMEOUT(start_->hasFocus(), uiTimeoutMs);
        QVERIFY(!window_->findChild<QWidget*>(QStringLiteral("imageOptions"))->isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(window_->height() < windowsHeight, uiTimeoutMs);
        QVERIFY(!window_->findChild<QComboBox*>(QStringLiteral("fileSystemCombo"))->isEnabled());
        QVERIFY(!window_->findChild<QCheckBox*>(QStringLiteral("quickFormat"))->isChecked());
        QVERIFY(!window_->findChild<QCheckBox*>(QStringLiteral("extendedLabel"))->isChecked());
        QVERIFY(controller_->selection().volumeLabel.isEmpty());
        QCOMPARE(progress_->format(), QStringLiteral("READY"));
        QCOMPARE(helperStarts, 0);
    }

    void realWindowsIsoLoadsWithoutStartingCreation()
    {
        const QString image = qEnvironmentVariable("RUFUS_TEST_WINDOWS_ISO");
        if (image.isEmpty())
            QSKIP("Set RUFUS_TEST_WINDOWS_ISO for read-only selection of a Windows 11 ISO.");
        QVERIFY(QFileInfo(image).isFile());
        // Only advertised capacity changes. No 32-GiB file is allocated and
        // START is never pressed: the existing regular-file target stays intact.
        constexpr quint64 usbCapacity = 32ULL * 1024 * 1024 * 1024;
        objects_[disk][blockInterface][QStringLiteral("Size")] = QVariant::fromValue(usbCapacity);
        setObjects();
        const QString owner = device_.serviceOwner;
        QVERIFY(rufus::DescribeUDisksDevice(objects_, disk.path(), device_));
        device_.id = QStringLiteral("windows-preview-device");
        device_.serviceOwner = owner;
        controller_->setDiscovery({device_}, {});
        controller_->selectDevice(device_.id);
        controller_->isoWriter().configure(*connection_, QCoreApplication::applicationDirPath() + QStringLiteral("/iso_helper_fixture"));
        activateMainWindow();
        controller_->selectImage(image);
        constexpr int realScanTimeoutMs = 60000;
        QTRY_VERIFY_WITH_TIMEOUT(!controller_->isAnalyzing(), realScanTimeoutMs);
        const rufus::Selection& selection = controller_->selection();
        QVERIFY2(HAS_WINTOGO(selection.imageAnalysis.report), qPrintable(controller_->imageStatusText()));
        QVERIFY(selection.imageAnalysis.report.has_4GB_file);
        QCOMPARE(selection.imageAnalysis.report.win_version.major, quint16(11));
        QCOMPARE(selection.fileSystem, FS_NTFS);
        QVERIFY(!selection.syslinux);
        QCOMPARE(window_->findChild<QComboBox*>(QStringLiteral("imageOptionCombo"))->currentText(),
            QStringLiteral("Standard Windows installation"));
        QVERIFY(window_->findChild<QWidget*>(QStringLiteral("imageOptions"))->isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(start_->hasFocus(), uiTimeoutMs);
        QCOMPARE(progress_->format(), QStringLiteral("READY"));
        QCOMPARE(progress_->value(), 0);
        QCOMPARE(helperStarts, 0);
        QCOMPARE(stat(QStringLiteral("opens")), 0);
        const QString screenshot = qEnvironmentVariable("RUFUS_TEST_WINDOWS_SELECTION_SCREENSHOT");
        if (!screenshot.isEmpty())
            QVERIFY(window_->grab().save(screenshot));
        auto* partition = window_->findChild<QComboBox*>(QStringLiteral("partitionCombo"));
        auto* target = window_->findChild<QComboBox*>(QStringLiteral("targetCombo"));
        QVERIFY(partition && target);
        QVERIFY(controller_->canSelectWindowsBios());
        QCOMPARE(partition->count(), 2);
        QCOMPARE(partition->currentText(), QStringLiteral("GPT"));
        partition->setCurrentIndex(partition->findData(int(rufus::PartitionScheme::Mbr)));
        QVERIFY(QMetaObject::invokeMethod(partition, "activated", Q_ARG(int, partition->currentIndex())));
        QCOMPARE(target->currentText(), QStringLiteral("BIOS (or UEFI-CSM)"));
        QCOMPARE(controller_->selection().fileSystems, quint32(FS_MASK(FS_NTFS)));
        QTest::keyClick(window_.get(), Qt::Key_E, Qt::AltModifier);
        QCOMPARE(target->currentText(), QStringLiteral("BIOS or UEFI"));
        QCOMPARE(partition->currentText(), QStringLiteral("MBR"));
        QCOMPARE(controller_->selection().fileSystem, int(FS_NTFS));
        auto* filesystems = window_->findChild<QComboBox*>(QStringLiteral("fileSystemCombo"));
        QVERIFY(filesystems);
        QCOMPARE(filesystems->findData(FS_FAT16), -1);
        QVERIFY(filesystems->findData(FS_FAT32) >= 0);
        controller_->setFileSystem(FS_FAT32);
        QCOMPARE(controller_->selection().fileSystem, int(FS_FAT32));
        QCOMPARE(target->currentText(), QStringLiteral("BIOS or UEFI"));
        const QString fatScreenshot = qEnvironmentVariable("RUFUS_TEST_WINDOWS_FAT_SELECTION_SCREENSHOT");
        if (!fatScreenshot.isEmpty())
            QVERIFY(window_->grab().save(fatScreenshot));
        QTest::keyClick(window_.get(), Qt::Key_A, Qt::AltModifier);
        QVERIFY(!controller_->windowsRufusMbr());
        QCOMPARE(helperStarts, 0);
        QCOMPARE(stat(QStringLiteral("opens")), 0);
        QCOMPARE(readFile(targetPath_), expectedTarget_);
        QCOMPARE(progress_->format(), QStringLiteral("READY"));
        QCOMPARE(progress_->value(), 0);
    }

    void ddOnlyIsoDisablesFormattingAndExplainsMode()
    {
        selectIsoFixture(true, false, "win11", nullptr, true, true);
        auto* notice = window_->findChild<QMessageBox*>(QStringLiteral("imageNotice"));
        QVERIFY(notice && notice->isVisible());
        QCOMPARE(notice->windowTitle(), QStringLiteral("ISOHybrid image detected"));
        QVERIFY(notice->text().contains(QStringLiteral("DD image writing mode will be enforced")));
        QVERIFY(!window_->findChild<QComboBox*>(QStringLiteral("partitionCombo"))->isEnabled());
        QVERIFY(!window_->findChild<QComboBox*>(QStringLiteral("targetCombo"))->isEnabled());
        QVERIFY(!window_->findChild<QComboBox*>(QStringLiteral("fileSystemCombo"))->isEnabled());
        QVERIFY(!window_->findChild<QComboBox*>(QStringLiteral("clusterCombo"))->isEnabled());
        QVERIFY(!window_->findChild<QLineEdit*>(QStringLiteral("volumeEdit"))->isEnabled());
        QVERIFY(!window_->findChild<QCheckBox*>(QStringLiteral("quickFormat"))->isChecked());
        QVERIFY(!window_->findChild<QCheckBox*>(QStringLiteral("extendedLabel"))->isChecked());
        QCOMPARE(progress_->format(), QStringLiteral("READY"));
        QCOMPARE(progress_->value(), 0);
        QCOMPARE(window_->findChild<QComboBox*>(QStringLiteral("partitionCombo"))->count(), 0);
        QCOMPARE(window_->findChild<QComboBox*>(QStringLiteral("targetCombo"))->count(), 0);
        QTest::mouseClick(notice->button(QMessageBox::Ok), Qt::LeftButton);
        activateMainWindow();
        QTRY_VERIFY_WITH_TIMEOUT(start_->hasFocus(), uiTimeoutMs);
        startPlan(); // DD goes straight to the normal destructive confirmation.
        QVERIFY(!window_->findChild<rufus::ImageModeDialog*>());
        QCOMPARE(stat(QStringLiteral("opens")), 0);
        QCOMPARE(helperStarts, 0);
    }

    void isoLabelOverridesDeviceLabelAndPreservesManualEdit()
    {
        objects_[disk][blockInterface][QStringLiteral("IdUsage")] = QStringLiteral("filesystem");
        objects_[disk][blockInterface][QStringLiteral("IdLabel")] = QStringLiteral("EXISTING");
        objects_[disk][blockInterface][QStringLiteral("IdType")] = QStringLiteral("ntfs");
        selectIsoFixture(false, true);
        controller_->selectImage(sourcePath_);
        QTRY_VERIFY_WITH_TIMEOUT(!controller_->isAnalyzing(), writeTimeoutMs);
        auto* label = window_->findChild<QLineEdit*>(QStringLiteral("volumeEdit"));
        QCOMPARE(label->text(), QStringLiteral("SOURCE"));
        QCOMPARE(controller_->selection().fileSystem, FS_FAT32);
        controller_->setFileSystem(FS_NTFS);
        controller_->setClusterSize(8192);
        device_.volumeLabel = QStringLiteral("CHANGED");
        device_.fileSystemVersion = QStringLiteral("3.1");
        controller_->setDiscovery({device_}, {});
        QCOMPARE(label->text(), QStringLiteral("SOURCE"));
        QCOMPARE(controller_->selection().fileSystem, FS_NTFS);
        QCOMPARE(controller_->selection().clusterSize, quint32(8192));
        label->selectAll();
        QTest::keyClicks(label, QStringLiteral("CUSTOM"));
        controller_->setDiscovery({device_}, {});
        QCOMPARE(label->text(), QStringLiteral("CUSTOM"));
        controller_->selectImage(directory_.filePath(QStringLiteral("missing.iso")));
        QCOMPARE(label->text(), QStringLiteral("CUSTOM"));
        QCOMPARE(controller_->selection().fileSystem, FS_NTFS);
        QVERIFY(!label->isEnabled());
        QVERIFY(!start_->isEnabled());
        controller_->selectImage(sourcePath_);
        QTRY_VERIFY_WITH_TIMEOUT(!controller_->isAnalyzing(), writeTimeoutMs);
        QCOMPARE(label->text(), QStringLiteral("SOURCE"));
        QCOMPARE(helperStarts, 0);
        QCOMPARE(stat(QStringLiteral("opens")), 0);
    }

    void deviceChangeKeepsFilesystemAndUserLabel()
    {
        selectIsoFixture(false, true);
        controller_->setFileSystem(FS_NTFS);
        controller_->setClusterSize(8192);
        rufus::Device second = device_;
        second.id = QStringLiteral("second-device");
        second.path = QStringLiteral("/test/second-device");
        second.fileSystemType = QStringLiteral("vfat");
        second.fileSystemVersion = QStringLiteral("FAT32");
        controller_->setDiscovery({device_, second}, {});
        auto* devices = window_->findChild<QComboBox*>(QStringLiteral("deviceCombo"));
        devices->setCurrentIndex(devices->findData(second.id));
        QVERIFY(QMetaObject::invokeMethod(devices, "activated", Q_ARG(int, devices->currentIndex())));
        QCOMPARE(controller_->selection().deviceId, second.id);
        QCOMPARE(controller_->selection().fileSystem, FS_NTFS);
        QCOMPARE(controller_->selection().clusterSize, controller_->selection().clusterSizes.Default);
        QCOMPARE(controller_->selection().volumeLabel, QStringLiteral("NEW LABEL"));
        QCOMPARE(window_->findChild<QComboBox*>(QStringLiteral("fileSystemCombo"))->currentData().toInt(), FS_NTFS);
        QCOMPARE(helperStarts, 0);
        QCOMPARE(stat(QStringLiteral("opens")), 0);
    }

    void syslinuxSelectionAndCompletion_data()
    {
        QTest::addColumn<QByteArray>("version");
        QTest::addColumn<int>("filesystem");
        QTest::newRow("known-FAT32") << QByteArray("6.04 6.04-pre1") << int(FS_FAT32);
        QTest::newRow("known-FAT16") << QByteArray("6.04 6.04-pre1") << int(FS_FAT16);
        QTest::newRow("unknown-FAT32") << QByteArray("unknown") << int(FS_FAT32);
        QTest::newRow("unknown-FAT16") << QByteArray("unknown") << int(FS_FAT16);
        QTest::newRow("known-NTFS") << QByteArray("6.04 6.04-pre1") << int(FS_NTFS);
        QTest::newRow("unknown-NTFS") << QByteArray("unknown") << int(FS_NTFS);
    }

    void syslinuxSelectionAndCompletion()
    {
        QFETCH(QByteArray, version);
        QFETCH(int, filesystem);
        const QString filesystemName = filesystem == FS_NTFS ? QStringLiteral("NTFS") :
            filesystem == FS_FAT16 ? QStringLiteral("FAT16") : QStringLiteral("FAT32");
        if (version == "unknown" && !QFileInfo::exists(syslinuxModulePath()))
            QSKIP("A qualified upstream companion is required for unknown-version success.");
        selectIsoFixture(true, false, "win11", version.constData());
        QVERIFY(controller_->selection().syslinux);
        QCOMPARE(controller_->selection().fileSystems, quint32(FS_MASK(FS_FAT16) | FS_MASK(FS_FAT32) | FS_MASK(FS_NTFS)));
        QCOMPARE(controller_->selection().fileSystem, int(FS_FAT32));
        auto* filesystems = window_->findChild<QComboBox*>(QStringLiteral("fileSystemCombo"));
        QCOMPARE(filesystems->itemText(0), QStringLiteral("FAT16 (Default)"));
        auto* partitions = window_->findChild<QComboBox*>(QStringLiteral("partitionCombo"));
        QCOMPARE(partitions->currentText(), QStringLiteral("MBR"));
        QCOMPARE(partitions->count(), 2);
        // Use the real Widgets signal path for both directions.
        partitions->setCurrentIndex(partitions->findData(int(rufus::PartitionScheme::Gpt)));
        QVERIFY(QMetaObject::invokeMethod(partitions, "activated", Q_ARG(int, partitions->currentIndex())));
        QVERIFY(!controller_->selection().syslinux);
        QCOMPARE(window_->findChild<QComboBox*>(QStringLiteral("targetCombo"))->currentText(), QStringLiteral("UEFI (non CSM)"));
        filesystems->setCurrentIndex(filesystems->findData(filesystem));
        QVERIFY(QMetaObject::invokeMethod(filesystems, "activated", Q_ARG(int, filesystems->currentIndex())));
        QCOMPARE(controller_->selection().fileSystem, filesystem);
        partitions->setCurrentIndex(partitions->findData(int(rufus::PartitionScheme::Mbr)));
        QVERIFY(QMetaObject::invokeMethod(partitions, "activated", Q_ARG(int, partitions->currentIndex())));
        QCOMPARE(controller_->selection().fileSystem, filesystem);
        QVERIFY(filesystems->findData(FS_FAT16) >= 0);
        const quint32 clusterSize = filesystem == FS_NTFS ? 4096 : filesystem == FS_FAT16 ? 2048 : 512;
        QCOMPARE(controller_->selection().clusterSize, clusterSize);
        QCOMPARE(window_->findChild<QComboBox*>(QStringLiteral("targetCombo"))->currentText(), QStringLiteral("BIOS or UEFI"));
        controller_->toggleEnhancedInstall(); // Windows-only preference must not enter this job.
        chooseImageMode();
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::AwaitingConfirmation, writeTimeoutMs);
        const auto& request = controller_->isoWriter().request();
        QVERIFY(request.syslinux && !request.enhancedInstall);
        QCOMPARE(request.fileSystem, filesystem);
        QCOMPARE(request.uefiNtfs, filesystem == FS_NTFS);
        QVERIFY(!request.isStandaloneUefiNtfs());
        QCOMPARE(request.imagePath, sourcePath_);
        QCOMPARE(request.clusterSize, clusterSize);
        QCOMPARE(request.windowsFlags, 0);
        QCOMPARE(controller_->isoWriter().check().tree.bytes_written, helperCopiedBytes);
        if (version == "unknown")
        {
            QCOMPARE(controller_->isoWriter().check().analysis.report.sl_detected_version, quint16(0));
            QVERIFY(controller_->isoWriter().check().analysis.report.sl_version_inferred);
        }
        auto* confirmation = window_->findChild<QMessageBox*>(QStringLiteral("isoWriteConfirmation"));
        QVERIFY(confirmation && confirmation->informativeText().contains(
            QStringLiteral("ISO Image — MBR, %1, BIOS or UEFI\n").arg(filesystemName)));
        QCOMPARE(helperStarts, 0);
        const int toolChecks = filesystem == FS_NTFS ? 1 : 3;
        QCOMPARE(syslinuxToolChecks.load(), toolChecks);
        helperMode = QStringLiteral("hold-after-result");
        approveIsoPlan();
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().outcome().copiedBytes, helperCopiedBytes, writeTimeoutMs);
        QCOMPARE(helperStarts, 1);
        QCOMPARE(syslinuxToolChecks.load(), 2 * toolChecks);
        QCOMPARE(progress_->value(), filesystem == FS_NTFS ? 64224 : 64879); // First NTFS/FAT finalization slot: 98%/99%.
        QCOMPARE(completionBeeps, 0);
        QVERIFY(!partitions->isEnabled());
        helperProcess->closeWriteChannel();
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::Complete, writeTimeoutMs);
        QCOMPARE(controller_->isoWriter().statusText(), QStringLiteral("ISO image creation completed."));
        QCOMPARE(progress_->format(), QStringLiteral("READY"));
        QCOMPARE(progress_->value(), MAX_PROGRESS);
        QCOMPARE(completionBeeps, 1);
        QCOMPARE(completionAlerts, 1);
        controller_->setDiscovery({}, {});
        QCOMPARE(controller_->isoWriter().state(), rufus::IsoWriteState::Complete);
        QCOMPARE(progress_->format(), QStringLiteral("READY"));
        QCOMPARE(completionBeeps, 1);
        QCOMPARE(completionAlerts, 1);
        QCOMPARE(readFile(targetPath_), expectedTarget_);
    }

    void exfatNonBootableControls()
    {
        controller_->selectImage(directory_.filePath(QStringLiteral("missing.iso")));
        selectFormat();
        auto* boot = window_->findChild<QComboBox*>(QStringLiteral("bootCombo"));
        auto* filesystems = window_->findChild<QComboBox*>(QStringLiteral("fileSystemCombo"));
        auto* clusters = window_->findChild<QComboBox*>(QStringLiteral("clusterCombo"));
        auto* partitions = window_->findChild<QComboBox*>(QStringLiteral("partitionCombo"));
        QCOMPARE(boot->currentText(), QStringLiteral("Non bootable"));
        QCOMPARE(boot->count(), 4);
        QCOMPARE(boot->itemText(2), QStringLiteral("MS-DOS"));
        QCOMPARE(boot->itemText(3), QStringLiteral("FreeDOS"));
        QVERIFY(select_->isEnabled());
        QCOMPARE(filesystems->count(), 5);
        QVERIFY(filesystems->findData(FS_UDF) >= 0);
        QCOMPARE(filesystems->itemText(0), QStringLiteral("FAT16 (Default)"));
        QCOMPARE(filesystems->currentText(), QStringLiteral("exFAT"));
        QCOMPARE(clusters->currentText(), QStringLiteral("4096 bytes (Default)"));
        QCOMPARE(partitions->count(), 3);
        QCOMPARE(partitions->currentText(), QStringLiteral("MBR"));
        QVERIFY((controller_->selection().partition == rufus::PartitionScheme::Mbr));
        QCOMPARE(window_->findChild<QComboBox*>(QStringLiteral("targetCombo"))->currentText(), QStringLiteral("BIOS or UEFI"));
        const QString screenshot = qEnvironmentVariable("RUFUS_TEST_EXFAT_SCREENSHOT");
        if (!screenshot.isEmpty())
            QVERIFY(window_->grab().save(screenshot));
        QVERIFY(window_->findChild<QLineEdit*>(QStringLiteral("volumeEdit"))->isEnabled());
        QVERIFY(window_->findChild<QCheckBox*>(QStringLiteral("quickFormat"))->isChecked());
        QVERIFY(!window_->findChild<QCheckBox*>(QStringLiteral("extendedLabel"))->isChecked());
        controller_->setPartitionScheme(rufus::PartitionScheme::Mbr);
        QCOMPARE(controller_->selection().fileSystem, int(FS_EXFAT));
        QVERIFY(!controller_->selection().syslinux);
        QVERIFY(!controller_->canSelectSyslinux());
        QCOMPARE(controller_->isoWriteChoices(0, true, false).count, 0);
        controller_->setClusterSize(32 * 1024 * 1024); // Metadata does not fit this disk.
        QCOMPARE(controller_->selection().clusterSize, quint32(4096));
        boot->setCurrentIndex(1);
        QVERIFY(QMetaObject::invokeMethod(boot, "activated", Q_ARG(int, 1)));
        QVERIFY(controller_->selection().bootType == BT_IMAGE);
        QVERIFY(select_->isEnabled());
        QVERIFY(!start_->isEnabled());
        QCOMPARE(filesystems->findData(FS_EXFAT), -1);
        QCOMPARE(helperStarts, 0);
        QCOMPARE(readFile(targetPath_), expectedTarget_);
    }

    void revokedBootloaderWarning()
    {
        selectIsoFixture();
        const QString tree = directory_.filePath(QStringLiteral("iso-tree"));
        const QByteArray bootloader = secureboot_fixture::signedBootloader(secureboot_fixture::sbatImage("shim,3,Fixture\n"));
        QVERIFY(!bootloader.isEmpty());
        writeFile(tree + QStringLiteral("/EFI/BOOT/BOOTX64.EFI"), bootloader);
        QProcess iso;
        iso.start(QStringLiteral(RUFUS_MKISOFS), {QStringLiteral("-quiet"), QStringLiteral("-R"), QStringLiteral("-J"),
            QStringLiteral("-V"), QStringLiteral("SOURCE"), QStringLiteral("-o"), sourcePath_, tree});
        QVERIFY(iso.waitForFinished(writeTimeoutMs)); QCOMPARE(iso.exitCode(), 0);
        sourceBytes_ = readFile(sourcePath_); controller_->selectImage(sourcePath_);
        QTRY_VERIFY_WITH_TIMEOUT(!controller_->isAnalyzing(), writeTimeoutMs);
        QCOMPARE(controller_->selection().imageAnalysis.report.has_secureboot_bootloader, uint8_t(8));
        QVERIFY(!(controller_->selection().imageAnalysis.pending & ISO_PENDING_SECURE_BOOT));
        const auto originalPolicy = controller_->secureBootPolicy();
        auto refreshedPolicy = originalPolicy; refreshedPolicy.sbat = "shim,1";
        controller_->setSecureBootPolicy(refreshedPolicy);
        QCOMPARE(controller_->selection().imageAnalysis.report.has_secureboot_bootloader, uint8_t(8));
        controller_->selectImage(sourcePath_);
        QTRY_VERIFY_WITH_TIMEOUT(!controller_->isAnalyzing(),writeTimeoutMs);
        QCOMPARE(controller_->selection().imageAnalysis.report.has_secureboot_bootloader, uint8_t(0));
        controller_->setSecureBootPolicy(originalPolicy); controller_->selectImage(sourcePath_);
        QTRY_VERIFY_WITH_TIMEOUT(!controller_->isAnalyzing(),writeTimeoutMs);
        QCOMPARE(controller_->selection().imageAnalysis.report.has_secureboot_bootloader, uint8_t(8));
        QVERIFY(controller_->canStartWrite());
        QTest::mouseClick(start_, Qt::LeftButton);
        auto* warning = window_->findChild<QMessageBox*>(QStringLiteral("revocationWarning"));
        QVERIFY(warning && warning->isVisible());
        QCOMPARE(warning->defaultButton(), warning->button(QMessageBox::Cancel));
        QVERIFY(warning->text().contains(QStringLiteral("Security Violation")));
        QCOMPARE(helperStarts, 0); QCOMPARE(stat(QStringLiteral("opens")), 0);
        QTest::mouseClick(warning->button(QMessageBox::Cancel), Qt::LeftButton);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCOMPARE(controller_->isoWriter().state(), rufus::IsoWriteState::Idle);
        QTest::mouseClick(start_, Qt::LeftButton);
        warning = window_->findChild<QMessageBox*>(QStringLiteral("revocationWarning"));
        QVERIFY(warning && warning->isVisible());
        QTest::mouseClick(warning->button(QMessageBox::Ok), Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::AwaitingConfirmation, writeTimeoutMs);
        QCOMPARE(helperStarts, 0); QCOMPARE(stat(QStringLiteral("opens")), 0);
        controller_->isoWriter().cancel();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCOMPARE(readFile(targetPath_), expectedTarget_);
    }

    void runtimeValidationControlsAndDdChoice()
    {
        selectIsoFixture(true);
        auto* option = window_->findChild<QCheckBox*>(QStringLiteral("uefiValidation"));
        auto* advanced = window_->findChild<QToolButton*>(QStringLiteral("advancedDriveButton"));
        if (!option->isVisible()) QTest::mouseClick(advanced, Qt::LeftButton);
        QVERIFY(option->isEnabled()); QVERIFY(!option->isChecked());
        QTest::mouseClick(option, Qt::LeftButton); QVERIFY(controller_->runtimeValidationEnabled());
        controller_->setBootType(BT_NON_BOOTABLE);
        QVERIFY(!option->isEnabled()); QVERIFY(!option->isChecked());
        controller_->setBootType(BT_IMAGE);
        QVERIFY(option->isEnabled()); QVERIFY(option->isChecked());
        rufus::SelectionController fresh; QVERIFY(!fresh.runtimeValidationEnabled());
        QSignalSpy dd(&controller_->isoWriter(), &rufus::IsoWriteController::ddRequested);
        chooseImageMode(IMAGE_WRITE_DD, 3); // ISO, ISO-to-ESP and DD remain available.
        QTRY_COMPARE_WITH_TIMEOUT(state(), State::AwaitingConfirmation, writeTimeoutMs);
        QCOMPARE(dd.size(), 1);
        const auto source = qvariant_cast<rufus::PrepareImageRequest>(dd.first().first());
        QVERIFY(!source.runtimeValidation); QCOMPARE(helperStarts, 0);
        writer().cancel();
        QTRY_VERIFY_WITH_TIMEOUT(!writer().isActive(), writeTimeoutMs);
        QCOMPARE(readFile(targetPath_), expectedTarget_);
    }

    void fullFormatChoice_data()
    {
        QTest::addColumn<int>("filesystem");
        for (int filesystem : {FS_FAT16, FS_FAT32, FS_NTFS, FS_EXFAT})
            QTest::newRow(qPrintable(QString::number(filesystem))) << filesystem;
    }

    void fullFormatChoice()
    {
        QFETCH(int, filesystem);
        selectFormat(filesystem);
        auto* advanced = window_->findChild<QToolButton*>(QStringLiteral("advancedFormatButton"));
        if (!window_->findChild<QWidget*>(QStringLiteral("advancedFormatOptions"))->isVisible())
            QTest::mouseClick(advanced, Qt::LeftButton);
        auto* quick = window_->findChild<QCheckBox*>(QStringLiteral("quickFormat"));
        QVERIFY(quick->isEnabled() && quick->isChecked());
        QTest::mouseClick(quick, Qt::LeftButton, Qt::NoModifier, QPoint(8, quick->height() / 2));
        QVERIFY(!quick->isChecked());
        QVERIFY(!controller_->quickFormat());
        QTest::mouseClick(start_, Qt::LeftButton);
        auto& writer = controller_->isoWriter();
        QTRY_COMPARE_WITH_TIMEOUT(writer.state(), rufus::IsoWriteState::AwaitingConfirmation, writeTimeoutMs);
        QVERIFY(!writer.request().quickFormat && !quick->isEnabled());
        controller_->setQuickFormat(true);
        QVERIFY(!writer.request().quickFormat && !controller_->quickFormat());
        QCOMPARE(helperStarts, 0);
        approveIsoPlan();
        QTRY_COMPARE_WITH_TIMEOUT(writer.state(), rufus::IsoWriteState::Complete, writeTimeoutMs);
        QCOMPARE(helperStarts, 1);
        QCOMPARE(progress_->value(), progress_->maximum());
        QVERIFY(!quick->isChecked() && quick->isEnabled());
    }

    void fullFormatEligibility()
    {
        selectFormat(FS_FAT32);
        controller_->setQuickFormat(false);
        selectFixture();
        auto* quick = window_->findChild<QCheckBox*>(QStringLiteral("quickFormat"));
        QVERIFY(!quick->isChecked() && !quick->isEnabled());
        selectFormat(FS_FAT32);
        QVERIFY(!quick->isChecked() && quick->isEnabled());
        selectFormat(FS_FAT32, quint64(64) << 30);
        QVERIFY(quick->isChecked() && !quick->isEnabled());
        controller_->setQuickFormat(false);
        QVERIFY(controller_->quickFormat());
        selectFormat(FS_NTFS, quint64(64) << 30);
        QVERIFY(quick->isChecked() && quick->isEnabled());
    }

    void nonBootablePartitionSelection()
    {
        device_.partitionTableType = QStringLiteral("gpt");
        controller_->setDiscovery({device_}, {});
        controller_->setBootType(BT_NON_BOOTABLE);
        QVERIFY((controller_->selection().partition == rufus::PartitionScheme::Mbr)); // Win32 ignores the existing GPT here.
        selectFormat(FS_NTFS);
        auto* partitions = window_->findChild<QComboBox*>(QStringLiteral("partitionCombo"));
        partitions->setCurrentIndex(partitions->findData(int(rufus::PartitionScheme::Gpt)));
        QVERIFY(QMetaObject::invokeMethod(partitions, "activated", Q_ARG(int, partitions->currentIndex())));
        QVERIFY(!(controller_->selection().partition == rufus::PartitionScheme::Mbr));
        controller_->setFileSystem(FS_EXFAT);
        controller_->setVolumeLabel(QStringLiteral("KEEP LABEL"));
        controller_->setBootType(BT_IMAGE);
        controller_->setBootType(BT_NON_BOOTABLE);
        QCOMPARE(partitions->currentText(), QStringLiteral("GPT"));
        QCOMPARE(controller_->selection().fileSystem, int(FS_EXFAT));
        QCOMPARE(controller_->selection().volumeLabel, QStringLiteral("KEEP LABEL"));
        rufus::Device other = device_;
        other.id = QStringLiteral("other-format-device");
        other.size = 128 * 1024 * 1024;
        controller_->setDiscovery({device_, other}, {});
        controller_->selectDevice(other.id);
        QVERIFY(!(controller_->selection().partition == rufus::PartitionScheme::Mbr));
        controller_->setPartitionScheme(rufus::PartitionScheme::Mbr);
        QVERIFY((controller_->selection().partition == rufus::PartitionScheme::Mbr));
        controller_->selectDevice(device_.id);
        QVERIFY((controller_->selection().partition == rufus::PartitionScheme::Mbr));
        constexpr quint64 mbrLimit = quint64(1) << 41;
        other.size = mbrLimit;
        controller_->setDiscovery({device_, other}, {});
        controller_->selectDevice(other.id);
        QVERIFY((controller_->selection().partition == rufus::PartitionScheme::Mbr));
        QCOMPARE(partitions->count(), 3);
        other.size = mbrLimit + 512;
        controller_->setDiscovery({device_, other}, {});
        QVERIFY(controller_->selection().deviceId.isEmpty());
        controller_->selectDevice(other.id);
        QCOMPARE(partitions->count(), 2);
        QCOMPARE(partitions->currentText(), QStringLiteral("GPT"));
        QVERIFY(!(controller_->selection().partition == rufus::PartitionScheme::Mbr));
        controller_->setPartitionScheme(rufus::PartitionScheme::Mbr);
        QVERIFY(!(controller_->selection().partition == rufus::PartitionScheme::Mbr)); // Native MBR cannot represent this extent.
        controller_->selectDevice(device_.id);
        QVERIFY(!(controller_->selection().partition == rufus::PartitionScheme::Mbr)); // Win32 remembers the forced GPT preference.
        QCOMPARE(controller_->selection().volumeLabel, QStringLiteral("KEEP LABEL"));
        QCOMPARE(helperStarts, 0);
        QCOMPARE(readFile(targetPath_), expectedTarget_);
    }

    void standaloneRetainsImage_data()
    {
        QTest::addColumn<int>("imageKind");
        QTest::newRow("DD") << 0;
        QTest::newRow("Linux-ISO") << 1;
        QTest::newRow("Windows-ISO") << 2;
    }

    void standaloneRetainsImage()
    {
        QFETCH(int, imageKind);
        if (imageKind == 0)
            selectFixture();
        else
            selectIsoFixture(false, imageKind == 2);
        const QString image = controller_->selection().imagePath;
        selectFormat(FS_NTFS, 64 * 1024 * 1024, BT_UEFI_NTFS);
        auto* boot = window_->findChild<QComboBox*>(QStringLiteral("bootCombo"));
        auto* advanced = window_->findChild<QToolButton*>(QStringLiteral("advancedDriveButton"));
        auto* partitions = window_->findChild<QComboBox*>(QStringLiteral("partitionCombo"));
        QCOMPARE(boot->currentText(), QStringLiteral("UEFI:NTFS"));
        QCOMPARE(partitions->currentText(), QStringLiteral("GPT"));
        QCOMPARE(controller_->selection().imagePath, image);
        QCOMPARE(boot->itemData(1).toString(), image);
        QVERIFY(boot->toolTip().isEmpty());
        QVERIFY(!window_->findChild<QLabel*>(QStringLiteral("statusLabel"))->text().contains("Using image:"));
        QVERIFY(!window_->findChild<QWidget*>(QStringLiteral("imageOptions"))->isVisible());
        QVERIFY(!controller_->canSelectSyslinux());
        QCOMPARE(controller_->isoWriteChoices(0, true, false).count, 0);
        QVERIFY(start_->isEnabled() && select_->isEnabled());
        QCOMPARE(progress_->format(), QStringLiteral("READY"));
        QCOMPARE(progress_->value(), 0);
        QTest::mouseClick(advanced, Qt::LeftButton);
        QCOMPARE(controller_->selection().bootType, int(BT_IMAGE));
        QCOMPARE(controller_->selection().imagePath, image);
        QCOMPARE(boot->currentIndex(), 1);
        QVERIFY(start_->isEnabled());
        QCOMPARE(window_->findChild<QWidget*>(QStringLiteral("imageOptions"))->isVisible(), imageKind == 2);
        QTest::mouseClick(advanced, Qt::LeftButton);
        controller_->setBootType(BT_UEFI_NTFS);
        selectFixture(); // SELECT works from the standalone mode, just as on Windows.
        QCOMPARE(controller_->selection().bootType, int(BT_IMAGE));
        QCOMPARE(boot->currentIndex(), 1);
        QVERIFY(controller_->selection().rawImage);
        QCOMPARE(helperStarts, 0);
        QCOMPARE(readFile(targetPath_), expectedTarget_);
    }

    void exfatRetainsImage_data()
    {
        QTest::addColumn<bool>("raw");
        QTest::newRow("ISO") << false;
        QTest::newRow("DD") << true;
    }

    void exfatRetainsImage()
    {
        QFETCH(bool, raw);
        if (raw)
            selectFixture();
        else
            selectIsoFixture();
        const QString image = controller_->selection().imagePath;
        const int filesystem = controller_->selection().fileSystem;
        selectFormat();
        QCOMPARE(controller_->selection().imagePath, image);
        QCOMPARE(window_->findChild<QComboBox*>(QStringLiteral("bootCombo"))->itemData(1).toString(), image);
        QVERIFY(!window_->findChild<QLabel*>(QStringLiteral("statusLabel"))->text().contains("Using image:"));
        controller_->setBootType(BT_IMAGE);
        QCOMPARE(controller_->selection().imagePath, image);
        QCOMPARE(controller_->selection().rawImage, raw);
        QCOMPARE(controller_->selection().fileSystem, filesystem);
        QVERIFY(start_->isEnabled());
        QVERIFY(select_->isEnabled());
        QCOMPARE(window_->findChild<QComboBox*>(QStringLiteral("fileSystemCombo"))->findData(FS_EXFAT), -1);
    }

    void bareBootRetainsSelection_data()
    {
        QTest::addColumn<int>("imageKind");
        QTest::addColumn<int>("bootType");
        for (int type : {BT_FREEDOS, BT_SYSLINUX_V4, BT_SYSLINUX_V6})
            for (int image = 0; image < 3; ++image)
                QTest::newRow(qPrintable(QStringLiteral("%1-%2").arg(type).arg(image))) << image << type;
    }

    void bareBootRetainsSelection()
    {
        QFETCH(int, imageKind);
        QFETCH(int, bootType);
        if (imageKind == 0) selectFixture();
        else selectIsoFixture(false, imageKind == 2);
        const QString image = controller_->selection().imagePath;
        selectFormat(FS_EXFAT);
        controller_->setPartitionScheme(rufus::PartitionScheme::Sfd);
        if (bootType == BT_SYSLINUX_V4 || bootType == BT_SYSLINUX_V6)
            QTest::mouseClick(window_->findChild<QToolButton*>(QStringLiteral("advancedDriveButton")), Qt::LeftButton);
        controller_->setBootType(bootType);
        auto* boot = window_->findChild<QComboBox*>(QStringLiteral("bootCombo"));
        auto* partitions = window_->findChild<QComboBox*>(QStringLiteral("partitionCombo"));
        auto* filesystems = window_->findChild<QComboBox*>(QStringLiteral("fileSystemCombo"));
        QCOMPARE(boot->currentText(), bootType == BT_FREEDOS ? QStringLiteral("FreeDOS")
            : bootType == BT_SYSLINUX_V4 ? QStringLiteral("Syslinux 4.07") : QStringLiteral("Syslinux 6.04"));
        QCOMPARE(partitions->count(), 1);
        QCOMPARE(partitions->currentText(), QStringLiteral("MBR"));
        QCOMPARE(window_->findChild<QComboBox*>(QStringLiteral("targetCombo"))->currentText(), QStringLiteral("BIOS (or UEFI-CSM)"));
        QCOMPARE(filesystems->count(), bootType == BT_SYSLINUX_V6 ? 3 : 2);
        QCOMPARE(filesystems->itemData(0).toInt(), int(FS_FAT16));
        QCOMPARE(filesystems->itemData(1).toInt(), int(FS_FAT32));
        QCOMPARE(controller_->selection().fileSystem, int(FS_FAT16));
        QVERIFY(start_->isEnabled() && select_->isEnabled());
        QVERIFY(!controller_->canSelectSfd() && !controller_->canSelectSyslinux());
        if (bootType != BT_FREEDOS)
        {
            controller_->setClusterSize(4096);
            QCOMPARE(controller_->selection().clusterSize, quint32(4096));
            const QString label = controller_->selection().volumeLabel;
            const int otherVersion = bootType == BT_SYSLINUX_V4 ? BT_SYSLINUX_V6 : BT_SYSLINUX_V4;
            controller_->setBootType(otherVersion);
            QCOMPARE(controller_->selection().fileSystem, int(FS_FAT16));
            // Win32's boot selection calls SetClusterSizes, restoring the default.
            QCOMPARE(controller_->selection().clusterSize, quint32(2048));
            QCOMPARE(controller_->selection().volumeLabel, label);
            controller_->setClusterSize(4096);
            controller_->setBootType(bootType);
            QCOMPARE(controller_->selection().clusterSize, quint32(2048));
        }
        QVERIFY(!window_->findChild<QWidget*>(QStringLiteral("imageOptions"))->isVisible());
        controller_->setPartitionScheme(rufus::PartitionScheme::Gpt);
        controller_->setPartitionScheme(rufus::PartitionScheme::Sfd);
        QCOMPARE(controller_->selection().partition, rufus::PartitionScheme::Mbr);
        controller_->setFileSystem(FS_NTFS);
        QCOMPARE(controller_->selection().fileSystem, int(bootType == BT_SYSLINUX_V6 ? FS_NTFS : FS_FAT16));
        if (bootType == BT_SYSLINUX_V6)
        {
            QCOMPARE(filesystems->itemData(2).toInt(), int(FS_NTFS));
            QCOMPARE(controller_->selection().clusterSize, quint32(4096));
            controller_->setClusterSize(65536);
            QCOMPARE(controller_->selection().clusterSize, quint32(65536));
            controller_->setBootType(BT_SYSLINUX_V4);
            QCOMPARE(filesystems->findData(FS_NTFS), -1);
            QCOMPARE(controller_->selection().fileSystem, int(FS_FAT16));
            QCOMPARE(controller_->selection().clusterSize, quint32(2048));
            controller_->setBootType(BT_SYSLINUX_V6);
            QCOMPARE(controller_->selection().fileSystem, int(FS_NTFS));
            QCOMPARE(controller_->selection().clusterSize, quint32(4096));
        }
        auto* advanced = window_->findChild<QToolButton*>(QStringLiteral("advancedDriveButton"));
        if (bootType == BT_FREEDOS)
            QTest::mouseClick(advanced, Qt::LeftButton);
        QTest::mouseClick(advanced, Qt::LeftButton);
        QCOMPARE(controller_->selection().bootType, bootType == BT_FREEDOS ? BT_FREEDOS : BT_IMAGE);
        QCOMPARE(boot->findText(QStringLiteral("Syslinux 4.07")), -1);
        QCOMPARE(boot->findText(QStringLiteral("Syslinux 6.04")), -1);
        QTest::mouseClick(advanced, Qt::LeftButton);
        QCOMPARE(controller_->selection().bootType, bootType == BT_FREEDOS ? BT_FREEDOS : BT_IMAGE);
        QCOMPARE(controller_->selection().imagePath, image);
        QCOMPARE(boot->itemData(1).toString(), image);
        controller_->setBootType(BT_NON_BOOTABLE);
        QCOMPARE(controller_->selection().partition, rufus::PartitionScheme::Sfd);
        controller_->setBootType(BT_IMAGE);
        QCOMPARE(controller_->selection().imagePath, image);
        QCOMPARE(boot->currentIndex(), 1);
        QVERIFY(start_->isEnabled());
        QCOMPARE(window_->findChild<QWidget*>(QStringLiteral("imageOptions"))->isVisible(), imageKind == 2);
        controller_->setBootType(bootType);
        selectFixture(); // SELECT still switches to a new image from either bare loader.
        QCOMPARE(controller_->selection().bootType, int(BT_IMAGE));
        QCOMPARE(helperStarts, 0);
    }

    void standaloneSyslinuxCreation_data()
    {
        QTest::addColumn<int>("filesystem");
        QTest::addColumn<bool>("large");
        QTest::addColumn<int>("bootType");
        QTest::addColumn<quint32>("cluster");
        QTest::addColumn<QString>("missingTool");
        for (int type : {BT_SYSLINUX_V4, BT_SYSLINUX_V6})
        {
            const QString otherInstaller = type == BT_SYSLINUX_V4 ? QStringLiteral("/rufus-syslinux") : QStringLiteral("/rufus-syslinux4");
            QTest::newRow(qPrintable(QStringLiteral("%1-FAT16").arg(type))) << int(FS_FAT16) << false << type << quint32(0) << otherInstaller;
            QTest::newRow(qPrintable(QStringLiteral("%1-FAT32").arg(type))) << int(FS_FAT32) << false << type << quint32(0) << otherInstaller;
            QTest::newRow(qPrintable(QStringLiteral("%1-large-FAT32").arg(type))) << int(FS_FAT32) << true << type << quint32(0) << otherInstaller;
        }
        const char* unusedTools[] = {"/rufus-syslinux", "/rufus-syslinux4", "/mcopy", "/mattrib", "/mcopy"};
        for (int i = 0; i < 5; ++i)
            QTest::newRow(qPrintable(QStringLiteral("v6-NTFS-%1K").arg(4 << i)))
                << int(FS_NTFS) << false << int(BT_SYSLINUX_V6) << quint32(4096 << i) << QString::fromLatin1(unusedTools[i]);
    }

    void standaloneSyslinuxCreation()
    {
        QFETCH(int, filesystem);
        QFETCH(bool, large);
        QFETCH(int, bootType);
        QFETCH(quint32, cluster);
        QFETCH(QString, missingTool);
        const bool ntfs = filesystem == FS_NTFS;
        const int toolChecks = ntfs ? 1 : 3;
        const QString label = ntfs ? QString::fromUtf8("Rufus ș😀.test") : QStringLiteral("usb.name*:long");
        const QString normalized = ntfs ? QString::fromUtf8("Rufus ș😀_test") : QStringLiteral("USB_NAMELON");
        const QString name = bootType == BT_SYSLINUX_V4 ? QStringLiteral("Syslinux 4.07") : QStringLiteral("Syslinux 6.04");
        selectIsoFixture(false, true); // Retained Windows image must not enter this job.
        const quint64 capacity = large ? 64ULL * 1024 * 1024 * 1024 : 64ULL * 1024 * 1024;
        selectFormat(filesystem, capacity, bootType);
        if (cluster != 0) controller_->setClusterSize(cluster);
        // Only the selected provider should be needed, before and after confirmation.
        missingSyslinuxTool = missingTool;
        missingFatTool = ntfs;
        controller_->toggleEnhancedInstall();
        controller_->setVolumeLabel(label);
        helperMode = QStringLiteral("syslinux-steps");
        QCoreApplication::processEvents(); // Let the hidden Windows options leave the layout before capture.
        QString screenshot = qEnvironmentVariable("RUFUS_TEST_SYSLINUX_SCREENSHOT");
        if (!screenshot.isEmpty()) screenshot += bootType == BT_SYSLINUX_V4 ? QStringLiteral("-v4") : QStringLiteral("-v6");
        if (!screenshot.isEmpty() && ntfs) screenshot += QStringLiteral("-ntfs");
        const bool capture = !screenshot.isEmpty() && !large && (filesystem == FS_FAT32 || (ntfs && cluster == 4096));
        if (capture)
            QVERIFY(window_->grab().save(screenshot + QStringLiteral("-selection.png")));
        QTest::mouseClick(start_, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::AwaitingConfirmation, writeTimeoutMs);
        const auto& request = controller_->isoWriter().request();
        QVERIFY(request.syslinux && request.syslinuxSelection == bootType);
        QVERIFY(!request.mbr && !request.nonBootable && !request.uefiNtfs && !request.sfd && !request.freeDos);
        QVERIFY(request.imagePath.isEmpty() && request.imageSize == 0);
        QVERIFY(!request.enhancedInstall && !request.hasWindowsInstallSettings());
        QCOMPARE(request.windowsFlags, 0);
        QCOMPARE(request.label, normalized);
        QCOMPARE(request.fileSystem, filesystem);
        if (ntfs)
        {
            QCOMPARE(request.clusterSize, cluster);
            QCOMPARE(request.volumeId, quint32(0));
            QVERIFY(!request.usesUefiNtfs());
        }
        QCOMPARE(rufus::FormatMbrBootCode(request), MBR_BOOT_SYSLINUX);
        QCOMPARE(syslinuxToolChecks.load(), toolChecks);
        QVERIFY(!window_->findChild<rufus::FreeDosLocaleDialog*>());
        auto* confirmation = window_->findChild<QMessageBox*>(QStringLiteral("isoWriteConfirmation"));
        const QString formatName = ntfs ? QStringLiteral("NTFS") : QStringLiteral("FAT");
        QVERIFY(confirmation && confirmation->informativeText().contains(name + QStringLiteral(" — MBR, ") + formatName));
        QVERIFY(!confirmation->informativeText().contains(QStringLiteral("Image:")));
        QCOMPARE(confirmation->defaultButton(), confirmation->button(QMessageBox::Cancel));
        QCOMPARE(helperStarts, 0);
        controller_->setBootType(BT_IMAGE);
        controller_->setVolumeLabel(QStringLiteral("CHANGED"));
        QCOMPARE(controller_->selection().bootType, bootType);
        QCOMPARE(request.label, normalized);
        approveIsoPlan();
        // Win32: fourteen discrete slots, three reserved for copy but two events.
        // Large FAT32 replaces five filesystem slots with an analog format range.
        const int positions[] = {46810, 51491};
        const int largePositions[] = {62913, 63568};
        // NTFS reserves twelve filesystem steps: twenty-one total slots.
        const int ntfsPositions[] = {53052, 56172};
        for (int i = 0; i < 2; ++i)
        {
            QTRY_COMPARE_WITH_TIMEOUT(progress_->value(), ntfs ? ntfsPositions[i] : large ? largePositions[i] : positions[i], writeTimeoutMs);
            QCOMPARE(progress_->format(), QStringLiteral("Installing %1...").arg(name));
            QCOMPARE(helperStarts, 1);
            QCOMPARE(syslinuxToolChecks.load(), 2 * toolChecks);
            QCOMPARE(completionBeeps, 0);
            QVERIFY(!select_->isEnabled());
            if (capture && i == 0)
                QVERIFY(window_->grab().save(screenshot + QStringLiteral("-installing.png")));
            helperProcess->write("continue\n");
        }
        QTRY_VERIFY_WITH_TIMEOUT(controller_->isoWriter().outcome().filesystemBytes > 0, writeTimeoutMs);
        QCOMPARE(controller_->isoWriter().outcome().copiedBytes, quint64(0));
        QCOMPARE(progress_->value(), ntfs ? 62414 : large ? 64879 : 60853);
        QCOMPARE(progress_->format(), QStringLiteral("Finalizing, please wait..."));
        QVERIFY(controller_->isoWriter().isActive());
        QCOMPARE(completionAlerts, 0);
        helperProcess->closeWriteChannel();
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::Complete, writeTimeoutMs);
        QCOMPARE(progress_->value(), MAX_PROGRESS);
        QCOMPARE(progress_->format(), QStringLiteral("READY"));
        QCOMPARE(completionBeeps, 1);
        QCOMPARE(completionAlerts, 1);
        QCOMPARE(helperStarts, 1);
        QCOMPARE(controller_->isoWriter().statusText(), name + QStringLiteral(" creation completed."));
        if (capture)
            QVERIFY(window_->grab().save(screenshot + QStringLiteral("-ready.png")));
        controller_->setDiscovery({}, {});
        QCOMPARE(progress_->value(), MAX_PROGRESS);
        QCOMPARE(progress_->format(), QStringLiteral("READY"));
        QCOMPARE(completionBeeps, 1);
        QCOMPARE(readFile(targetPath_), expectedTarget_);
    }

    void standaloneGrub2AdvancedExtChoices()
    {
        selectFormat(FS_FAT32, 320ULL * 1024 * 1024, BT_GRUB2);
        auto* filesystems = window_->findChild<QComboBox*>(QStringLiteral("fileSystemCombo"));
        auto* advanced = window_->findChild<QToolButton*>(QStringLiteral("advancedFormatButton"));
        QCOMPARE(filesystems->findData(FS_EXT2), -1);
        QTest::mouseClick(advanced, Qt::LeftButton);
        QVERIFY(filesystems->findData(FS_EXT2) >= 0);
        QVERIFY(filesystems->findData(FS_EXT3) >= 0);
        QCOMPARE(filesystems->findData(FS_EXT4), -1);
        controller_->setFileSystem(FS_EXT3);
        controller_->setVolumeLabel(QStringLiteral("usb.Name*:.test"));
        QTest::mouseClick(advanced, Qt::LeftButton);
        QCOMPARE(filesystems->findData(FS_EXT3), -1);
        QCOMPARE(controller_->selection().fileSystem, int(FS_FAT16));
        QTest::mouseClick(advanced, Qt::LeftButton);
        QCOMPARE(controller_->selection().fileSystem, int(FS_EXT3));
        QCOMPARE(controller_->selection().volumeLabel, QStringLiteral("usb.Name*:.test"));
        controller_->setBootType(BT_NON_BOOTABLE);
        QVERIFY(filesystems->findData(FS_EXT2) >= 0);
        QVERIFY(filesystems->findData(FS_EXT3) >= 0);
        controller_->setBootType(BT_GRUB2);
        auto small = device_;
        small.size = 256ULL * 1024 * 1024 - 512;
        controller_->setDiscovery({small}, {});
        QCOMPARE(filesystems->findData(FS_EXT2), -1);
    }

    void standaloneGrub2Creation_data()
    {
        QTest::addColumn<int>("filesystem");
        QTest::addColumn<bool>("large");
        QTest::newRow("FAT16") << int(FS_FAT16) << false;
        QTest::newRow("FAT32") << int(FS_FAT32) << false;
        QTest::newRow("NTFS") << int(FS_NTFS) << false;
        QTest::newRow("ext2") << int(FS_EXT2) << false;
        QTest::newRow("ext3") << int(FS_EXT3) << false;
        QTest::newRow("large-FAT32") << int(FS_FAT32) << true;
    }

    void grubDownloadWarning_data()
    {
        QTest::addColumn<QString>("action");
        QTest::addColumn<bool>("grubImage");
        for (const char* action : {"no", "cancel", "enter", "escape", "close", "unplug", "source-change"})
            for (bool image : {false, true})
                QTest::newRow((QByteArray(image ? "grub-" : "syslinux-") + action).constData()) << QString::fromLatin1(action) << image;
    }

    void grubDownloadWarning()
    {
        QFETCH(QString, action);
        QFETCH(bool, grubImage);
        selectIsoFixture(false, false, "win11", grubImage ? nullptr : "6.04 6.04-pre1", true, false, "2.12");
        controller_->setPartitionScheme(rufus::PartitionScheme::Mbr);
        QCOMPARE(controller_->selection().syslinux, !grubImage);
        const QString files = directory_.filePath(QStringLiteral("grub-warning-files"));
        controller_->isoWriter().configure(*connection_, QCoreApplication::applicationDirPath() + QStringLiteral("/iso_helper_fixture"),
            files, QUrl(QStringLiteral("http://127.0.0.1:1/files")));
        QSignalSpy warnings(&controller_->isoWriter(), &rufus::IsoWriteController::grubDownloadRequired);
        QTest::mouseClick(start_, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(warnings.size(), 1, writeTimeoutMs);
        QCOMPARE(controller_->isoWriter().state(), rufus::IsoWriteState::SelectingGrubCore);
        auto* dialog = window_->findChild<QMessageBox*>(QStringLiteral("grubDownloadDialog"));
        QVERIFY(dialog && dialog->isVisible());
        QCOMPARE(dialog->icon(), QMessageBox::Warning);
        QCOMPARE(dialog->windowTitle(), QStringLiteral("Download required"));
        QCOMPARE(dialog->standardButtons(), QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);
        QCOMPARE(dialog->defaultButton(), dialog->button(QMessageBox::Cancel));
        QCOMPARE(dialog->escapeButton(), dialog->button(QMessageBox::Cancel));
        QCOMPARE(dialog->textFormat(), Qt::PlainText);
        QVERIFY(dialog->text().contains(QStringLiteral("This image uses Grub 2.12")));
        QVERIFY(dialog->text().contains(files));
        QVERIFY(!start_->isEnabled() && !select_->isEnabled());
        QCOMPARE(helperStarts, 0);
        QCOMPARE(stat(QStringLiteral("opens")), 0);
        if (action == QStringLiteral("unplug")) controller_->setDiscovery({}, {});
        else if (action == QStringLiteral("enter")) QTest::keyClick(dialog, Qt::Key_Return);
        else if (action == QStringLiteral("escape")) QTest::keyClick(dialog, Qt::Key_Escape);
        else if (action == QStringLiteral("close")) dialog->close();
        else
        {
            if (action == QStringLiteral("source-change")) writeFile(sourcePath_, sourceBytes_);
            QTest::mouseClick(dialog->button(action == QStringLiteral("cancel") ? QMessageBox::Cancel : QMessageBox::No), Qt::LeftButton);
        }
        const bool failed = action == QStringLiteral("unplug") || action == QStringLiteral("source-change");
        const auto expected = action == QStringLiteral("no") ? rufus::IsoWriteState::AwaitingConfirmation
            : failed ? rufus::IsoWriteState::Failed : rufus::IsoWriteState::Cancelled;
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), expected, writeTimeoutMs);
        QCOMPARE(warnings.size(), 1);
        QCOMPARE(helperStarts, 0);
        QCOMPARE(stat(QStringLiteral("opens")), 0);
        if (action == QStringLiteral("no"))
        {
            QCOMPARE(controller_->isoWriter().grubCoreResult().origin, rufus::Grub2CoreOrigin::Embedded);
            QVERIFY(controller_->isoWriter().grubCoreResult().bytes.isEmpty());
            controller_->isoWriter().cancel();
        }
    }

    void grubDownloadFlow_data()
    {
        QTest::addColumn<QString>("mode");
        QTest::addColumn<bool>("grubImage");
        for (const char* mode : {"download", "cache", "unusable-cache", "not-found", "bad-signature", "cancel-payload", "cancel-signature"})
            for (bool image : {false, true})
                QTest::newRow((QByteArray(image ? "grub-" : "syslinux-") + mode).constData()) << QString::fromLatin1(mode) << image;
    }

    void grubDownloadExcluded_data()
    {
        QTest::addColumn<QString>("mode");
        QTest::addColumn<bool>("grubImage");
        for (const char* mode : {"unknown", "embedded", "gpt", "dd"})
            for (bool image : {false, true})
                QTest::newRow((QByteArray(image ? "grub-" : "syslinux-") + mode).constData()) << QString::fromLatin1(mode) << image;
    }

    void grubDownloadExcluded()
    {
        QFETCH(QString, mode);
        QFETCH(bool, grubImage);
        const QByteArray version = mode == QStringLiteral("unknown") ? QByteArray()
            : mode == QStringLiteral("embedded") ? QByteArray(GRUB2_PACKAGE_VERSION) : QByteArray("2.12");
        selectIsoFixture(mode == QStringLiteral("dd"), false, "win11", grubImage ? nullptr : "6.04 6.04-pre1", true, false, version.constData());
        const QString files = directory_.filePath(QStringLiteral("grub-excluded-files"));
        controller_->isoWriter().configure(*connection_, QCoreApplication::applicationDirPath() + QStringLiteral("/iso_helper_fixture"),
            files, QUrl(QStringLiteral("http://127.0.0.1:1/files")));
        if (mode == QStringLiteral("gpt")) controller_->setPartitionScheme(rufus::PartitionScheme::Gpt);
        else controller_->setPartitionScheme(rufus::PartitionScheme::Mbr);
        QSignalSpy warnings(&controller_->isoWriter(), &rufus::IsoWriteController::grubDownloadRequired);
        QTest::mouseClick(start_, Qt::LeftButton);
        if (mode == QStringLiteral("dd"))
        {
            chooseImageMode(IMAGE_WRITE_DD);
            QTRY_COMPARE_WITH_TIMEOUT(state(), State::AwaitingConfirmation, writeTimeoutMs);
            writer().cancel();
        }
        else
        {
            QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::AwaitingConfirmation, writeTimeoutMs);
            controller_->isoWriter().cancel();
        }
        QCOMPARE(QString::fromLatin1(controller_->selection().imageAnalysis.report.grub2_version), QString::fromLatin1(version));
        QCOMPARE(warnings.size(), 0);
        QVERIFY(!window_->findChild<QMessageBox*>(QStringLiteral("grubDownloadDialog")));
        QCOMPARE(helperStarts, 0);
        QCOMPARE(stat(QStringLiteral("opens")), 0);
        QVERIFY(!QFileInfo::exists(files));
    }

    void grubDownloadFlow()
    {
        QFETCH(QString, mode);
        QFETCH(bool, grubImage);
        selectIsoFixture(false, false, "win11", grubImage ? nullptr : "6.04 6.04-pre1", true, false, "2.12");
        controller_->setPartitionScheme(rufus::PartitionScheme::Mbr);
        QCOMPARE(controller_->selection().syslinux, !grubImage);
        const QString files = directory_.filePath(QStringLiteral("grub-flow-files"));
        QDir(files).removeRecursively();
        GrubDownloadServer server;
        server.core = readFile(QStringLiteral(RUFUS_SIGNED_FIXTURE "/core.img"));
        server.signature = readFile(QStringLiteral(RUFUS_SIGNED_FIXTURE "/core.img.sig"));
        QVERIFY(!server.core.isEmpty()); QCOMPARE(server.signature.size(), 256);
        server.mode = mode == QStringLiteral("download") || mode == QStringLiteral("cancel-signature")
            ? QStringLiteral("hold-signature") : mode == QStringLiteral("cancel-payload") ? QStringLiteral("hold-payload") : mode;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        const QString cached = files + QStringLiteral("/grub-2.12/core.img");
        if (mode == QStringLiteral("cache") || mode == QStringLiteral("unusable-cache"))
        {
            QVERIFY(QDir().mkpath(QFileInfo(cached).absolutePath()));
            writeFile(cached, mode == QStringLiteral("cache") ? server.core : QByteArray("short"));
        }
        controller_->isoWriter().configure(*connection_, QCoreApplication::applicationDirPath() + QStringLiteral("/iso_helper_fixture"), files, server.root());
        QSignalSpy warnings(&controller_->isoWriter(), &rufus::IsoWriteController::grubDownloadRequired);
        QTest::mouseClick(start_, Qt::LeftButton);
        const bool cachedInput = mode == QStringLiteral("cache") || mode == QStringLiteral("unusable-cache");
        if (!cachedInput)
        {
            QTRY_COMPARE_WITH_TIMEOUT(warnings.size(), 1, writeTimeoutMs);
            auto* dialog = window_->findChild<QMessageBox*>(QStringLiteral("grubDownloadDialog"));
            QVERIFY(dialog && dialog->isVisible());
            QTest::mouseClick(dialog->button(QMessageBox::Yes), Qt::LeftButton);
        }
        const bool held = server.mode == QStringLiteral("hold-signature") || server.mode == QStringLiteral("hold-payload");
        if (held)
        {
            QTRY_VERIFY_WITH_TIMEOUT(server.held, writeTimeoutMs);
            const int position = server.mode == QStringLiteral("hold-payload") ? MAX_PROGRESS / 2 : MAX_PROGRESS;
            QTRY_COMPARE_WITH_TIMEOUT(progress_->value(), position, writeTimeoutMs);
            QCOMPARE(progress_->format(), position == MAX_PROGRESS ? QStringLiteral("Downloading: 100.0%") : QStringLiteral("Downloading: 50.0%"));
            QCOMPARE(controller_->isoWriter().state(), rufus::IsoWriteState::SelectingGrubCore);
            QCOMPARE(helperStarts, 0);
            QCOMPARE(stat(QStringLiteral("opens")), 0);
            if (mode.startsWith(QStringLiteral("cancel-"))) controller_->isoWriter().cancel();
            else server.releaseSignature();
        }
        const auto expected = mode.startsWith(QStringLiteral("cancel-")) ? rufus::IsoWriteState::Cancelled
            : mode == QStringLiteral("bad-signature") ? rufus::IsoWriteState::Failed : rufus::IsoWriteState::AwaitingConfirmation;
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), expected, writeTimeoutMs);
        QCOMPARE(warnings.size(), cachedInput ? 0 : 1);
        QCOMPARE(helperStarts, 0);
        QCOMPARE(stat(QStringLiteral("opens")), 0);
        const auto& result = controller_->isoWriter().grubCoreResult();
        const bool selected = mode == QStringLiteral("download") || mode == QStringLiteral("cache");
        QCOMPARE(result.bytes, selected ? server.core : QByteArray());
        if (selected)
        {
            QCOMPARE(result.origin, mode == QStringLiteral("cache") ? rufus::Grub2CoreOrigin::Cache : rufus::Grub2CoreOrigin::Download);
            writeFile(cached, QByteArray(server.core.size(), 'x'));
            QCOMPARE(result.bytes, server.core); // Later cache changes cannot replace the snapshot.
        }
        if (expected == rufus::IsoWriteState::AwaitingConfirmation)
        {
            QCOMPARE(controller_->isoWriter().request().syslinux, !grubImage);
            QCOMPARE(controller_->isoWriter().request().grub2, grubImage);
            QCOMPARE(GetSecondaryBootType(BT_IMAGE, &controller_->isoWriter().check().analysis.report, false), grubImage ? int(BT_GRUB2) : int(BT_IMAGE));
            QVERIFY(!controller_->isoWriter().request().hasGrub2Core());
            if (selected)
            {
                if (grubImage) expectedGrubCore = result.sha256;
                approveIsoPlan();
                QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::Complete, writeTimeoutMs);
                QCOMPARE(helperStarts, 1);
                QCOMPARE(progress_->format(), QStringLiteral("READY"));
                QCOMPARE(warnings.size(), cachedInput ? 0 : 1);
            }
            else controller_->isoWriter().cancel();
        }
        if (cachedInput) QVERIFY(server.requests.isEmpty());
    }

    void grub4dosIsoSelection_data()
    {
        QTest::addColumn<QString>("mode");
        for (const char* mode : {"plain", "grub2-first", "syslinux-first", "efi", "efi-ntfs-required", "small", "hybrid-small", "fat16-small"})
            QTest::newRow(mode) << QString::fromLatin1(mode);
    }

    void grub4dosIsoSelection()
    {
        QFETCH(QString, mode);
        const bool hybrid = mode == QStringLiteral("hybrid-small");
        const bool ntfsRequired = mode == QStringLiteral("efi-ntfs-required");
        const bool efi = mode == QStringLiteral("efi") || ntfsRequired;
        const bool grub2 = mode == QStringLiteral("grub2-first");
        const bool syslinux = mode == QStringLiteral("syslinux-first");
        if (ntfsRequired) faultKind = LargeLinuxFile;
        selectIsoFixture(hybrid, false, "win11", syslinux ? "6.04 6.04-pre1" : nullptr,
            efi, false, grub2 ? GRUB2_PACKAGE_VERSION : nullptr, false, false, true);
        QVERIFY(controller_->selection().imageAnalysis.report.has_grub4dos);
        auto* partitions = window_->findChild<QComboBox*>(QStringLiteral("partitionCombo"));
        auto* formats = window_->findChild<QComboBox*>(QStringLiteral("fileSystemCombo"));
        const bool fat16Only = mode == QStringLiteral("fat16-small");
        if (mode == QStringLiteral("small") || hybrid || fat16Only)
        {
            const quint64 smallDiskBytes = (fat16Only ? 8 : 2) * 1024 * 1024;
            nativeCapacity = smallDiskBytes;
            device_.size = smallDiskBytes;
            objects_[disk][blockInterface][QStringLiteral("Size")] = QVariant::fromValue(smallDiskBytes);
            setObjects();
            controller_->setDiscovery({device_}, {});
            controller_->selectDevice(device_.id); // Changed capacity correctly invalidates the old selection.
            const IMAGE_WRITE_CHOICES choices = controller_->isoWriteChoices(0, true, true);
            if (fat16Only)
            {
                QVERIFY(controller_->canSelectGrub());
                QCOMPARE(choices.count, 1);
                QCOMPARE(choices.mode[0], IMAGE_WRITE_ISO);
                QCOMPARE(formats->count(), 1);
                QCOMPARE(formats->currentText(), QStringLiteral("FAT16 (Default)"));
                QCOMPARE(controller_->selection().fileSystem, FS_FAT16);
                QCOMPARE(controller_->selection().clusterSize, quint32(512));
                QVERIFY(start_->isEnabled());
            }
            else
            {
                QVERIFY(!controller_->canSelectGrub());
                QCOMPARE(choices.count, hybrid ? 1 : 0);
                if (hybrid) QCOMPARE(choices.mode[0], IMAGE_WRITE_DD);
                QCOMPARE(start_->isEnabled(), hybrid);
            }
        }
        else if (ntfsRequired)
        {
            QVERIFY(controller_->canSelectGrub());
            QCOMPARE(partitions->count(), 2);
            QCOMPARE(partitions->currentText(), QStringLiteral("GPT"));
            controller_->setPartitionScheme(rufus::PartitionScheme::Mbr);
            QCOMPARE(controller_->selection().fileSystems, FS_MASK(FS_NTFS));
            QCOMPARE(formats->count(), 1);
            QCOMPARE(controller_->selection().fileSystem, FS_NTFS);
            QCOMPARE(controller_->selection().clusterSize, quint32(4096));
            QCOMPARE(window_->findChild<QComboBox*>(QStringLiteral("targetCombo"))->currentText(), QStringLiteral("BIOS or UEFI"));
            const IMAGE_WRITE_CHOICES choices = controller_->isoWriteChoices(0, false, false);
            QCOMPARE(choices.count, 1);
            QCOMPARE(choices.mode[0], IMAGE_WRITE_ISO);
            QVERIFY(start_->isEnabled());
        }
        else if (efi)
        {
            QVERIFY(controller_->canSelectGrub());
            QCOMPARE(partitions->count(), 2);
            QCOMPARE(partitions->currentText(), QStringLiteral("GPT"));
            QCOMPARE(window_->findChild<QComboBox*>(QStringLiteral("targetCombo"))->currentText(), QStringLiteral("UEFI (non CSM)"));
            controller_->setPartitionScheme(rufus::PartitionScheme::Mbr);
            QCOMPARE(formats->count(), 3);
            QCOMPARE(formats->currentText(), QStringLiteral("FAT16 (Default)"));
            QCOMPARE(controller_->selection().fileSystem, FS_FAT16);
            QCOMPARE(controller_->selection().defaultFileSystem, FS_FAT16);
            QCOMPARE(controller_->selection().clusterSize, quint32(2048));
            QCOMPARE(window_->findChild<QComboBox*>(QStringLiteral("targetCombo"))->currentText(), QStringLiteral("BIOS or UEFI"));
            QCOMPARE(formats->itemText(formats->findData(FS_FAT16)), QStringLiteral("FAT16 (Default)"));
            controller_->setFileSystem(FS_FAT16);
            controller_->selectImage(sourcePath_);
            QTRY_VERIFY_WITH_TIMEOUT(!controller_->isAnalyzing(), writeTimeoutMs);
            QCOMPARE(controller_->selection().fileSystem, FS_FAT16);
            QCOMPARE(controller_->selection().clusterSize, quint32(2048));
            controller_->setFileSystem(FS_NTFS);
            QCOMPARE(controller_->selection().fileSystem, FS_NTFS);
            QCOMPARE(controller_->selection().clusterSize, quint32(4096));
            controller_->selectImage(sourcePath_);
            QTRY_VERIFY_WITH_TIMEOUT(!controller_->isAnalyzing(), writeTimeoutMs);
            QCOMPARE(controller_->selection().fileSystem, FS_NTFS);
            controller_->setFileSystem(FS_FAT32);
            controller_->selectImage(sourcePath_);
            QTRY_VERIFY_WITH_TIMEOUT(!controller_->isAnalyzing(), writeTimeoutMs);
            QCOMPARE(controller_->selection().fileSystem, FS_FAT32);
            controller_->setPartitionScheme(rufus::PartitionScheme::Gpt);
            QVERIFY(formats->findData(FS_NTFS) >= 0);
            controller_->setFileSystem(FS_NTFS);
            QCOMPARE(controller_->selection().fileSystem, FS_NTFS);
            controller_->setPartitionScheme(rufus::PartitionScheme::Mbr);
            controller_->selectImage(sourcePath_);
            QTRY_VERIFY_WITH_TIMEOUT(!controller_->isAnalyzing(), writeTimeoutMs);
            QCOMPARE(controller_->selection().partition, rufus::PartitionScheme::Mbr);
            QCOMPARE(controller_->selection().fileSystem, FS_NTFS);
        }
        else
        {
            QCOMPARE(GetSecondaryBootType(BT_IMAGE, &controller_->selection().imageAnalysis.report, false),
                syslinux ? int(BT_IMAGE) : grub2 ? int(BT_GRUB2) : int(BT_GRUB4DOS));
            QCOMPARE(controller_->canSelectGrub(), !syslinux);
            QCOMPARE(controller_->canSelectSyslinux(), syslinux);
            QCOMPARE(partitions->count(), 1);
            QCOMPARE(partitions->currentText(), QStringLiteral("MBR"));
            if (!grub2 && !syslinux)
            {
                QCOMPARE(formats->count(), 3);
                QCOMPARE(formats->currentText(), QStringLiteral("FAT16 (Default)"));
                QCOMPARE(controller_->selection().defaultFileSystem, FS_FAT16);
                QCOMPARE(controller_->selection().fileSystem, FS_FAT16);
                QCOMPARE(controller_->selection().clusterSize, quint32(2048));
                controller_->setFileSystem(FS_NTFS);
                QCOMPARE(controller_->selection().fileSystem, FS_NTFS);
                QCOMPARE(controller_->selection().clusterSize, quint32(4096));
                controller_->selectImage(sourcePath_);
                QTRY_VERIFY_WITH_TIMEOUT(!controller_->isAnalyzing(), writeTimeoutMs);
                QCOMPARE(controller_->selection().fileSystem, FS_NTFS);
                controller_->setFileSystem(FS_FAT32);
                controller_->selectImage(sourcePath_);
                QTRY_VERIFY_WITH_TIMEOUT(!controller_->isAnalyzing(), writeTimeoutMs);
                QCOMPARE(controller_->selection().fileSystem, FS_FAT32);
            }
        }
        QCOMPARE(helperStarts, 0);
        QCOMPARE(progress_->format(), QStringLiteral("READY"));
        QCOMPARE(progress_->value(), 0);
    }

    void grub4dosHybridMode_data()
    {
        QTest::addColumn<int>("mode");
        QTest::addColumn<bool>("efi");
        for (bool efi : {false, true})
        {
            const QByteArray prefix = efi ? "EFI-" : "";
            QTest::newRow((prefix + "ISO").constData()) << int(IMAGE_WRITE_ISO) << efi;
            QTest::newRow((prefix + "DD").constData()) << int(IMAGE_WRITE_DD) << efi;
            QTest::newRow((prefix + "cancel").constData()) << int(IMAGE_WRITE_NONE) << efi;
        }
    }

    void grub4dosHybridMode()
    {
        QFETCH(int, mode);
        QFETCH(bool, efi);
        selectIsoFixture(true, false, "win11", nullptr, efi, false, nullptr, false, false, true);
        QTest::mouseClick(start_, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::ChoosingMode, writeTimeoutMs);
        auto* dialog = window_->findChild<rufus::ImageModeDialog*>(QStringLiteral("imageModeDialog"));
        QVERIFY(dialog && dialog->isVisible());
        auto* iso = dialog->findChild<QRadioButton*>(QStringLiteral("imageModeChoice1"));
        auto* dd = dialog->findChild<QRadioButton*>(QStringLiteral("imageModeChoice2"));
        QVERIFY(iso && dd && iso->isChecked() && dd->isEnabled());
        if (mode == IMAGE_WRITE_DD)
        {
            QTest::mouseClick(dd, Qt::LeftButton, Qt::NoModifier, QPoint(6, dd->height() / 2));
            QVERIFY(dd->isChecked());
        }
        auto* buttons = dialog->findChild<QDialogButtonBox*>();
        QTest::mouseClick(buttons->button(mode == IMAGE_WRITE_NONE ? QDialogButtonBox::Cancel : QDialogButtonBox::Ok), Qt::LeftButton);
        if (mode == IMAGE_WRITE_ISO)
        {
            QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::AwaitingConfirmation, writeTimeoutMs);
            QVERIFY(controller_->isoWriter().request().grub4dos && !controller_->isoWriter().request().hasGrub2Core());
        }
        else if (mode == IMAGE_WRITE_DD)
            QTRY_COMPARE_WITH_TIMEOUT(controller_->writer().status().state, rufus::WriteState::AwaitingConfirmation, writeTimeoutMs);
        else
            QCOMPARE(controller_->isoWriter().state(), rufus::IsoWriteState::Cancelled);
        QCOMPARE(helperStarts, 0);
        QCOMPARE(readFile(targetPath_), expectedTarget_);
    }

    void grub4dosIsoBootStatus()
    {
        selectIsoFixture(false, false, "win11", nullptr, false, false, nullptr, false, false, true);
        helperMode = QStringLiteral("hold-bootloader");
        QTest::mouseClick(start_, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::AwaitingConfirmation, writeTimeoutMs);
        approveIsoPlan();
        QTRY_COMPARE_WITH_TIMEOUT(progress_->format(), QStringLiteral("Writing master boot record..."), writeTimeoutMs);
        QVERIFY(controller_->isoWriter().isActive());
        QVERIFY(!controller_->isoWriter().request().hasGrub2Core());
        QCOMPARE(helperStarts, 1);
        QCOMPARE(completionBeeps, 0);
        controller_->isoWriter().cancel();
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::Cancelled, cancelTimeoutMs);
        QVERIFY(progress_->format() != QStringLiteral("READY"));
        QVERIFY(progress_->value() < MAX_PROGRESS);
        QCOMPARE(completionBeeps, 0);
        QCOMPARE(completionAlerts, 0);
        QCOMPARE(readFile(targetPath_), expectedTarget_);
    }

    void grubIsoCreation_data()
    {
        QTest::addColumn<int>("filesystem");
        QTest::addColumn<quint32>("cluster");
        QTest::addColumn<QString>("formatName");
        QTest::addColumn<bool>("efi");
        QTest::addColumn<bool>("grub4dos");
        QTest::newRow("FAT16") << int(FS_FAT16) << quint32(2048) << QStringLiteral("FAT16") << false << false;
        QTest::newRow("FAT32") << int(FS_FAT32) << quint32(512) << QStringLiteral("FAT32") << false << false;
        QTest::newRow("NTFS") << int(FS_NTFS) << quint32(4096) << QStringLiteral("NTFS") << false << false;
        QTest::newRow("NTFS-8KiB-clusters") << int(FS_NTFS) << quint32(8192) << QStringLiteral("NTFS") << false << false;
        QTest::newRow("EFI-NTFS") << int(FS_NTFS) << quint32(4096) << QStringLiteral("NTFS") << true << false;
        QTest::newRow("EFI-NTFS-8KiB-clusters") << int(FS_NTFS) << quint32(8192) << QStringLiteral("NTFS") << true << false;
        QTest::newRow("EFI-FAT32") << int(FS_FAT32) << quint32(512) << QStringLiteral("FAT32") << true << false;
        QTest::newRow("GRUB4DOS-FAT16") << int(FS_FAT16) << quint32(2048) << QStringLiteral("FAT16") << false << true;
        QTest::newRow("GRUB4DOS-FAT32") << int(FS_FAT32) << quint32(512) << QStringLiteral("FAT32") << false << true;
        QTest::newRow("GRUB4DOS-NTFS") << int(FS_NTFS) << quint32(4096) << QStringLiteral("NTFS") << false << true;
        QTest::newRow("GRUB4DOS-NTFS-8KiB-clusters") << int(FS_NTFS) << quint32(8192) << QStringLiteral("NTFS") << false << true;
        QTest::newRow("GRUB4DOS-EFI-FAT32") << int(FS_FAT32) << quint32(512) << QStringLiteral("FAT32") << true << true;
        QTest::newRow("GRUB4DOS-EFI-FAT16") << int(FS_FAT16) << quint32(2048) << QStringLiteral("FAT16") << true << true;
        QTest::newRow("GRUB4DOS-EFI-NTFS") << int(FS_NTFS) << quint32(4096) << QStringLiteral("NTFS") << true << true;
        QTest::newRow("GRUB4DOS-EFI-NTFS-8KiB-clusters") << int(FS_NTFS) << quint32(8192) << QStringLiteral("NTFS") << true << true;
    }

    void grubIsoCreation()
    {
        QFETCH(int, filesystem);
        QFETCH(quint32, cluster);
        QFETCH(QString, formatName);
        QFETCH(bool, efi);
        QFETCH(bool, grub4dos);
        selectIsoFixture(false, false, "win11", nullptr, efi, false, grub4dos ? nullptr : "2.12", false, false, grub4dos);
        controller_->setPartitionScheme(rufus::PartitionScheme::Mbr);
        QVERIFY(controller_->canSelectGrub());
        QVERIFY(!controller_->selection().syslinux);
        QCOMPARE(controller_->selection().partition, rufus::PartitionScheme::Mbr);
        QCOMPARE(window_->findChild<QComboBox*>(QStringLiteral("partitionCombo"))->currentText(), QStringLiteral("MBR"));
        QCOMPARE(window_->findChild<QComboBox*>(QStringLiteral("targetCombo"))->currentText(), efi ? QStringLiteral("BIOS or UEFI") : QStringLiteral("BIOS (or UEFI-CSM)"));
        QCOMPARE(controller_->selection().fileSystem, FS_FAT16);
        auto* formats = window_->findChild<QComboBox*>(QStringLiteral("fileSystemCombo"));
        const int selected = formats->findData(filesystem);
        QVERIFY(selected >= 0);
        formats->setCurrentIndex(selected);
        QVERIFY(QMetaObject::invokeMethod(formats, "activated", Q_ARG(int, selected)));
        QCOMPARE(controller_->selection().fileSystem, filesystem);
        controller_->toggleEnhancedInstall(); // Win32 Alt+E refreshes the cluster choices.
        auto* clusters = window_->findChild<QComboBox*>(QStringLiteral("clusterCombo"));
        const int selectedCluster = clusters->findData(cluster);
        QVERIFY(selectedCluster >= 0);
        clusters->setCurrentIndex(selectedCluster);
        QVERIFY(QMetaObject::invokeMethod(clusters, "activated", Q_ARG(int, selectedCluster)));
        const QString files = directory_.filePath(grub4dos ? QStringLiteral("grub4dos-iso-files") : QStringLiteral("grub-iso-files"));
        const QString cached = files + QStringLiteral("/grub-2.12/core.img");
        const QByteArray core = grub4dos ? QByteArray() : readFile(QStringLiteral(RUFUS_SIGNED_FIXTURE "/core.img"));
        if (!grub4dos)
        {
            QVERIFY(QDir().mkpath(QFileInfo(cached).absolutePath()));
            writeFile(cached, core);
            expectedGrubCore = QCryptographicHash::hash(core, QCryptographicHash::Sha256);
        }
        controller_->isoWriter().configure(*connection_, QCoreApplication::applicationDirPath() + QStringLiteral("/iso_helper_fixture"), files);
        QTest::mouseClick(start_, Qt::LeftButton);
        auto& writer = controller_->isoWriter();
        QTRY_COMPARE_WITH_TIMEOUT(writer.state(), rufus::IsoWriteState::AwaitingConfirmation, writeTimeoutMs);
        QCOMPARE(writer.request().grub4dos, grub4dos);
        QCOMPARE(writer.request().grub2, !grub4dos);
        QVERIFY(writer.request().mbr);
        QVERIFY(!writer.request().syslinux && !writer.request().enhancedInstall);
        QCOMPARE(writer.request().fileSystem, filesystem);
        QCOMPARE(writer.request().clusterSize, cluster);
        const bool uefiNtfs = efi && filesystem == FS_NTFS;
        QCOMPARE(writer.request().usesUefiNtfs(), uefiNtfs);
        QVERIFY(!writer.request().isSourceFree());
        QCOMPARE(writer.check().extraPartitions, quint8(uefiNtfs ? XP_UEFI_NTFS : 0));
        QCOMPARE(writer.check().partition.uefi_ntfs_size > 0, uefiNtfs);
        if (filesystem == FS_NTFS) QCOMPARE(writer.request().volumeId, quint32(0));
        QCOMPARE(writer.request().imagePath, sourcePath_);
        QCOMPARE(writer.grubCoreResult().bytes, core);
        if (grub4dos)
        {
            QVERIFY(!writer.request().hasGrub2Core());
            QCOMPARE(writer.check().analysis.report.grub2_version[0], '\0');
            QVERIFY(!QFileInfo::exists(files));
        }
        auto* dialog = window_->findChild<QMessageBox*>(QStringLiteral("isoWriteConfirmation"));
        const QString system = efi ? QStringLiteral("BIOS or UEFI") : QStringLiteral("BIOS");
        QVERIFY(dialog && dialog->informativeText().contains(QStringLiteral("ISO Image — MBR, %1, %2").arg(formatName, system)));
        QCOMPARE(helperStarts, 0);
        if (!grub4dos) writeFile(cached, QByteArray(core.size(), 'x')); // Handoff uses retained bytes.
        helperMode = QStringLiteral("iso-steps");
        approveIsoPlan();
        QTRY_COMPARE_WITH_TIMEOUT(progress_->format(), QStringLiteral("Copying ISO files: 50.0%"), writeTimeoutMs);
        QCOMPARE(progress_->value(), filesystem == FS_NTFS ? 37027 : 35061); // Win32 NTFS: 16% + half of 81%; FAT: 9% + half of 89%.
        QCOMPARE(helperStarts, 1);
        QCOMPARE(completionBeeps, 0);
        QVERIFY(!select_->isEnabled() && !start_->isEnabled());
        helperProcess->write("continue\n");
        QTRY_COMPARE_WITH_TIMEOUT(progress_->format(), QStringLiteral("Finalizing, please wait..."), writeTimeoutMs);
        QCOMPARE(progress_->value(), filesystem == FS_NTFS ? 64224 : 64879); // Original ISO finalization, not standalone GRUB slots.
        QVERIFY(writer.outcome().copiedBytes == 0);
        QCOMPARE(completionBeeps, 0);
        helperProcess->write("continue\n");
        QTRY_COMPARE_WITH_TIMEOUT(writer.outcome().copiedBytes, writer.check().tree.bytes_written, writeTimeoutMs);
        QVERIFY(writer.isActive());
        if (filesystem == FS_NTFS) QCOMPARE(writer.outcome().filesystemBytes, writer.outcome().size);
        QCOMPARE(progress_->format(), QStringLiteral("Finalizing, please wait..."));
        QCOMPARE(completionBeeps, 0);
        helperProcess->closeWriteChannel();
        QTRY_COMPARE_WITH_TIMEOUT(writer.state(), rufus::IsoWriteState::Complete, writeTimeoutMs);
        QCOMPARE(writer.statusText(), QStringLiteral("ISO image creation completed."));
        QCOMPARE(progress_->format(), QStringLiteral("READY"));
        QCOMPARE(progress_->value(), MAX_PROGRESS);
        QCOMPARE(completionBeeps, 1);
        QCOMPARE(completionAlerts, 1);
        controller_->setDiscovery({}, {});
        QCOMPARE(writer.state(), rufus::IsoWriteState::Complete);
        QCOMPARE(completionBeeps, 1);
        QCOMPARE(readFile(targetPath_), expectedTarget_); // Protocol fixture never opens media.
    }

    void grubIsoFailure_data()
    {
        QTest::addColumn<QString>("mode");
        QTest::addColumn<int>("filesystem");
        QTest::addColumn<bool>("efi");
        QTest::addColumn<bool>("grub4dos");
        for (int filesystem : {FS_FAT16, FS_FAT32, FS_NTFS})
        for (bool efi : {false, true})
        {
            if (efi && filesystem != FS_NTFS) continue;
            for (const char* mode : {"deny", "sync-error", "cleanup-error", "short-copy", "wrong-layout", "skip-finalizing", "extract-error", "hold", "source-change", "unplug"})
                QTest::newRow((QByteArray(filesystem == FS_FAT16 ? "FAT16-" : filesystem == FS_FAT32 ? "FAT32-" : "NTFS-") + (efi ? "uefi-" : "") + mode).constData())
                    << QString::fromLatin1(mode) << filesystem << efi << false;
        }
        QTest::newRow("NTFS-short-format") << QStringLiteral("short-format") << int(FS_NTFS) << false << false;
        QTest::newRow("EFI-NTFS-short-format") << QStringLiteral("short-format") << int(FS_NTFS) << true << false;
        QTest::newRow("pending-ext2") << QStringLiteral("ext2") << int(FS_EXT2) << false << false;
        for (int filesystem : {FS_FAT16, FS_FAT32, FS_NTFS})
        for (const char* mode : {"deny", "sync-error", "cleanup-error", "short-copy", "wrong-layout", "skip-finalizing", "extract-error", "hold", "source-change", "unplug"})
            QTest::newRow((QByteArray(filesystem == FS_FAT16 ? "GRUB4DOS-FAT16-" : filesystem == FS_FAT32 ? "GRUB4DOS-FAT32-" : "GRUB4DOS-NTFS-") + mode).constData())
                << QString::fromLatin1(mode) << filesystem << false << true;
        QTest::newRow("GRUB4DOS-NTFS-short-format") << QStringLiteral("short-format") << int(FS_NTFS) << false << true;
        for (int filesystem : {FS_FAT16, FS_FAT32, FS_NTFS})
        for (const char* mode : {"deny", "sync-error", "cleanup-error", "short-copy", "wrong-layout", "skip-finalizing", "extract-error", "hold", "source-change", "unplug"})
            QTest::newRow((QByteArray(filesystem == FS_FAT16 ? "GRUB4DOS-EFI-FAT16-" : filesystem == FS_FAT32 ? "GRUB4DOS-EFI-FAT32-" : "GRUB4DOS-EFI-NTFS-") + mode).constData())
                << QString::fromLatin1(mode) << filesystem << true << true;
        QTest::newRow("GRUB4DOS-EFI-NTFS-short-format") << QStringLiteral("short-format") << int(FS_NTFS) << true << true;
    }

    void grubIsoFailure()
    {
        QFETCH(QString, mode);
        QFETCH(int, filesystem);
        QFETCH(bool, efi);
        QFETCH(bool, grub4dos);
        selectIsoFixture(false, false, "win11", nullptr, efi, false, grub4dos ? nullptr : GRUB2_PACKAGE_VERSION, false, false, grub4dos);
        controller_->setPartitionScheme(rufus::PartitionScheme::Mbr);
        const quint32 formats = FS_MASK(FS_FAT16) | FS_MASK(FS_FAT32) | FS_MASK(FS_NTFS);
        QCOMPARE(controller_->selection().fileSystems, formats);
        QCOMPARE(controller_->selection().fileSystem, FS_FAT16);
        helperMode = mode;
        auto& writer = controller_->isoWriter();
        const bool pending = filesystem == FS_EXT2;
        if (pending)
        {
            // Direct callers are gated too, even though ext is absent from the image format combo.
            auto source = controller_->selection().imageSource;
            source.grub2 = !grub4dos;
            source.grub4dos = grub4dos;
            source.mbr = true;
            IMAGE_WRITE_CHOICES choices = {};
            choices.count = 1;
            choices.mode[0] = IMAGE_WRITE_ISO;
            writer.prepare(source, device_, QStringLiteral("NEW LABEL"), 4096, choices,
                filesystem);
        }
        else
        {
            controller_->setFileSystem(filesystem);
            QTest::mouseClick(start_, Qt::LeftButton);
        }
        if (!pending)
        {
            QTRY_COMPARE_WITH_TIMEOUT(writer.state(), rufus::IsoWriteState::AwaitingConfirmation, writeTimeoutMs);
            if (mode == QStringLiteral("source-change")) writeFile(sourcePath_, sourceBytes_);
            if (mode == QStringLiteral("unplug")) controller_->setDiscovery({}, {});
            else approveIsoPlan();
        }
        const bool cancel = mode == QStringLiteral("hold");
        if (cancel)
        {
            QTRY_COMPARE_WITH_TIMEOUT(progress_->format(), QStringLiteral("Copying ISO files: 50.0%"), writeTimeoutMs);
            writer.cancel();
        }
        QTRY_COMPARE_WITH_TIMEOUT(writer.state(), cancel ? rufus::IsoWriteState::Cancelled : rufus::IsoWriteState::Failed, writeTimeoutMs);
        if (mode == QStringLiteral("short-format"))
            QVERIFY(writer.statusText().startsWith(QStringLiteral("The helper did not report formatting the complete partition.")));
        const bool beforeAuthorization = pending || mode == QStringLiteral("source-change") || mode == QStringLiteral("unplug");
        QCOMPARE(helperStarts, beforeAuthorization ? 0 : 1);
        QCOMPARE(completionBeeps, 0);
        QCOMPARE(completionAlerts, 0);
        QVERIFY(progress_->value() < MAX_PROGRESS);
        QCOMPARE(readFile(targetPath_), expectedTarget_);
    }

    void grubIsoSelection_data()
    {
        QTest::addColumn<quint64>("capacity");
        QTest::addColumn<quint32>("formats");
        QTest::addColumn<int>("filesystem");
        QTest::addColumn<quint32>("cluster");
        QTest::addColumn<bool>("efi");
        constexpr quint64 MiB = 1024 * 1024;
        const quint32 formats = FS_MASK(FS_FAT16) | FS_MASK(FS_FAT32) | FS_MASK(FS_NTFS);
        QTest::newRow("FAT16-only-small-target") << quint64(8 * MiB) << quint32(FS_MASK(FS_FAT16)) << int(FS_FAT16) << quint32(512) << false;
        QTest::newRow("small-BIOS-default") << quint64(64 * MiB) << formats << int(FS_FAT16) << quint32(2048) << false;
        QTest::newRow("512-MiB-default") << quint64(512 * MiB) << formats << int(FS_FAT16) << quint32(16384) << false;
        QTest::newRow("FAT16-upper-bound") << quint64(4096 * MiB) << quint32(FS_MASK(FS_FAT32) | FS_MASK(FS_NTFS)) << int(FS_FAT32) << quint32(4096) << false;
        QTest::newRow("small-EFI-MBR") << quint64(64 * MiB) << formats << int(FS_FAT16) << quint32(2048) << true;
        QTest::newRow("EFI-FAT16-upper-bound") << quint64(4096 * MiB) << quint32(FS_MASK(FS_FAT32) | FS_MASK(FS_NTFS)) << int(FS_FAT32) << quint32(4096) << true;
    }

    void grubIsoSelection()
    {
        QFETCH(quint64, capacity);
        QFETCH(quint32, formats);
        QFETCH(int, filesystem);
        QFETCH(quint32, cluster);
        QFETCH(bool, efi);
        selectIsoFixture(false, false, "win11", nullptr, efi, false, GRUB2_PACKAGE_VERSION);
        nativeCapacity = capacity;
        objects_[disk][blockInterface][QStringLiteral("Size")] = QVariant::fromValue(capacity);
        setObjects();
        const QString owner = device_.serviceOwner;
        QVERIFY(rufus::DescribeUDisksDevice(objects_, disk.path(), device_));
        device_.id = QStringLiteral("grub-selection-device");
        device_.serviceOwner = owner;
        controller_->setDiscovery({device_}, {});
        controller_->selectDevice(device_.id);
        if (efi) QCOMPARE(controller_->selection().partition, rufus::PartitionScheme::Gpt);
        controller_->setPartitionScheme(rufus::PartitionScheme::Mbr);
        const auto& selection = controller_->selection();
        QVERIFY(controller_->canSelectGrub());
        QCOMPARE(selection.partition, rufus::PartitionScheme::Mbr);
        QCOMPARE(selection.fileSystems, formats);
        QCOMPARE(selection.fileSystem, filesystem);
        QCOMPARE(selection.clusterSize, cluster);
        QCOMPARE(window_->findChild<QComboBox*>(QStringLiteral("targetCombo"))->currentText(), efi ? QStringLiteral("BIOS or UEFI") : QStringLiteral("BIOS (or UEFI-CSM)"));
        QTest::mouseClick(start_, Qt::LeftButton);
        auto& writer = controller_->isoWriter();
        QTRY_COMPARE_WITH_TIMEOUT(writer.state(), rufus::IsoWriteState::AwaitingConfirmation, writeTimeoutMs);
        QCOMPARE(writer.request().fileSystem, filesystem);
        QCOMPARE(writer.request().clusterSize, cluster);
        QCOMPARE(writer.request().size, capacity);
        writer.cancel();
        QCOMPARE(helperStarts, 0);
        // Keep an explicit filesystem choice through a fresh scan, as Windows does.
        if (formats & FS_MASK(FS_NTFS))
        {
            controller_->setFileSystem(FS_NTFS);
            controller_->selectImage(sourcePath_);
            QTRY_VERIFY_WITH_TIMEOUT(!controller_->isAnalyzing(), writeTimeoutMs);
            QCOMPARE(controller_->selection().fileSystem, FS_NTFS);
            if (efi)
            {
                controller_->setPartitionScheme(rufus::PartitionScheme::Gpt);
                QCOMPARE(controller_->selection().partition, rufus::PartitionScheme::Gpt);
                QCOMPARE(controller_->selection().fileSystem, FS_NTFS);
                controller_->setPartitionScheme(rufus::PartitionScheme::Mbr);
                QCOMPARE(controller_->selection().partition, rufus::PartitionScheme::Mbr);
                QCOMPARE(controller_->selection().fileSystem, FS_NTFS);
            }
        }
        QCOMPARE(readFile(targetPath_), expectedTarget_);
    }

    void grubIsoNtfsPolicy_data()
    {
        QTest::addColumn<bool>("efi");
        QTest::addColumn<bool>("largeFile");
        QTest::newRow("BIOS") << false << false;
        QTest::newRow("BIOS-large-file") << false << true;
        QTest::newRow("EFI") << true << false;
        QTest::newRow("EFI-large-file") << true << true;
    }

    void grubIsoNtfsPolicy()
    {
        QFETCH(bool, efi);
        QFETCH(bool, largeFile);
        if (largeFile) faultKind = LargeLinuxFile; // Simulate the report, without a large copied ISO.
        selectIsoFixture(false, false, "win11", nullptr, efi, false, GRUB2_PACKAGE_VERSION);
        controller_->setPartitionScheme(rufus::PartitionScheme::Mbr);
        QVERIFY(controller_->canSelectGrub());
        QCOMPARE(controller_->selection().partition, rufus::PartitionScheme::Mbr);
        const quint32 fat = FS_MASK(FS_FAT16) | FS_MASK(FS_FAT32);
        const quint32 formats = largeFile ? FS_MASK(FS_NTFS) : fat | FS_MASK(FS_NTFS);
        QCOMPARE(controller_->selection().fileSystems, formats);
        QCOMPARE(controller_->selection().fileSystem, largeFile ? FS_NTFS : FS_FAT16);
        QVERIFY(window_->findChild<QComboBox*>(QStringLiteral("fileSystemCombo"))->findData(FS_NTFS) >= 0);
        controller_->setFileSystem(FS_NTFS);
        controller_->selectImage(sourcePath_);
        QTRY_VERIFY_WITH_TIMEOUT(!controller_->isAnalyzing(), writeTimeoutMs);
        QCOMPARE(controller_->selection().fileSystem, FS_NTFS); // Preserve the explicit choice across rescans.
        QTest::mouseClick(start_, Qt::LeftButton);
        auto& writer = controller_->isoWriter();
        QTRY_COMPARE_WITH_TIMEOUT(writer.state(), rufus::IsoWriteState::AwaitingConfirmation, writeTimeoutMs);
        QCOMPARE(writer.request().fileSystem, FS_NTFS);
        QCOMPARE(writer.request().clusterSize, quint32(4096));
        QCOMPARE(writer.request().volumeId, quint32(0));
        QVERIFY(writer.request().grub2 && writer.request().mbr);
        QCOMPARE(writer.request().usesUefiNtfs(), efi);
        QCOMPARE(writer.check().partition.uefi_ntfs_size > 0, efi);
        writer.cancel();
        // Direct callers must supply exactly the image's required UEFI:NTFS layout.
        auto source = controller_->selection().imageSource;
        source.grub2 = source.mbr = true;
        source.uefiNtfs = !efi;
        IMAGE_WRITE_CHOICES choices = {};
        choices.count = 1;
        choices.mode[0] = IMAGE_WRITE_ISO;
        writer.prepare(source, device_, QStringLiteral("NEW LABEL"), 4096, choices, FS_NTFS);
        QTRY_COMPARE_WITH_TIMEOUT(writer.state(), rufus::IsoWriteState::Failed, writeTimeoutMs);
        QVERIFY(!writer.statusText().isEmpty());
        QCOMPARE(completionBeeps, 0);
        QCOMPARE(helperStarts, 0);
        QCOMPARE(stat(QStringLiteral("opens")), 0);
        QCOMPARE(readFile(targetPath_), expectedTarget_);
    }

    void grubIsoHybridDd_data()
    {
        QTest::addColumn<int>("filesystem");
        QTest::addColumn<bool>("efi");
        QTest::newRow("FAT32") << int(FS_FAT32) << false;
        QTest::newRow("NTFS") << int(FS_NTFS) << false;
        QTest::newRow("EFI-FAT32") << int(FS_FAT32) << true;
        QTest::newRow("EFI-NTFS") << int(FS_NTFS) << true;
    }

    void grubIsoHybridDd()
    {
        QFETCH(int, filesystem);
        QFETCH(bool, efi);
        selectIsoFixture(true, false, "win11", nullptr, efi, false, "2.12");
        controller_->setFileSystem(filesystem);
        QTest::mouseClick(start_, Qt::LeftButton);
        auto* dialog = window_->findChild<rufus::ImageModeDialog*>();
        QVERIFY(dialog && dialog->isVisible());
        const auto choices = dialog->findChildren<QRadioButton*>();
        QVERIFY(choices.first()->isChecked()); // Rufus recommends ISO mode.
        QCOMPARE(helperStarts, 0);
        chooseImageMode(IMAGE_WRITE_DD);
        QTRY_COMPARE_WITH_TIMEOUT(state(), State::AwaitingConfirmation, writeTimeoutMs);
        QVERIFY(!controller_->isoWriter().isActive());
        QVERIFY(!window_->findChild<QMessageBox*>(QStringLiteral("grubDownloadDialog")));
        approvePlan();
        QTRY_COMPARE_WITH_TIMEOUT(state(), State::Complete, writeTimeoutMs);
        QCOMPARE(helperStarts, 1);
        QCOMPARE(readFile(targetPath_), expectedTarget_);
    }

    void standaloneGrub4dosFlow_data()
    {
        QTest::addColumn<QString>("mode");
        QTest::addColumn<int>("filesystem");
        for (int filesystem : {FS_FAT16, FS_FAT32, FS_NTFS})
        {
            for (const char* mode : {"cache", "download", "no", "cancel", "unusable-cache", "not-found",
                "bad-signature", "cancel-signature", "unplug", "cancel-confirmation", "formatter-disappeared",
                "deny", "sync-error", "cleanup-error", "wrong-layout", "skip-finalizing", "short-copy",
                "hold-formatting", "hold-bootloader", "hold-finalizing"})
                QTest::newRow(qPrintable(QStringLiteral("%1-%2").arg(filesystem == FS_NTFS ? "ntfs" : filesystem == FS_FAT16 ? "fat16" : "fat32", mode)))
                    << QString::fromLatin1(mode) << filesystem;
            if (filesystem == FS_NTFS)
                QTest::newRow("ntfs-short-format") << QStringLiteral("short-format") << filesystem;
        }
    }

    void standaloneGrub4dosFlow()
    {
        QFETCH(QString, mode);
        QFETCH(int, filesystem);
        const bool ntfs = filesystem == FS_NTFS;
        const QString filesystemName = ntfs ? QStringLiteral("NTFS")
            : filesystem == FS_FAT16 ? QStringLiteral("FAT16") : QStringLiteral("FAT32");
        selectIsoFixture(false, true);
        const QString retainedImage = controller_->selection().imagePath;
        selectFormat(filesystem, 64 * 1024 * 1024, BT_GRUB4DOS);
        const QString label = ntfs ? QString::fromUtf8("Rufus ș😀.test") : QStringLiteral("usb.name*:long");
        const QString normalizedLabel = ntfs ? QString::fromUtf8("Rufus ș😀_test") : QStringLiteral("USB_NAMELON");
        controller_->setVolumeLabel(label);
        const QString files = directory_.filePath(QStringLiteral("grub4dos-files"));
        QDir(files).removeRecursively();
        GrubDownloadServer server;
        server.core = readFile(QStringLiteral(RUFUS_SIGNED_FIXTURE "/../grub4dos-0.4.6a/grldr"));
        server.signature = readFile(QStringLiteral(RUFUS_SIGNED_FIXTURE "/../grub4dos-0.4.6a/grldr.sig"));
        QCOMPARE(server.core.size(), 322965);
        expectedGrubCore = QCryptographicHash::hash(server.core, QCryptographicHash::Sha256);
        server.mode = mode == QStringLiteral("cancel-signature") ? QStringLiteral("hold-signature") : mode;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        const QString screenshot = ntfs && mode == QStringLiteral("cache")
            ? qEnvironmentVariable("RUFUS_TEST_GRUB4DOS_SCREENSHOT") : QString();
        if (!screenshot.isEmpty()) QVERIFY(window_->grab().save(screenshot + QStringLiteral("-selection.png")));
        const bool prompt = mode == QStringLiteral("download") || mode == QStringLiteral("no")
            || mode == QStringLiteral("cancel") || mode == QStringLiteral("not-found")
            || mode == QStringLiteral("bad-signature") || mode == QStringLiteral("cancel-signature");
        const QString cached = files + QStringLiteral("/grub4dos-0.4.6a/grldr");
        if (!prompt)
        {
            QVERIFY(QDir().mkpath(QFileInfo(cached).absolutePath()));
            writeFile(cached, mode == QStringLiteral("unusable-cache") ? QByteArray("short") : server.core);
        }
        controller_->isoWriter().configure(*connection_, QCoreApplication::applicationDirPath() + QStringLiteral("/iso_helper_fixture"), files, server.root());
        auto& writer = controller_->isoWriter();
        QSignalSpy warnings(&writer, &rufus::IsoWriteController::grubDownloadRequired);
        helperMode = mode == QStringLiteral("cache") || mode == QStringLiteral("download")
            ? QStringLiteral("hold-after-result") : mode;
        QTest::mouseClick(start_, Qt::LeftButton);
        if (prompt)
        {
            QTRY_COMPARE_WITH_TIMEOUT(warnings.size(), 1, writeTimeoutMs);
            auto* dialog = window_->findChild<QMessageBox*>(QStringLiteral("grubDownloadDialog"));
            QVERIFY(dialog && dialog->isVisible());
            QCOMPARE(dialog->windowTitle(), QStringLiteral("Download grldr?"));
            QVERIFY(dialog->text().contains(QStringLiteral("Grub4DOS 0.4 or later")));
            QVERIFY(dialog->text().contains(QStringLiteral("stop and provide this file manually before starting again")));
            QVERIFY(!dialog->text().contains(QStringLiteral("core.img")));
            QVERIFY(dialog->text().contains(cached));
            QCOMPARE(dialog->defaultButton(), dialog->button(QMessageBox::Cancel));
            QCOMPARE(helperStarts, 0);
            const auto choice = mode == QStringLiteral("no") ? QMessageBox::No
                : mode == QStringLiteral("cancel") ? QMessageBox::Cancel : QMessageBox::Yes;
            QTest::mouseClick(dialog->button(choice), Qt::LeftButton);
        }
        if (mode == QStringLiteral("cancel-signature"))
        {
            QTRY_VERIFY_WITH_TIMEOUT(server.held, writeTimeoutMs);
            QCOMPARE(writer.state(), rufus::IsoWriteState::SelectingGrubCore);
            QCOMPARE(helperStarts, 0);
            writer.cancel();
        }
        const bool cancelledEarly = mode == QStringLiteral("cancel") || mode == QStringLiteral("cancel-signature");
        const bool failedEarly = mode == QStringLiteral("no") || mode == QStringLiteral("unusable-cache")
            || mode == QStringLiteral("not-found") || mode == QStringLiteral("bad-signature");
        if (cancelledEarly || failedEarly)
        {
            QTRY_COMPARE_WITH_TIMEOUT(writer.state(), cancelledEarly ? rufus::IsoWriteState::Cancelled : rufus::IsoWriteState::Failed, writeTimeoutMs);
            QCOMPARE(helperStarts, 0);
            QCOMPARE(completionBeeps, 0);
            QVERIFY(writer.grubCoreResult().bytes.isEmpty());
            QCOMPARE(readFile(targetPath_), expectedTarget_);
            return;
        }
        QTRY_COMPARE_WITH_TIMEOUT(writer.state(), rufus::IsoWriteState::AwaitingConfirmation, writeTimeoutMs);
        QCOMPARE(warnings.size(), prompt ? 1 : 0);
        QCOMPARE(writer.grubCoreResult().bytes, server.core);
        QVERIFY(writer.request().grub4dos && writer.request().mbr && writer.request().hasGrub2Core());
        QVERIFY(writer.request().imagePath.isEmpty() && !writer.request().grub2 && !writer.request().enhancedInstall);
        QCOMPARE(writer.request().fileSystem, filesystem);
        const quint32 expectedCluster = ntfs ? 4096 : filesystem == FS_FAT16 ? 2048 : 512;
        QCOMPARE(writer.request().clusterSize, expectedCluster);
        QCOMPARE(writer.request().label, normalizedLabel);
        QCOMPARE(writer.request().volumeId == 0, ntfs);
        QVERIFY(!writer.request().usesUefiNtfs() && !writer.request().uefiNtfs && !writer.request().sfd);
        QCOMPARE(controller_->selection().imagePath, retainedImage);
        QCOMPARE(rufus::FormatMbrBootCode(writer.request()), MBR_BOOT_GRUB4DOS);
        auto* confirmation = window_->findChild<QMessageBox*>(QStringLiteral("isoWriteConfirmation"));
        QVERIFY(confirmation && confirmation->informativeText().contains(QStringLiteral("Grub4DOS 0.4.6a — MBR, %1").arg(filesystemName)));
        QCOMPARE(helperStarts, 0);
        if (!screenshot.isEmpty()) QVERIFY(confirmation->grab().save(screenshot + QStringLiteral("-confirmation.png")));
        if (mode == QStringLiteral("unplug") || mode == QStringLiteral("cancel-confirmation"))
        {
            if (mode == QStringLiteral("unplug")) controller_->setDiscovery({}, {});
            else QTest::mouseClick(confirmation->button(QMessageBox::Cancel), Qt::LeftButton);
            QTRY_COMPARE_WITH_TIMEOUT(writer.state(), mode == QStringLiteral("unplug") ? rufus::IsoWriteState::Failed : rufus::IsoWriteState::Cancelled, writeTimeoutMs);
            QCOMPARE(helperStarts, 0);
            QCOMPARE(readFile(targetPath_), expectedTarget_);
            return;
        }
        // The helper receives retained bytes, even if the cache is replaced.
        writeFile(cached, QByteArray(server.core.size(), 'x'));
        if (mode == QStringLiteral("formatter-disappeared"))
        {
            missingNtfsTool = ntfs;
            missingFatTool = !ntfs;
        }
        controller_->setVolumeLabel(QStringLiteral("CHANGED"));
        QCOMPARE(writer.request().label, normalizedLabel);
        approveIsoPlan();
        if (mode == QStringLiteral("formatter-disappeared"))
        {
            QTRY_COMPARE_WITH_TIMEOUT(writer.state(), rufus::IsoWriteState::Failed, writeTimeoutMs);
            QCOMPARE(helperStarts, 0);
            return;
        }
        const bool success = mode == QStringLiteral("cache") || mode == QStringLiteral("download");
        const bool cancelJob = mode.startsWith(QStringLiteral("hold-"));
        if (success)
        {
            QTRY_COMPARE_WITH_TIMEOUT(writer.outcome().copiedBytes, quint64(server.core.size()), writeTimeoutMs);
            QCOMPARE(writer.state(), rufus::IsoWriteState::Writing);
            QVERIFY(helperProcess && helperProcess->state() == QProcess::Running);
            QCOMPARE(progress_->format(), QStringLiteral("Finalizing, please wait..."));
            // Original NTFS has twelve filesystem steps, FAT has five. First
            // finalization completes 20/21 or 13/14 of the discrete slots.
            // These independent Win32 expectations truncate at 0..65535.
            const int windowsFinalizingPosition = ntfs ? 62414 : 60853;
            QCOMPARE(progress_->value(), windowsFinalizingPosition);
            QCOMPARE(completionBeeps, 0);
            if (!screenshot.isEmpty()) QVERIFY(window_->grab().save(screenshot + QStringLiteral("-finalizing.png")));
            helperProcess->closeWriteChannel();
        }
        else if (cancelJob)
        {
            const QString caption = mode == QStringLiteral("hold-formatting") ? QStringLiteral("Formatting %1...").arg(filesystemName)
                : mode == QStringLiteral("hold-bootloader") ? QStringLiteral("Installing Grub4DOS 0.4.6a...") : QStringLiteral("Finalizing, please wait...");
            QTRY_COMPARE_WITH_TIMEOUT(progress_->format(), caption, writeTimeoutMs);
            writer.cancel();
        }
        QTRY_COMPARE_WITH_TIMEOUT(writer.state(), success ? rufus::IsoWriteState::Complete
            : cancelJob ? rufus::IsoWriteState::Cancelled : rufus::IsoWriteState::Failed, writeTimeoutMs);
        QCOMPARE(helperStarts, 1);
        QCOMPARE(completionBeeps, success ? 1 : 0);
        QCOMPARE(completionAlerts, success ? 1 : 0);
        if (success)
        {
            QCOMPARE(progress_->value(), MAX_PROGRESS);
            QCOMPARE(progress_->format(), QStringLiteral("READY"));
            QCOMPARE(writer.statusText(), QStringLiteral("Grub4DOS 0.4.6a creation completed."));
            if (!screenshot.isEmpty()) QVERIFY(window_->grab().save(screenshot + QStringLiteral("-ready.png")));
            controller_->setDiscovery({}, {});
            QCOMPARE(progress_->format(), QStringLiteral("READY"));
            QCOMPARE(completionBeeps, 1);
        }
        else QVERIFY(progress_->value() < MAX_PROGRESS);
        QCOMPARE(readFile(targetPath_), expectedTarget_);
    }

    void standaloneGrub2Creation()
    {
        QFETCH(int, filesystem);
        QFETCH(bool, large);
        selectIsoFixture(false, true); // The retained image must not enter this standalone request.
        const bool ext = filesystem == FS_EXT2 || filesystem == FS_EXT3;
        const quint64 capacity = ext ? 320ULL * 1024 * 1024 : large ? 64ULL * 1024 * 1024 * 1024 : 64ULL * 1024 * 1024;
        selectFormat(filesystem, capacity, BT_GRUB2);
        missingSyslinuxTool = QStringLiteral("/rufus-syslinux");
        controller_->toggleEnhancedInstall();
        const bool ntfs = filesystem == FS_NTFS;
        const QString label = ext ? QString::fromUtf8("12345678901234😀!") : ntfs ? QString::fromUtf8("Rufus ș😀.test") : QStringLiteral("usb.name*:long");
        const QString normalized = ext ? QStringLiteral("12345678901234") : ntfs ? QString::fromUtf8("Rufus ș😀_test") : QStringLiteral("USB_NAMELON");
        controller_->setVolumeLabel(label);
        helperMode = QStringLiteral("hold-after-result");
        auto* partitions = window_->findChild<QComboBox*>(QStringLiteral("partitionCombo"));
        auto* filesystems = window_->findChild<QComboBox*>(QStringLiteral("fileSystemCombo"));
        QCOMPARE(partitions->count(), 1);
        QCOMPARE(partitions->currentText(), QStringLiteral("MBR"));
        QCOMPARE(filesystems->count(), ext ? 5 : large ? 2 : 3);
        QCOMPARE(window_->findChild<QComboBox*>(QStringLiteral("targetCombo"))->currentText(), QStringLiteral("BIOS (or UEFI-CSM)"));
        QCOMPARE(controller_->selection().imagePath.isEmpty(), false);
        QVERIFY(start_->isEnabled());
        const QString screenshot = qEnvironmentVariable("RUFUS_TEST_GRUB2_SCREENSHOT");
        if (!screenshot.isEmpty() && !large && filesystem == FS_FAT32)
            QVERIFY(window_->grab().save(screenshot + QStringLiteral("-selection.png")));
        QTest::mouseClick(start_, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::AwaitingConfirmation, writeTimeoutMs);
        const auto& request = controller_->isoWriter().request();
        QVERIFY(request.grub2 && request.mbr && request.isDevice);
        QVERIFY(!request.syslinux && !request.freeDos && !request.nonBootable && !request.uefiNtfs && !request.sfd);
        QVERIFY(request.imagePath.isEmpty() && !request.enhancedInstall && !request.hasWindowsInstallSettings());
        QCOMPARE(request.windowsFlags, 0);
        QCOMPARE(request.fileSystem, filesystem);
        QCOMPARE(request.label, normalized);
        QCOMPARE(request.volumeId == 0, ntfs || ext);
        const quint32 expectedCluster = ext ? 0x100 : large ? 32768 : ntfs ? 4096 : filesystem == FS_FAT16 ? 2048 : 512;
        QCOMPARE(request.clusterSize, expectedCluster);
        if (ext)
        {
            auto* clusters = window_->findChild<QComboBox*>(QStringLiteral("clusterCombo"));
            QCOMPARE(clusters->count(), 1);
            QCOMPARE(clusters->currentText(), QStringLiteral("Default"));
        }
        QCOMPARE(rufus::FormatMbrBootCode(request), MBR_BOOT_GRUB2);
        QCOMPARE(syslinuxToolChecks.load(), 0);
        auto* confirmation = window_->findChild<QMessageBox*>(QStringLiteral("isoWriteConfirmation"));
        QVERIFY(confirmation && confirmation->isVisible());
        const QString format = ext ? QStringLiteral("ext%1").arg(filesystem == FS_EXT2 ? 2 : 3) : ntfs ? QStringLiteral("NTFS")
            : QStringLiteral("FAT%1").arg(filesystem == FS_FAT16 ? 16 : 32);
        QVERIFY(confirmation->informativeText().contains(QStringLiteral("Grub 2.14 — MBR, ") + format));
        QVERIFY(!confirmation->informativeText().contains(QStringLiteral("Image:")));
        if (ext) QVERIFY(confirmation->informativeText().contains(QStringLiteral("Cluster size: Default")));
        QCOMPARE(confirmation->defaultButton(), confirmation->button(QMessageBox::Cancel));
        QCOMPARE(helperStarts, 0);
        controller_->setBootType(BT_IMAGE);
        controller_->setVolumeLabel(QStringLiteral("CHANGED"));
        QCOMPARE(controller_->selection().bootType, int(BT_GRUB2));
        QCOMPARE(request.label, normalized);
        approveIsoPlan();
        QTRY_VERIFY_WITH_TIMEOUT(controller_->isoWriter().outcome().filesystemBytes > 0, writeTimeoutMs);
        QVERIFY(helperProcess && helperProcess->state() == QProcess::Running);
        QCOMPARE(controller_->isoWriter().state(), rufus::IsoWriteState::Writing);
        QCOMPARE(controller_->isoWriter().outcome().copiedBytes, quint64(0));
        QCOMPARE(progress_->format(), QStringLiteral("Finalizing, please wait..."));
        QVERIFY(progress_->value() > 0 && progress_->value() < MAX_PROGRESS);
        QCOMPARE(completionBeeps, 0);
        QCOMPARE(completionAlerts, 0);
        QCOMPARE(helperStarts, 1);
        QCOMPARE(syslinuxToolChecks.load(), 0);
        if (!screenshot.isEmpty() && !large && filesystem == FS_FAT32)
            QVERIFY(window_->grab().save(screenshot + QStringLiteral("-finalizing.png")));
        helperProcess->closeWriteChannel();
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::Complete, writeTimeoutMs);
        QCOMPARE(progress_->value(), MAX_PROGRESS);
        QCOMPARE(progress_->format(), QStringLiteral("READY"));
        QCOMPARE(controller_->isoWriter().statusText(), QStringLiteral("Grub 2.14 creation completed."));
        QCOMPARE(completionBeeps, 1);
        QCOMPARE(completionAlerts, 1);
        QCOMPARE(helperStarts, 1);
        if (!screenshot.isEmpty() && !large && filesystem == FS_FAT32)
            QVERIFY(window_->grab().save(screenshot + QStringLiteral("-ready.png")));
        controller_->setDiscovery({}, {});
        QCOMPARE(progress_->format(), QStringLiteral("READY"));
        QCOMPARE(completionBeeps, 1);
        QCOMPARE(readFile(targetPath_), expectedTarget_);
    }

    void standaloneGrub2PreparationStops_data()
    {
        QTest::addColumn<QString>("reason");
        QTest::addColumn<int>("filesystem");
        for (int filesystem : {FS_FAT32, FS_NTFS, FS_EXT2, FS_EXT3})
            for (const char* reason : {"cancel-confirmation", "unplug", "missing-formatter", "formatter-disappeared"})
                QTest::newRow(qPrintable(QStringLiteral("%1-%2").arg(filesystem).arg(reason)))
                    << QString::fromLatin1(reason) << filesystem;
    }

    void standaloneGrub2RestoresImage()
    {
        selectIsoFixture();
        const QString image = controller_->selection().imagePath;
        selectFormat(FS_FAT32, 64 * 1024 * 1024, BT_GRUB2);
        auto* boot = window_->findChild<QComboBox*>(QStringLiteral("bootCombo"));
        auto* advanced = window_->findChild<QToolButton*>(QStringLiteral("advancedDriveButton"));
        QCOMPARE(boot->currentText(), QStringLiteral("Grub 2.14"));
        QCOMPARE(controller_->selection().imagePath, image);
        QCOMPARE(boot->itemData(1).toString(), image);
        QVERIFY(boot->toolTip().isEmpty());
        QVERIFY(!controller_->canSelectSyslinux());
        QCOMPARE(controller_->isoWriteChoices(0, true, true).count, 0);
        QTest::mouseClick(advanced, Qt::LeftButton);
        QCOMPARE(controller_->selection().bootType, int(BT_IMAGE));
        QCOMPARE(controller_->selection().imagePath, image);
        QCOMPARE(boot->currentIndex(), 1);
        QTest::mouseClick(advanced, Qt::LeftButton);
        controller_->setBootType(BT_GRUB2);
        selectFixture();
        QCOMPARE(controller_->selection().bootType, int(BT_IMAGE));
        QCOMPARE(boot->currentIndex(), 1);
        QCOMPARE(helperStarts, 0);
    }

    void standaloneGrub2PreparationStops()
    {
        QFETCH(QString, reason);
        QFETCH(int, filesystem);
        const bool ext = filesystem == FS_EXT2 || filesystem == FS_EXT3;
        selectFormat(filesystem, (ext ? 320ULL : 64ULL) * 1024 * 1024, BT_GRUB2);
        const bool ntfs = filesystem == FS_NTFS;
        missingExtTool = ext && reason == QStringLiteral("missing-formatter");
        missingFatTool = !ntfs && !ext && reason == QStringLiteral("missing-formatter");
        missingNtfsTool = ntfs && reason == QStringLiteral("missing-formatter");
        QTest::mouseClick(start_, Qt::LeftButton);
        const bool cancelled = reason == QStringLiteral("cancel-confirmation");
        if (!missingFatTool && !missingNtfsTool && !missingExtTool)
        {
            QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::AwaitingConfirmation, writeTimeoutMs);
            if (reason == QStringLiteral("unplug")) controller_->setDiscovery({}, {});
            else if (cancelled)
            {
                auto* confirmation = window_->findChild<QMessageBox*>(QStringLiteral("isoWriteConfirmation"));
                QTest::mouseClick(confirmation->button(QMessageBox::Cancel), Qt::LeftButton);
            }
            else
            {
                if (ext) missingExtTool = true;
                else if (ntfs) missingNtfsTool = true;
                else missingFatTool = true;
                approveIsoPlan();
            }
        }
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), cancelled ? rufus::IsoWriteState::Cancelled : rufus::IsoWriteState::Failed, writeTimeoutMs);
        QCOMPARE(helperStarts, 0);
        QCOMPARE(completionBeeps, 0);
        QCOMPARE(progress_->value(), 0);
        QCOMPARE(readFile(targetPath_), expectedTarget_);
    }

    void standaloneGrub2FailureAndCancel_data()
    {
        QTest::addColumn<QString>("mode");
        QTest::addColumn<QString>("caption");
        QTest::addColumn<int>("filesystem");
        for (const char* mode : {"deny", "sync-error", "cleanup-error", "wrong-layout", "skip-finalizing", "short-copy"})
            QTest::newRow(mode) << QString::fromLatin1(mode) << QString() << int(FS_FAT32);
        QTest::newRow("cancel-format") << QStringLiteral("hold-formatting") << QStringLiteral("Formatting FAT32...") << int(FS_FAT32);
        QTest::newRow("cancel-bootloader") << QStringLiteral("hold-bootloader") << QStringLiteral("Installing Grub 2.14...") << int(FS_FAT32);
        QTest::newRow("cancel-finalize") << QStringLiteral("hold-finalizing") << QStringLiteral("Finalizing, please wait...") << int(FS_FAT32);
        QTest::newRow("ntfs-sync-error") << QStringLiteral("sync-error") << QString() << int(FS_NTFS);
        QTest::newRow("ntfs-cancel-format") << QStringLiteral("hold-formatting") << QStringLiteral("Formatting NTFS...") << int(FS_NTFS);
        QTest::newRow("ntfs-cancel-bootloader") << QStringLiteral("hold-bootloader") << QStringLiteral("Installing Grub 2.14...") << int(FS_NTFS);
    }

    void standaloneGrub2FailureAndCancel()
    {
        QFETCH(QString, mode);
        QFETCH(QString, caption);
        QFETCH(int, filesystem);
        selectFormat(filesystem, 64 * 1024 * 1024, BT_GRUB2);
        helperMode = mode;
        QTest::mouseClick(start_, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::AwaitingConfirmation, writeTimeoutMs);
        approveIsoPlan();
        if (!caption.isEmpty())
        {
            QTRY_COMPARE_WITH_TIMEOUT(progress_->format(), caption, writeTimeoutMs);
            controller_->isoWriter().cancel();
        }
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), caption.isEmpty() ? rufus::IsoWriteState::Failed : rufus::IsoWriteState::Cancelled, writeTimeoutMs);
        QCOMPARE(helperStarts, 1);
        QCOMPARE(completionBeeps, 0);
        QCOMPARE(completionAlerts, 0);
        QVERIFY(progress_->value() < MAX_PROGRESS);
        QCOMPARE(readFile(targetPath_), expectedTarget_);
    }

    void standaloneSyslinuxPreparationStops_data()
    {
        QTest::addColumn<QString>("reason");
        QTest::addColumn<int>("bootType");
        QTest::addColumn<int>("filesystem");
        for (int type : {BT_SYSLINUX_V4, BT_SYSLINUX_V6})
        {
            for (int fs : {FS_FAT32, FS_NTFS})
            {
                if (type == BT_SYSLINUX_V4 && fs == FS_NTFS) continue;
                for (const char* reason : {"cancel-confirmation", "unplug", "missing-formatter", "missing-installer", "installer-disappeared", "formatter-disappeared"})
                    QTest::newRow(qPrintable(QStringLiteral("%1-%2-%3").arg(type).arg(fs).arg(reason)))
                        << QString::fromLatin1(reason) << type << fs;
            }
            for (const char* reason : {"missing-mcopy", "missing-mattrib"})
                QTest::newRow(qPrintable(QStringLiteral("%1-%2").arg(type).arg(reason)))
                    << QString::fromLatin1(reason) << type << int(FS_FAT32);
        }
    }

    void standaloneSyslinuxPreparationStops()
    {
        QFETCH(QString, reason);
        QFETCH(int, bootType);
        QFETCH(int, filesystem);
        const bool ntfs = filesystem == FS_NTFS;
        const QString installer = ntfs ? QStringLiteral("/rufus-extlinux") : bootType == BT_SYSLINUX_V4 ? QStringLiteral("/rufus-syslinux4") : QStringLiteral("/rufus-syslinux");
        selectFormat(filesystem, 64 * 1024 * 1024, bootType);
        missingFatTool = !ntfs && reason == QStringLiteral("missing-formatter");
        missingNtfsTool = ntfs && reason == QStringLiteral("missing-formatter");
        if (reason == QStringLiteral("missing-installer")) missingSyslinuxTool = installer;
        if (reason == QStringLiteral("missing-mcopy")) missingSyslinuxTool = QStringLiteral("/mcopy");
        if (reason == QStringLiteral("missing-mattrib")) missingSyslinuxTool = QStringLiteral("/mattrib");
        QTest::mouseClick(start_, Qt::LeftButton);
        const bool cancelled = reason == QStringLiteral("cancel-confirmation");
        if (!reason.startsWith(QStringLiteral("missing-")))
        {
            QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::AwaitingConfirmation, writeTimeoutMs);
            if (reason == QStringLiteral("unplug"))
                controller_->setDiscovery({}, {});
            else if (cancelled)
            {
                auto* confirmation = window_->findChild<QMessageBox*>(QStringLiteral("isoWriteConfirmation"));
                QTest::mouseClick(confirmation->button(QMessageBox::Cancel), Qt::LeftButton);
            }
            else
            {
                missingFatTool = !ntfs && reason == QStringLiteral("formatter-disappeared");
                missingNtfsTool = ntfs && reason == QStringLiteral("formatter-disappeared");
                if (reason == QStringLiteral("installer-disappeared")) missingSyslinuxTool = installer;
                approveIsoPlan();
            }
        }
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), cancelled ? rufus::IsoWriteState::Cancelled : rufus::IsoWriteState::Failed, writeTimeoutMs);
        QVERIFY(!controller_->isoWriter().isActive());
        QCOMPARE(helperStarts, 0);
        QCOMPARE(completionBeeps, 0);
        QCOMPARE(progress_->value(), 0);
        QCOMPARE(readFile(targetPath_), expectedTarget_);
    }

    void standaloneSyslinuxFailureAndCancel_data()
    {
        QTest::addColumn<QString>("mode");
        QTest::addColumn<QString>("caption");
        QTest::addColumn<int>("bootType");
        QTest::addColumn<int>("filesystem");
        for (int type : {BT_SYSLINUX_V4, BT_SYSLINUX_V6})
        {
            const QString installing = type == BT_SYSLINUX_V4 ? QStringLiteral("Installing Syslinux 4.07...")
                : QStringLiteral("Installing Syslinux 6.04...");
            for (int fs : {FS_FAT32, FS_NTFS})
            {
                if (type == BT_SYSLINUX_V4 && fs == FS_NTFS) continue;
                const QString prefix = QStringLiteral("%1-%2-").arg(type).arg(fs);
                for (const char* mode : {"deny", "sync-error", "cleanup-error", "wrong-layout", "skip-finalizing"})
                    QTest::newRow(qPrintable(prefix + QLatin1String(mode))) << QString::fromLatin1(mode) << QString() << type << fs;
                const QString formatting = fs == FS_NTFS ? QStringLiteral("Formatting NTFS...") : QStringLiteral("Formatting FAT32...");
                QTest::newRow(qPrintable(prefix + "cancel-format")) << QStringLiteral("hold-formatting") << formatting << type << fs;
                QTest::newRow(qPrintable(prefix + "cancel-bootloader")) << QStringLiteral("hold-bootloader") << installing << type << fs;
                QTest::newRow(qPrintable(prefix + "cancel-step")) << QStringLiteral("syslinux-steps") << installing << type << fs;
                QTest::newRow(qPrintable(prefix + "cancel-finalize")) << QStringLiteral("hold-finalizing") << QStringLiteral("Finalizing, please wait...") << type << fs;
                if (fs == FS_NTFS)
                {
                    QTest::newRow(qPrintable(prefix + "cancel-mount")) << QStringLiteral("hold-mounting") << installing << type << fs;
                    QTest::newRow(qPrintable(prefix + "mount-error")) << QStringLiteral("mount-error") << QString() << type << fs;
                    QTest::newRow(qPrintable(prefix + "short-format")) << QStringLiteral("short-format") << QString() << type << fs;
                }
            }
        }
    }

    void standaloneSyslinuxFailureAndCancel()
    {
        QFETCH(QString, mode);
        QFETCH(QString, caption);
        QFETCH(int, bootType);
        QFETCH(int, filesystem);
        selectFormat(filesystem, 64 * 1024 * 1024, bootType);
        helperMode = mode;
        QTest::mouseClick(start_, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::AwaitingConfirmation, writeTimeoutMs);
        approveIsoPlan();
        if (!caption.isEmpty())
        {
            QTRY_COMPARE_WITH_TIMEOUT(progress_->format(), caption, writeTimeoutMs);
            controller_->isoWriter().cancel();
        }
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), caption.isEmpty() ? rufus::IsoWriteState::Failed : rufus::IsoWriteState::Cancelled, writeTimeoutMs);
        QCOMPARE(helperStarts, 1);
        QCOMPARE(completionBeeps, 0);
        QCOMPARE(completionAlerts, 0);
        QVERIFY(progress_->value() < MAX_PROGRESS);
        if (mode != QStringLiteral("deny"))
            QVERIFY(controller_->isoWriter().statusText().contains(QStringLiteral("incomplete")));
        QCOMPARE(readFile(targetPath_), expectedTarget_);
    }

    void legacyBootChoicesAndDownloadRefusal()
    {
        selectFormat(FS_FAT32,320ULL*1024*1024,BT_MSDOS);
        controller_->isoWriter().configure(*connection_,QCoreApplication::applicationDirPath()+QStringLiteral("/iso_helper_fixture"),
            directory_.filePath(QStringLiteral("missing-msdos-cache")));
        QTest::mouseClick(start_,Qt::LeftButton);
        QCOMPARE(controller_->isoWriter().state(),rufus::IsoWriteState::ChoosingDosOptions);
        auto* locale = window_->findChild<rufus::FreeDosLocaleDialog*>(); QVERIFY(locale);
        QCOMPARE(locale->windowTitle(),QStringLiteral("MS-DOS settings"));
        QTest::mouseClick(locale->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok),Qt::LeftButton);
        auto* download = window_->findChild<QMessageBox*>(QStringLiteral("msDosDownloadDialog")); QVERIFY(download && download->isVisible());
        QCOMPARE(download->defaultButton(),download->button(QMessageBox::No));
        QCOMPARE(helperStarts,0);
        QTest::mouseClick(download->button(QMessageBox::No),Qt::LeftButton);
        QCOMPARE(controller_->isoWriter().state(),rufus::IsoWriteState::Cancelled);
        QCOMPARE(helperStarts,0); QCOMPARE(readFile(targetPath_),expectedTarget_);
        selectFormat(FS_FAT32,320ULL*1024*1024,BT_REACTOS);
        QTest::mouseClick(start_,Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(),rufus::IsoWriteState::AwaitingConfirmation,fixtureTimeoutMs);
        auto* confirm = window_->findChild<QMessageBox*>(QStringLiteral("isoWriteConfirmation")); QVERIFY(confirm);
        QVERIFY(confirm->informativeText().contains(QStringLiteral("ReactOS — MBR, FAT32")));
        QCOMPARE(helperStarts,0); QTest::mouseClick(confirm->button(QMessageBox::Cancel),Qt::LeftButton);
        QCOMPARE(helperStarts,0); QCOMPARE(readFile(targetPath_),expectedTarget_);
    }

    void freeDosCreation_data()
    {
        QTest::addColumn<int>("filesystem");
        QTest::addColumn<bool>("romanian");
        QTest::addColumn<bool>("large");
        for (int fs : {FS_FAT16, FS_FAT32})
            for (bool romanian : {false, true})
                QTest::newRow(qPrintable(QStringLiteral("FAT%1-%2").arg(fs == FS_FAT16 ? 16 : 32).arg(romanian ? "ro852" : "us437")))
                    << fs << romanian << false;
        QTest::newRow("large-FAT32-ro852") << int(FS_FAT32) << true << true;
    }

    void freeDosCreation()
    {
        QFETCH(int, filesystem);
        QFETCH(bool, romanian);
        QFETCH(bool, large);
        selectIsoFixture(false, true); // Retained Windows image must not enter this job.
        const quint64 capacity = large ? 64ULL * 1024 * 1024 * 1024 : 64ULL * 1024 * 1024;
        selectFormat(filesystem, capacity, BT_FREEDOS);
        controller_->toggleEnhancedInstall();
        controller_->setVolumeLabel(QStringLiteral("usb.name*:long"));
        helperMode = QStringLiteral("freedos-steps");
        helperCopiedBytes = 401073; // Original payload plus RO startup files; byte count is not progress.
        QTest::mouseClick(start_, Qt::LeftButton);
        QCOMPARE(controller_->isoWriter().state(), rufus::IsoWriteState::ChoosingDosOptions);
        auto* dialog = window_->findChild<rufus::FreeDosLocaleDialog*>();
        QVERIFY(dialog && dialog->isVisible());
        auto* keyboard = dialog->findChild<QComboBox*>(QStringLiteral("freeDosKeyboard"));
        auto* codepage = dialog->findChild<QComboBox*>(QStringLiteral("freeDosCodepage"));
        QCOMPARE(keyboard->currentData().toString(), QStringLiteral("us"));
        QCOMPARE(codepage->currentData().toUInt(), quint32(437));
        QCOMPARE(helperStarts, 0);
        QVERIFY(!start_->isEnabled() && !select_->isEnabled());
        controller_->setBootType(BT_IMAGE);
        controller_->setVolumeLabel(QStringLiteral("CHANGED"));
        QCOMPARE(controller_->selection().bootType, int(BT_FREEDOS));
        if (romanian)
        {
            keyboard->setCurrentIndex(keyboard->findData(QStringLiteral("ro")));
            codepage->setCurrentIndex(codepage->findData(852));
        }
        const QString screenshot = qEnvironmentVariable("RUFUS_TEST_FREEDOS_SCREENSHOT");
        if (!screenshot.isEmpty() && romanian && filesystem == FS_FAT32)
            QVERIFY(dialog->grab().save(screenshot));
        QTest::mouseClick(dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok), Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::AwaitingConfirmation, writeTimeoutMs);
        const auto& request = controller_->isoWriter().request();
        QVERIFY(request.freeDos && request.mbr && request.dosRufusMbr);
        QVERIFY(!request.nonBootable && !request.uefiNtfs && !request.sfd && !request.syslinux);
        QVERIFY(request.imagePath.isEmpty() && !request.enhancedInstall && !request.hasWindowsInstallSettings());
        QCOMPARE(request.windowsFlags, 0);
        QCOMPARE(request.dosKeyboard, romanian ? QStringLiteral("ro") : QStringLiteral("us"));
        QCOMPARE(request.dosCodepage, quint32(romanian ? 852 : 437));
        QCOMPARE(request.label, QStringLiteral("USB_NAMELON"));
        QCOMPARE(request.fileSystem, filesystem);
        auto* confirmation = window_->findChild<QMessageBox*>(QStringLiteral("isoWriteConfirmation"));
        QVERIFY(confirmation && confirmation->informativeText().contains(QStringLiteral("FreeDOS — MBR, FAT")));
        QVERIFY(!confirmation->informativeText().contains(QStringLiteral("Image:")));
        QCOMPARE(confirmation->defaultButton(), confirmation->button(QMessageBox::Cancel));
        controller_->isoWriter().chooseDosOptions(QStringLiteral("fr"), 858);
        QCOMPARE(request.dosCodepage, quint32(romanian ? 852 : 437)); // Choice is frozen.
        QCOMPARE(helperStarts, 0);
        approveIsoPlan();
        // Original Win32 weighting: 17 discrete slots, 6 for DOS.
        // Preserve its float rounding, including the first three truncated units.
        const int positions[] = {38549, 42404, 46259, 50115, 53970, 57825};
        const int largePositions[] = {60947, 61602, 62258, 62913, 63568, 64224};
        for (int i = 0; i < 6; ++i)
        {
            QTRY_COMPARE_WITH_TIMEOUT(progress_->value(), large ? largePositions[i] : positions[i], writeTimeoutMs);
            QCOMPARE(progress_->format(), QStringLiteral("Copying DOS files..."));
            QCOMPARE(helperStarts, 1);
            QCOMPARE(completionBeeps, 0);
            QVERIFY(!select_->isEnabled());
            helperProcess->write("continue\n");
        }
        QTRY_VERIFY_WITH_TIMEOUT(controller_->isoWriter().outcome().filesystemBytes > 0, writeTimeoutMs);
        QCOMPARE(progress_->value(), large ? 64879 : 61680);
        QCOMPARE(progress_->format(), QStringLiteral("Finalizing, please wait..."));
        QVERIFY(controller_->isoWriter().isActive());
        QCOMPARE(completionAlerts, 0);
        helperProcess->closeWriteChannel();
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::Complete, writeTimeoutMs);
        QCOMPARE(progress_->value(), MAX_PROGRESS);
        QCOMPARE(progress_->format(), QStringLiteral("READY"));
        QCOMPARE(completionBeeps, 1);
        QCOMPARE(completionAlerts, 1);
        QCOMPARE(helperStarts, 1);
        controller_->setDiscovery({}, {});
        QCOMPARE(progress_->value(), MAX_PROGRESS);
        QCOMPARE(progress_->format(), QStringLiteral("READY"));
        QCOMPARE(completionBeeps, 1);
        QCOMPARE(readFile(targetPath_), expectedTarget_);
    }

    void bareBootCapacityLimits_data()
    {
        QTest::addColumn<int>("bootType");
        QTest::newRow("FreeDOS") << int(BT_FREEDOS);
        QTest::newRow("Syslinux4") << int(BT_SYSLINUX_V4);
        QTest::newRow("Syslinux6") << int(BT_SYSLINUX_V6);
    }

    void bareBootCapacityLimits()
    {
        QFETCH(int, bootType);
        selectFormat(FS_FAT16, 64 * 1024 * 1024, bootType);
        device_.size = 4ULL * 1024 * 1024 * 1024;
        controller_->setDiscovery({device_}, {});
        controller_->selectDevice(device_.id);
        const quint32 filesystems = FS_MASK(FS_FAT32) | (bootType == BT_SYSLINUX_V6 ? FS_MASK(FS_NTFS) : 0);
        QCOMPARE(controller_->selection().fileSystems, filesystems);
        QCOMPARE(controller_->selection().fileSystem, int(FS_FAT32));
        QVERIFY(start_->isEnabled());
        device_.size = (quint64(1) << 41) + 512;
        controller_->setDiscovery({device_}, {});
        controller_->selectDevice(device_.id);
        QCOMPARE(controller_->selection().fileSystems, quint32(0));
        QCOMPARE(controller_->selection().partition, rufus::PartitionScheme::Mbr);
        auto* partitions = window_->findChild<QComboBox*>(QStringLiteral("partitionCombo"));
        QCOMPARE(partitions->count(), 1);
        QCOMPARE(partitions->currentText(), QStringLiteral("MBR"));
        QVERIFY(!start_->isEnabled());
        QCOMPARE(helperStarts, 0);
    }

    void freeDosPreparationStops_data()
    {
        QTest::addColumn<QString>("reason");
        for (const char* reason : {"cancel-locale", "cancel-confirmation", "unplug", "invalid-locale", "missing-formatter", "formatter-disappeared"})
            QTest::newRow(reason) << QString::fromLatin1(reason);
    }

    void freeDosPreparationStops()
    {
        QFETCH(QString, reason);
        selectFormat(FS_FAT32, 64 * 1024 * 1024, BT_FREEDOS);
        QTest::mouseClick(start_, Qt::LeftButton);
        auto* dialog = window_->findChild<rufus::FreeDosLocaleDialog*>();
        QVERIFY(dialog && dialog->isVisible());
        QCOMPARE(controller_->isoWriter().state(), rufus::IsoWriteState::ChoosingDosOptions);
        const bool cancelled = reason.startsWith(QStringLiteral("cancel-"));
        if (reason == QStringLiteral("cancel-locale"))
            QTest::mouseClick(dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Cancel), Qt::LeftButton);
        else if (reason == QStringLiteral("unplug"))
            controller_->setDiscovery({}, {});
        else if (reason == QStringLiteral("invalid-locale"))
            controller_->isoWriter().chooseDosOptions(QStringLiteral("bogus"), 437);
        else
        {
            missingFatTool = reason == QStringLiteral("missing-formatter");
            dialog->accept();
            if (!missingFatTool)
            {
                QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::AwaitingConfirmation, writeTimeoutMs);
                auto* confirmation = window_->findChild<QMessageBox*>(QStringLiteral("isoWriteConfirmation"));
                if (cancelled)
                    QTest::mouseClick(confirmation->button(QMessageBox::Cancel), Qt::LeftButton);
                else
                {
                    missingFatTool = true;
                    approveIsoPlan();
                }
            }
        }
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), cancelled ? rufus::IsoWriteState::Cancelled : rufus::IsoWriteState::Failed, writeTimeoutMs);
        QVERIFY(!controller_->isoWriter().isActive());
        QCOMPARE(helperStarts, 0);
        QCOMPARE(completionBeeps, 0);
        QCOMPARE(progress_->value(), 0);
        QCOMPARE(readFile(targetPath_), expectedTarget_);
    }

    void freeDosFailureAndCancel_data()
    {
        QTest::addColumn<QString>("mode");
        QTest::addColumn<QString>("caption");
        for (const char* mode : {"deny", "sync-error", "cleanup-error", "wrong-layout", "skip-finalizing"})
            QTest::newRow(mode) << QString::fromLatin1(mode) << QString();
        QTest::newRow("cancel-format") << QStringLiteral("hold-formatting") << QStringLiteral("Formatting FAT32...");
        QTest::newRow("cancel-bootloader") << QStringLiteral("hold-bootloader") << QStringLiteral("Writing partition boot record...");
        QTest::newRow("cancel-copy") << QStringLiteral("freedos-steps") << QStringLiteral("Copying DOS files...");
        QTest::newRow("cancel-finalize") << QStringLiteral("hold-finalizing") << QStringLiteral("Finalizing, please wait...");
    }

    void freeDosFailureAndCancel()
    {
        QFETCH(QString, mode);
        QFETCH(QString, caption);
        selectFormat(FS_FAT32, 64 * 1024 * 1024, BT_FREEDOS);
        helperMode = mode;
        QTest::mouseClick(start_, Qt::LeftButton);
        auto* dialog = window_->findChild<rufus::FreeDosLocaleDialog*>();
        QVERIFY(dialog && dialog->isVisible());
        dialog->accept();
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::AwaitingConfirmation, writeTimeoutMs);
        approveIsoPlan();
        if (!caption.isEmpty())
        {
            QTRY_COMPARE_WITH_TIMEOUT(progress_->format(), caption, writeTimeoutMs);
            controller_->isoWriter().cancel();
        }
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), caption.isEmpty() ? rufus::IsoWriteState::Failed : rufus::IsoWriteState::Cancelled, writeTimeoutMs);
        QCOMPARE(helperStarts, 1);
        QCOMPARE(completionBeeps, 0);
        QCOMPARE(completionAlerts, 0);
        QVERIFY(progress_->value() < MAX_PROGRESS);
        if (mode != QStringLiteral("deny"))
            QVERIFY(controller_->isoWriter().statusText().contains(QStringLiteral("incomplete")));
        QCOMPARE(readFile(targetPath_), expectedTarget_);
    }

    void isoEspCreation_data()
    {
        QTest::addColumn<int>("filesystem");
        QTest::newRow("FAT16") << int(FS_FAT16);
        QTest::newRow("FAT32") << int(FS_FAT32);
    }

    void isoEspCreation()
    {
        QFETCH(int, filesystem);
        selectIsoFixture(); controller_->setFileSystem(filesystem);
        controller_->setQuickFormat(false); chooseEsp_ = true;
        const quint32 selectedCluster = controller_->selection().clusterSize;
        QTest::mouseClick(start_, Qt::LeftButton);
        auto& process = controller_->isoWriter();
        QTRY_COMPARE_WITH_TIMEOUT(process.state(), rufus::IsoWriteState::AwaitingConfirmation, writeTimeoutMs);
        QVERIFY(process.request().isoEsp); QVERIFY(!process.request().mbr);
        QCOMPARE(process.request().clusterSize, selectedCluster);
        QCOMPARE(process.request().quickFormat, filesystem == FS_FAT32);
        QCOMPARE(helperStarts, 0);
        helperMode = QStringLiteral("hold-after-result"); approveIsoPlan();
        QTRY_VERIFY_WITH_TIMEOUT(process.outcome().started, writeTimeoutMs);
        QVERIFY(process.isActive()); QCOMPARE(completionBeeps, 0);
        helperProcess->closeWriteChannel();
        QTRY_COMPARE_WITH_TIMEOUT(process.state(), rufus::IsoWriteState::Complete, writeTimeoutMs);
        QCOMPARE(progress_->value(), MAX_PROGRESS); QCOMPARE(helperStarts, 1); QCOMPARE(completionBeeps, 1);
        QCOMPARE(readFile(targetPath_), expectedTarget_);
    }

    void extendedLabelCreation_data()
    {
        QTest::addColumn<int>("filesystem"); QTest::addColumn<bool>("missing");
        QTest::newRow("FAT16") << int(FS_FAT16) << false;
        QTest::newRow("FAT32") << int(FS_FAT32) << false;
        QTest::newRow("NTFS") << int(FS_NTFS) << false;
        QTest::newRow("exFAT") << int(FS_EXFAT) << false;
        QTest::newRow("missing-proof") << int(FS_FAT32) << true;
    }

    void extendedLabelCreation()
    {
        QFETCH(int, filesystem); QFETCH(bool, missing);
        selectFormat(filesystem);
        controller_->setExtendedLabel(true);
        const QString original = QString::fromUtf8("Măriuș 安装 💾 / long original label");
        controller_->setVolumeLabel(original);
        QTest::mouseClick(start_, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::AwaitingConfirmation, writeTimeoutMs);
        const auto frozen = controller_->isoWriter().request();
        QVERIFY(frozen.extendedLabel); QCOMPARE(frozen.displayLabel, original);
        QVERIFY(frozen.label != original);
        helperMode = missing ? QStringLiteral("missing-appearance") : QStringLiteral("hold-after-result");
        approveIsoPlan();
        if (missing)
        {
            QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::Failed, writeTimeoutMs);
            QCOMPARE(completionBeeps, 0); return;
        }
        QTRY_VERIFY_WITH_TIMEOUT(controller_->isoWriter().outcome().appearanceDone, writeTimeoutMs);
        QVERIFY(controller_->isoWriter().isActive()); QCOMPARE(completionBeeps, 0);
        controller_->setExtendedLabel(false); controller_->setVolumeLabel(QStringLiteral("changed"));
        QCOMPARE(controller_->isoWriter().request().displayLabel, original);
        QVERIFY(controller_->extendedLabelEnabled());
        helperProcess->closeWriteChannel();
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::Complete, writeTimeoutMs);
        QCOMPARE(completionBeeps, 1); QCOMPARE(progress_->value(), MAX_PROGRESS); QCOMPARE(helperStarts, 1);
        QCOMPARE(readFile(targetPath_), expectedTarget_);
    }

    void udfIndexingSelectionSnapshot_data()
    {
        QTest::addColumn<bool>("indexing");
        QTest::newRow("default-disable-hint") << false;
        QTest::newRow("saved-allow-indexing") << true;
    }

    void udfIndexingSelectionSnapshot()
    {
        QFETCH(bool, indexing);
        selectFormat(FS_UDF);
        if (controller_->fileIndexing() != indexing) controller_->toggleFileIndexing();
        controller_->toggleNtfsCompression();
        QVERIFY(controller_->ntfsCompression());
        for (int filesystem : {FS_NTFS, FS_FAT32, FS_UDF}) {
            controller_->setFileSystem(filesystem);
            QCOMPARE(controller_->selection().fileSystem, filesystem);
            QCOMPARE(controller_->fileIndexing(), indexing);
            QVERIFY(controller_->ntfsCompression());
        }
        QTest::mouseClick(start_, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::AwaitingConfirmation, writeTimeoutMs);
        const auto frozen = controller_->isoWriter().request();
        QCOMPARE(frozen.fileSystem, int(FS_UDF)); QVERIFY(frozen.nonBootable);
        QCOMPARE(frozen.fileIndexing, indexing); QVERIFY(!frozen.ntfsCompression);
        const auto packet = rufus::EncodePrepareRequest(frozen);
        QVERIFY(!packet.isEmpty()); QVERIFY(!packet.contains("ntfsOptions"));
        QCOMPARE(packet.contains("udfIndexing"), indexing);
        rufus::PrepareImageRequest decoded;
        QVERIFY(rufus::DecodePrepareRequest(packet, decoded));
        QCOMPARE(decoded.fileIndexing, indexing);
        controller_->toggleFileIndexing(); controller_->toggleNtfsCompression();
        QCOMPARE(controller_->fileIndexing(), indexing); QVERIFY(controller_->ntfsCompression());
        QCOMPARE(rufus::EncodePrepareRequest(controller_->isoWriter().request()), packet);
        QCOMPARE(helperStarts, 0);
        controller_->isoWriter().cancel();
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::Cancelled, writeTimeoutMs);
        QCOMPARE(readFile(targetPath_), expectedTarget_);
    }

    void sourceFreeCreationWaitsForExit_data()
    {
        QTest::addColumn<int>("filesystem");
        QTest::addColumn<bool>("windows");
        QTest::addColumn<bool>("large");
        QTest::addColumn<quint32>("cluster");
        QTest::addColumn<int>("finalizingPosition");
        QTest::addColumn<bool>("mbr");
        QTest::addColumn<int>("bootType");
        for (bool mbr : {false, true})
        {
            const QByteArray prefix = mbr ? "MBR-" : "GPT-";
            QTest::newRow((prefix + "fat16-job").constData()) << int(FS_FAT16) << false << false << quint32(2048) << 59577 << mbr << int(BT_NON_BOOTABLE);
            QTest::newRow((prefix + "fat32-job").constData()) << int(FS_FAT32) << false << false << quint32(512) << 59577 << mbr << int(BT_NON_BOOTABLE);
            QTest::newRow((prefix + "ntfs-job").constData()) << int(FS_NTFS) << false << false << quint32(4096) << 61894 << mbr << int(BT_NON_BOOTABLE);
            QTest::newRow((prefix + "ntfs-retained-windows").constData()) << int(FS_NTFS) << true << false << quint32(4096) << 61894 << mbr << int(BT_NON_BOOTABLE);
            QTest::newRow((prefix + "exfat-job").constData()) << int(FS_EXFAT) << false << false << quint32(128 * 1024) << 61439 << mbr << int(BT_NON_BOOTABLE);
            QTest::newRow((prefix + "fat16-retained-windows").constData()) << int(FS_FAT16) << true << false << quint32(2048) << 59577 << mbr << int(BT_NON_BOOTABLE);
            QTest::newRow((prefix + "fat32-retained-windows").constData()) << int(FS_FAT32) << true << false << quint32(512) << 59577 << mbr << int(BT_NON_BOOTABLE);
            QTest::newRow((prefix + "fat32-large").constData()) << int(FS_FAT32) << false << true << quint32(32768) << 64879 << mbr << int(BT_NON_BOOTABLE);
            for (bool windows : {false, true})
            {
                const QByteArray retained = windows ? "retained-windows-" : "";
                QTest::newRow((prefix + retained + "standalone-NTFS").constData())
                    << int(FS_NTFS) << windows << false << quint32(4096) << 62414 << mbr << int(BT_UEFI_NTFS);
                QTest::newRow((prefix + retained + "standalone-exFAT").constData())
                    << int(FS_EXFAT) << windows << false << quint32(128 * 1024) << 62085 << mbr << int(BT_UEFI_NTFS);
            }
        }
    }

    void sourceFreeCreationWaitsForExit()
    {
        QFETCH(int, filesystem);
        QFETCH(bool, mbr);
        QFETCH(int, bootType);
        QFETCH(bool, windows);
        QFETCH(bool, large);
        QFETCH(quint32, cluster);
        QFETCH(int, finalizingPosition);
        if (windows)
            selectIsoFixture(false, true);
        const quint64 capacity = large ? 64ULL * 1024 * 1024 * 1024 : 64ULL * 1024 * 1024;
        selectFormat(filesystem, capacity, bootType);
        controller_->setPartitionScheme(mbr ? rufus::PartitionScheme::Mbr : rufus::PartitionScheme::Gpt);
        const bool ntfs = filesystem == FS_NTFS;
        const QString label = ntfs ? QString::fromUtf8("Rufus ș😀.test") : QStringLiteral("usb.name*:long");
        const QString normalized = ntfs ? QString::fromUtf8("Rufus ș😀_test") : QStringLiteral("USB_NAMELON");
        controller_->setVolumeLabel(label);
        controller_->setClusterSize(cluster);
        controller_->toggleEnhancedInstall();
        QTest::mouseClick(start_, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::AwaitingConfirmation, writeTimeoutMs);
        const auto& request = controller_->isoWriter().request();
        QCOMPARE(request.label, normalized);
        QCOMPARE(request.clusterSize, cluster);
        QCOMPARE(request.fileSystem, filesystem);
        if (ntfs || filesystem == FS_EXFAT)
            QCOMPARE(request.volumeId, quint32(0));
        QCOMPARE(request.nonBootable, bootType == BT_NON_BOOTABLE);
        QCOMPARE(request.uefiNtfs, bootType == BT_UEFI_NTFS);
        QCOMPARE(request.mbr, mbr);
        QVERIFY(request.imagePath.isEmpty() && !request.syslinux && !request.enhancedInstall);
        QCOMPARE(request.windowsFlags, 0);
        auto* confirmation = window_->findChild<QMessageBox*>(QStringLiteral("isoWriteConfirmation"));
        const QString formatName = ntfs ? QStringLiteral("NTFS") : filesystem == FS_EXFAT ? QStringLiteral("exFAT") :
            QStringLiteral("FAT%1").arg(filesystem == FS_FAT16 ? 16 : 32);
        const QString bootName = bootType == BT_UEFI_NTFS ? QStringLiteral("UEFI:NTFS") : QStringLiteral("Non bootable");
        const QString scheme = mbr ? QStringLiteral("MBR") : QStringLiteral("GPT");
        QVERIFY(confirmation && confirmation->informativeText().contains(bootName + QStringLiteral(" — ") + scheme + QStringLiteral(", ") + formatName));
        QVERIFY(!confirmation->informativeText().contains(QStringLiteral("Image:")));
        QVERIFY(confirmation->informativeText().contains(normalized));
        QCOMPARE(helperStarts, 0);
        helperMode = QStringLiteral("hold-after-result");
        approveIsoPlan();
        QTRY_VERIFY_WITH_TIMEOUT(controller_->isoWriter().outcome().filesystemBytes > 0, writeTimeoutMs);
        QCOMPARE(controller_->isoWriter().outcome().copiedBytes, quint64(0));
        // Win32 non-bootable: NTFS has twelve FS steps (17/18), exFAT ten
        // (15/16), FAT five (10/11), with two finalization slots in each case.
        // Large FAT32 reserves an analog format range; first finalization is 99%.
        // Standalone UEFI:NTFS uses Rufus's three discrete boot-copy slots:
        // 20/21 NTFS or 18/19 exFAT at first finalization, with no ISO range.
        QCOMPARE(progress_->value(), finalizingPosition);
        QCOMPARE(progress_->format(), QStringLiteral("Finalizing, please wait..."));
        QCOMPARE(helperStarts, 1);
        QCOMPARE(completionBeeps, 0);
        QVERIFY(!window_->findChild<QComboBox*>(QStringLiteral("bootCombo"))->isEnabled());
        controller_->setPartitionScheme(mbr ? rufus::PartitionScheme::Gpt : rufus::PartitionScheme::Mbr); // Opposite choice is blocked while active.
        QCOMPARE((controller_->selection().partition == rufus::PartitionScheme::Mbr), mbr);
        QCOMPARE(controller_->isoWriter().request().mbr, mbr);
        controller_->setBootType(BT_IMAGE);
        QCOMPARE(controller_->selection().bootType, bootType);
        QVERIFY(!select_->isEnabled());
        QVERIFY(!window_->findChild<QComboBox*>(QStringLiteral("partitionCombo"))->isEnabled());
        helperProcess->closeWriteChannel();
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::Complete, writeTimeoutMs);
        QCOMPARE(progress_->format(), QStringLiteral("READY"));
        QCOMPARE(progress_->value(), MAX_PROGRESS);
        QCOMPARE(completionBeeps, 1);
        QCOMPARE(completionAlerts, 1);
        controller_->setDiscovery({}, {});
        QCOMPARE(progress_->format(), QStringLiteral("READY"));
        QCOMPARE(progress_->value(), MAX_PROGRESS);
        QCOMPARE(completionBeeps, 1);
        QCOMPARE(readFile(targetPath_), expectedTarget_);
    }

    void sfdSelection()
    {
        selectFormat(FS_EXFAT);
        auto* partitions = window_->findChild<QComboBox*>(QStringLiteral("partitionCombo"));
        auto* filesystems = window_->findChild<QComboBox*>(QStringLiteral("fileSystemCombo"));
        QCOMPARE(partitions->count(), 3);
        QCOMPARE(partitions->itemText(2), QStringLiteral("Super Floppy Disk"));
        partitions->setCurrentIndex(2);
        QVERIFY(QMetaObject::invokeMethod(partitions, "activated", Q_ARG(int, 2)));
        QCOMPARE(controller_->selection().partition, rufus::PartitionScheme::Sfd);
        QCOMPARE(filesystems->count(), 5);
        QVERIFY(filesystems->findData(FS_UDF) >= 0);
        QCOMPARE(filesystems->currentText(), QStringLiteral("exFAT"));
        QCOMPARE(controller_->selection().clusterSize, quint32(4096));
        QCOMPARE(window_->findChild<QComboBox*>(QStringLiteral("targetCombo"))->currentText(), QStringLiteral("BIOS or UEFI"));
        QVERIFY(start_->isEnabled());
        QVERIFY(!controller_->selection().syslinux);
        const QString screenshot = qEnvironmentVariable("RUFUS_TEST_SFD_SCREENSHOT");
        if (!screenshot.isEmpty())
            QVERIFY(window_->grab().save(screenshot));
        controller_->setFileSystem(FS_EXFAT);
        QCOMPARE(controller_->selection().fileSystem, int(FS_EXFAT));
        controller_->setPartitionScheme(rufus::PartitionScheme::Gpt);
        QCOMPARE(controller_->selection().fileSystem, int(FS_EXFAT));
        controller_->setPartitionScheme(rufus::PartitionScheme::Sfd);
        // Explicitly choosing FAT16 replaces the old exFAT preference.
        filesystems->setCurrentIndex(filesystems->findData(FS_FAT16));
        QVERIFY(QMetaObject::invokeMethod(filesystems, "activated", Q_ARG(int, filesystems->currentIndex())));
        controller_->setPartitionScheme(rufus::PartitionScheme::Gpt);
        QCOMPARE(controller_->selection().fileSystem, int(FS_FAT16));
        controller_->setPartitionScheme(rufus::PartitionScheme::Sfd);
        rufus::Device other = device_;
        other.id = QStringLiteral("sfd-other");
        other.size = 16 * 1024 * 1024;
        controller_->setDiscovery({device_, other}, {});
        controller_->selectDevice(other.id);
        QCOMPARE(partitions->count(), 3);
        QCOMPARE(controller_->selection().partition, rufus::PartitionScheme::Sfd);
        QCOMPARE(controller_->selection().fileSystems, quint32(FS_MASK(FS_FAT16) | FS_MASK(FS_NTFS) | FS_MASK(FS_UDF) | FS_MASK(FS_EXFAT)));
        QCOMPARE(controller_->selection().clusterSize, quint32(512));
        other.size = 2 * 1024 * 1024;
        controller_->setDiscovery({device_, other}, {});
        controller_->selectDevice(other.id);
        QCOMPARE(partitions->count(), 3); // UDF is the only eligible whole-device format.
        QCOMPARE(controller_->selection().partition, rufus::PartitionScheme::Sfd);
        QCOMPARE(controller_->selection().fileSystems, quint32(FS_MASK(FS_UDF)));
        QCOMPARE(controller_->selection().clusterSize, quint32(256));
        other.size = 512 * 1024;
        controller_->setDiscovery({device_, other}, {});
        controller_->selectDevice(other.id);
        QCOMPARE(partitions->count(), 2); // No qualified SFD filesystem fits.
        QCOMPARE(controller_->selection().partition, rufus::PartitionScheme::Mbr);
        controller_->setPartitionScheme(rufus::PartitionScheme::Sfd);
        QCOMPARE(controller_->selection().partition, rufus::PartitionScheme::Mbr);
        controller_->selectDevice(device_.id);
        QCOMPARE(controller_->selection().partition, rufus::PartitionScheme::Sfd);
        other.size = (quint64(1) << 41) + 512;
        controller_->setDiscovery({device_, other}, {});
        controller_->selectDevice(other.id);
        QCOMPARE(partitions->count(), 2); // GPT default; NTFS also permits an explicit SFD choice.
        QCOMPARE(controller_->selection().partition, rufus::PartitionScheme::Gpt);
        controller_->selectDevice(device_.id);
        QCOMPARE(controller_->selection().partition, rufus::PartitionScheme::Gpt);
        QCOMPARE(helperStarts, 0);
        QCOMPARE(readFile(targetPath_), expectedTarget_);
    }

    void sfdFat16CapacitySelection_data()
    {
        QTest::addColumn<quint64>("capacity");
        QTest::addColumn<bool>("hasFat16");
        QTest::addColumn<bool>("hasFat32");
        constexpr quint64 MiB = 1024 * 1024;
        QTest::newRow("small-whole-volume") << 3 * MiB << true << false;
        QTest::newRow("FAT16-only") << 16 * MiB << true << false;
        QTest::newRow("both") << 64 * MiB << true << true;
        QTest::newRow("near-limit") << 4095 * MiB << true << true;
        QTest::newRow("native-cluster-limit") << 4096 * MiB - 512 << false << true;
        QTest::newRow("Windows-limit") << 4096 * MiB << false << true;
    }

    void sfdFat16CapacitySelection()
    {
        QFETCH(quint64, capacity);
        QFETCH(bool, hasFat16);
        QFETCH(bool, hasFat32);
        selectFormat(FS_FAT16);
        controller_->setPartitionScheme(rufus::PartitionScheme::Sfd);
        rufus::Device other = device_;
        other.id = QStringLiteral("sfd-capacity");
        other.size = capacity; // Discovery only; no large allocation.
        controller_->setDiscovery({device_, other}, {});
        controller_->selectDevice(other.id);
        QCOMPARE(controller_->selection().partition, rufus::PartitionScheme::Sfd);
        auto* filesystems = window_->findChild<QComboBox*>(QStringLiteral("fileSystemCombo"));
        QCOMPARE(filesystems->findData(FS_FAT16) >= 0, hasFat16);
        QCOMPARE(filesystems->findData(FS_FAT32) >= 0, hasFat32);
        QCOMPARE(filesystems->findData(FS_NTFS) >= 0, capacity >= 16 * 1024 * 1024);
        QCOMPARE(filesystems->findData(FS_EXFAT) >= 0, capacity >= 16 * 1024 * 1024);
        QCOMPARE(controller_->selection().fileSystem, hasFat16 ? int(FS_FAT16) : int(FS_FAT32));
        QCOMPARE(filesystems->itemText(0), hasFat16 ? QStringLiteral("FAT16 (Default)") : QStringLiteral("FAT32 (Default)"));
        QVERIFY(start_->isEnabled());
        controller_->selectDevice(device_.id);
        QCOMPARE(controller_->selection().fileSystem, int(FS_FAT16)); // Keep explicit preference through fallback.
        QCOMPARE(helperStarts, 0);
        QCOMPARE(readFile(targetPath_), expectedTarget_);
    }

    void sfdNativeCapacitySelection_data()
    {
        QTest::addColumn<quint64>("capacity");
        QTest::addColumn<int>("defaultFilesystem");
        QTest::addColumn<int>("filesystem");
        QTest::addColumn<quint32>("cluster");
        constexpr quint64 MiB = 1024 * 1024;
        constexpr quint64 GiB = 1024 * MiB;
        constexpr quint64 TiB = 1024 * GiB;
        const struct { const char* name; quint64 size; int defaultFs; quint32 ntfsCluster; quint32 exfatCluster; } cases[] = {
            {"minimum", 16 * MiB, FS_FAT16, 4096, 4096},
            {"all-four", 64 * MiB, FS_FAT16, 4096, 4096},
            {"before-medium-cluster", 256 * MiB - 512, FS_FAT16, 4096, 4096},
            {"medium-cluster", 256 * MiB, FS_FAT16, 4096, 32768},
            {"before-large-cluster", 32 * GiB - 512, FS_FAT32, 4096, 32768},
            {"large-cluster", 32 * GiB, FS_FAT32, 4096, 131072},
            {"NTFS-default", 2 * TiB, FS_NTFS, 4096, 131072},
            {"above-MBR-limit", 2 * TiB + 512, FS_NTFS, 4096, 131072},
            {"last-supported-sector", 256 * TiB - 512, FS_NTFS, 65536, 131072}
        };
        for (const auto& test : cases)
        for (int filesystem : {FS_NTFS, FS_EXFAT})
        {
            const bool exfat = filesystem == FS_EXFAT;
            const QByteArray row = QByteArray(exfat ? "exFAT-" : "NTFS-") + test.name;
            QTest::newRow(row.constData()) << test.size << test.defaultFs << filesystem
                << (exfat ? test.exfatCluster : test.ntfsCluster);
        }
    }

    void sfdNativeCapacitySelection()
    {
        QFETCH(quint64, capacity);
        QFETCH(int, filesystem);
        QFETCH(quint32, cluster);
        QFETCH(int, defaultFilesystem);
        selectFormat(filesystem);
        controller_->setPartitionScheme(rufus::PartitionScheme::Sfd);
        rufus::Device other = device_;
        other.id = QStringLiteral("sfd-ntfs-capacity");
        other.size = capacity; // Discovery only; no corresponding disk allocation.
        controller_->setDiscovery({device_, other}, {});
        controller_->selectDevice(other.id);
        if (capacity > (quint64(1) << 41))
            QCOMPARE(controller_->selection().partition, rufus::PartitionScheme::Gpt);
        auto* partitions = window_->findChild<QComboBox*>(QStringLiteral("partitionCombo"));
        const int sfdIndex = partitions->findData(int(rufus::PartitionScheme::Sfd));
        QVERIFY(sfdIndex >= 0);
        partitions->setCurrentIndex(sfdIndex);
        QVERIFY(QMetaObject::invokeMethod(partitions, "activated", Q_ARG(int, sfdIndex)));
        QCOMPARE(controller_->selection().partition, rufus::PartitionScheme::Sfd);
        QCOMPARE(controller_->selection().fileSystem, filesystem);
        QCOMPARE(controller_->selection().defaultFileSystem, defaultFilesystem);
        auto* filesystems = window_->findChild<QComboBox*>(QStringLiteral("fileSystemCombo"));
        const QString name = filesystem == FS_EXFAT ? QStringLiteral("exFAT") : QStringLiteral("NTFS");
        QCOMPARE(filesystems->currentText(), defaultFilesystem == filesystem ? name + QStringLiteral(" (Default)") : name);
        QCOMPARE(controller_->selection().clusterSize, cluster);
        QCOMPARE(window_->findChild<QComboBox*>(QStringLiteral("clusterCombo"))->currentData().toUInt(), cluster);
        QCOMPARE(filesystems->findData(FS_EXFAT) >= 0, capacity >= 16 * 1024 * 1024);
        QVERIFY(start_->isEnabled());
        const QString screenshot = qEnvironmentVariable(filesystem == FS_EXFAT
            ? "RUFUS_TEST_EXFAT_SFD_SCREENSHOT" : "RUFUS_TEST_NTFS_SFD_SCREENSHOT");
        if (!screenshot.isEmpty() && capacity == 2 * (quint64(1) << 40))
            QVERIFY(window_->grab().save(screenshot));
        // A FAT16-only target temporarily replaces, but does not forget, the native filesystem.
        other.size = 3 * 1024 * 1024;
        controller_->setDiscovery({device_, other}, {});
        controller_->selectDevice(other.id);
        QCOMPARE(controller_->selection().partition, rufus::PartitionScheme::Sfd);
        QCOMPARE(controller_->selection().fileSystem, int(FS_FAT16));
        controller_->selectDevice(device_.id);
        QCOMPARE(controller_->selection().fileSystem, filesystem);
        QCOMPARE(helperStarts, 0);
        QCOMPARE(readFile(targetPath_), expectedTarget_);
    }

    void sfdRetainsImage_data()
    {
        QTest::addColumn<bool>("raw");
        QTest::newRow("ISO") << false;
        QTest::newRow("DD") << true;
    }

    void sfdRetainsImage()
    {
        QFETCH(bool, raw);
        if (raw)
            selectFixture();
        else
            selectIsoFixture();
        const QString image = controller_->selection().imagePath;
        selectFormat(FS_FAT32);
        controller_->setPartitionScheme(rufus::PartitionScheme::Sfd);
        controller_->setBootType(BT_IMAGE);
        QCOMPARE(controller_->selection().imagePath, image);
        QCOMPARE(controller_->selection().rawImage, raw);
        auto* partitions = window_->findChild<QComboBox*>(QStringLiteral("partitionCombo"));
        QCOMPARE(partitions->findData(int(rufus::PartitionScheme::Sfd)), -1);
        controller_->setPartitionScheme(rufus::PartitionScheme::Sfd);
        QVERIFY(controller_->selection().partition != rufus::PartitionScheme::Sfd);
        controller_->setBootType(BT_NON_BOOTABLE);
        QCOMPARE(controller_->selection().partition, rufus::PartitionScheme::Sfd);
        QCOMPARE(controller_->selection().fileSystem, int(FS_FAT32));
        QCOMPARE(helperStarts, 0);
    }

    void sfdGuiJob_data()
    {
        QTest::addColumn<QString>("mode");
        QTest::addColumn<int>("filesystem");
        QTest::addColumn<quint64>("capacity");
        QTest::addColumn<QString>("filesystemName");
        QTest::addColumn<int>("finalizingPosition");
        constexpr quint64 MiB = 1024 * 1024;
        const struct { int filesystem; const char* name; int finalizing; } formats[] = {
            {FS_FAT16, "FAT16", 59577}, {FS_FAT32, "FAT32", 59577},
            {FS_NTFS, "NTFS", 61894}, {FS_EXFAT, "exFAT", 61439}
        };
        for (const auto& format : formats)
        for (const char* mode : {"hold-after-result", "gpt-layout", "wrong-layout", "sync-error", "skip-finalizing", "hold-formatting", "hold-finalizing"})
        {
            const QByteArray name = QByteArray(format.name) + "-" + mode;
            QTest::newRow(name.constData()) << QString::fromLatin1(mode) << format.filesystem << 64 * MiB
                << QString::fromLatin1(format.name) << format.finalizing;
        }
        QTest::newRow("small-FAT16") << QStringLiteral("hold-after-result") << int(FS_FAT16) << 16 * MiB << QStringLiteral("FAT16") << 59577;
        QTest::newRow("large-FAT32") << QStringLiteral("hold-after-result") << int(FS_FAT32) << 64 * 1024 * MiB << QStringLiteral("FAT32") << 64879;
        QTest::newRow("NTFS-only") << QStringLiteral("hold-after-result") << int(FS_NTFS) << (quint64(1) << 41) << QStringLiteral("NTFS") << 61894;
        QTest::newRow("NTFS-above-MBR-limit") << QStringLiteral("hold-after-result") << int(FS_NTFS) << ((quint64(1) << 41) + 512) << QStringLiteral("NTFS") << 61894;
        QTest::newRow("minimum-exFAT") << QStringLiteral("hold-after-result") << int(FS_EXFAT) << 16 * MiB << QStringLiteral("exFAT") << 61439;
        QTest::newRow("large-exFAT") << QStringLiteral("hold-after-result") << int(FS_EXFAT) << 64 * 1024 * MiB << QStringLiteral("exFAT") << 61439;
    }

    void sfdGuiJob()
    {
        QFETCH(QString, mode);
        QFETCH(int, filesystem);
        QFETCH(quint64, capacity);
        QFETCH(QString, filesystemName);
        QFETCH(int, finalizingPosition);
        selectFormat(filesystem);
        controller_->setPartitionScheme(rufus::PartitionScheme::Sfd);
        if (capacity != device_.size)
            selectFormat(filesystem, capacity);
        controller_->setPartitionScheme(rufus::PartitionScheme::Sfd);
        const bool ntfs = filesystem == FS_NTFS;
        const QString label = ntfs ? QString::fromUtf8("sfd.ș😀") : QStringLiteral("sfd.test");
        const QString normalized = ntfs ? QString::fromUtf8("sfd_ș😀") : QStringLiteral("SFD_TEST");
        controller_->setVolumeLabel(label);
        helperMode = mode;
        QTest::mouseClick(start_, Qt::LeftButton);
        auto& writer = controller_->isoWriter();
        QTRY_COMPARE_WITH_TIMEOUT(writer.state(), rufus::IsoWriteState::AwaitingConfirmation, writeTimeoutMs);
        QVERIFY(writer.request().sfd && writer.request().nonBootable && !writer.request().mbr);
        QVERIFY(writer.request().imagePath.isEmpty() && !writer.request().syslinux);
        QCOMPARE(writer.request().fileSystem, filesystem);
        QCOMPARE(writer.request().label, normalized);
        if (ntfs || filesystem == FS_EXFAT)
            QCOMPARE(writer.request().volumeId, quint32(0));
        QCOMPARE(writer.check().partition.offset, quint64(0));
        QCOMPARE(writer.check().partition.size, capacity);
        auto* confirmation = window_->findChild<QMessageBox*>(QStringLiteral("isoWriteConfirmation"));
        QVERIFY(confirmation);
        QVERIFY(confirmation->informativeText().contains(QStringLiteral("Non bootable — Super Floppy Disk, ") + filesystemName));
        QVERIFY(confirmation->informativeText().contains(normalized));
        QVERIFY(!confirmation->informativeText().contains(QStringLiteral("Image:")));
        QCOMPARE(confirmation->defaultButton(), confirmation->button(QMessageBox::Cancel));
        QCOMPARE(helperStarts, 0);
        approveIsoPlan();
        const bool success = mode == QStringLiteral("hold-after-result");
        const bool cancelFormat = mode == QStringLiteral("hold-formatting");
        const bool cancelled = cancelFormat || mode == QStringLiteral("hold-finalizing");
        if (success)
        {
            QTRY_VERIFY_WITH_TIMEOUT(writer.outcome().filesystemBytes > 0, writeTimeoutMs);
            QCOMPARE(progress_->value(), finalizingPosition);
            QCOMPARE(progress_->format(), QStringLiteral("Finalizing, please wait..."));
            QCOMPARE(completionBeeps, 0);
            QVERIFY(!window_->findChild<QComboBox*>(QStringLiteral("partitionCombo"))->isEnabled());
            controller_->setPartitionScheme(rufus::PartitionScheme::Gpt);
            QCOMPARE(controller_->selection().partition, rufus::PartitionScheme::Sfd);
            helperProcess->closeWriteChannel();
        }
        if (cancelled)
        {
            const QString phase = cancelFormat ? QStringLiteral("Formatting %1...").arg(filesystemName) : QStringLiteral("Finalizing, please wait...");
            QTRY_COMPARE_WITH_TIMEOUT(progress_->format(), phase, writeTimeoutMs);
            writer.cancel();
        }
        const auto expected = success ? rufus::IsoWriteState::Complete
            : cancelled ? rufus::IsoWriteState::Cancelled : rufus::IsoWriteState::Failed;
        QTRY_COMPARE_WITH_TIMEOUT(writer.state(), expected, writeTimeoutMs);
        QCOMPARE(helperStarts, 1);
        QCOMPARE(completionBeeps, success ? 1 : 0);
        if (success)
        {
            QCOMPARE(progress_->value(), MAX_PROGRESS);
            QCOMPARE(progress_->format(), QStringLiteral("READY"));
            controller_->setDiscovery({}, {});
            QCOMPARE(progress_->value(), MAX_PROGRESS);
            QCOMPARE(progress_->format(), QStringLiteral("READY"));
            QCOMPARE(completionBeeps, 1);
        }
        else
            QVERIFY(progress_->value() < MAX_PROGRESS);
        QCOMPARE(readFile(targetPath_), expectedTarget_);
    }

    void sfdPreparation_data()
    {
        QTest::addColumn<QString>("mode");
        QTest::addColumn<int>("filesystem");
        for (int filesystem : {FS_FAT16, FS_FAT32, FS_NTFS, FS_EXFAT})
        for (const char* mode : {"hold-after-result", "gpt-layout", "wrong-layout", "sync-error", "skip-finalizing"})
        {
            const QByteArray name = QByteArray(filesystem == FS_EXFAT ? "exFAT-" : filesystem == FS_NTFS ? "NTFS-" : filesystem == FS_FAT16 ? "FAT16-" : "FAT32-") + mode;
            QTest::newRow(name.constData()) << QString::fromLatin1(mode) << filesystem;
        }
    }

    void sfdPreparation()
    {
        QFETCH(QString, mode);
        QFETCH(int, filesystem);
        selectFormat(FS_FAT32);
        rufus::IsoWriteController sfd;
        sfd.configure(*connection_, QCoreApplication::applicationDirPath() + QStringLiteral("/iso_helper_fixture"));
        rufus::PrepareImageRequest request;
        request.nonBootable = true;
        request.sfd = true;
        const quint32 cluster = filesystem == FS_EXFAT ? 128 * 1024 : filesystem == FS_NTFS ? 4096 : filesystem == FS_FAT16 ? 2048 : 512;
        sfd.prepare(request, device_, QStringLiteral("RUFUS SFD"), cluster, {}, filesystem);
        QTRY_COMPARE_WITH_TIMEOUT(sfd.state(), rufus::IsoWriteState::AwaitingConfirmation, writeTimeoutMs);
        QCOMPARE(sfd.check().partition.offset, quint64(0));
        QCOMPARE(sfd.check().partition.size, device_.size);
        QCOMPARE(helperStarts, 0);
        helperMode = mode;
        sfd.confirm();
        const bool success = mode == QStringLiteral("hold-after-result");
        if (success)
        {
            QTRY_VERIFY_WITH_TIMEOUT(sfd.outcome().filesystemBytes > 0, writeTimeoutMs);
            QCOMPARE(sfd.state(), rufus::IsoWriteState::Writing);
            QVERIFY(sfd.progressPosition() < MAX_PROGRESS);
            helperProcess->closeWriteChannel();
        }
        const auto expected = success ? rufus::IsoWriteState::Complete : rufus::IsoWriteState::Failed;
        QTRY_COMPARE_WITH_TIMEOUT(sfd.state(), expected, writeTimeoutMs);
        QCOMPARE(helperStarts, 1);
        QCOMPARE(readFile(targetPath_), expectedTarget_);
    }

    void sfdPreflightRefusal_data()
    {
        QTest::addColumn<QString>("mode");
        for (const char* mode : {"missing-tool", "missing-tool-after-confirmation", "mixed-layout", "unsupported-filesystem"})
            QTest::newRow(mode) << QString::fromLatin1(mode);
    }

    void sfdPreflightRefusal()
    {
        QFETCH(QString, mode);
        selectFormat(FS_FAT32);
        rufus::IsoWriteController sfd;
        sfd.configure(*connection_, QCoreApplication::applicationDirPath() + QStringLiteral("/iso_helper_fixture"));
        rufus::PrepareImageRequest request;
        request.nonBootable = true;
        request.sfd = true;
        request.mbr = mode == QStringLiteral("mixed-layout");
        missingFatTool = mode == QStringLiteral("missing-tool");
        const int filesystem = mode == QStringLiteral("unsupported-filesystem") ? FS_UNKNOWN : FS_FAT32;
        sfd.prepare(request, device_, QStringLiteral("RUFUS SFD"), 512, {}, filesystem);
        if (mode == QStringLiteral("missing-tool-after-confirmation"))
        {
            QTRY_COMPARE_WITH_TIMEOUT(sfd.state(), rufus::IsoWriteState::AwaitingConfirmation, writeTimeoutMs);
            missingFatTool = true;
            sfd.confirm();
        }
        QTRY_COMPARE_WITH_TIMEOUT(sfd.state(), rufus::IsoWriteState::Failed, writeTimeoutMs);
        QCOMPARE(helperStarts, 0);
        QCOMPARE(readFile(targetPath_), expectedTarget_);
    }

    void sourceFreeFailures_data()
    {
        QTest::addColumn<int>("filesystem");
        QTest::addColumn<QString>("mode");
        QTest::addColumn<bool>("mbr");
        QTest::addColumn<int>("bootType");
        for (int bootType : {BT_NON_BOOTABLE, BT_UEFI_NTFS})
        for (bool mbr : {false, true})
        for (int filesystem : {FS_FAT16, FS_FAT32, FS_NTFS, FS_EXFAT})
            for (const char* mode : {"deny", "sync-error", "cleanup-error", "wrong-layout", "gpt-layout", "short-format", "skip-finalizing"})
            {
                if (bootType == BT_UEFI_NTFS && (filesystem == FS_FAT16 || filesystem == FS_FAT32))
                    continue;
                if (bootType == BT_UEFI_NTFS && filesystem == FS_EXFAT && QByteArray(mode) == "gpt-layout")
                    continue; // Cluster rounding gives identical main-partition sizes here.
                if (!mbr && QByteArray(mode) == "gpt-layout")
                    continue;
                if ((filesystem == FS_FAT16 || filesystem == FS_FAT32) && QByteArray(mode) == "short-format")
                    continue; // Native FAT permits a small track-rounding tail.
                const QByteArray name = QByteArray(bootType == BT_UEFI_NTFS ? "standalone-" : "") + QByteArray(mbr ? "MBR-" : "GPT-") + (filesystem == FS_NTFS ? "ntfs-" : filesystem == FS_EXFAT ? "exfat-" : filesystem == FS_FAT16 ? "fat16-" : "fat32-") + QByteArray(mode);
                QTest::newRow(name.constData()) << filesystem << QString::fromLatin1(mode) << mbr << bootType;
            }
    }

    void sourceFreeFailures()
    {
        QFETCH(QString, mode);
        QFETCH(int, filesystem);
        QFETCH(bool, mbr);
        QFETCH(int, bootType);
        selectFormat(filesystem, 64 * 1024 * 1024, bootType);
        controller_->setPartitionScheme(mbr ? rufus::PartitionScheme::Mbr : rufus::PartitionScheme::Gpt);
        helperMode = mode;
        QTest::mouseClick(start_, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::AwaitingConfirmation, writeTimeoutMs);
        approveIsoPlan();
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::Failed, writeTimeoutMs);
        QCOMPARE(progress_->format(), QStringLiteral("FAILED"));
        QVERIFY(progress_->value() < MAX_PROGRESS);
        QCOMPARE(completionBeeps, 0);
        QCOMPARE(helperStarts, 1);
        QCOMPARE(readFile(targetPath_), expectedTarget_);
    }

    void sourceFreeCancel_data()
    {
        QTest::addColumn<int>("filesystem");
        QTest::addColumn<QString>("mode");
        QTest::addColumn<QString>("caption");
        QTest::addColumn<bool>("mbr");
        QTest::addColumn<int>("bootType");
        for (int bootType : {BT_NON_BOOTABLE, BT_UEFI_NTFS})
        for (bool mbr : {false, true})
        for (int filesystem : {FS_FAT16, FS_FAT32, FS_NTFS, FS_EXFAT})
        {
            if (bootType == BT_UEFI_NTFS && (filesystem == FS_FAT16 || filesystem == FS_FAT32))
                continue;
            const QByteArray name = QByteArray(bootType == BT_UEFI_NTFS ? "standalone-" : "") + QByteArray(mbr ? "MBR-" : "GPT-") + (filesystem == FS_NTFS ? "ntfs-" : filesystem == FS_EXFAT ? "exfat-" : filesystem == FS_FAT16 ? "fat16-" : "fat32-");
            const QString caption = filesystem == FS_NTFS ? QStringLiteral("Formatting NTFS...") :
                filesystem == FS_EXFAT ? QStringLiteral("Formatting exFAT...") :
                QStringLiteral("Formatting FAT%1...").arg(filesystem == FS_FAT16 ? 16 : 32);
            QTest::newRow((name + "formatter").constData()) << filesystem << QStringLiteral("hold-formatting") << caption << mbr << bootType;
            if (bootType == BT_UEFI_NTFS)
                QTest::newRow((name + "bootloader").constData()) << filesystem << QStringLiteral("hold-bootloader") << QStringLiteral("Installing UEFI:NTFS...") << mbr << bootType;
            QTest::newRow((name + "final-sync").constData()) << filesystem << QStringLiteral("hold-finalizing") << QStringLiteral("Finalizing, please wait...") << mbr << bootType;
        }
    }

    void sourceFreeCancel()
    {
        QFETCH(QString, mode);
        QFETCH(QString, caption);
        QFETCH(int, filesystem);
        QFETCH(bool, mbr);
        QFETCH(int, bootType);
        selectFormat(filesystem, 64 * 1024 * 1024, bootType);
        controller_->setPartitionScheme(mbr ? rufus::PartitionScheme::Mbr : rufus::PartitionScheme::Gpt);
        helperMode = mode;
        QTest::mouseClick(start_, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::AwaitingConfirmation, writeTimeoutMs);
        approveIsoPlan();
        QTRY_COMPARE_WITH_TIMEOUT(progress_->format(), caption, writeTimeoutMs);
        controller_->isoWriter().cancel();
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::Cancelled, writeTimeoutMs);
        QCOMPARE(helperStarts, 1);
        QCOMPARE(completionBeeps, 0);
        QVERIFY(progress_->value() < MAX_PROGRESS);
        QCOMPARE(readFile(targetPath_), expectedTarget_);
    }

    void sourceFreePreflightRefusal_data()
    {
        QTest::addColumn<int>("filesystem");
        QTest::addColumn<bool>("afterConfirmation");
        QTest::addColumn<bool>("mbr");
        QTest::addColumn<int>("bootType");
        for (int bootType : {BT_NON_BOOTABLE, BT_UEFI_NTFS})
        for (bool mbr : {false, true})
        for (int filesystem : {FS_FAT16, FS_FAT32, FS_NTFS, FS_EXFAT})
        {
            if (bootType == BT_UEFI_NTFS && (filesystem == FS_FAT16 || filesystem == FS_FAT32))
                continue;
            const QByteArray name = QByteArray(bootType == BT_UEFI_NTFS ? "standalone-" : "") + QByteArray(mbr ? "MBR-" : "GPT-") + (filesystem == FS_NTFS ? "ntfs-" : filesystem == FS_EXFAT ? "exfat-" : filesystem == FS_FAT16 ? "fat16-" : "fat32-");
            QTest::newRow((name + "before-confirmation").constData()) << filesystem << false << mbr << bootType;
            QTest::newRow((name + "before-authorization").constData()) << filesystem << true << mbr << bootType;
        }
    }

    void sourceFreePreflightRefusal()
    {
        QFETCH(bool, afterConfirmation);
        QFETCH(int, filesystem);
        QFETCH(bool, mbr);
        QFETCH(int, bootType);
        selectFormat(filesystem, 64 * 1024 * 1024, bootType);
        controller_->setPartitionScheme(mbr ? rufus::PartitionScheme::Mbr : rufus::PartitionScheme::Gpt);
        auto& missingTool = filesystem == FS_NTFS ? missingNtfsTool : filesystem == FS_EXFAT ? missingExfatTool : missingFatTool;
        missingTool = !afterConfirmation;
        QTest::mouseClick(start_, Qt::LeftButton);
        if (afterConfirmation)
        {
            QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::AwaitingConfirmation, writeTimeoutMs);
            missingTool = true;
            approveIsoPlan();
        }
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::Failed, writeTimeoutMs);
        QCOMPARE(helperStarts, 0);
        QCOMPARE(readFile(targetPath_), expectedTarget_);
    }

    void syslinuxFat16CapacityAndReload_data()
    {
        QTest::addColumn<quint64>("capacity");
        QTest::addColumn<int>("expected");
        QTest::addColumn<bool>("hasFat16");
        QTest::addColumn<bool>("hasNtfs");
        constexpr quint64 MiB = 1024 * 1024;
        QTest::newRow("FAT16-only") << 16 * MiB << int(FS_FAT16) << true << false;
        QTest::newRow("both") << 64 * MiB << int(FS_FAT32) << true << true;
        QTest::newRow("last-FAT16-sector") << 4096 * MiB - 512 << int(FS_FAT32) << true << true;
        QTest::newRow("FAT16-limit") << 4096 * MiB << int(FS_FAT32) << false << true;
    }

    void syslinuxFat16CapacityAndReload()
    {
        QFETCH(quint64, capacity);
        QFETCH(int, expected);
        QFETCH(bool, hasFat16);
        QFETCH(bool, hasNtfs);
        selectIsoFixture(true, false, "win11", "6.04 6.04-pre1");
        rufus::Device target = device_;
        target.id = QStringLiteral("syslinux-capacity-target");
        target.size = capacity;
        controller_->setDiscovery({device_, target}, {});
        controller_->selectDevice(target.id);
        QVERIFY(controller_->canSelectSyslinux());
        QVERIFY(controller_->selection().syslinux);
        QCOMPARE(controller_->selection().fileSystem, expected);
        auto* filesystems = window_->findChild<QComboBox*>(QStringLiteral("fileSystemCombo"));
        QCOMPARE(filesystems->findData(FS_FAT16) >= 0, hasFat16);
        QCOMPARE(filesystems->findData(FS_NTFS) >= 0, hasNtfs);
        // Keep the FAT16-only row automatic; other rows exercise manual selection/refusal.
        if (expected != FS_FAT16)
            controller_->setFileSystem(FS_FAT16);
        const int selected = hasFat16 ? FS_FAT16 : FS_FAT32;
        QCOMPARE(controller_->selection().fileSystem, selected);
        controller_->selectImage(sourcePath_);
        QTRY_VERIFY_WITH_TIMEOUT(!controller_->isAnalyzing(), writeTimeoutMs);
        QVERIFY(controller_->selection().syslinux);
        // Like Win32, a successful scan reapplies the image's FAT32 preference.
        QCOMPARE(controller_->selection().fileSystem, expected);
        QVERIFY(start_->isEnabled());
        controller_->selectDevice(device_.id);
        const bool manualFat16 = hasFat16 && expected != FS_FAT16;
        QCOMPARE(controller_->selection().fileSystem, manualFat16 ? int(FS_FAT16) : int(FS_FAT32));
        QCOMPARE(helperStarts, 0);
        QCOMPARE(stat(QStringLiteral("opens")), 0);
    }

    void syslinuxImageFailures_data()
    {
        QTest::addColumn<QString>("mode");
        QTest::addColumn<int>("filesystem");
        QTest::addColumn<bool>("efi");
        for (int fs : {FS_FAT16, FS_NTFS})
            for (bool efi : {false, true})
                for (const char* mode : {"deny", "cleanup-error", "sync-error", "wrong-layout", "short-copy"})
                {
                    const QByteArray name = QByteArray(fs == FS_NTFS ? "ntfs-" : "fat16-") + (efi ? "uefi-" : "bios-") + mode;
                    QTest::newRow(name.constData()) << QString::fromLatin1(mode) << fs << efi;
                }
    }

    void syslinuxImageFailures()
    {
        QFETCH(QString, mode);
        QFETCH(int, filesystem);
        QFETCH(bool, efi);
        selectIsoFixture(true, false, "win11", "6.04 6.04-pre1", efi);
        controller_->setFileSystem(filesystem);
        helperMode = mode;
        chooseImageMode();
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::AwaitingConfirmation, writeTimeoutMs);
        approveIsoPlan();
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::Failed, writeTimeoutMs);
        QCOMPARE(helperStarts, 1);
        QCOMPARE(progress_->format(), QStringLiteral("FAILED"));
        QVERIFY(progress_->value() < MAX_PROGRESS);
        QCOMPARE(completionBeeps, 0);
        QCOMPARE(completionAlerts, 0);
    }

    void syslinuxNtfsSelectionAndReload()
    {
        // Exercise Rufus's large-file policy without allocating a 4-GiB fixture.
        faultKind = LargeLinuxFile;
        selectIsoFixture(true, false, "win11", "6.04 6.04-pre1");
        QVERIFY(controller_->selection().syslinux);
        QCOMPARE(controller_->selection().fileSystems, quint32(FS_MASK(FS_NTFS)));
        QCOMPARE(controller_->selection().fileSystem, int(FS_NTFS));
        controller_->setFileSystem(FS_FAT32);
        QCOMPARE(controller_->selection().fileSystem, int(FS_NTFS));
        chooseImageMode();
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::AwaitingConfirmation, writeTimeoutMs);
        QCOMPARE(controller_->isoWriter().check().extraPartitions, quint8(XP_UEFI_NTFS));
        const auto& request = controller_->isoWriter().request();
        QVERIFY(request.syslinux && request.uefiNtfs);
        QCOMPARE(request.fileSystem, int(FS_NTFS));
        controller_->setFileSystem(FS_FAT32);
        controller_->setPartitionScheme(rufus::PartitionScheme::Gpt);
        QVERIFY(request.syslinux && request.uefiNtfs); // Frozen through confirmation.
        QCOMPARE(request.fileSystem, int(FS_NTFS));
        auto* confirmation = window_->findChild<QMessageBox*>(QStringLiteral("isoWriteConfirmation"));
        QVERIFY(confirmation->informativeText().contains(sourcePath_));
        QTest::keyClick(confirmation, Qt::Key_Escape);
        QCOMPARE(controller_->isoWriter().state(), rufus::IsoWriteState::Cancelled);
        QCOMPARE(progress_->format(), QStringLiteral("READY"));
        QCOMPARE(progress_->value(), 0);
        QCOMPARE(helperStarts, 0);
        faultKind = NoFault;
        controller_->selectImage(sourcePath_);
        QTRY_VERIFY_WITH_TIMEOUT(!controller_->isAnalyzing(), writeTimeoutMs);
        QCOMPARE(controller_->selection().fileSystem, int(FS_FAT32));
        controller_->setFileSystem(FS_NTFS);
        controller_->selectImage(sourcePath_);
        QTRY_VERIFY_WITH_TIMEOUT(!controller_->isAnalyzing(), writeTimeoutMs);
        QCOMPARE(controller_->selection().fileSystem, int(FS_FAT32));
    }

    void syslinuxNtfsPreparationStops_data()
    {
        QTest::addColumn<QString>("tool");
        QTest::addColumn<bool>("afterConfirmation");
        for (bool after : {false, true})
            for (const char* tool : {"installer", "formatter"})
            {
                const QByteArray name = QByteArray(after ? "after-" : "before-") + tool;
                QTest::newRow(name.constData()) << QString::fromLatin1(tool) << after;
            }
    }

    void syslinuxNtfsPreparationStops()
    {
        QFETCH(QString, tool);
        QFETCH(bool, afterConfirmation);
        selectIsoFixture(true, false, "win11", "6.04 6.04-pre1");
        controller_->setFileSystem(FS_NTFS);
        if (afterConfirmation)
        {
            chooseImageMode();
            QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::AwaitingConfirmation, writeTimeoutMs);
        }
        missingNtfsTool = tool == QStringLiteral("formatter");
        if (!missingNtfsTool) missingSyslinuxTool = QStringLiteral("/rufus-extlinux");
        if (afterConfirmation) approveIsoPlan();
        else chooseImageMode();
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::Failed, writeTimeoutMs);
        QCOMPARE(helperStarts, 0);
        QCOMPARE(stat(QStringLiteral("opens")), 0);
    }

    void syslinuxNtfsCancelStages_data()
    {
        QTest::addColumn<QString>("mode");
        QTest::addColumn<QString>("status");
        QTest::newRow("formatting") << QStringLiteral("hold-formatting") << QStringLiteral("Formatting NTFS...");
        QTest::newRow("uefi-loader") << QStringLiteral("hold-bootloader") << QStringLiteral("Installing UEFI:NTFS...");
        QTest::newRow("cleanup") << QStringLiteral("hold-finalizing") << QStringLiteral("Finalizing, please wait...");
    }

    void syslinuxNtfsCancelStages()
    {
        QFETCH(QString, mode);
        QFETCH(QString, status);
        selectIsoFixture(true, false, "win11", "6.04 6.04-pre1");
        controller_->setFileSystem(FS_NTFS);
        helperMode = mode;
        chooseImageMode();
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::AwaitingConfirmation, writeTimeoutMs);
        approveIsoPlan();
        QTRY_COMPARE_WITH_TIMEOUT(progress_->format(), status, writeTimeoutMs);
        QVERIFY(progress_->value() < MAX_PROGRESS);
        QVERIFY(controller_->isoWriter().isWriting());
        controller_->isoWriter().cancel();
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::Cancelled, writeTimeoutMs);
        QCOMPARE(helperStarts, 1);
        QCOMPARE(completionBeeps, 0);
        QCOMPARE(completionAlerts, 0);
    }

    void syslinuxFat16FormattingStatus()
    {
        selectIsoFixture(true, false, "win11", "6.04 6.04-pre1");
        controller_->setFileSystem(FS_FAT16);
        controller_->setClusterSize(4096);
        QCOMPARE(controller_->selection().clusterSize, quint32(4096));
        helperMode = QStringLiteral("hold-formatting");
        chooseImageMode();
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::AwaitingConfirmation, writeTimeoutMs);
        QCOMPARE(controller_->isoWriter().request().clusterSize, quint32(4096));
        auto* confirmation = window_->findChild<QMessageBox*>(QStringLiteral("isoWriteConfirmation"));
        QVERIFY(confirmation && confirmation->informativeText().contains(QStringLiteral("Cluster size: 4096 bytes")));
        approveIsoPlan();
        QTRY_COMPARE_WITH_TIMEOUT(progress_->format(), QStringLiteral("Formatting FAT16..."), writeTimeoutMs);
        controller_->isoWriter().cancel();
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::Cancelled, writeTimeoutMs);
        QCOMPARE(helperStarts, 1);
        QCOMPARE(completionBeeps, 0);
    }

    void syslinuxMissingTool_data()
    {
        QTest::addColumn<QString>("tool");
        QTest::addColumn<bool>("afterConfirmation");
        QTest::newRow("installer") << QStringLiteral("/rufus-syslinux") << false;
        QTest::newRow("mcopy") << QStringLiteral("/mcopy") << false;
        QTest::newRow("mattrib") << QStringLiteral("/mattrib") << false;
        QTest::newRow("removed-before-authorization") << QStringLiteral("/rufus-syslinux") << true;
    }

    void syslinuxMissingTool()
    {
        QFETCH(QString, tool);
        QFETCH(bool, afterConfirmation);
        selectIsoFixture(true, false, "win11", "6.04 6.04-pre1");
        if (!afterConfirmation)
            missingSyslinuxTool = tool;
        chooseImageMode();
        if (afterConfirmation)
        {
            QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::AwaitingConfirmation, writeTimeoutMs);
            missingSyslinuxTool = tool;
            approveIsoPlan();
        }
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::Failed, writeTimeoutMs);
        QVERIFY(controller_->isoWriter().statusText().contains(tool));
        QCOMPARE(helperStarts, 0);
        QCOMPARE(stat(QStringLiteral("opens")), 0);
    }

    void syslinuxUnknownWrongCompanionFailsBeforeAuthorization_data()
    {
        QTest::addColumn<int>("filesystem");
        QTest::newRow("FAT32") << int(FS_FAT32);
        QTest::newRow("NTFS") << int(FS_NTFS);
    }

    void syslinuxUnknownWrongCompanionFailsBeforeAuthorization()
    {
        QFETCH(int, filesystem);
        selectIsoFixture(true, false, "win11", "unknown");
        // Change the source before SELECT, not while a retained request is active.
        QByteArray damaged = sourceBytes_;
        const QByteArray module = syslinuxModule();
        const qsizetype position = damaged.indexOf(module);
        QVERIFY(position >= 0);
        damaged[position + module.size() - 1] ^= 1;
        writeFile(sourcePath_, damaged);
        sourceBytes_ = damaged;
        controller_->selectImage(sourcePath_);
        QTRY_VERIFY_WITH_TIMEOUT(!controller_->isAnalyzing(), writeTimeoutMs);
        controller_->setFileSystem(filesystem);
        chooseImageMode();
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::Failed, writeTimeoutMs);
        QVERIFY(controller_->isoWriter().statusText().contains(QStringLiteral("does not match")));
        QCOMPARE(controller_->isoWriter().check().analysis.report.sl_detected_version, quint16(0));
        QCOMPARE(helperStarts, 0);
    }

    void syslinuxDifferentVersionOffersMbr()
    {
        selectIsoFixture(true, false, "win11", "6.04 6.04-pre3");
        QVERIFY(controller_->canSelectSyslinux());
        auto* partitions = window_->findChild<QComboBox*>(QStringLiteral("partitionCombo"));
        QCOMPARE(partitions->count(), 2);
        QCOMPARE(partitions->currentText(), QStringLiteral("MBR"));
        QVERIFY(partitions->findText(QStringLiteral("GPT")) >= 0);
        controller_->setPartitionScheme(rufus::PartitionScheme::Mbr);
        QVERIFY(controller_->selection().syslinux);
        chooseImageMode();
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::SelectingGrubCore, writeTimeoutMs);
        QVERIFY(controller_->isoWriter().isSelectingSyslinux());
        auto* download = window_->findChild<QMessageBox*>(QStringLiteral("grubDownloadDialog"));
        QVERIFY(download && download->isVisible());
        QTest::mouseClick(download->button(QMessageBox::No),Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::Failed, writeTimeoutMs);
        QCOMPARE(helperStarts,0);
        QCOMPARE(stat(QStringLiteral("opens")),0);
    }

    void syslinuxBiosOnlyImageUsesMbr_data()
    {
        QTest::addColumn<int>("filesystem");
        QTest::newRow("FAT16") << int(FS_FAT16);
        QTest::newRow("FAT32") << int(FS_FAT32);
        QTest::newRow("NTFS") << int(FS_NTFS);
    }

    void syslinuxBiosOnlyImageUsesMbr()
    {
        QFETCH(int, filesystem);
        selectIsoFixture(false, false, "win11", "6.04 6.04-pre1", false);
        controller_->setFileSystem(filesystem);
        QVERIFY(controller_->selection().syslinux);
        auto* partitions = window_->findChild<QComboBox*>(QStringLiteral("partitionCombo"));
        QCOMPARE(partitions->count(), 1);
        QCOMPARE(partitions->currentText(), QStringLiteral("MBR"));
        QCOMPARE(window_->findChild<QComboBox*>(QStringLiteral("targetCombo"))->currentText(), QStringLiteral("BIOS (or UEFI-CSM)"));
        controller_->setPartitionScheme(rufus::PartitionScheme::Gpt);
        QVERIFY(controller_->selection().syslinux);
        startIsoPlan(); // No hybrid marker: no ISO/DD dialog.
        QVERIFY(!controller_->isoWriter().request().uefiNtfs);
        QCOMPARE(controller_->isoWriter().check().extraPartitions, quint8(0));
        auto* confirmation = window_->findChild<QMessageBox*>(QStringLiteral("isoWriteConfirmation"));
        QVERIFY(confirmation && confirmation->informativeText().contains(QStringLiteral("ISO Image — MBR, %1, BIOS\n").arg(filesystem == FS_NTFS ? QStringLiteral("NTFS") : filesystem == FS_FAT16 ? QStringLiteral("FAT16") : QStringLiteral("FAT32"))));
        approveIsoPlan();
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::Complete, writeTimeoutMs);
        QCOMPARE(helperStarts, 1);
    }

    void syslinuxDdDoesNotNeedNativeTools_data()
    {
        QTest::addColumn<int>("filesystem");
        QTest::newRow("FAT16") << int(FS_FAT16);
        QTest::newRow("FAT32") << int(FS_FAT32);
        QTest::newRow("NTFS") << int(FS_NTFS);
    }

    void syslinuxDdDoesNotNeedNativeTools()
    {
        QFETCH(int, filesystem);
        selectIsoFixture(true, false, "win11", "6.04 6.04-pre1");
        controller_->setFileSystem(filesystem);
        missingSyslinuxTool = filesystem == FS_NTFS ? QStringLiteral("/rufus-extlinux") : QStringLiteral("/rufus-syslinux");
        chooseImageMode(IMAGE_WRITE_DD);
        QTRY_COMPARE_WITH_TIMEOUT(state(), State::AwaitingConfirmation, writeTimeoutMs);
        approvePlan();
        QTRY_COMPARE_WITH_TIMEOUT(state(), State::Complete, writeTimeoutMs);
        QCOMPARE(helperStarts, 1);
        QCOMPARE(syslinuxToolChecks.load(), 0);
        QCOMPARE(readFile(targetPath_), expectedTarget_);
    }

    void isoDdWithoutPreparationHelper_data()
    {
        QTest::addColumn<bool>("ddOnly");
        QTest::addColumn<bool>("nonExecutable");
        for (bool ddOnly : {false, true})
            for (bool nonExecutable : {false, true})
            {
                const QByteArray name = QByteArray(ddOnly ? "DD-only-" : "hybrid-")
                    + (nonExecutable ? "not-executable" : "missing");
                QTest::newRow(name.constData()) << ddOnly << nonExecutable;
            }
    }

    void isoDdWithoutPreparationHelper()
    {
        QFETCH(bool, ddOnly); QFETCH(bool, nonExecutable);
        selectIsoFixture(true, false, "win11", nullptr, true, ddOnly);
        const QString helper = directory_.filePath(QStringLiteral("unavailable-helper"));
        if (nonExecutable) writeFile(helper, QByteArray("not executable"));
        controller_->isoWriter().configure(*connection_, helper);
        writer().setHelper(helper);
        QVERIFY(!controller_->isoWriter().isAvailable(IMAGE_WRITE_DD));
        QVERIFY(!writer().isAvailable());
        QVERIFY(!start_->isEnabled());
        QCOMPARE(helperStarts, 0);
        QCOMPARE(stat(QStringLiteral("opens")), 0);
        QVERIFY(QFile::remove(helper) || !nonExecutable);
    }

    void isoDdWithoutHelperStopsBeforeWriting_data()
    {
        QTest::addColumn<QString>("action");
        for (const char* action : {"cancel-mode", "cancel-confirmation", "deny"})
            QTest::newRow(action) << QString::fromLatin1(action);
    }

    void isoDdWithoutHelperStopsBeforeWriting()
    {
        QFETCH(QString, action);
        selectIsoFixture(true);
        if (action == QStringLiteral("cancel-mode"))
        {
            QTest::mouseClick(start_, Qt::LeftButton);
            auto* dialog = window_->findChild<rufus::ImageModeDialog*>();
            QVERIFY(dialog && dialog->isVisible());
            QTest::mouseClick(dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Cancel), Qt::LeftButton);
            QTRY_VERIFY_WITH_TIMEOUT(!controller_->isWritingActive(), uiTimeoutMs);
        }
        else
        {
            chooseImageMode(IMAGE_WRITE_DD, 3);
            QTRY_COMPARE_WITH_TIMEOUT(state(), State::AwaitingConfirmation, writeTimeoutMs);
            if (action == QStringLiteral("deny"))
            {
                helperMode = QStringLiteral("deny");
                approvePlan();
                QTRY_COMPARE_WITH_TIMEOUT(state(), State::Failed, writeTimeoutMs);
            }
            else
            {
                auto* dialog = window_->findChild<QMessageBox*>(QStringLiteral("writeConfirmation"));
                QTest::mouseClick(dialog->button(QMessageBox::Cancel), Qt::LeftButton);
                QTRY_VERIFY_WITH_TIMEOUT(!controller_->isWritingActive(), uiTimeoutMs);
            }
        }
        QCOMPARE(stat(QStringLiteral("opens")), 0);
        QCOMPARE(helperStarts, action == QStringLiteral("deny") ? 1 : 0);
        QCOMPARE(actualWritten.load(), uint64_t(0));
        QCOMPARE(completionBeeps, 0);
        QCOMPARE(readFile(targetPath_), expectedTarget_);
    }

    void isoHelperDisappears_data()
    {
        QTest::addColumn<bool>("afterConfirmation");
        QTest::newRow("before-ISO-mode") << false;
        QTest::newRow("before-authorization") << true;
    }

    void isoHelperDisappears()
    {
        QFETCH(bool, afterConfirmation);
        selectIsoFixture(true);
        const QString helper = directory_.filePath(QStringLiteral("temporary-helper"));
        writeFile(helper, QByteArray("#!/bin/sh\nexit 1\n"));
        QVERIFY(QFile::setPermissions(helper, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        controller_->isoWriter().configure(*connection_, helper);
        if (afterConfirmation)
        {
            chooseImageMode(IMAGE_WRITE_ISO, 3);
            QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::AwaitingConfirmation, writeTimeoutMs);
            QVERIFY(QFile::remove(helper));
            approveIsoPlan();
        }
        else
        {
            QTest::mouseClick(start_, Qt::LeftButton);
            auto* dialog = window_->findChild<rufus::ImageModeDialog*>();
            QVERIFY(dialog && dialog->isVisible());
            QVERIFY(QFile::remove(helper));
            // Keep the recommended ISO radio selected; never silently switch to DD.
            QTest::mouseClick(dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok), Qt::LeftButton);
        }
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::Failed, writeTimeoutMs);
        QVERIFY(controller_->isoWriter().statusText().contains(QStringLiteral("helper is unavailable")));
        QCOMPARE(helperStarts, 0);
        QCOMPARE(stat(QStringLiteral("opens")), 0);
        QCOMPARE(readFile(targetPath_), expectedTarget_);
    }

    void missingHelperKeepsIsoAndFormatGated()
    {
        selectIsoFixture(); // No DD choice.
        controller_->isoWriter().configure(*connection_, directory_.filePath(QStringLiteral("absent-helper")));
        QVERIFY(!start_->isEnabled());
        controller_->setBootType(BT_NON_BOOTABLE);
        QVERIFY(!start_->isEnabled());
        QCOMPARE(helperStarts, 0);
        QCOMPARE(stat(QStringLiteral("opens")), 0);
        QCOMPARE(readFile(targetPath_), expectedTarget_);
    }

    void syslinuxCancellationUsesRufusProgress_data()
    {
        QTest::addColumn<int>("filesystem");
        QTest::newRow("FAT16") << int(FS_FAT16);
        QTest::newRow("FAT32") << int(FS_FAT32);
        QTest::newRow("NTFS") << int(FS_NTFS);
    }

    void syslinuxCancellationUsesRufusProgress()
    {
        QFETCH(int, filesystem);
        selectIsoFixture(true, false, "win11", "6.04 6.04-pre1");
        controller_->setFileSystem(filesystem);
        chooseImageMode();
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::AwaitingConfirmation, writeTimeoutMs);
        helperMode = QStringLiteral("hold");
        approveIsoPlan();
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().statusText(), QStringLiteral("Copying ISO files: 50.0%"), writeTimeoutMs);
        QCOMPARE(progress_->value(), filesystem == FS_NTFS ? 37027 : 35061); // Win32 NTFS: 16% + half of 81%; FAT: 9% + half of 89%.
        controller_->isoWriter().cancel();
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::Cancelled, writeTimeoutMs);
        QCOMPARE(helperStarts, 1);
        QCOMPARE(completionBeeps, 0);
    }

    void realAlpineSyslinuxSelection()
    {
        const QString path = qEnvironmentVariable("RUFUS_TEST_ALPINE_ISO");
        if (path.isEmpty())
            QSKIP("Set RUFUS_TEST_ALPINE_ISO for the retained CP12l input.");
        selectIsoFixture();
        // No writes: retain the small regular-file target, expose a larger
        // synthetic block capacity to the existing UDisks/ioctl fixture.
        nativeCapacity = 512 * 1024 * 1024;
        objects_[disk][blockInterface][QStringLiteral("Size")] = QVariant::fromValue(quint64(nativeCapacity));
        setObjects();
        const QString owner = device_.serviceOwner;
        QVERIFY(rufus::DescribeUDisksDevice(objects_, disk.path(), device_));
        device_.id = QStringLiteral("alpine-device");
        device_.serviceOwner = owner;
        controller_->setDiscovery({device_}, {});
        controller_->selectDevice(device_.id);
        controller_->selectImage(path);
        QTRY_VERIFY_WITH_TIMEOUT(!controller_->isAnalyzing(), writeTimeoutMs);
        QVERIFY(controller_->selection().syslinux);
        QCOMPARE(controller_->selection().imageAnalysis.report.sl_detected_version, quint16(0x0604));
        QCOMPARE(window_->findChild<QComboBox*>(QStringLiteral("targetCombo"))->currentText(), QStringLiteral("BIOS or UEFI"));
        auto* status = window_->findChild<QLabel*>(QStringLiteral("statusLabel"));
        QCOMPARE(status->text(), QStringLiteral("Using image: %1").arg(QFileInfo(path).fileName()));
        QVERIFY(status->toolTip().contains(QStringLiteral("Pending checks")));
        auto* elapsed = window_->findChild<QLabel*>(QStringLiteral("elapsedLabel"));
        QTRY_VERIFY(elapsed->width() >= elapsed->fontMetrics().horizontalAdvance(elapsed->text()));
        QTRY_VERIFY(elapsed->geometry().right() < elapsed->parentWidget()->width());
        QVERIFY(status->geometry().right() < elapsed->geometry().left());
        QCOMPARE(progress_->format(), QStringLiteral("READY"));
        QCOMPARE(progress_->value(), 0);
        const QString screenshot = qEnvironmentVariable("RUFUS_TEST_ALPINE_SCREENSHOT");
        if (!screenshot.isEmpty())
            QVERIFY(window_->grab().save(screenshot));
        chooseImageMode();
        QTRY_VERIFY_WITH_TIMEOUT(controller_->isoWriter().state() != rufus::IsoWriteState::Checking, writeTimeoutMs);
        QVERIFY2(controller_->isoWriter().state() == rufus::IsoWriteState::AwaitingConfirmation,
            qPrintable(controller_->isoWriter().statusText()));
        QCOMPARE(controller_->isoWriter().check().tree.bytes_written, quint64(283426236));
        QCOMPARE(controller_->isoWriter().request().label, QStringLiteral("ALPINE-STD"));
        QCOMPARE(controller_->isoWriter().check().partition.offset, quint64(1048576));
        QCOMPARE(controller_->isoWriter().check().partition.size, quint64(535801856));
        QCOMPARE(helperStarts, 0);
        controller_->isoWriter().cancel();
    }

    void isoDdRejectsChangesDuringModeSelection_data()
    {
        QTest::addColumn<bool>("missingHelper");
        QTest::newRow("helper-installed") << false;
        QTest::newRow("helper-missing") << true;
    }

    void isoDdRejectsChangesDuringModeSelection()
    {
        QFETCH(bool, missingHelper);
        selectIsoFixture(true);
        if (missingHelper)
        {
            writer().setHelper({});
            QVERIFY(!start_->isEnabled());
            return;
        }
        QTest::mouseClick(start_, Qt::LeftButton);
        auto* dialog = window_->findChild<rufus::ImageModeDialog*>();
        QVERIFY(dialog && dialog->isVisible());
        writeFile(sourcePath_, sourceBytes_);
        dialog->findChildren<QRadioButton*>().last()->setChecked(true);
        QTest::mouseClick(dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok), Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(state(), State::Failed, fixtureTimeoutMs);
        QCOMPARE(helperStarts, 0);
        QCOMPARE(stat(QStringLiteral("opens")), 0);
        QCOMPARE(readFile(targetPath_), expectedTarget_);
    }

private:
    QString syslinuxModulePath()
    {
        return qEnvironmentVariable("RUFUS_TEST_SYSLINUX_MODULE", QStringLiteral(RUFUS_SYSLINUX_MODULE));
    }

    QByteArray syslinuxModule()
    {
        if (QFileInfo::exists(syslinuxModulePath()))
            return readFile(syslinuxModulePath());
        // A known source preserves its companion; no payload download is needed
        // for these protocol tests. Exact unknown-version success needs upstream.
        return QByteArray(122656, '\0');
    }

    void chooseImageMode(IMAGE_WRITE_MODE mode = IMAGE_WRITE_ISO, int expectedChoices = 2)
    {
        QTest::mouseClick(start_, Qt::LeftButton);
        auto* dialog = window_->findChild<rufus::ImageModeDialog*>();
        QVERIFY(dialog && dialog->isVisible());
        const auto choices = dialog->findChildren<QRadioButton*>();
        QCOMPARE(choices.size(), expectedChoices);
        QVERIFY(choices.first()->isChecked() && choices.first()->isEnabled());
        QVERIFY(choices.last()->isEnabled());
        if (expectedChoices == 3)
            QVERIFY(choices[1]->isEnabled());
        if (mode == IMAGE_WRITE_DD)
            choices.last()->setChecked(true);
        QTest::mouseClick(dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok), Qt::LeftButton);
    }

    void activateMainWindow()
    {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        window_->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(window_.get()));
        QTRY_COMPARE(QApplication::activeWindow(), window_.get());
    }

    void startUncustomizedWindowsPlan()
    {
        QTest::mouseClick(start_, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(controller_->isoWriter().state() == rufus::IsoWriteState::ChoosingWindowsOptions
            || controller_->isoWriter().state() == rufus::IsoWriteState::AwaitingConfirmation, writeTimeoutMs);
        if (controller_->isoWriter().state() == rufus::IsoWriteState::ChoosingWindowsOptions)
        {
            auto* dialog = window_->findChild<rufus::WindowsUserDialog*>();
            QVERIFY(dialog && dialog->isVisible());
            for (auto* box : dialog->findChildren<QCheckBox*>())
                box->setChecked(false);
            dialog->accept();
        }
        QCOMPARE(controller_->isoWriter().state(), rufus::IsoWriteState::AwaitingConfirmation);
    }

    void startSilentPlan()
    {
        selectIsoFixture(false, true);
        QTest::mouseClick(start_, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::ChoosingWindowsOptions, writeTimeoutMs);
        auto* dialog = window_->findChild<rufus::WindowsUserDialog*>();
        QVERIFY(dialog);
        for (auto* choice : dialog->findChildren<QCheckBox*>())
        {
            const int flag = choice->property("unattendFlag").toInt();
            choice->setChecked(flag == UNATTEND_SET_USER || flag == UNATTEND_NO_DATA_COLLECTION);
        }
        dialog->findChild<QLineEdit*>()->setText(QStringLiteral("  Măriuș.Test  "));
        QTest::mouseClick(dialog->findChild<QPushButton*>(QStringLiteral("windowsRegionButton")), Qt::LeftButton);
        auto* editor = dialog->findChild<rufus::WindowsRegionDialog*>();
        QVERIFY(editor && editor->isVisible());
        editor->findChild<QComboBox*>(QStringLiteral("windowsDisplayLanguage"))->setEditText(QStringLiteral("en-US"));
        editor->findChild<QComboBox*>(QStringLiteral("windowsUserLocale"))->setEditText(QStringLiteral("en-GB"));
        editor->findChild<QComboBox*>(QStringLiteral("windowsSystemLocale"))->setEditText(QStringLiteral("ro-RO"));
        editor->findChild<QComboBox*>(QStringLiteral("windowsInputLocale"))->setEditText(QStringLiteral("0409:00020409;ro-RO"));
        auto* timezone = editor->findChild<QComboBox*>(QStringLiteral("windowsTimezone"));
        timezone->setCurrentIndex(timezone->findData(QStringLiteral("GTB Standard Time")));
        editor->accept();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        auto* silent = dialog->findChild<QCheckBox*>(QStringLiteral("windowsOption%1").arg(UNATTEND_SILENT_INSTALL));
        QVERIFY(silent->isEnabled() && silent->isVisible());
        silent->setChecked(true);
        dialog->findChild<QComboBox*>(QStringLiteral("windowsEdition"))->setCurrentIndex(0);
    }

    void approveSilentWarning()
    {
        auto* dialog = window_->findChild<rufus::WindowsUserDialog*>();
        QVERIFY(dialog);
        dialog->accept();
        auto* warning = dialog->findChild<rufus::WindowsSilentDialog*>();
        QVERIFY(warning && warning->isVisible());
        QCOMPARE(helperStarts, 0);
        for (auto* box : warning->findChildren<QCheckBox*>())
            box->setChecked(true);
        QTest::mouseClick(warning->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok), Qt::LeftButton);
    }

    void selectWinPeFixture(const QString& family, bool udf, bool hybrid = false, const QString& problem = {},
        bool syslinuxData = false, bool grubData = false, const char* grubVersion = "2.12", bool grub4dosOnly = false)
    {
        selectIsoFixture();
        const QString tree = directory_.filePath(QStringLiteral("iso-tree"));
        QVERIFY(QDir(tree + QStringLiteral("/EFI")).removeRecursively());
        helperCopiedBytes = sizeof("payload") - 1; // The original fixture's remaining file.
        const bool partialMinint = family.endsWith(QStringLiteral("-partial-minint"));
        const bool minintOptions = family.endsWith(QStringLiteral("-minint-options"));
        const QString setupFamily = partialMinint || minintOptions ? family.section(QLatin1Char('-'), 0, 0) : family;
        const QString loader = tree + (setupFamily == QStringLiteral("minint") ? QStringLiteral("/minint") : QStringLiteral("/i386"));
        const QString setup = tree + QLatin1Char('/') + setupFamily;
        QVERIFY(QDir().mkpath(loader) && QDir().mkpath(setup));
        QByteArray bytes(problem == QStringLiteral("short-loader") ? 7 : 9000, 'x');
        const bool prepatched = problem == QStringLiteral("root-loader-prepatched");
        if (prepatched || partialMinint)
        {
            constexpr int pathOffset = 128, driveOffset = 256, crcOffset = 0x2060;
            const char path[] = "\\minint\\txtsetup.sif", drive[] = "rdisk(0)";
            bytes.replace(pathOffset, sizeof(path), QByteArray(path, sizeof(path)));
            bytes.replace(driveOffset, sizeof(drive), QByteArray(drive, sizeof(drive)));
            bytes.replace(crcOffset, 2, QByteArray::fromHex("7403"));
        }
        const QByteArray detector("detector");
        const bool usesMinint = setupFamily == QStringLiteral("minint") || partialMinint || minintOptions;
        const QByteArray sif = usesMinint
            ? QByteArray("[SetupData]\r\nOsLoadOptions=/minint\r\n") : QByteArray("[SetupData]\r\nOsLoadOptions=/fastdetect\r\n");
        writeFile(loader + QStringLiteral("/setupldr.bin"), bytes);
        writeFile(loader + QStringLiteral("/ntdetect.com"), detector);
        writeFile(setup + QStringLiteral("/txtsetup.sif"), sif);
        if (partialMinint)
        {
            QVERIFY(QDir().mkdir(tree + QStringLiteral("/MiNiNt")));
            const QByteArray unused("unselected minint SIF");
            writeFile(tree + QStringLiteral("/MiNiNt/TxTsEtUp.SiF"), unused);
            helperCopiedBytes += quint64(unused.size());
        }
        if (problem.startsWith(QStringLiteral("root-loader-")))
        {
            QByteArray root = bytes;
            if (prepatched)
                QVERIFY(PatchWinPeLoader(root.data(), uint32_t(root.size()), family == QStringLiteral("amd64") ? 1 : 0, false));
            if (problem == QStringLiteral("root-loader-different")) root[root.size() - 1] = 'y';
            if (problem == QStringLiteral("root-loader-empty")) root.clear();
            const QString destination = tree + QStringLiteral("/BoOtMgR");
            if (problem == QStringLiteral("root-loader-directory")) QVERIFY(QDir().mkdir(destination));
            else writeFile(destination, root);
            if (problem == QStringLiteral("root-loader-duplicate")) writeFile(tree + QStringLiteral("/BOOTMGR"), root);
            helperCopiedBytes += quint64(root.size());
        }
        const bool retainedDetector = problem == QStringLiteral("root-loader-identical") || prepatched;
        if (retainedDetector || problem.startsWith(QStringLiteral("root-detector-")))
        {
            QByteArray root("different existing root detector");
            const QString destination = tree + QStringLiteral("/NtDetect.Com");
            if (problem == QStringLiteral("root-detector-empty")) root.clear();
            if (problem == QStringLiteral("root-detector-directory")) QVERIFY(QDir().mkdir(destination));
            else writeFile(destination, root);
            if (problem == QStringLiteral("root-detector-duplicate"))
                writeFile(tree + QStringLiteral("/NTDETECT.COM"), root);
            helperCopiedBytes += quint64(root.size());
        }
        if (problem == QStringLiteral("root-sif-missing-section") || problem == QStringLiteral("root-sif-no-bom"))
        {
            const QByteArray rootSif = problem == QStringLiteral("root-sif-missing-section")
                ? QByteArray("[Other]\r\nKeep=yes\r\n") : QString::fromUtf8("[SetupData]\r\n;é\r\n").toUtf8();
            writeFile(tree + QStringLiteral("/TxTsEtUp.SiF"), rootSif);
            helperCopiedBytes += quint64(rootSif.size());
        }
        if (problem == QStringLiteral("ntldr") || problem == QStringLiteral("io.sys") || problem == QStringLiteral("msdos.sys"))
            writeFile(tree + QLatin1Char('/') + problem, QByteArray("existing root loader"));
        helperCopiedBytes += quint64(bytes.size() + detector.size() + sif.size());
        if (syslinuxData)
        {
            QVERIFY(QDir().mkpath(tree + QStringLiteral("/isolinux")));
            writeFile(tree + QStringLiteral("/isolinux/isolinux.cfg"), QByteArray("append LABEL=WINPE\r\n"));
        }
        if (grub4dosOnly)
            writeFile(tree + QStringLiteral("/grldr"), QByteArray("GRUB4DOS loader data"));
        if (grubData)
        {
            QVERIFY(QDir().mkpath(tree + QStringLiteral("/boot/grub/i386-pc")));
            QByteArray module(4096, '\0');
            QByteArray banner("GRUB  version %s", sizeof("GRUB  version %s"));
            banner.append(grubVersion);
            banner.append('\0');
            module.replace(64, banner.size(), banner);
            writeFile(tree + QStringLiteral("/boot/grub/i386-pc/normal.mod"), module);
            writeFile(tree + QStringLiteral("/boot/grub/grub.cfg"), QByteArray("linux /vmlinuz LABEL=WINPE\r\n"));
            writeFile(tree + QStringLiteral("/grldr"), QByteArray("GRUB4DOS loader data"));
        }
        QProcess iso;
        QStringList arguments = {QStringLiteral("-quiet"), QStringLiteral("-R"), QStringLiteral("-J"),
            QStringLiteral("-V"), QStringLiteral("WINPE"), QStringLiteral("-o"), sourcePath_, tree};
        if (udf) arguments.prepend(QStringLiteral("-udf"));
        iso.start(QStringLiteral(RUFUS_MKISOFS), arguments);
        QVERIFY(iso.waitForFinished(writeTimeoutMs));
        QCOMPARE(iso.exitCode(), 0);
        if (hybrid)
        {
            QFile source(sourcePath_);
            QVERIFY(source.open(QIODevice::ReadWrite) && source.seek(510));
            QCOMPARE(source.write(QByteArray::fromHex("55aa")), 2);
        }
        sourceBytes_ = readFile(sourcePath_);
        controller_->selectImage(sourcePath_);
        QTRY_VERIFY_WITH_TIMEOUT(!controller_->isAnalyzing(), writeTimeoutMs);
        const bool pendingLegacyVersion = controller_->selection().imageAnalysis.pending & ISO_PENDING_WINDOWS;
        QCOMPARE(controller_->selection().imageState, pendingLegacyVersion ? rufus::ImageState::Partial : rufus::ImageState::Complete);
        QCOMPARE(start_->isEnabled(), controller_->canSelectWindowsBios() || controller_->canSelectSyslinux() || controller_->canSelectGrub() || hybrid);
    }

    void selectIsoFixture(bool hybrid = false, bool windows = false, const char* windowsVersion = "win11",
        const char* syslinuxVersion = nullptr, bool efi = true, bool ddOnly = false, const char* grubVersion = nullptr, bool windowsBios = false,
        bool rootEfiOnly = false, bool grub4dos = false, int legacyKind = 0)
    {
        nativeCapacity = 64 * 1024 * 1024;
        expectedTarget_ = QByteArray(qsizetype(nativeCapacity), '\x5a');
        writeFile(targetPath_, expectedTarget_);
        objects_[disk][blockInterface][QStringLiteral("Size")] = QVariant::fromValue(quint64(nativeCapacity));
        setObjects();
        const QString owner = device_.serviceOwner;
        QVERIFY(rufus::DescribeUDisksDevice(objects_, disk.path(), device_));
        device_.id = QStringLiteral("iso-device");
        device_.serviceOwner = owner;
        controller_->setDiscovery({device_}, {});
        controller_->selectDevice(device_.id);
        controller_->isoWriter().configure(*connection_, QCoreApplication::applicationDirPath() + QStringLiteral("/iso_helper_fixture"));
        const QString tree = directory_.filePath(QStringLiteral("iso-tree"));
        QVERIFY(QDir().mkpath(tree + QStringLiteral("/EFI/BOOT")));
        QByteArray header(512, '\0');
        header.replace(0, 2, "MZ");
        qToLittleEndian<quint32>(128, header.data() + 60);
        header.replace(128, 4, QByteArray("PE\0\0", 4));
        qToLittleEndian<quint16>(0x8664, header.data() + 132);
        qToLittleEndian<quint16>(240, header.data() + 148);
        qToLittleEndian<quint16>(0x20b, header.data() + 152);
        qToLittleEndian<quint16>(10, header.data() + 220);
        if (efi && !rootEfiOnly)
            writeFile(tree + QStringLiteral("/EFI/BOOT/BOOTX64.EFI"), header);
        else
            helperCopiedBytes -= quint64(header.size());
        writeFile(tree + QStringLiteral("/payload"), QByteArray("payload"));
        if (legacyKind == 1)
        {
            const QByteArray loader("generated ReactOS loader");
            writeFile(tree + QStringLiteral("/freeldr.sys"),loader);
            helperCopiedBytes += quint64(loader.size());
        }
        else if (legacyKind == 2)
        {
            QVERIFY(QDir().mkpath(tree + QStringLiteral("/HD_Load/USB_Boot")));
            const QByteArray loader("generated Kolibri loader"), marker("Kolibri marker");
            writeFile(tree + QStringLiteral("/HD_Load/USB_Boot/MTLD_F32"),loader);
            writeFile(tree + QStringLiteral("/kolibri.img"),marker);
            helperCopiedBytes += quint64(loader.size()+marker.size());
        }
        else if (legacyKind == 3)
        {
            QVERIFY(QDir().mkpath(tree + QStringLiteral("/sources")));
            const QString path = tree + QStringLiteral("/sources/boot.wim");
            QProcess wim; wim.start(QStringLiteral(RUFUS_WIM_FIXTURE),{path,QStringLiteral("unknown")});
            QVERIFY(wim.waitForFinished(writeTimeoutMs)); QCOMPARE(wim.exitCode(),0);
            const QByteArray loader("generated BOOTMGR PE");
            writeFile(tree + QStringLiteral("/BOOTMGR"),loader);
            helperCopiedBytes += quint64(loader.size()+QFileInfo(path).size());
        }
        if (grub4dos)
        {
            const QByteArray loader("image-owned grldr");
            const QByteArray menu("title Image boot\n");
            writeFile(tree + QStringLiteral("/grldr"), loader);
            writeFile(tree + QStringLiteral("/menu.lst"), menu);
            helperCopiedBytes += quint64(loader.size() + menu.size());
        }
        if (syslinuxVersion != nullptr)
        {
            QVERIFY(QDir().mkpath(tree + QStringLiteral("/isolinux")));
            QByteArray loader(512, '\0');
            const QByteArray banner = QByteArray("ISOLINUX ") + syslinuxVersion;
            loader.replace(64, banner.size(), banner);
            const QByteArray module = syslinuxModule();
            QVERIFY(!module.isEmpty());
            const QByteArray config("DEFAULT linux\n");
            writeFile(tree + QStringLiteral("/isolinux/isolinux.cfg"), config);
            writeFile(tree + QStringLiteral("/isolinux/isolinux.bin"), loader);
            writeFile(tree + QStringLiteral("/isolinux/ldlinux.c32"), module);
            // Rufus copy counters exclude generated configuration bytes.
            helperCopiedBytes += quint64(loader.size() + config.size() + module.size());
        }
        if (grubVersion != nullptr)
        {
            QVERIFY(QDir().mkpath(tree + QStringLiteral("/boot/grub/i386-pc")));
            QByteArray module(4096, '\0');
            const QByteArray banner = QByteArray("GRUB  version %s") + '\0' + grubVersion + '\0';
            module.replace(64, banner.size(), banner);
            writeFile(tree + QStringLiteral("/boot/grub/i386-pc/normal.mod"), module);
            helperCopiedBytes += quint64(module.size());
            if (syslinuxVersion == nullptr)
            {
                const QByteArray config("search --label SOURCE --set=root\n");
                writeFile(tree + QStringLiteral("/boot/grub/grub.cfg"), config);
                helperCopiedBytes += quint64(config.size());
            }
        }
        if (windows)
        {
            QVERIFY(QDir().mkpath(tree + QStringLiteral("/sources")));
            const QString path = tree + QStringLiteral("/sources/install.wim");
            QProcess wim;
            QStringList arguments = {path, QString::fromLatin1(windowsVersion)};
            if (rootEfiOnly)
            {
                const QString repairPath = directory_.filePath(QStringLiteral("windows-efi-repair"));
                writeFile(repairPath, header);
                arguments << QStringLiteral("efi") << repairPath;
            }
            wim.start(QStringLiteral(RUFUS_WIM_FIXTURE), arguments);
            QVERIFY(wim.waitForFinished(writeTimeoutMs));
            QCOMPARE(wim.exitCode(), 0);
            if (windowsBios)
            {
                const QByteArray bootmgr("BIOS BOOTMGR fixture");
                writeFile(tree + QStringLiteral("/bootmgr"), bootmgr);
                helperCopiedBytes += quint64(bootmgr.size());
            }
            if (efi || !windowsBios)
            {
                writeFile(tree + QStringLiteral("/bootmgr.efi"), header);
                helperCopiedBytes += quint64(header.size());
            }
            helperCopiedBytes += quint64(QFileInfo(path).size());
        }
        sourcePath_ = directory_.filePath(QStringLiteral("source.iso"));
        QProcess iso;
        iso.start(QStringLiteral(RUFUS_MKISOFS), {QStringLiteral("-quiet"), QStringLiteral("-R"),
            QStringLiteral("-J"), QStringLiteral("-V"), ddOnly ? QStringLiteral("openSUSE-Tumbleweed") : QStringLiteral("SOURCE"),
            QStringLiteral("-o"), sourcePath_, tree});
        QVERIFY(iso.waitForFinished(writeTimeoutMs));
        QCOMPARE(iso.exitCode(), 0);
        if (hybrid)
        {
            QFile file(sourcePath_);
            QVERIFY(file.open(QIODevice::ReadWrite));
            QVERIFY(file.seek(510));
            QCOMPARE(file.write(QByteArray::fromHex("55aa")), 2);
        }
        sourceBytes_ = readFile(sourcePath_);
        controller_->selectImage(sourcePath_);
        QTRY_VERIFY_WITH_TIMEOUT(!controller_->isAnalyzing(), writeTimeoutMs);
        const bool partial = legacyKind ? controller_->selection().imageAnalysis.pending != 0 : efi || (windows && !windowsBios);
        QCOMPARE(controller_->selection().imageState, partial ? rufus::ImageState::Partial : rufus::ImageState::Complete);
        if (ddOnly)
        {
            QVERIFY(start_->isEnabled());
            return;
        }
        const bool nativeBios = controller_->canSelectWindowsBios() || controller_->canSelectSyslinux() || controller_->canSelectGrub();
        if (windows && (controller_->selection().imageAnalysis.pending & ISO_PENDING_WINDOWS) && !nativeBios)
        {
            QVERIFY(!start_->isEnabled());
            return;
        }
        auto* label = window_->findChild<QLineEdit*>(QStringLiteral("volumeEdit"));
        QVERIFY(label && label->isEnabled());
        label->selectAll();
        QTest::keyClicks(label, QStringLiteral("NEW LABEL"));
        QCOMPARE(controller_->selection().volumeLabel, QStringLiteral("NEW LABEL"));
        QVERIFY(start_->isEnabled());
    }

    void startIsoPlan()
    {
        QTest::mouseClick(start_, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(controller_->isoWriter().state() == rufus::IsoWriteState::ChoosingWindowsOptions
            || controller_->isoWriter().state() == rufus::IsoWriteState::AwaitingConfirmation, writeTimeoutMs);
        if (controller_->isoWriter().state() == rufus::IsoWriteState::ChoosingWindowsOptions)
        {
            auto* dialog = window_->findChild<rufus::WindowsUserDialog*>();
            QVERIFY(dialog);
            auto* actions = dialog->findChild<QDialogButtonBox*>();
            QVERIFY(actions);
            QTest::mouseClick(actions->button(QDialogButtonBox::Ok), Qt::LeftButton);
        }
        QTRY_COMPARE_WITH_TIMEOUT(controller_->isoWriter().state(), rufus::IsoWriteState::AwaitingConfirmation, writeTimeoutMs);
        auto* dialog = window_->findChild<QMessageBox*>(QStringLiteral("isoWriteConfirmation"));
        QVERIFY(dialog && dialog->isVisible());
        QCOMPARE(dialog->defaultButton(), dialog->button(QMessageBox::Cancel));
        const int selected = controller_->isoWriter().request().fileSystem;
        const QString filesystem = selected == FS_NTFS ? QStringLiteral("NTFS") :
            (selected == FS_FAT16 ? QStringLiteral("FAT16") : QStringLiteral("FAT32"));
        const QString layout = controller_->isoWriter().request().syslinux ? QStringLiteral("MBR, %1, BIOS").arg(filesystem)
            : QStringLiteral("GPT, %1, UEFI").arg(filesystem);
        QVERIFY(dialog->informativeText().contains(layout));
    }

    void approveIsoPlan()
    {
        auto* dialog = window_->findChild<QMessageBox*>(QStringLiteral("isoWriteConfirmation"));
        QVERIFY(dialog && dialog->isVisible());
        QTest::mouseClick(dialog->button(QMessageBox::Ok), Qt::LeftButton);
    }
};

QTEST_MAIN(TestRawWriteWidgets)
#include "test_raw_write_widgets.moc"
