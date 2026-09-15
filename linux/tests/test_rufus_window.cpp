// SPDX-License-Identifier: GPL-3.0-or-later
#include "qt/main_window.h"
#include "qt/selection_controller.h"

#include <QComboBox>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QTest>
#include <QToolButton>

#include <memory>

static constexpr int uiTimeoutMs = 1000;

namespace
{

QList<rufus::Device> testDevices()
{
    // Descriptive paths only: these records must never open host devices.
    return {
        {QStringLiteral("test-usb-a"), QStringLiteral("/test/usb-a"),
         QStringLiteral("Test USB"), 16ULL * 1024 * 1024 * 1024, true, false},
        {QStringLiteral("test-usb-b"), QStringLiteral("/test/usb-b"),
         QStringLiteral("Test USB"), 16ULL * 1024 * 1024 * 1024, true, false},
        {QStringLiteral("test-system"), QStringLiteral("/test/system"),
         QStringLiteral("Internal disk"), 128ULL * 1024 * 1024 * 1024, false, false},
        {QStringLiteral("test-readonly"), QStringLiteral("/test/readonly"),
         QStringLiteral("Read-only USB"), 8ULL * 1024 * 1024 * 1024, true, true}
    };
}

} // namespace

class TestRufusWindow : public QObject
{
    Q_OBJECT

private:
    void chooseFirstDevice()
    {
        deviceCombo_->setFocus();
        QTest::keyClick(deviceCombo_, Qt::Key_Home);
    }

    std::unique_ptr<rufus::SelectionController> controller_;
    std::unique_ptr<rufus::MainWindow> window_;
    QComboBox* deviceCombo_ = nullptr;
    QComboBox* bootCombo_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QPushButton* selectButton_ = nullptr;
    QPushButton* startButton_ = nullptr;
    QToolButton* logButton_ = nullptr;

private slots:
    void initTestCase()
    {
        // Only tests force a Widgets file picker. The application uses the
        // native dialog where the desktop provides one.
        QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    }

    void init()
    {
        controller_ = std::make_unique<rufus::SelectionController>();
        window_ = std::make_unique<rufus::MainWindow>(*controller_);
        deviceCombo_ = window_->findChild<QComboBox*>(QStringLiteral("deviceCombo"));
        bootCombo_ = window_->findChild<QComboBox*>(QStringLiteral("bootCombo"));
        statusLabel_ = window_->findChild<QLabel*>(QStringLiteral("statusLabel"));
        selectButton_ = window_->findChild<QPushButton*>(QStringLiteral("selectImageButton"));
        startButton_ = window_->findChild<QPushButton*>(QStringLiteral("startButton"));
        logButton_ = window_->findChild<QToolButton*>(QStringLiteral("logButton"));
        QVERIFY(deviceCombo_ && bootCombo_ && statusLabel_);
        QVERIFY(selectButton_ && startButton_ && logButton_);
        window_->show();
        QVERIFY(QTest::qWaitForWindowExposed(window_.get(), 1000));
    }

    void cleanup()
    {
        window_.reset();
        controller_.reset();
    }

    void emptyStateExplainsUnavailableOperations()
    {
        QCOMPARE(deviceCombo_->count(), 0);
        QCOMPARE(deviceCombo_->currentIndex(), -1);
        QVERIFY(!deviceCombo_->isEnabled());
        QVERIFY(bootCombo_->currentData().toString().isEmpty());
        QCOMPARE(bootCombo_->currentText(), QStringLiteral("Disk or ISO image (Please select)"));
        QVERIFY(!bootCombo_->isEditable());
        QVERIFY(selectButton_->isEnabled());
        QVERIFY(!startButton_->isEnabled());
        QLabel* status = window_->findChild<QLabel*>(QStringLiteral("statusLabel"));
        QProgressBar* progress = window_->findChild<QProgressBar*>(QStringLiteral("progressBar"));
        QVERIFY(status && progress);
        QVERIFY(status->text().contains(QStringLiteral("unavailable")));
        QCOMPARE(progress->value(), 0);
        QCOMPARE(progress->format(), QStringLiteral("READY"));
        QVERIFY(!window_->windowIcon().isNull());
        QVERIFY(!logButton_->icon().isNull());
        const QString screenshot = qEnvironmentVariable("RUFUS_TEST_SCREENSHOT");
        if (!screenshot.isEmpty())
            QVERIFY(window_->grab().save(screenshot));
    }

