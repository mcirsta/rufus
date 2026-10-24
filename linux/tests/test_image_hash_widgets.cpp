// SPDX-License-Identifier: GPL-3.0-or-later
#include "qt/main_window.h"
#include "qt/selection_controller.h"
#include "qt/image_hash_dialog.h"
#include "platform/image_hash.h"
#include "platform/prepare_source.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QScreen>
#include <QScrollBar>
#include <QPushButton>
#include <QSemaphore>
#include <QSettings>
#include <QSignalSpy>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>

#include <atomic>
#include <memory>
#include <stdexcept>
#include <thread>
#include <unistd.h>

namespace
{
constexpr int workerTimeoutMs = 5000;
constexpr int uiTimeoutMs = 2000;
enum class HashFault { None, BeforeRead, HoldCleanup, IoError, Exception };
std::atomic<HashFault> nextFault{HashFault::None};
std::atomic<bool> holdCleanup{false};
std::atomic<QThread*> worker{nullptr};
std::atomic<uid_t> workerUid{0};
std::atomic<int> calls{0}, closed{0};
QSemaphore entered, releaseWorker;
int beeps, alerts, authorizations;

void waitForFixture()
{
    entered.release();
    if (!releaseWorker.tryAcquire(1, workerTimeoutMs))
        throw std::runtime_error("hash fixture release timed out");
}

void releaseAfterCancellation()
{
    QThread* thread = worker.load();
    for (int elapsed = 0; elapsed < workerTimeoutMs; elapsed++)
    {
        if (thread->isInterruptionRequested())
        {
            releaseWorker.release();
            return;
        }
        QThread::msleep(1);
    }
    releaseWorker.release();
}

QByteArray imageBytes()
{
    QByteArray data(131072, '\0');
    for (qsizetype i = 0; i < data.size(); i++)
        data[i] = char((i * 37 + i / 251) & 255);
    data[510] = char(0x55);
    data[511] = char(0xaa);
    return data;
}

bool writeFile(const QString& path, const QByteArray& data)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size() && file.flush();
}

QStringList expectedHashes(const QByteArray& bytes, bool extra)
{
    const QCryptographicHash::Algorithm algorithms[] = {QCryptographicHash::Md5,
        QCryptographicHash::Sha1, QCryptographicHash::Sha256, QCryptographicHash::Sha512};
    QStringList result;
    for (int i = 0; i < (extra ? 4 : 3); i++)
        result.append(QString::fromLatin1(QCryptographicHash::hash(bytes, algorithms[i]).toHex()));
    return result;
}
}

extern "C" int __real_HashImageFileSource(const IMAGE_FILE_SOURCE*, const char*, bool, IMAGE_HASH_RESULT*);
extern "C" int __wrap_HashImageFileSource(const IMAGE_FILE_SOURCE* source, const char* path,
    bool extra, IMAGE_HASH_RESULT* result)
{
    worker = QThread::currentThread();
    workerUid = geteuid();
    ++calls;
    const HashFault fault = nextFault.exchange(HashFault::None);
    if (fault == HashFault::BeforeRead)
        waitForFixture();
    if (fault == HashFault::Exception)
        throw std::runtime_error("injected hash worker failure");
    if (fault == HashFault::IoError)
        return EIO;
    const int error = __real_HashImageFileSource(source, path, extra, result);
    if (fault == HashFault::HoldCleanup)
        holdCleanup = true;
    return error;
}

extern "C" void __real_CloseImageFileSource(IMAGE_FILE_SOURCE*);
extern "C" void __wrap_CloseImageFileSource(IMAGE_FILE_SOURCE* source)
{
    const bool workerSource = QThread::currentThread() == worker.load() && source && source->fd >= 0;
    __real_CloseImageFileSource(source);
    if (workerSource)
    {
        ++closed;
        if (holdCleanup.exchange(false))
            waitForFixture();
    }
}

