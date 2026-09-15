// SPDX-License-Identifier: GPL-3.0-or-later
#include "main_window.h"
#include "selection_controller.h"
#include "ui_main_window.h"

#include <QCloseEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QLocale>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QSaveFile>
#include <QShortcut>
#include <QSignalBlocker>
#include <QVBoxLayout>

// Explicit initialization keeps the resource object in the static library
// when linking both the application and functional tests.
static void initializeResources()
{
    static bool initialized = false;
    if (!initialized)
    {
        Q_INIT_RESOURCE(rufus_icons);
        initialized = true;
    }
}

namespace rufus
{

MainWindow::MainWindow(SelectionController& controller, QWidget* parent)
    : QMainWindow(parent), controller_(controller), ui_(std::make_unique<Ui::RufusWindow>())
{
    initializeResources();
    ui_->setupUi(this);
    configureToolbarIcons();
    for (QLabel* heading : {ui_->driveHeader, ui_->formatHeader, ui_->statusHeader})
    {
        QFont font = heading->font();
        font.setWeight(QFont::DemiBold);
        heading->setFont(font);
        heading->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);
    }
    setWindowFlags(Qt::Window | Qt::WindowTitleHint | Qt::WindowSystemMenuHint
                   | Qt::WindowMinimizeButtonHint | Qt::WindowCloseButtonHint);
    layout()->setSizeConstraint(QLayout::SetFixedSize);
    ui_->imageOptions->hide();
    ui_->advancedDriveOptions->hide();
    ui_->advancedFormatOptions->hide();
    ui_->saveDeviceButton->hide();

    statusLabel_ = new QLabel(this);
    statusLabel_->setObjectName(QStringLiteral("statusLabel"));
    statusLabel_->setTextFormat(Qt::PlainText);
    statusLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    ui_->statusBar->addWidget(statusLabel_, 1);
    createLogWindow();

    connect(ui_->deviceCombo, &QComboBox::activated, this, &MainWindow::chooseDevice);
    connect(ui_->selectImageButton, &QPushButton::clicked, this, &MainWindow::chooseImage);
    connect(ui_->advancedDriveButton, &QToolButton::clicked, this, &MainWindow::toggleAdvancedDrive);
    connect(ui_->advancedFormatButton, &QToolButton::clicked, this, &MainWindow::toggleAdvancedFormat);
    connect(ui_->logButton, &QToolButton::clicked, this, &MainWindow::showLog);
    connect(ui_->closeButton, &QPushButton::clicked, this, &QWidget::close);
    connect(ui_->startButton, &QPushButton::clicked, &controller_, &SelectionController::startWrite);
    connect(&controller_.writer(), &RawWriteController::confirmationRequired, this, &MainWindow::confirmWrite);
    QShortcut* escape = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    connect(escape, &QShortcut::activated, this, &QWidget::close);
    connect(&controller_, &SelectionController::selectionChanged,
            this, &MainWindow::refreshSelection);
    refreshSelection();
    ui_->selectImageButton->setFocus();
}

MainWindow::~MainWindow() = default;

void MainWindow::configureToolbarIcons()
{
    struct ToolbarIcon
    {
        QToolButton* button;
        const char* name;
    };
    const ToolbarIcon icons[] = {
        {ui_->languageButton, "lang"}, {ui_->aboutButton, "info"},
        {ui_->settingsButton, "settings"}, {ui_->logButton, "log"},
        {ui_->saveDeviceButton, "save"}, {ui_->hashButton, "hash"}
    };
    for (const ToolbarIcon& entry : icons)
    {
        QIcon icon;
        for (const int size : {16, 24, 32})
            icon.addFile(QStringLiteral(":/rufus/%1-%2.png")
                .arg(QLatin1String(entry.name)).arg(size), QSize(size, size));
        entry.button->setIcon(icon);
    }
}