    void eligibleDevicesCanBeSelectedWithKeyboard()
    {
        controller_->setDevices(testDevices());
        QCOMPARE(deviceCombo_->count(), 2);
        QCOMPARE(deviceCombo_->currentIndex(), -1);
        QVERIFY(deviceCombo_->itemText(0).contains(QStringLiteral("/test/usb-a")));
        QVERIFY(deviceCombo_->itemText(1).contains(QStringLiteral("/test/usb-b")));
        chooseFirstDevice();
        QCOMPARE(controller_->selection().deviceId, QStringLiteral("test-usb-a"));
        QTest::keyClick(deviceCombo_, Qt::Key_End);
        QCOMPARE(controller_->selection().deviceId, QStringLiteral("test-usb-b"));
        QVERIFY(!startButton_->isEnabled());
    }

    void refreshPreservesIdentityAcrossReordering()
    {
        QList<rufus::Device> devices = testDevices();
        controller_->setDevices(devices);
        chooseFirstDevice();
        QSignalSpy logs(controller_.get(), &rufus::SelectionController::logMessage);
        devices.swapItemsAt(0, 1);
        devices[1].name = QStringLiteral("Renamed USB");
        controller_->setDevices(devices);
        QCOMPARE(deviceCombo_->currentIndex(), 1);
        QCOMPARE(controller_->selection().deviceId, QStringLiteral("test-usb-a"));
        QVERIFY(deviceCombo_->currentText().contains(QStringLiteral("Renamed USB")));
        QCOMPARE(logs.size(), 0);
    }

    void changedDeviceRequiresReselection_data()
    {
        QTest::addColumn<QString>("change");
        QTest::newRow("removed") << QStringLiteral("removed");
        QTest::newRow("replacement-at-same-path") << QStringLiteral("replacement");
        QTest::newRow("capacity-changed") << QStringLiteral("capacity");
        QTest::newRow("path-changed") << QStringLiteral("path");
        QTest::newRow("became-readonly") << QStringLiteral("readonly");
        QTest::newRow("ambiguous-identity") << QStringLiteral("ambiguous");
    }

    void changedDeviceRequiresReselection()
    {
        QFETCH(QString, change);
        QList<rufus::Device> devices = testDevices();
        controller_->setDevices(devices);
        chooseFirstDevice();
        if (change == QStringLiteral("removed"))
            devices.removeFirst();
        else if (change == QStringLiteral("replacement"))
            devices[0].id = QStringLiteral("test-replacement");
        else if (change == QStringLiteral("capacity"))
            devices[0].size /= 2;
        else if (change == QStringLiteral("path"))
            devices[0].path = QStringLiteral("/test/changed-path");
        else if (change == QStringLiteral("readonly"))
            devices[0].readOnly = true;
        else if (change == QStringLiteral("ambiguous"))
            devices[1].id = devices[0].id;
        controller_->setDevices(devices);
        QVERIFY(controller_->selection().deviceId.isEmpty());
        QCOMPARE(deviceCombo_->currentIndex(), -1);
        QVERIFY(!startButton_->isEnabled());
        controller_->setDevices(testDevices());
        QCOMPARE(deviceCombo_->currentIndex(), -1);
    }

    void filePickerSelectsImageAndCancellationPreservesIt()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QString::fromUtf8("Mediu și fișiere.img"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("test image"), qint64(10));
        file.close();