extern "C" void __wrap__ZN12QApplication4beepEv() { ++beeps; }
extern "C" void __wrap__ZN12QApplication5alertEP7QWidgeti(QWidget*, int) { ++alerts; }
extern "C" void __real__ZN8QProcess5startERK7QStringRK5QListIS0_E6QFlagsIN13QIODeviceBase12OpenModeFlagEE(
    QProcess*, const QString&, const QStringList&, QIODevice::OpenMode);
extern "C" void __wrap__ZN8QProcess5startERK7QStringRK5QListIS0_E6QFlagsIN13QIODeviceBase12OpenModeFlagEE(
    QProcess* process, const QString& program, const QStringList& arguments, QIODevice::OpenMode mode)
{
    if (program.contains(QStringLiteral("pkexec")))
    {
        ++authorizations;
        qFatal("A read-only hash operation must not request authorization");
    }
    __real__ZN8QProcess5startERK7QStringRK5QListIS0_E6QFlagsIN13QIODeviceBase12OpenModeFlagEE(
        process, program, arguments, mode);
}

class TestImageHashWidgets : public QObject
{
    Q_OBJECT
private:
    QTemporaryDir settings_;
    QTemporaryDir images_;
    std::unique_ptr<rufus::SelectionController> controller_;
    std::unique_ptr<rufus::MainWindow> window_;
    QString path_;
    QByteArray bytes_;
    QFont applicationFont_;

    QToolButton* hashButton() const { return window_->findChild<QToolButton*>(QStringLiteral("hashButton")); }
    QPushButton* closeButton() const { return window_->findChild<QPushButton*>(QStringLiteral("closeButton")); }
    QProgressBar* progress() const { return window_->findChild<QProgressBar*>(QStringLiteral("progressBar")); }

    void makeWindow()
    {
        window_ = std::make_unique<rufus::MainWindow>(*controller_);
        window_->show();
        QVERIFY(QTest::qWaitForWindowExposed(window_.get(), uiTimeoutMs));
    }

    void closeResult()
    {
        auto* dialog = window_->findChild<rufus::ImageHashDialog*>();
        QVERIFY(dialog);
        QTest::keyClick(dialog, Qt::Key_Escape);
        QTRY_VERIFY_WITH_TIMEOUT(window_->findChild<rufus::ImageHashDialog*>() == nullptr, uiTimeoutMs);
    }