void MainWindow::createLogWindow()
{
    logWindow_ = new QDialog(this);
    logWindow_->setObjectName(QStringLiteral("logWindow"));
    logWindow_->setWindowTitle(tr("Log"));
    QVBoxLayout* layout = new QVBoxLayout(logWindow_);
    log_ = new QPlainTextEdit(logWindow_);
    log_->setObjectName(QStringLiteral("logText"));
    log_->setReadOnly(true);
    log_->setMaximumBlockCount(2000);
    log_->appendPlainText(tr("Rufus Linux preview. No media operations have been started."));
    QDialogButtonBox* buttons = new QDialogButtonBox(logWindow_);
    QPushButton* clear = buttons->addButton(tr("Clear"), QDialogButtonBox::ActionRole);
    clear->setObjectName(QStringLiteral("clearLogButton"));
    QPushButton* save = buttons->addButton(tr("Save"), QDialogButtonBox::ActionRole);
    save->setObjectName(QStringLiteral("saveLogButton"));
    buttons->addButton(QDialogButtonBox::Close);
    layout->addWidget(log_);
    layout->addWidget(buttons);
    connect(clear, &QPushButton::clicked, log_, &QPlainTextEdit::clear);
    connect(save, &QPushButton::clicked, this, &MainWindow::chooseLogFile);
    connect(buttons, &QDialogButtonBox::rejected, logWindow_, &QDialog::reject);
    connect(&controller_, &SelectionController::logMessage,
            log_, &QPlainTextEdit::appendPlainText);
    logWindow_->resize(640, 500);
}

void MainWindow::refreshSelection()
{
    const Selection& selection = controller_.selection();
    const QSignalBlocker blocker(ui_->deviceCombo);
    ui_->deviceCombo->clear();
    for (const Device& device : selection.devices)
    {
        const QString size = QLocale().formattedDataSize(qint64(device.size));
        ui_->deviceCombo->addItem(tr("%1 — %2 (%3)").arg(device.name, device.path, size), device.id);
    }
    ui_->deviceCombo->setCurrentIndex(ui_->deviceCombo->findData(selection.deviceId));
    const bool analyzing = controller_.isAnalyzing();
    const bool busy = analyzing || controller_.writer().isActive();
    ui_->deviceCombo->setEnabled(!busy && !selection.devices.isEmpty());
    ui_->selectImageButton->setEnabled(!busy);
    ui_->bootCombo->setEnabled(!busy);
    ui_->advancedDriveButton->setEnabled(!busy);
    ui_->advancedFormatButton->setEnabled(!busy);
    ui_->closeButton->setText(busy ? tr("CANCEL") : tr("CLOSE"));
    ui_->closeButton->setEnabled(selection.imageState != ImageState::Cancelling
        && controller_.writer().status().state != WriteState::Cancelling);
    ui_->startButton->setEnabled(controller_.canStartWrite());
    // Rufus clears these checks for a DD-only image; formatting stays disabled.
    ui_->quickFormat->setChecked(!selection.rawImage);
    ui_->extendedLabel->setChecked(!selection.rawImage);
    ui_->deviceCombo->setToolTip(ui_->deviceCombo->currentText());
    ui_->bootCombo->setItemText(0, selection.imagePath.isEmpty()
        ? tr("Disk or ISO image (Please select)") : QFileInfo(selection.imagePath).fileName());
    ui_->bootCombo->setItemData(0, selection.imagePath);

    QString status = tr("Device discovery unavailable; writing disabled.");
    if (selection.discoveryAvailable)
        status = tr("%n device(s) found", nullptr, selection.devices.size());
    if (!selection.discoveryProblem.isEmpty())
        status = selection.discoveryProblem;
    QString imageDetails;
    if (selection.imageState != ImageState::None)
        status = controller_.imageStatusText();
    if (!selection.imagePath.isEmpty())
    {
        imageDetails = tr("%1\n%2\n%3").arg(selection.imagePath,
            QLocale().formattedDataSize(qint64(selection.imageSize)), controller_.imageStatusText());
    }
    ui_->volumeEdit->setText(selection.imageAnalysis.tree_complete
        ? QString::fromUtf8(selection.imageAnalysis.report.label) : QString());
    ui_->progressBar->setRange(0, analyzing ? 0 : 100);
    ui_->progressBar->setValue(0);
    QString progress = tr("READY");
    if (selection.imageState == ImageState::Scanning)
        progress = tr("Scanning image...");
    else if (selection.imageState == ImageState::Cancelling)
        progress = tr("Cancelling...");
    else if (selection.imageState == ImageState::Cancelled)
        progress = tr("CANCELLED");
    else if (selection.imageState == ImageState::Failed)
        progress = tr("FAILED");
    else if (selection.imageState == ImageState::Partial)
        progress = tr("INCOMPLETE");
    ui_->progressBar->setFormat(progress);
    ui_->bootCombo->setToolTip(imageDetails);
    statusLabel_->setText(status);
    statusLabel_->setToolTip(status);
    refreshWrite();
}