        QTest::mouseClick(selectButton_, Qt::LeftButton);
        QFileDialog* dialog = window_->findChild<QFileDialog*>(QStringLiteral("imageDialog"));
        QVERIFY(dialog && dialog->isVisible());
        dialog->setDirectory(directory.path());
        dialog->selectFile(path);
        QDialogButtonBox* buttons = dialog->findChild<QDialogButtonBox*>();
        QVERIFY(buttons && buttons->button(QDialogButtonBox::Open));
        QTRY_VERIFY_WITH_TIMEOUT(buttons->button(QDialogButtonBox::Open)->isEnabled(), uiTimeoutMs);
        QTest::mouseClick(buttons->button(QDialogButtonBox::Open), Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(bootCombo_->currentData().toString(), path, uiTimeoutMs);
        QCOMPARE(bootCombo_->currentText(), QString::fromUtf8("Mediu și fișiere.img"));
        QCOMPARE(controller_->selection().imageSize, quint64(10));
        QTRY_VERIFY_WITH_TIMEOUT(!controller_->isAnalyzing(), 3000);
        QCOMPARE(controller_->selection().imageState, rufus::ImageState::Failed);
        QVERIFY(statusLabel_->text().contains(QStringLiteral("512-byte sectors")));
        QVERIFY(bootCombo_->toolTip().contains(path));
        controller_->setDevices(testDevices());
        chooseFirstDevice();
        QVERIFY(!startButton_->isEnabled());

        QTest::mouseClick(selectButton_, Qt::LeftButton);
        QVERIFY(dialog->isVisible());
        QTest::keyClick(dialog, Qt::Key_Escape);
        QTRY_VERIFY_WITH_TIMEOUT(!dialog->isVisible(), uiTimeoutMs);
        QCOMPARE(bootCombo_->currentData().toString(), path);
        QCOMPARE(controller_->selection().deviceId, QStringLiteral("test-usb-a"));
    }

    void invalidImageClearsPreviousSelection_data()
    {
        QTest::addColumn<QString>("kind");
        QTest::newRow("missing") << QStringLiteral("missing");
        QTest::newRow("directory") << QStringLiteral("directory");
        QTest::newRow("empty") << QStringLiteral("empty");
    }

    void invalidImageClearsPreviousSelection()
    {
        QFETCH(QString, kind);
        QTemporaryFile previous;
        QVERIFY(previous.open());
        QCOMPARE(previous.write("image"), qint64(5));
        QVERIFY(previous.flush());
        controller_->selectImage(previous.fileName());
        QVERIFY(!bootCombo_->currentData().toString().isEmpty());
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QString invalidPath = directory.filePath(QStringLiteral("missing.img"));
        if (kind == QStringLiteral("directory"))
            invalidPath = directory.path();
        else if (kind == QStringLiteral("empty"))
        {
            QFile empty(invalidPath);
            QVERIFY(empty.open(QIODevice::WriteOnly));
        }
        controller_->selectImage(invalidPath);
        QVERIFY(bootCombo_->currentData().toString().isEmpty());
        QCOMPARE(controller_->selection().imageSize, quint64(0));
        QVERIFY(!controller_->selection().imageProblem.isEmpty());
        QCOMPARE(statusLabel_->text(), controller_->selection().imageProblem);
        QVERIFY(!startButton_->isEnabled());
    }

    void logCanBeClosedAndReopenedWithoutLosingMessages()
    {
        QTest::mouseClick(logButton_, Qt::LeftButton);
        QDialog* logWindow = window_->findChild<QDialog*>(QStringLiteral("logWindow"));
        QPlainTextEdit* text = window_->findChild<QPlainTextEdit*>(QStringLiteral("logText"));
        QVERIFY(logWindow && text && logWindow->isVisible());
        QVERIFY(text->isReadOnly());
        QTest::keyClick(logWindow, Qt::Key_Escape);
        QVERIFY(!logWindow->isVisible());
        controller_->setDevices(testDevices());
        chooseFirstDevice();
        QTest::mouseClick(logButton_, Qt::LeftButton);
        QVERIFY(logWindow->isVisible());
        QVERIFY(text->toPlainText().contains(QStringLiteral("/test/usb-a")));
        QCOMPARE(window_->findChildren<QDialog*>(QStringLiteral("logWindow")).size(), 1);
    }