    void saveScreenshot(const QString& name)
    {
        const QString directory = qEnvironmentVariable("RUFUS_HASH_SCREENSHOTS");
        if (!directory.isEmpty())
        {
            auto* dialog = window_->findChild<rufus::ImageHashDialog*>();
            QVERIFY(dialog);
            QVERIFY(dialog->grab().save(directory + QLatin1Char('/') + name + QStringLiteral(".png")));
        }
    }

private slots:
    void initTestCase()
    {
        QVERIFY(settings_.isValid() && images_.isValid());
        qputenv("XDG_DATA_HOME", settings_.filePath(QStringLiteral("application-data")).toUtf8());
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_.path());
        QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    }

    void init()
    {
        applicationFont_ = QApplication::font();
        QSettings settings(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("Rufus"), QStringLiteral("Linux"));
        settings.clear();
        nextFault = HashFault::None;
        holdCleanup = false;
        worker = nullptr;
        calls = 0;
        closed = 0;
        beeps = alerts = authorizations = 0;
        entered.tryAcquire(entered.available());
        releaseWorker.tryAcquire(releaseWorker.available());
        bytes_ = imageBytes();
        path_ = images_.filePath(QStringLiteral("image <&> ü.img"));
        QVERIFY(writeFile(path_, bytes_));
        controller_ = std::make_unique<rufus::SelectionController>();
    }

    void cleanup()
    {
        releaseWorker.release();
        window_.reset();
        controller_.reset();
        QApplication::setFont(applicationFont_);
        QCOMPARE(authorizations, 0);
        QCOMPARE(entered.available(), 0);
        for (const QString& name : QDir(images_.path()).entryList(QDir::Files | QDir::System))
            QVERIFY(QFile::remove(images_.filePath(name)));
    }

    void workerAndDialog_data()
    {
        QTest::addColumn<bool>("extra");
        QTest::newRow("defaults") << false;
        QTest::newRow("extra") << true;
    }

    void workerAndDialog()
    {
        QFETCH(bool, extra);
        controller_->selectImage(path_);
        QVERIFY(controller_->canHashImage());
        QCOMPARE(controller_->selection().devices.size(), 0);
        QVERIFY(!controller_->canStartWrite());
        if (extra)
            controller_->toggleExtraHashes();
        makeWindow();
        QVERIFY(hashButton()->isEnabled());
        QSignalSpy finished(&controller_->hasher(), &rufus::ImageHashController::finished);
        QSignalSpy logs(controller_.get(), &rufus::SelectionController::logMessage);
        nextFault = HashFault::HoldCleanup;
        QTest::mouseClick(hashButton(), Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(entered.available() > 0, workerTimeoutMs);
        entered.acquire();
        QTRY_COMPARE_WITH_TIMEOUT(controller_->hasher().processed(), quint64(bytes_.size()), uiTimeoutMs);
        QCOMPARE(workerUid.load(), geteuid());
        QVERIFY(worker.load() != QThread::currentThread());
        QCOMPARE(closed.load(), 1);
        QVERIFY(controller_->isBusy());
        QVERIFY(controller_->hasher().result().digests.isEmpty());
        QVERIFY(!hashButton()->isEnabled());
        QVERIFY(!window_->findChild<QPushButton*>(QStringLiteral("selectImageButton"))->isEnabled());
        QVERIFY(!window_->findChild<QComboBox*>(QStringLiteral("bootCombo"))->isEnabled());
        QCOMPARE(closeButton()->text(), QStringLiteral("CANCEL"));
        QVERIFY(progress()->value() < progress()->maximum());
        QVERIFY(window_->findChild<rufus::ImageHashDialog*>() == nullptr);
        QCOMPARE(finished.size(), 0);
        QCOMPARE(beeps, 0);
        const QString label = controller_->selection().volumeLabel;
        const bool dual = controller_->enhancedInstall();
        const bool mbr = controller_->windowsRufusMbr();
        controller_->selectImage(images_.filePath(QStringLiteral("missing.img")));
        controller_->setBootType(BT_NON_BOOTABLE);
        controller_->setVolumeLabel(QStringLiteral("changed"));
        controller_->toggleEnhancedInstall();
        controller_->toggleWindowsMbr();
        controller_->toggleExtraHashes();
        const bool expert = controller_->isoWriter().expertMode();
        QTest::keyClick(window_.get(), Qt::Key_E, Qt::ControlModifier | Qt::AltModifier);
        QCOMPARE(controller_->isoWriter().expertMode(), expert);
        controller_->startWrite();
        controller_->startHashes();
        QCOMPARE(controller_->selection().imagePath, path_);
        QCOMPARE(controller_->selection().bootType, int(BT_IMAGE));
        QCOMPARE(controller_->selection().volumeLabel, label);
        QCOMPARE(controller_->enhancedInstall(), dual);
        QCOMPARE(controller_->windowsRufusMbr(), mbr);
        QCOMPARE(controller_->extraHashes(), extra);
        QCOMPARE(calls.load(), 1);
        rufus::Device device;
        device.id = QStringLiteral("hash-test-usb");
        device.path = QStringLiteral("/test/never-open-device");
        device.name = QStringLiteral("Hash test device");
        device.size = 16ULL * 1024 * 1024 * 1024;
        device.defaultVisible = true;
        controller_->setDevices({device});
        controller_->setDevices({}); // A hash is independent of USB hotplug.
        QVERIFY(controller_->hasher().isActive());
        controller_->setDevices({device});
        QVERIFY(controller_->selection().deviceId.isEmpty());
        releaseWorker.release();
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, workerTimeoutMs);
        QCOMPARE(controller_->hasher().state(), rufus::ImageHashState::Complete);
        QCOMPARE(controller_->hasher().result().digests, expectedHashes(bytes_, extra));
        QVERIFY(!controller_->isBusy());
        QVERIFY(hashButton()->isEnabled());
        QCOMPARE(progress()->value(), progress()->maximum());
        QCOMPARE(progress()->format(), QStringLiteral("READY"));
        QCOMPARE(closeButton()->text(), QStringLiteral("CLOSE"));
        QCOMPARE(beeps, 1);
        QCOMPARE(alerts, 1);
        QCOMPARE(logs.size(), extra ? 5 : 4); // Digests and deferred first-device selection.
        QCOMPARE(controller_->selection().deviceId, device.id);
        auto* dialog = window_->findChild<rufus::ImageHashDialog*>();
        QVERIFY(dialog);
        QCOMPARE(dialog->thread(), QThread::currentThread());
        QCOMPARE(dialog->windowTitle(), QFileInfo(path_).fileName());
        QVERIFY(dialog->isModal());
        auto* md5 = dialog->findChild<QLineEdit*>(QStringLiteral("md5Hash"));
        auto* sha512 = dialog->findChild<QPlainTextEdit*>(QStringLiteral("sha512Hash"));
        QVERIFY(md5 && sha512 && md5->isReadOnly() && sha512->isReadOnly());
        auto* sha256 = dialog->findChild<QPlainTextEdit*>(QStringLiteral("sha256Hash"));
        QVERIFY(sha256);
        QCOMPARE(sha256->verticalScrollBar()->maximum(), 0);
        QCOMPARE(sha512->verticalScrollBar()->maximum(), 0);
        QCOMPARE(md5->text(), expectedHashes(bytes_, extra).first());
        if (extra)
        {
            QCOMPARE(sha512->toPlainText(), expectedHashes(bytes_, true).last());
            sha512->setFocus();
            QTest::keyClick(sha512, Qt::Key_A, Qt::ControlModifier);
            QTest::keyClick(sha512, Qt::Key_C, Qt::ControlModifier);
            QCOMPARE(QApplication::clipboard()->text(), expectedHashes(bytes_, true).last());
        }
        else
            QCOMPARE(sha512->toPlainText(), QStringLiteral("Use Alt+H (in the main application window) to enable."));
        saveScreenshot(extra ? QStringLiteral("extra") : QStringLiteral("defaults"));
        if (extra)
            closeResult();
        else
        {
            QTest::keyClick(dialog, Qt::Key_Return);
            QTRY_VERIFY_WITH_TIMEOUT(window_->findChild<rufus::ImageHashDialog*>() == nullptr, uiTimeoutMs);
        }
        controller_->setDevices({});
        QCOMPARE(beeps, 1); // Refreshing the view must not repeat a notification.
    }

    void cancellation_data()
    {
        QTest::addColumn<bool>("resultQueued");
        QTest::newRow("while-reading") << false;
        QTest::newRow("result-queued-cleanup-held") << true;
    }

    void cancellation()
    {
        QFETCH(bool, resultQueued);
        controller_->selectImage(path_);
        makeWindow();
        nextFault = resultQueued ? HashFault::HoldCleanup : HashFault::BeforeRead;
        QSignalSpy finished(&controller_->hasher(), &rufus::ImageHashController::finished);
        QTest::mouseClick(hashButton(), Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(entered.available() > 0, workerTimeoutMs);
        entered.acquire();
        QTest::mouseClick(closeButton(), Qt::LeftButton);
        QCOMPARE(controller_->hasher().state(), rufus::ImageHashState::Cancelling);
        QVERIFY(window_->isVisible());
        QVERIFY(!closeButton()->isEnabled());
        QCOMPARE(window_->findChildren<QMessageBox*>().size(), 1); // Existing hidden image notice only.
        QCOMPARE(finished.size(), 0);
        releaseWorker.release();
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, workerTimeoutMs);
        QCOMPARE(controller_->hasher().state(), rufus::ImageHashState::Cancelled);
        QVERIFY(controller_->hasher().result().digests.isEmpty());
        QVERIFY(window_->findChild<rufus::ImageHashDialog*>() == nullptr);
        QCOMPARE(beeps, 0);
        QCOMPARE(progress()->format(), QStringLiteral("CANCELLED"));
        QVERIFY(progress()->value() < progress()->maximum());
        QVERIFY(hashButton()->isEnabled());
        // A cancelled operation can be followed by a fresh frozen request.
        QTest::mouseClick(hashButton(), Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 2, workerTimeoutMs);
        QCOMPARE(controller_->hasher().state(), rufus::ImageHashState::Complete);
        QCOMPARE(controller_->hasher().result().digests, expectedHashes(bytes_, false));
        QCOMPARE(beeps, 1);
        closeResult();
    }

    void failures_data()
    {
        QTest::addColumn<int>("fault");
        QTest::newRow("io") << int(HashFault::IoError);
        QTest::newRow("exception") << int(HashFault::Exception);
        QTest::newRow("changed-before-read") << int(HashFault::BeforeRead);
    }

    void failures()
    {
        QFETCH(int, fault);
        controller_->selectImage(path_);
        makeWindow();
        nextFault = HashFault(fault);
        QSignalSpy finished(&controller_->hasher(), &rufus::ImageHashController::finished);
        QTest::mouseClick(hashButton(), Qt::LeftButton);
        if (HashFault(fault) == HashFault::BeforeRead)
        {
            QTRY_VERIFY_WITH_TIMEOUT(entered.available() > 0, workerTimeoutMs);
            entered.acquire();
            QVERIFY(QFile::rename(path_, path_ + QStringLiteral(".old")));
            QVERIFY(writeFile(path_, bytes_));
            releaseWorker.release();
        }
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, workerTimeoutMs);
        QCOMPARE(controller_->hasher().state(), rufus::ImageHashState::Failed);
        QVERIFY(controller_->hasher().result().digests.isEmpty());
        QCOMPARE(progress()->format(), QStringLiteral("FAILED"));
        QVERIFY(progress()->value() < progress()->maximum());
        auto* error = window_->findChild<QMessageBox*>(QStringLiteral("imageHashError"));
        QVERIFY(error && error->isVisible());
        QVERIFY(!error->text().isEmpty());
        QCOMPARE(beeps, 0);
        QCOMPARE(closed.load(), 1);
        error->accept();
    }

    void changedSinceSelection()
    {
        controller_->selectImage(path_);
        QVERIFY(QFile::rename(path_, path_ + QStringLiteral(".old")));
        QVERIFY(writeFile(path_, bytes_));
        QSignalSpy finished(&controller_->hasher(), &rufus::ImageHashController::finished);
        controller_->startHashes();
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, workerTimeoutMs);
        QCOMPARE(controller_->hasher().state(), rufus::ImageHashState::Failed);
        QCOMPARE(controller_->hasher().result().error, ESTALE);
        QCOMPARE(calls.load(), 0); // Rejected before hashing any replacement bytes.
        QVERIFY(controller_->hasher().result().digests.isEmpty());
    }

    void isoSelectionAndAnalysisGate()
    {
        QTemporaryDir tree;
        QVERIFY(tree.isValid());
        QVERIFY(writeFile(tree.filePath(QStringLiteral("syslinux.cfg")), QByteArray("default linux\n")));
        const QString image = images_.filePath(QStringLiteral("checksum.iso"));
        QProcess makeIso;
        makeIso.start(QStringLiteral(RUFUS_MKISOFS), {QStringLiteral("-quiet"), QStringLiteral("-R"),
            QStringLiteral("-J"), QStringLiteral("-V"), QStringLiteral("HASH_TEST"),
            QStringLiteral("-o"), image, tree.path()});
        QVERIFY(makeIso.waitForFinished(workerTimeoutMs));
        QCOMPARE(makeIso.exitStatus(), QProcess::NormalExit);
        QCOMPARE(makeIso.exitCode(), 0);
        makeWindow();
        controller_->selectImage(image);
        QVERIFY(controller_->isAnalyzing());
        QVERIFY(!controller_->canHashImage() && !hashButton()->isEnabled());
        controller_->startHashes();
        QCOMPARE(calls.load(), 0);
        QTRY_VERIFY_WITH_TIMEOUT(!controller_->isAnalyzing(), workerTimeoutMs);
        QVERIFY2(controller_->canHashImage(), qPrintable(controller_->imageStatusText()));
        QCOMPARE(controller_->selection().imageSource.imagePath, image);
        QFile source(image);
        QVERIFY(source.open(QIODevice::ReadOnly));
        const QByteArray bytes = source.readAll();
        source.close();
        QSignalSpy finished(&controller_->hasher(), &rufus::ImageHashController::finished);
        QTest::mouseClick(hashButton(), Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, workerTimeoutMs);
        QCOMPARE(controller_->hasher().state(), rufus::ImageHashState::Complete);
        QCOMPARE(controller_->hasher().result().digests, expectedHashes(bytes, false));
        closeResult();
    }

    void shortcutAndPersistence()
    {
        QVERIFY(!controller_->extraHashes());
        makeWindow();
        QTest::keyClick(window_.get(), Qt::Key_H, Qt::AltModifier);
        QVERIFY(controller_->extraHashes());
        QVERIFY(window_->findChild<QStatusBar*>()->currentMessage().contains(QStringLiteral("enabled")));
        window_.reset();
        controller_.reset();
        controller_ = std::make_unique<rufus::SelectionController>();
        QVERIFY(controller_->extraHashes());
        makeWindow();
        QTest::keyClick(window_.get(), Qt::Key_H, Qt::AltModifier);
        QVERIFY(!controller_->extraHashes());
        controller_->setBootType(BT_NON_BOOTABLE);
        QVERIFY(!hashButton()->isEnabled());
    }

    void teardownJoinsWithoutGuiEvents()
    {
        controller_->selectImage(path_);
        nextFault = HashFault::BeforeRead;
        controller_->startHashes();
        QTRY_VERIFY_WITH_TIMEOUT(entered.available() > 0, workerTimeoutMs);
        entered.acquire();
        std::thread releaser(releaseAfterCancellation);
        controller_.reset();
        releaser.join();
        QCOMPARE(closed.load(), 1);
    }

    void largeFontDialog()
    {
        controller_->selectImage(path_);
        controller_->toggleExtraHashes();
        makeWindow();
        QFont large = QApplication::font();
        large.setPointSize(18);
        QApplication::setFont(large);
        QTest::mouseClick(hashButton(), Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(window_->findChild<rufus::ImageHashDialog*>() != nullptr, workerTimeoutMs);
        auto* dialog = window_->findChild<rufus::ImageHashDialog*>();
        QCOMPARE(dialog->font().pointSize(), 18);
        QCOMPARE(dialog->findChild<QLineEdit*>(QStringLiteral("md5Hash"))->font().pointSize(), 18);
        QVERIFY(dialog->width() <= dialog->screen()->availableGeometry().width());
        QVERIFY(dialog->height() <= dialog->screen()->availableGeometry().height());
        for (QWidget* field : dialog->findChildren<QWidget*>())
        {
            if (!field->isVisible() || field->isWindow())
                continue;
            const QRect bounds(field->mapTo(dialog, QPoint(0, 0)), field->size());
            QVERIFY2(dialog->rect().contains(bounds), qPrintable(field->objectName()));
        }
        saveScreenshot(QStringLiteral("large-font"));
        auto* buttons = dialog->findChild<QDialogButtonBox*>();
        QVERIFY(buttons);
        QTest::mouseClick(buttons->button(QDialogButtonBox::Ok), Qt::LeftButton);
    }
};

QTEST_MAIN(TestImageHashWidgets)
#include "test_image_hash_widgets.moc"