void MainWindow::refreshWrite()
{
    RawWriteController& writer = controller_.writer();
    const WriteState state = writer.status().state;
    if (state != WriteState::AwaitingConfirmation && writeConfirmation_)
        writeConfirmation_->reject();
    if (!writer.isWriting() && cancelConfirmation_)
        cancelConfirmation_->reject();
    if (state == WriteState::Idle)
        return;
    statusLabel_->setText(writer.statusText());
    statusLabel_->setToolTip(writer.statusText());
    const bool waiting = state == WriteState::Checking || state == WriteState::Authorizing
        || (writer.isWriting() && writer.status().outcome.io.phase == RAW_WRITE_SYNC);
    ui_->progressBar->setRange(0, waiting ? 0 : 100);
    ui_->progressBar->setValue(writer.percent());
    QString text = writer.statusText();
    if (state == WriteState::Complete)
        text = tr("READY");
    else if (state == WriteState::Cancelled)
        text = tr("CANCELLED");
    else if (state == WriteState::Failed)
        text = tr("FAILED");
    else if (state == WriteState::Writing && !waiting)
        text = writer.status().outcome.io.phase == RAW_WRITE_VERIFY ? tr("Verifying image: %p%") : tr("Writing image: %p%");
    ui_->progressBar->setFormat(text);
}

void MainWindow::confirmWrite()
{
    const RawWritePlan* plan = controller_.writer().plan();
    if (!plan || controller_.writer().status().state != WriteState::AwaitingConfirmation)
        return;
    const Device& device = plan->target();
    const QString target = tr("%1 — %2 (%3)").arg(device.name, device.path,
        QLocale().formattedDataSize(qint64(device.size)));
    writeConfirmation_ = new QMessageBox(QMessageBox::Warning, tr("Rufus"),
        tr("WARNING: ALL DATA ON DEVICE '%1' WILL BE DESTROYED.\n"
           "To continue with this operation, click OK. To quit click CANCEL.").arg(target),
        QMessageBox::Ok | QMessageBox::Cancel, this);
    writeConfirmation_->setObjectName(QStringLiteral("writeConfirmation"));
    writeConfirmation_->setTextFormat(Qt::PlainText);
    writeConfirmation_->setInformativeText(tr("Image: %1\n%2 — DD Image")
        .arg(plan->sourcePath(), QLocale().formattedDataSize(qint64(plan->source().info.st_size))));
    writeConfirmation_->setDefaultButton(QMessageBox::Cancel);
    writeConfirmation_->setAttribute(Qt::WA_DeleteOnClose);
    connect(writeConfirmation_, &QDialog::finished, this, &MainWindow::writeConfirmationFinished);
    writeConfirmation_->open();
}

void MainWindow::writeConfirmationFinished(int result)
{
    writeConfirmation_ = nullptr;
    if (result == QMessageBox::Ok)
        controller_.writer().confirm();
    else if (controller_.writer().status().state == WriteState::AwaitingConfirmation)
        controller_.writer().cancel();
}

void MainWindow::confirmCancel()
{
    if (cancelConfirmation_)
        return;
    cancelConfirmation_ = new QMessageBox(QMessageBox::Warning, tr("Rufus - Cancellation"),
        tr("Cancelling may leave the device in an UNUSABLE state.\n"
           "If you are sure you want to cancel, click YES. Otherwise, click NO."),
        QMessageBox::Yes | QMessageBox::No, this);
    cancelConfirmation_->setObjectName(QStringLiteral("cancelConfirmation"));
    cancelConfirmation_->setTextFormat(Qt::PlainText);
    cancelConfirmation_->setDefaultButton(QMessageBox::No);
    cancelConfirmation_->setAttribute(Qt::WA_DeleteOnClose);
    connect(cancelConfirmation_, &QDialog::finished, this, &MainWindow::cancelConfirmationFinished);
    cancelConfirmation_->open();
}