    void keyboardNavigationAndCloseLeaveNoOrphanWindow()
    {
        selectButton_->setFocus();
        QTest::keyClick(selectButton_, Qt::Key_Return);
        QFileDialog* picker = window_->findChild<QFileDialog*>(QStringLiteral("imageDialog"));
        QVERIFY(picker && picker->isVisible());
        QTest::keyClick(picker, Qt::Key_Escape);
        QVERIFY(!picker->isVisible());
        selectButton_->setFocus();
        QTest::keyClick(selectButton_, Qt::Key_Tab);
        QWidget* advancedDrive = window_->findChild<QWidget*>(QStringLiteral("advancedDriveButton"));
        QWidget* advancedFormat = window_->findChild<QWidget*>(QStringLiteral("advancedFormatButton"));
        QVERIFY(advancedDrive && advancedFormat);
        QVERIFY(advancedDrive->hasFocus());
        QTest::keyClick(advancedDrive, Qt::Key_Tab);
        QVERIFY(advancedFormat->hasFocus());
        QTest::keyClick(advancedFormat, Qt::Key_Tab);
        QVERIFY(logButton_->hasFocus());
        QTest::keyClick(logButton_, Qt::Key_Space);
        QDialog* logWindow = window_->findChild<QDialog*>(QStringLiteral("logWindow"));
        QVERIFY(logWindow && logWindow->isVisible());
        QPushButton* close = window_->findChild<QPushButton*>(QStringLiteral("closeButton"));
        QVERIFY(close);
        QTest::mouseClick(close, Qt::LeftButton);
        QVERIFY(!window_->isVisible());
        QVERIFY(!logWindow->isVisible());
    }

    void escapeClosesTheMainWindow()
    {
        QTest::keyClick(window_.get(), Qt::Key_Escape);
        QVERIFY(!window_->isVisible());
    }

    void win32ControlOrderAndDefaults()
    {
        // These captions and pairings come from upstream IDD_DIALOG, rather
        // than from a redesigned Linux workflow.
        const QStringList labelNames = {QStringLiteral("driveHeader"), QStringLiteral("deviceLabel"),
            QStringLiteral("bootLabel"), QStringLiteral("partitionLabel"), QStringLiteral("formatHeader"),
            QStringLiteral("volumeLabel"), QStringLiteral("fileSystemLabel"), QStringLiteral("statusHeader")};
        const QStringList captions = {QStringLiteral("Drive Properties"), QStringLiteral("Device"),
            QStringLiteral("Boot selection"), QStringLiteral("Partition scheme"), QStringLiteral("Format Options"),
            QStringLiteral("Volume label"), QStringLiteral("File system"), QStringLiteral("Status")};
        int previousY = -1;
        for (qsizetype i = 0; i < labelNames.size(); ++i)
        {
            QLabel* label = window_->findChild<QLabel*>(labelNames[i]);
            QVERIFY(label);
            QCOMPARE(label->text(), captions[i]);
            const int y = label->mapTo(window_.get(), QPoint()).y();
            QVERIFY(y > previousY);
            previousY = y;
        }
        QComboBox* partition = window_->findChild<QComboBox*>(QStringLiteral("partitionCombo"));
        QComboBox* target = window_->findChild<QComboBox*>(QStringLiteral("targetCombo"));
        QComboBox* filesystem = window_->findChild<QComboBox*>(QStringLiteral("fileSystemCombo"));
        QComboBox* cluster = window_->findChild<QComboBox*>(QStringLiteral("clusterCombo"));
        QVERIFY(partition && target && filesystem && cluster);
        QCOMPARE(partition->y(), target->y());
        QCOMPARE(filesystem->y(), cluster->y());
        QCOMPARE(partition->x(), filesystem->x());
        QCOMPARE(target->x(), cluster->x());
        QVERIFY(target->x() > partition->x());
        QVERIFY(!partition->isEnabled() && !target->isEnabled());
        QVERIFY(!filesystem->isEnabled() && !cluster->isEnabled());
        QCOMPARE(selectButton_->text(), QStringLiteral("SELECT"));
        QCOMPARE(startButton_->text(), QStringLiteral("START"));
        QCheckBox* quick = window_->findChild<QCheckBox*>(QStringLiteral("quickFormat"));
        QCheckBox* extended = window_->findChild<QCheckBox*>(QStringLiteral("extendedLabel"));
        QVERIFY(quick && extended && quick->isChecked() && extended->isChecked());
        QVERIFY(!window_->findChild<QWidget*>(QStringLiteral("imageOptions"))->isVisible());
    }