void MainWindow::cancelConfirmationFinished(int result)
{
    cancelConfirmation_ = nullptr;
    if (result == QMessageBox::Yes)
        controller_.writer().cancel();
}

void MainWindow::chooseDevice(int index)
{
    controller_.selectDevice(ui_->deviceCombo->itemData(index).toString());
}

void MainWindow::chooseImage()
{
    if (!imageDialog_)
    {
        imageDialog_ = new QFileDialog(this, tr("Select a boot image"));
        imageDialog_->setObjectName(QStringLiteral("imageDialog"));
        imageDialog_->setFileMode(QFileDialog::ExistingFile);
        imageDialog_->setAcceptMode(QFileDialog::AcceptOpen);
        imageDialog_->setNameFilter(tr("Disk images (*.iso *.img);;All files (*)"));
        connect(imageDialog_, &QFileDialog::fileSelected,
                &controller_, &SelectionController::selectImage);
    }
    if (!controller_.selection().imagePath.isEmpty())
        imageDialog_->setDirectory(QFileInfo(controller_.selection().imagePath).absolutePath());
    imageDialog_->open();
}

void MainWindow::showLog()
{
    logWindow_->show();
    logWindow_->raise();
    logWindow_->activateWindow();
}

void MainWindow::chooseLogFile()
{
    if (!saveLogDialog_)
    {
        saveLogDialog_ = new QFileDialog(logWindow_, tr("Save log"));
        saveLogDialog_->setObjectName(QStringLiteral("saveLogDialog"));
        saveLogDialog_->setAcceptMode(QFileDialog::AcceptSave);
        saveLogDialog_->setNameFilter(tr("Log files (*.log);;All files (*)"));
        saveLogDialog_->setDefaultSuffix(QStringLiteral("log"));
        saveLogDialog_->selectFile(QStringLiteral("rufus.log"));
        connect(saveLogDialog_, &QFileDialog::fileSelected, this, &MainWindow::saveLogFile);
    }
    saveLogDialog_->open();
}

void MainWindow::saveLogFile(const QString& path)
{
    const QByteArray contents = log_->toPlainText().toUtf8();
    QSaveFile file(path);
    bool saved = file.open(QIODevice::WriteOnly);
    if (saved)
        saved = file.write(contents) == contents.size();
    if (saved)
        saved = file.commit();
    if (!saved)
    {
        const QString message = tr("Cannot save the log: %1").arg(file.errorString());
        log_->appendPlainText(message);
        statusLabel_->setText(message);
        statusLabel_->setToolTip(message);
    }
}

void MainWindow::toggleAdvancedDrive()
{
    const bool expanded = !ui_->advancedDriveOptions->isVisible();
    ui_->advancedDriveOptions->setVisible(expanded);
    ui_->saveDeviceButton->setVisible(expanded);
    ui_->advancedDriveButton->setText(expanded
        ? tr("Hide advanced drive properties") : tr("Show advanced drive properties"));
    ui_->advancedDriveButton->setArrowType(expanded ? Qt::UpArrow : Qt::DownArrow);
    adjustSize();
}

void MainWindow::toggleAdvancedFormat()
{
    const bool expanded = !ui_->advancedFormatOptions->isVisible();
    ui_->advancedFormatOptions->setVisible(expanded);
    ui_->advancedFormatButton->setText(expanded
        ? tr("Hide advanced format options") : tr("Show advanced format options"));
    ui_->advancedFormatButton->setArrowType(expanded ? Qt::UpArrow : Qt::DownArrow);
    adjustSize();
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    if (controller_.writer().isActive())
    {
        if (controller_.writer().status().state == WriteState::Cancelling)
        {
            event->ignore();
            return;
        }
        if (controller_.writer().isWriting())
            confirmCancel();
        else
            controller_.writer().cancel();
        event->ignore();
        return;
    }
    if (controller_.isAnalyzing())
    {
        controller_.cancelAnalysis();
        event->ignore();
        return;
    }
    if (imageDialog_)
        imageDialog_->close();
    if (saveLogDialog_)
        saveLogDialog_->close();
    logWindow_->close();
    QMainWindow::closeEvent(event);
}

} // namespace rufus