    void advancedSectionsExpandAndCollapseLikeWin32()
    {
        QToolButton* drive = window_->findChild<QToolButton*>(QStringLiteral("advancedDriveButton"));
        QToolButton* format = window_->findChild<QToolButton*>(QStringLiteral("advancedFormatButton"));
        QWidget* driveOptions = window_->findChild<QWidget*>(QStringLiteral("advancedDriveOptions"));
        QWidget* formatOptions = window_->findChild<QWidget*>(QStringLiteral("advancedFormatOptions"));
        QWidget* saveDevice = window_->findChild<QWidget*>(QStringLiteral("saveDeviceButton"));
        QVERIFY(drive && format && driveOptions && formatOptions && saveDevice);
        QVERIFY(!driveOptions->isVisible() && !formatOptions->isVisible());
        QVERIFY(!saveDevice->isVisible());
        const int initialHeight = window_->height();
        QTest::mouseClick(drive, Qt::LeftButton);
        QTest::mouseClick(format, Qt::LeftButton);
        QVERIFY(driveOptions->isVisible() && formatOptions->isVisible());
        QVERIFY(saveDevice->isVisible());
        QVERIFY(!driveOptions->isEnabled() && !formatOptions->isEnabled());
        QCOMPARE(drive->text(), QStringLiteral("Hide advanced drive properties"));
        QCOMPARE(format->arrowType(), Qt::UpArrow);
        QTRY_VERIFY_WITH_TIMEOUT(window_->height() > initialHeight, uiTimeoutMs);
        const QString screenshot = qEnvironmentVariable("RUFUS_TEST_EXPANDED_SCREENSHOT");
        if (!screenshot.isEmpty())
            QVERIFY(window_->grab().save(screenshot));
        QTest::mouseClick(drive, Qt::LeftButton);
        QTest::mouseClick(format, Qt::LeftButton);
        QVERIFY(!driveOptions->isVisible() && !formatOptions->isVisible());
        QTRY_COMPARE_WITH_TIMEOUT(window_->height(), initialHeight, uiTimeoutMs);
    }

    void logCanBeSavedAndCleared()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QTest::mouseClick(logButton_, Qt::LeftButton);
        QPlainTextEdit* text = window_->findChild<QPlainTextEdit*>(QStringLiteral("logText"));
        QPushButton* save = window_->findChild<QPushButton*>(QStringLiteral("saveLogButton"));
        QPushButton* clear = window_->findChild<QPushButton*>(QStringLiteral("clearLogButton"));
        QVERIFY(text && save && clear);
        const QByteArray expected = text->toPlainText().toUtf8();
        QTest::mouseClick(save, Qt::LeftButton);
        QFileDialog* dialog = window_->findChild<QFileDialog*>(QStringLiteral("saveLogDialog"));
        QVERIFY(dialog && dialog->isVisible());
        dialog->setDirectory(directory.path());
        dialog->selectFile(QStringLiteral("rufus.log"));
        QDialogButtonBox* buttons = dialog->findChild<QDialogButtonBox*>();
        QVERIFY(buttons && buttons->button(QDialogButtonBox::Save));
        QTest::mouseClick(buttons->button(QDialogButtonBox::Save), Qt::LeftButton);
        QFile output(directory.filePath(QStringLiteral("rufus.log")));
        QTRY_VERIFY_WITH_TIMEOUT(output.exists(), uiTimeoutMs);
        QVERIFY(output.open(QIODevice::ReadOnly));
        QCOMPARE(output.readAll(), expected);
        QTest::mouseClick(clear, Qt::LeftButton);
        QVERIFY(text->toPlainText().isEmpty());
    }

    void layoutFitsLargeFonts()
    {
        QFont font = window_->font();
        font.setPointSize(18);
        window_->setFont(font);
        controller_->setDevices(testDevices());
        chooseFirstDevice();
        window_->resize(window_->sizeHint());
        QCoreApplication::processEvents();
        QWidget* content = window_->centralWidget();
        const QList<QWidget*> controls = {
            deviceCombo_, bootCombo_, selectButton_, startButton_, logButton_
        };
        for (QWidget* control : controls)
        {
            const QRect bounds(control->mapTo(content, QPoint(0, 0)), control->size());
            QVERIFY2(content->rect().contains(bounds), qPrintable(control->objectName()));
            QTRY_VERIFY2_WITH_TIMEOUT(control->height() >= control->minimumSizeHint().height(),
                     qPrintable(QStringLiteral("%1: height %2, minimum %3, hint %4")
                         .arg(control->objectName()).arg(control->height())
                         .arg(control->minimumSizeHint().height()).arg(control->sizeHint().height())), 1000);
        }
    }
};

QTEST_MAIN(TestRufusWindow)
#include "test_rufus_window.moc"
