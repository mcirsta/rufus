// SPDX-License-Identifier: GPL-3.0-or-later
#include "main_window.h"
#include "device_policy.h"
#include "platform/device_operations.h"
#include "job_priority.h"
#include <QKeyEvent>
#include "platform/format_options.h"
#include "about_dialog.h"
#include "policy_update_dialog.h"
#include "selection_controller.h"
#include "image_mode_dialog.h"
#include "image_hash_dialog.h"
#include "windows_user_dialog.h"
#include "windows_image_dialog.h"
#include "freedos_locale_dialog.h"
#include "platform/syslinux_native.h"
#include "../../res/grub2/grub2_version.h"
#include "../../res/grub/grub_version.h"
#include "ui_main_window.h"
#include "rufus_build_info.h"
#include "device_size.h"
#include "image_select_button.h"
#include "image_download_dialog.h"
#include <QDesktopServices>

#include <QApplication>
#include <QCloseEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QUrl>
#include <QDialog>
#include <QDateTime>
#include <QDir>
#include <QDebug>
#include <QDialogButtonBox>
#include <QDoubleValidator>
#include <cmath>
#include <climits>
#include <cerrno>
#include <cstring>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QLocale>
#include <QMessageBox>
#include <QPlainTextEdit>
#include "application_storage.h"
#include "application_language.h"
#include <QActionGroup>
#include <QMenu>
#include "platform/iso_export.h"
#include <QShortcut>
#include <QSignalBlocker>
#include <QStandardItemModel>
#include <QVBoxLayout>

static QString fileSystemName(int filesystem)
{
    switch (filesystem)
    {
    case FS_FAT16: return QStringLiteral("FAT16");
    case FS_FAT32: return QStringLiteral("FAT32");
    case FS_NTFS: return QStringLiteral("NTFS");
    case FS_UDF: return QStringLiteral("UDF");
    case FS_EXFAT: return QStringLiteral("exFAT");
    case FS_EXT2: return QStringLiteral("ext2");
    case FS_EXT3: return QStringLiteral("ext3");
    default: return {};
    }
}

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
namespace
{
constexpr int shortcutStatusTimeoutMs = 3500; // Win32 STATUS_MSG_TIMEOUT.
constexpr int clockTickMs = 1000;
constexpr int nonBootableIndex = 0;
constexpr int imageBootIndex = 1;
constexpr int bootTypeRole = Qt::UserRole + 1;
}

MainWindow::MainWindow(SelectionController& controller, QWidget* parent,
    const ApplicationUpdateSources& applicationSources, const PolicyUpdateSources& policySources)
    : QMainWindow(parent), controller_(controller), ui_(std::make_unique<Ui::RufusWindow>()),
      policyUpdates_(this), applicationUpdates_(this, applicationSources), policySources_(policySources), applicationUpdateTimer_(this)
{
    initializeResources();
    ApplicationLanguages();
    displayLanguage_ = SelectedApplicationLanguage();
    ui_->setupUi(this);
    ApplicationSettings desktopSettings;
    ApplyApplicationAppearance(desktopSettings.value(QStringLiteral("DarkMode"),0).toInt());
    const int savedPriority = desktopSettings.value(QStringLiteral("DefaultThreadPriority"),0).toInt();
    SetJobPriority(qBound(MinimumJobPriority-DefaultJobPriority,savedPriority,MaximumJobPriority-DefaultJobPriority)+DefaultJobPriority);
    qApp->installEventFilter(this);
    setWindowTitle(tr("Rufus %1").arg(QLatin1String(RUFUS_LINUX_VERSION))
        + (IsPortableApplication() ? tr(" (Portable)") : QString()));
    ApplicationSettings settings;
    persistentLog_ = settings.value(QStringLiteral("PersistentLog"), false).toBool();
    properSizeUnits_ = settings.value(QStringLiteral("UseProperSizeUnits"), false).toBool();
    ui_->bootCombo->setItemData(nonBootableIndex, BT_NON_BOOTABLE, bootTypeRole);
    ui_->bootCombo->setItemData(imageBootIndex, BT_IMAGE, bootTypeRole);
    ui_->bootCombo->addItem(QStringLiteral("MS-DOS"));
    ui_->bootCombo->setItemData(ui_->bootCombo->count() - 1, BT_MSDOS, bootTypeRole);
    ui_->bootCombo->addItem(QStringLiteral("FreeDOS"));
    ui_->bootCombo->setItemData(ui_->bootCombo->count() - 1, BT_FREEDOS, bootTypeRole);
    configureToolbarIcons();
    for (QToolButton* button : {ui_->advancedDriveButton, ui_->advancedFormatButton})
        button->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    for (QLabel* heading : {ui_->driveHeader, ui_->formatHeader, ui_->statusHeader})
    {
        QFont font = heading->font();
        font.setWeight(QFont::DemiBold);
        heading->setFont(font);
        heading->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    }
    setWindowFlags(Qt::Window | Qt::WindowTitleHint | Qt::WindowSystemMenuHint
                   | Qt::WindowMinimizeButtonHint | Qt::WindowCloseButtonHint);
    layout()->setSizeConstraint(QLayout::SetFixedSize);
    setAcceptDrops(true);
    ui_->imageOptions->hide();
    ui_->persistenceSlider->hide();
    ui_->persistenceSize->hide();
    ui_->persistenceUnits->hide();
    ui_->persistenceSlider->setRange(0, 1000);
    ui_->persistenceSize->setValidator(new QDoubleValidator(0, 1000000000, 3, ui_->persistenceSize));
    ui_->persistenceSize->setMaximumWidth(95);
    ui_->persistenceUnits->addItem(QStringLiteral("MB"), QVariant::fromValue(PersistenceUnitBytes));
    ui_->persistenceUnits->addItem(QStringLiteral("GB"), QVariant::fromValue(1024 * PersistenceUnitBytes));
    ui_->persistenceUnits->addItem(QStringLiteral("TB"), QVariant::fromValue(1024 * 1024 * PersistenceUnitBytes));
    const QString persistenceTip = tr("Keep files and settings across live USB boots. Use 0 to disable persistence.");
    ui_->persistenceSlider->setToolTip(persistenceTip);
    ui_->persistenceSize->setToolTip(persistenceTip);
    ui_->imageOptionCombo->addItem(tr("Standard Windows installation"), false);
    ui_->imageOptionCombo->addItem(tr("Windows To Go"), true);
    ui_->passesCombo->clear();
    ui_->passesCombo->addItems({tr("1 pass"), tr("2 passes"), tr("4 passes (SLC)"), tr("4 passes (MLC)"), tr("4 passes (TLC)")});
    connect(ui_->badBlocks, &QCheckBox::clicked, &controller_, &SelectionController::setBadBlocksEnabled);
    connect(ui_->uefiValidation, &QCheckBox::clicked, &controller_, &SelectionController::setRuntimeValidation);
    ui_->uefiValidation->setToolTip(tr("Check media files on each UEFI boot, then start the original bootloader."));
    connect(ui_->passesCombo, &QComboBox::activated, &controller_, &SelectionController::setBadBlocksPattern);
    connect(&controller_, &SelectionController::badBlocksReviewRequired, this, &MainWindow::showBadBlocksReport);
    QShortcut* fakeDetection = new QShortcut(QKeySequence(Qt::ALT | Qt::Key_B), this);
    fakeDetection->setAutoRepeat(false);
    connect(fakeDetection, &QShortcut::activated, this, &MainWindow::toggleFakeDetection);
    ui_->advancedDriveOptions->hide();
    ui_->advancedFormatOptions->hide();

    statusLabel_ = new QLabel(this);
    statusLabel_->setObjectName(QStringLiteral("statusLabel"));
    statusLabel_->setTextFormat(Qt::PlainText);
    statusLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    ui_->statusBar->addWidget(statusLabel_, 1);
    elapsedLabel_ = new QLabel(QStringLiteral("00:00:00"), this);
    elapsedLabel_->setObjectName(QStringLiteral("elapsedLabel"));
    ui_->statusBar->addPermanentWidget(elapsedLabel_);
    elapsedTimer_.setInterval(clockTickMs);
    connect(&elapsedTimer_, &QTimer::timeout, this, &MainWindow::updateElapsedTime);
    imageNotice_ = new QMessageBox(this);
    imageNotice_->setObjectName(QStringLiteral("imageNotice"));
    imageNotice_->setTextFormat(Qt::PlainText);
    imageNotice_->setStandardButtons(QMessageBox::Ok);
    connect(imageNotice_, &QDialog::finished, this, &MainWindow::focusStart);
    // Qt styles omit a QProgressBar's text in marquee mode. Rufus keeps the
    // operation caption over that bar, including while scanning an image.
    busyLabel_ = new QLabel(ui_->progressBar);
    busyLabel_->setObjectName(QStringLiteral("busyProgressLabel"));
    busyLabel_->setTextFormat(Qt::PlainText);
    busyLabel_->setAlignment(Qt::AlignCenter);
    busyLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    busyLabel_->setAttribute(Qt::WA_TransparentForMouseEvents);
    QHBoxLayout* progressLayout = new QHBoxLayout(ui_->progressBar);
    progressLayout->setContentsMargins(0, 0, 0, 0);
    progressLayout->addWidget(busyLabel_);
    createLogWindow();
    appendLanguageNotice();

    connect(ui_->selectImageButton,&ImageSelectButton::modeChanged,this,&MainWindow::refreshSelection);
    connect(ui_->deviceCombo, &QComboBox::activated, this, &MainWindow::chooseDevice);
    connect(ui_->selectImageButton, &QPushButton::clicked, this, &MainWindow::chooseImage);
    connect(ui_->saveDeviceButton, &QToolButton::clicked, this, &MainWindow::chooseSaveImage);
    connect(ui_->bootCombo, &QComboBox::activated, this, &MainWindow::chooseBootMode);
    connect(ui_->advancedDriveButton, &QToolButton::clicked, this, &MainWindow::toggleAdvancedDrive);
    connect(ui_->extendedLabel, &QCheckBox::clicked, &controller_, &SelectionController::setExtendedLabel);
    connect(ui_->oldBiosFixes, &QCheckBox::clicked, &controller_, &SelectionController::setOldBiosFixes);
    connect(ui_->quickFormat, &QCheckBox::clicked, &controller_, &SelectionController::setQuickFormat);
    connect(ui_->advancedFormatButton, &QToolButton::clicked, this, &MainWindow::toggleAdvancedFormat);
    connect(ui_->languageButton, &QToolButton::clicked, this, &MainWindow::showLanguages);
    connect(ui_->logButton, &QToolButton::clicked, this, &MainWindow::showLog);
    connect(ui_->aboutButton, &QToolButton::clicked, this, &MainWindow::showAbout);
    ui_->settingsButton->setEnabled(true);
    ui_->settingsButton->setToolTip(tr("Rufus update policy"));
    connect(ui_->settingsButton, &QToolButton::clicked, this, &MainWindow::showPolicyUpdates);
    connect(&policyUpdates_, &PolicyUpdates::dbxAvailable, this, &MainWindow::confirmDbxUpdate);
    connect(&policyUpdates_, &PolicyUpdates::progress, this, &MainWindow::policyProgress);
    connect(&policyUpdates_, &PolicyUpdates::completed, this, &MainWindow::policyCompleted);
    connect(&policyUpdates_, &PolicyUpdates::logMessage, this, &MainWindow::appendLog);
    connect(&applicationUpdates_, &ApplicationUpdates::completed,this,&MainWindow::applicationCompleted);
    applicationUpdateTimer_.setSingleShot(true);
    applicationUpdateTimer_.setObjectName(QStringLiteral("applicationUpdateTimer"));
    connect(&applicationUpdateTimer_,&QTimer::timeout,this,&MainWindow::automaticApplicationCheck);
    connect(&applicationUpdates_,&ApplicationUpdates::logMessage,this,&MainWindow::appendLog);
    struct DesktopShortcut { QKeyCombination keys; void (MainWindow::*method)(); };
    const DesktopShortcut desktopShortcuts[] = {
        {Qt::ALT|Qt::Key_D,&MainWindow::clearDownloadedResources},
        {Qt::CTRL|Qt::ALT|Qt::Key_D,&MainWindow::toggleDarkAppearance},
        {Qt::CTRL|Qt::ALT|Qt::Key_Y,&MainWindow::toggleForceTimestampUpdate},
        {Qt::ALT|Qt::Key_Plus,&MainWindow::increaseJobPriority},
        {Qt::ALT|Qt::Key_Equal,&MainWindow::increaseJobPriority},
        {Qt::ALT|Qt::Key_Minus,&MainWindow::decreaseJobPriority}};
    for (const auto& binding : desktopShortcuts) {
        auto* shortcut = new QShortcut(QKeySequence(binding.keys),this);
        shortcut->setContext(Qt::ApplicationShortcut);
        shortcut->setAutoRepeat(false);
        connect(shortcut,&QShortcut::activated,this,binding.method);
    }
    auto* usbDebug = new QShortcut(QKeySequence(Qt::ALT|Qt::Key_Period),this);
    usbDebug->setContext(Qt::ApplicationShortcut);
    usbDebug->setAutoRepeat(false);
    connect(usbDebug,&QShortcut::activated,&controller_,&SelectionController::toggleUsbDebug);
    QShortcut* forceUpdate = new QShortcut(QKeySequence(Qt::ALT | Qt::Key_Y),this);
    forceUpdate->setAutoRepeat(false);
    connect(forceUpdate,&QShortcut::activated,this,&MainWindow::toggleForceUpdate);
    policyTimer_.setSingleShot(true);
    connect(&policyTimer_, &QTimer::timeout, this, &MainWindow::automaticPolicyCheck);
    connect(ui_->hashButton, &QToolButton::clicked, &controller_, &SelectionController::startHashes);
    connect(&controller_.hasher(), &ImageHashController::finished, this, &MainWindow::showHashResult);
    connect(ui_->closeButton, &QPushButton::clicked, this, &QWidget::close);
    connect(ui_->startButton, &QPushButton::clicked, &controller_, &SelectionController::startWrite);
    connect(&controller_, &SelectionController::revocationWarningRequired, this, &MainWindow::confirmRevokedImage);
    connect(&controller_.writer(), &RawWriteController::confirmationRequired, this, &MainWindow::confirmWrite);
    connect(&controller_.isoWriter(), &IsoWriteController::modeRequired, this, &MainWindow::chooseIsoMode);
    connect(&controller_.isoWriter(), &IsoWriteController::windowsOptionsRequired, this, &MainWindow::chooseWindowsOptions);
    connect(&controller_.isoWriter(), &IsoWriteController::windowsArchiveRequired, this, &MainWindow::chooseWindowsArchive);
    connect(&controller_.isoWriter(), &IsoWriteController::windowsEditionRequired, this, &MainWindow::chooseWindowsEdition);
    connect(&controller_.isoWriter(), &IsoWriteController::msDosDownloadRequired, this, &MainWindow::chooseMsDosDownload);
    connect(&controller_.isoWriter(), &IsoWriteController::dosOptionsRequired, this, &MainWindow::chooseDosOptions);
    connect(&controller_.isoWriter(), &IsoWriteController::grubDownloadRequired, this, &MainWindow::chooseGrubDownload);
    connect(&controller_.isoWriter(), &IsoWriteController::confirmationRequired, this, &MainWindow::confirmIsoWrite);
    connect(ui_->persistenceSize, &QLineEdit::editingFinished, this, &MainWindow::editPersistenceSize);
    connect(ui_->persistenceSlider, &QSlider::valueChanged, this, &MainWindow::movePersistenceSlider);
    connect(ui_->persistenceUnits, &QComboBox::activated, this, &MainWindow::choosePersistenceUnits);
    connect(ui_->volumeEdit, &QLineEdit::textEdited, &controller_, &SelectionController::setVolumeLabel);
    connect(ui_->clusterCombo, &QComboBox::activated, this, &MainWindow::chooseCluster);
    connect(ui_->fileSystemCombo, &QComboBox::activated, this, &MainWindow::chooseFileSystem);
    connect(ui_->partitionCombo, &QComboBox::activated, this, &MainWindow::choosePartition);
    connect(ui_->imageOptionCombo, &QComboBox::activated, this, &MainWindow::chooseWindowsImageOption);
    connect(ui_->targetCombo, &QComboBox::activated, this, &MainWindow::chooseWindowsTarget);
    struct ImageOptionShortcut { Qt::Key key; void (SelectionController::*method)(); };
    QShortcut* nonUsb = new QShortcut(QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_F), this);
    nonUsb->setAutoRepeat(false);
    connect(nonUsb, &QShortcut::activated, &controller_, &SelectionController::toggleNonUsbRemovable);
    const ImageOptionShortcut imageShortcuts[] = {
        {Qt::Key_G, &SelectionController::toggleVirtualDisks}, {Qt::Key_W, &SelectionController::toggleVmwareDisks},
        {Qt::Key_I, &SelectionController::toggleIsoSupport}, {Qt::Key_J, &SelectionController::toggleJoliet},
        {Qt::Key_K, &SelectionController::toggleRockRidge}, {Qt::Key_T, &SelectionController::togglePreserveTimestamps},
        {Qt::Key_S, &SelectionController::toggleSizeChecks}, {Qt::Key_N, &SelectionController::toggleNtfsCompression},
        {Qt::Key_Q, &SelectionController::toggleFileIndexing}};
    for (const auto& binding : imageShortcuts) {
        auto* shortcut = new QShortcut(QKeySequence(Qt::ALT | binding.key), this); shortcut->setAutoRepeat(false);
        connect(shortcut, &QShortcut::activated, &controller_, binding.method);
    }
    connect(&controller_, &SelectionController::optionStatusChanged, this, &MainWindow::showReadOptionStatus);
    connect(&controller_, &SelectionController::imageOptionRejected, this, &MainWindow::showImageOptionProblem);
    QShortcut* escape = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    connect(escape, &QShortcut::activated, this, &QWidget::close);
    QShortcut* expert = new QShortcut(QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_E), this);
    expert->setAutoRepeat(false);
    connect(expert, &QShortcut::activated, this, &MainWindow::toggleExpertMode);
    QShortcut* fixedUsb = new QShortcut(QKeySequence(Qt::ALT | Qt::Key_F), this);
    fixedUsb->setAutoRepeat(false);
    connect(fixedUsb, &QShortcut::activated, this, &MainWindow::toggleFixedUsb);
    connect(ui_->listUsbHardDrives, &QCheckBox::clicked, &controller_, &SelectionController::setAllowFixedUsb);
    connect(&controller_, &SelectionController::fixedUsbChanged, this, &MainWindow::showFixedUsbStatus);
    QShortcut* windowsMbr = new QShortcut(QKeySequence(Qt::ALT | Qt::Key_A), this);
    windowsMbr->setAutoRepeat(false);
    connect(windowsMbr, &QShortcut::activated, this, &MainWindow::toggleWindowsMbr);
    QShortcut* enhanced = new QShortcut(QKeySequence(Qt::ALT | Qt::Key_E), this);
    enhanced->setAutoRepeat(false);
    connect(enhanced, &QShortcut::activated, this, &MainWindow::toggleEnhancedInstall);
    QShortcut* zero = new QShortcut(QKeySequence(Qt::ALT | Qt::Key_Z), this);
    zero->setAutoRepeat(false);
    connect(zero, &QShortcut::activated, this, &MainWindow::zeroDevice);
    QShortcut* fastZero = new QShortcut(QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_Z), this);
    fastZero->setAutoRepeat(false);
    connect(fastZero, &QShortcut::activated, this, &MainWindow::fastZeroDevice);
    QShortcut* bootMarker = new QShortcut(QKeySequence(Qt::ALT | Qt::Key_M), this);
    bootMarker->setAutoRepeat(false);
    connect(bootMarker, &QShortcut::activated, this, &MainWindow::toggleBootMarker);
    QShortcut* extraHashes = new QShortcut(QKeySequence(Qt::ALT | Qt::Key_H), this);
    extraHashes->setAutoRepeat(false);
    connect(extraHashes, &QShortcut::activated, this, &MainWindow::toggleExtraHashes);
    QShortcut* log = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_L), this);
    log->setContext(Qt::ApplicationShortcut);
    log->setAutoRepeat(false);
    connect(log, &QShortcut::activated, this, &MainWindow::toggleLog);
    QShortcut* persistent = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_P), this);
    persistent->setContext(Qt::ApplicationShortcut);
    persistent->setAutoRepeat(false);
    connect(persistent, &QShortcut::activated, this, &MainWindow::togglePersistentLog);
    QShortcut* sizeUnits = new QShortcut(QKeySequence(Qt::ALT | Qt::Key_U), this);
    sizeUnits->setAutoRepeat(false);
    connect(sizeUnits, &QShortcut::activated, this, &MainWindow::toggleSizeUnits);
    QShortcut* resetPort = new QShortcut(QKeySequence(Qt::ALT | Qt::Key_C), this);
    resetPort->setAutoRepeat(false);
    resetPort->setObjectName(QStringLiteral("resetUsbPortShortcut"));
    resetPort->setContext(Qt::ApplicationShortcut);
    connect(resetPort, &QShortcut::activated, this, &MainWindow::resetUsbPort);
    QShortcut* esp = new QShortcut(QKeySequence(Qt::ALT | Qt::Key_P), this);
    esp->setAutoRepeat(false);
    esp->setObjectName(QStringLiteral("toggleEspShortcut"));
    esp->setContext(Qt::ApplicationShortcut);
    connect(esp, &QShortcut::activated, this, &MainWindow::toggleEspType);
    QShortcut* optical = new QShortcut(QKeySequence(Qt::ALT | Qt::Key_O), this);
    optical->setAutoRepeat(false);
    optical->setObjectName(QStringLiteral("opticalCaptureShortcut"));
    optical->setContext(Qt::ApplicationShortcut);
    connect(optical, &QShortcut::activated, this, &MainWindow::chooseOpticalImage);
    QShortcut* locking = new QShortcut(QKeySequence(Qt::ALT | Qt::Key_Comma), this);
    locking->setAutoRepeat(false);
    locking->setObjectName(QStringLiteral("deviceLockingShortcut"));
    locking->setContext(Qt::ApplicationShortcut);
    connect(locking, &QShortcut::activated, this, &MainWindow::toggleDeviceLocking);
    QShortcut* reset = new QShortcut(QKeySequence(Qt::ALT | Qt::Key_R), this);
    reset->setAutoRepeat(false);
    connect(reset, &QShortcut::activated, this, &MainWindow::resetUserSettings);
    connect(&controller_, &SelectionController::selectionChanged,
            this, &MainWindow::refreshSelection);
    connect(&controller_, &SelectionController::imageSelectionFinished,
            this, &MainWindow::finishImageSelection);
    if (settings.value(QStringLiteral("ShowAdvancedDriveProperties"), false).toBool()) toggleAdvancedDrive();
    if (settings.value(QStringLiteral("ShowAdvancedFormatOptions"), false).toBool()) toggleAdvancedFormat();
    refreshSelection();
    ui_->selectImageButton->setFocus();
}

MainWindow::~MainWindow() = default;

void MainWindow::showLanguages()
{
    if (controller_.isBusy() || updatesActive()) return;
    if (!languageMenu_) {
        languageMenu_ = new QMenu(this); languageMenu_->setObjectName(QStringLiteral("applicationLanguageMenu"));
        auto* group = new QActionGroup(languageMenu_); group->setExclusive(true);
        for (const auto& locale : ApplicationLanguages().locales()) {
            QAction* action = languageMenu_->addAction(locale.name);
            action->setData(locale.tag); action->setCheckable(true); group->addAction(action);
        }
        connect(languageMenu_, &QMenu::triggered, this, &MainWindow::chooseLanguage);
    }
    const bool rtl = layoutDirection() == Qt::RightToLeft;
    const auto& locales = ApplicationLanguages().locales();
    const auto actions = languageMenu_->actions();
    for (int index = 0; index < actions.size(); ++index) {
        const auto& locale = locales[index];
        actions[index]->setText(rtl && !locale.rightToLeft
            ? QChar(0x202a) + locale.name + QChar(0x202c) : locale.name);
        actions[index]->setChecked(locale.tag == SelectedApplicationLanguage());
    }
    languageMenu_->popup(ui_->languageButton->mapToGlobal(QPoint(0, ui_->languageButton->height())));
}

void MainWindow::chooseLanguage(QAction* action)
{
    if (controller_.isBusy() || updatesActive() || action == nullptr) return;
    QString problem;
    const QString previous = SelectedApplicationLanguage();
    if (!SetApplicationLanguage(action->data().toString(), problem)) {
        appendLog(problem); ui_->statusBar->showMessage(problem);
    } else if (previous != SelectedApplicationLanguage()) appendLanguageNotice();
}

void MainWindow::changeEvent(QEvent* event)
{
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::LanguageChange && statusLabel_ != nullptr) refreshLanguage();
}

void MainWindow::refreshLanguage()
{
    const QString language = SelectedApplicationLanguage();
    if (language == displayLanguage_) return;
    displayLanguage_ = language;
    const QSignalBlocker boot(ui_->bootCombo), passes(ui_->passesCombo), options(ui_->imageOptionCombo);
    ui_->retranslateUi(this);
    // retranslateUi writes the .ui default caption; preserve the split mode.
    ui_->selectImageButton->setText(ui_->selectImageButton->downloadMode() ? tr("DOWNLOAD") : tr("SELECT"));
    setWindowTitle(tr("Rufus %1").arg(QLatin1String(RUFUS_LINUX_VERSION))
        + (IsPortableApplication() ? tr(" (Portable)") : QString()));
    const QStringList patterns = {tr("1 pass"), tr("2 passes"), tr("4 passes (SLC)"), tr("4 passes (MLC)"), tr("4 passes (TLC)")};
    for (int index = 0; index < patterns.size(); ++index) ui_->passesCombo->setItemText(index, patterns[index]);
    ui_->imageOptionCombo->setItemText(0, tr("Standard Windows installation"));
    ui_->imageOptionCombo->setItemText(1, tr("Windows To Go"));
    ui_->advancedDriveButton->setText(ui_->advancedDriveOptions->isHidden()
        ? tr("Show advanced drive properties") : tr("Hide advanced drive properties"));
    ui_->advancedFormatButton->setText(ui_->advancedFormatOptions->isHidden()
        ? tr("Show advanced format options") : tr("Hide advanced format options"));
    ui_->settingsButton->setToolTip(tr("Rufus update policy"));
    const QString tip = tr("Keep files and settings across live USB boots. Use 0 to disable persistence.");
    ui_->persistenceSize->setToolTip(tip); ui_->persistenceSlider->setToolTip(tip);
    ui_->uefiValidation->setToolTip(tr("Check media files on each UEFI boot, then start the original bootloader."));
    logWindow_->setWindowTitle(tr("Log"));
    for (const auto& name : {QStringLiteral("clearLogButton"), QStringLiteral("saveLogButton")}) {
        auto* button = logWindow_->findChild<QPushButton*>(name);
        if (button) button->setText(name.startsWith(QLatin1String("clear")) ? tr("Clear") : tr("Save"));
    }
    // Cached auxiliary dialogs are recreated on demand in the new language.
    // The full session log and every controller/selection remain alive.
    for (QDialog* dialog : {static_cast<QDialog*>(imageDialog_), static_cast<QDialog*>(saveLogDialog_),
            static_cast<QDialog*>(saveImageDialog_), static_cast<QDialog*>(downloadFileDialog_), static_cast<QDialog*>(archiveDialog_),
            static_cast<QDialog*>(downloadDialog_), static_cast<QDialog*>(policyDialog_)}) {
        if (dialog) dialog->deleteLater();
    }
    imageDialog_ = nullptr; saveLogDialog_ = nullptr; saveImageDialog_ = nullptr;
    downloadFileDialog_ = nullptr; archiveDialog_ = nullptr;
    downloadDialog_ = nullptr; policyDialog_ = nullptr;
    refreshSelection();
}


bool MainWindow::updatesActive() const
{
    return policyUpdates_.isActive() || applicationUpdates_.isActive();
}
void MainWindow::cancelUpdates()
{
    policyUpdates_.cancel(); applicationUpdates_.cancel();
}
void MainWindow::updateFrequencyChanged(int interval)
{
    schedulePolicyUpdates(interval); scheduleApplicationUpdates(interval);
}
void MainWindow::applicationPreferenceChanged()
{
    ApplicationSettings settings;
    scheduleApplicationUpdates(settings.value(QStringLiteral("UpdateCheckInterval"), -1).toInt());
}
void MainWindow::scheduleApplicationUpdates(int interval)
{
    applicationUpdateTimer_.stop();
    ApplicationSettings settings;
    if (interval <= 0 || !settings.value(QStringLiteral("ApplicationUpdateConsent"), false).toBool()) return;
    const qint64 last = settings.value(QStringLiteral("LastApplicationUpdateCheck"), QStringLiteral("0")).toString().toLongLong();
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    const qint64 remaining = applicationRetryAfter_ > now ? applicationRetryAfter_ - now : last > now ? qint64(interval) : last > 0
        ? qMax(qint64(0), qint64(interval) - (now - last)) : qint64(15);
    applicationUpdateTimer_.start(int(qMin(qint64(INT_MAX), qMax(qint64(1000), remaining * 1000))));
}
void MainWindow::automaticApplicationCheck()
{
    ApplicationSettings settings;
    const int interval = settings.value(QStringLiteral("UpdateCheckInterval"), -1).toInt();
    if (!settings.value(QStringLiteral("ApplicationUpdateConsent"), false).toBool() || interval <= 0) return;
    if (applicationRetryAfter_ > QDateTime::currentSecsSinceEpoch()) { scheduleApplicationUpdates(interval); return; }
    const qint64 last = settings.value(QStringLiteral("LastApplicationUpdateCheck"), QStringLiteral("0")).toString().toLongLong();
    if (!ApplicationUpdateDue(interval, last, QDateTime::currentSecsSinceEpoch())) { scheduleApplicationUpdates(interval); return; }
    if (controller_.isBusy() || updatesActive()) { applicationUpdateTimer_.start(1000); return; }
    startApplicationUpdates(false);
}
void MainWindow::startApplicationUpdates(bool manual)
{
    if (controller_.isBusy() || applicationUpdates_.isActive()) return;
    ApplicationSettings settings;
    const bool betas = QLatin1String(RUFUS_NATIVE_ARCH) == QLatin1String("x86_64")
        && settings.value(QStringLiteral("CheckForBetas"), false).toBool();
    applicationCheckManual_ = manual;
    applicationRetryAfter_ = 0;
    applicationUpdateStatus_ = tr("Checking for Rufus updates...");
    applicationUpdates_.setVerbose(settings.value(QStringLiteral("VerboseUpdateCheck"),0).toInt());
    if (forceWindowsTimestamp_) appendLog(QStringLiteral("Ctrl+Alt+Y: forcing native release selection. Windows Authenticode timestamp overrides do not apply to package-manager verification."));
    const int error = applicationUpdates_.start(QLatin1String(RUFUS_LINUX_VERSION), QLatin1String(RUFUS_NATIVE_ARCH), betas, forceUpdate_);
    if (error != 0) applicationUpdateStatus_ = tr("Cannot start application update check: %1").arg(QString::fromLocal8Bit(strerror(error)));
    refreshUpdateStatus();
}
void MainWindow::applicationCompleted(ApplicationUpdateResult result)
{
    applicationRelease_ = {};
    if (result.error == ECANCELED) applicationUpdateStatus_ = tr("Application update check cancelled.");
    else if (result.error != 0) applicationUpdateStatus_ = tr("Application update check failed: %1").arg(QString::fromLocal8Bit(strerror(result.error)));
    else {
        ApplicationSettings settings;
        settings.setValue(QStringLiteral("LastApplicationUpdateCheck"), QString::number(result.serverTime));
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        if (result.serverTime > 0 && (now > result.serverTime + 600 || now < result.serverTime - 600))
            appendLog(QStringLiteral("IMPORTANT: Your local clock is more than 10 minutes in the %1. Unless you fix this, "
                "Rufus may not be able to check for updates...").arg(now > result.serverTime ? QStringLiteral("future") : QStringLiteral("past")));
        if (!result.releaseFound) applicationUpdateStatus_ = tr("No published Linux release matches this architecture and update channel.");
        else if (!result.available) applicationUpdateStatus_ = tr("No new version of Rufus was found");
        else {
            applicationRelease_ = result.release;
            applicationUpdateStatus_ = tr("Rufus Linux %1 is available.").arg(result.release.version);
        }
    }
    appendLog(applicationUpdateStatus_); refreshUpdateStatus();
    if (result.error == 0) applicationPreferenceChanged();
    else {
        // Preserve the last successful server timestamp, while avoiding a
        // tight automatic retry loop after unavailable or invalid responses.
        ApplicationSettings settings;
        const int interval = settings.value(QStringLiteral("UpdateCheckInterval"), -1).toInt();
        applicationUpdateTimer_.stop();
        if (interval > 0 && settings.value(QStringLiteral("ApplicationUpdateConsent"), false).toBool()) {
            applicationRetryAfter_ = QDateTime::currentSecsSinceEpoch() + interval;
            scheduleApplicationUpdates(interval);
        }
    }
    if (result.available && !applicationCheckManual_) notifyApplicationUpdate();
}
void MainWindow::refreshUpdateStatus()
{
    ui_->languageButton->setEnabled(!controller_.isBusy() && !updatesActive());
    if (!policyDialog_) return;
    const QString message = applicationUpdateStatus_.isEmpty() ? policyStatus_ : policyStatus_.isEmpty()
        ? applicationUpdateStatus_ : applicationUpdateStatus_ + QStringLiteral("\n\n") + policyStatus_;
    policyDialog_->setProgress(message, updatesActive());
    policyDialog_->setApplicationRelease(applicationRelease_.page);
}
void MainWindow::notifyApplicationUpdate()
{
    if (applicationRelease_.page.isEmpty() || applicationUpdateNotice_) return;
    if (controller_.isBusy() || updatesActive() || QApplication::activeModalWidget() != nullptr) {
        QTimer::singleShot(1000, this, &MainWindow::notifyApplicationUpdate); return;
    }
    applicationUpdateNotice_ = new QMessageBox(QMessageBox::Information, tr("A new version of Rufus is available!"),
        tr("Rufus Linux %1 is available. Open its release page to update using your package manager?").arg(applicationRelease_.version),
        QMessageBox::Yes | QMessageBox::No, this);
    applicationUpdateNotice_->setObjectName(QStringLiteral("applicationUpdateAvailable"));
    applicationUpdateNotice_->setTextFormat(Qt::PlainText);
    applicationUpdateNotice_->setDetailedText(applicationRelease_.notes);
    applicationUpdateNotice_->button(QMessageBox::Yes)->setText(tr("Open release page"));
    applicationUpdateNotice_->button(QMessageBox::No)->setText(tr("Close"));
    applicationUpdateNotice_->setDefaultButton(QMessageBox::No);
    applicationUpdateNotice_->setAttribute(Qt::WA_DeleteOnClose);
    connect(applicationUpdateNotice_, &QDialog::finished, this, &MainWindow::applicationNoticeFinished);
    applicationUpdateNotice_->open();
}
void MainWindow::applicationNoticeFinished(int result)
{
    applicationUpdateNotice_ = nullptr;
    if (result == QMessageBox::Yes) openReleasePage(applicationRelease_.page);
}
void MainWindow::openReleasePage(const QUrl& page)
{
    if (!page.isEmpty() && !QDesktopServices::openUrl(page)) {
        const QString message = tr("Cannot open the release page: %1").arg(page.toDisplayString());
        appendLog(message); ui_->statusBar->showMessage(message);
    }
}
void MainWindow::toggleForceUpdate()
{
    if (controller_.isBusy() || updatesActive()) return;
    forceUpdate_ = !forceUpdate_;
    forceWindowsTimestamp_ = false;
    const QString message = (forceUpdate_ ? tr("%1 enabled") : tr("%1 disabled")).arg(tr("Force update"));
    appendLog(message); ui_->statusBar->showMessage(message, shortcutStatusTimeoutMs);
}
void MainWindow::appendLanguageNotice()
{
    const auto& catalog = ApplicationLanguages();
    const auto& base = catalog.locales()[0];
    const auto& locale = catalog.locales()[catalog.selection(SelectedApplicationLanguage())];
    const bool older = locale.major < base.major || (locale.major == base.major && locale.minor < base.minor);
    if (!older) return;
    appendLog(QStringLiteral("NOTE: The %1 translation requires an update, but the current translator hasn't submitted "
        "one. Because of this, some messages will only be displayed in English.").arg(locale.name));
    appendLog(QStringLiteral("If you think you can help update this translation, please e-mail the author of this application"));
}

void MainWindow::showReadOptionStatus(QString message)
{
    ui_->statusBar->showMessage(message, shortcutStatusTimeoutMs);
}
void MainWindow::showImageOptionProblem(QString message)
{
    imageNotice_->setWindowTitle(tr("Image is too big")); imageNotice_->setText(message); imageNotice_->open();
}

void MainWindow::initializePolicyUpdates()
{
    ApplicationSettings settings; const int interval = settings.value(QStringLiteral("UpdateCheckInterval"),0).toInt();
    if (interval != 0) {
        updateFrequencyChanged(interval);
        const qint64 last = settings.value(QStringLiteral("LastPolicyUpdateCheck"),QStringLiteral("0")).toString().toLongLong();
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        // Windows refreshes the small lists each enabled session, independently
        // of its scheduled DBX/application update check.
        if (interval > 0 && last > 0 && last <= now && now-last < interval) startPolicies(false);
        return;
    }
    initialUpdateDialog_ = new QMessageBox(QMessageBox::Question,tr("Rufus update policy"),
        tr("Allow Rufus to periodically check online for application and Secure Boot policy updates?"),QMessageBox::Yes | QMessageBox::No,this);
    initialUpdateDialog_->setObjectName(QStringLiteral("initialPolicyUpdateChoice"));
    initialUpdateDialog_->setDefaultButton(QMessageBox::No); initialUpdateDialog_->setAttribute(Qt::WA_DeleteOnClose);
    connect(initialUpdateDialog_, &QDialog::finished,this,&MainWindow::initialUpdateChoice); initialUpdateDialog_->open();
}
void MainWindow::initialUpdateChoice(int result)
{
    initialUpdateDialog_ = nullptr;
    const int interval = result == QMessageBox::Yes ? 86400 : -1;
    ApplicationSettings settings; settings.setValue(QStringLiteral("UpdateCheckInterval"),interval);
    settings.setValue(QStringLiteral("ApplicationUpdateConsent"),result == QMessageBox::Yes);
    updateFrequencyChanged(interval);
}
void MainWindow::schedulePolicyUpdates(int interval)
{
    policyTimer_.stop(); if (interval <= 0) return;
    ApplicationSettings settings;
    const qint64 last = settings.value(QStringLiteral("LastPolicyUpdateCheck"),QStringLiteral("0")).toString().toLongLong();
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    const qint64 remaining = last > 0 && last <= now ? qMax(qint64(0),qint64(interval)-(now-last)) : 0;
    policyTimer_.start(int(qMin(qint64(INT_MAX),qMax(qint64(1),remaining*1000))));
}
void MainWindow::automaticPolicyCheck()
{
    ApplicationSettings settings; const int interval = settings.value(QStringLiteral("UpdateCheckInterval"),-1).toInt();
    if (interval <= 0) return;
    const qint64 last = settings.value(QStringLiteral("LastPolicyUpdateCheck"),QStringLiteral("0")).toString().toLongLong();
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    if (last > 0 && last <= now && now-last < interval) { schedulePolicyUpdates(interval); return; }
    if (controller_.isBusy() || updatesActive()) { policyTimer_.start(1000); return; }
    checkPolicies();
}
void MainWindow::showPolicyUpdates()
{
    if (!policyDialog_) {
        policyDialog_ = new PolicyUpdateDialog(this);
        connect(policyDialog_, &PolicyUpdateDialog::checkRequested,this,&MainWindow::checkPolicies);
        connect(policyDialog_, &PolicyUpdateDialog::cancelRequested,this,&MainWindow::cancelUpdates);
        connect(policyDialog_, &PolicyUpdateDialog::frequencyChanged,this,&MainWindow::updateFrequencyChanged);
        connect(policyDialog_, &PolicyUpdateDialog::applicationPreferenceChanged,this,&MainWindow::applicationPreferenceChanged);
        connect(policyDialog_, &PolicyUpdateDialog::releasePageRequested,this,&MainWindow::openReleasePage);
    }
    refreshUpdateStatus(); policyDialog_->setApplicationRelease(applicationRelease_.page); policyDialog_->show(); policyDialog_->raise();
}
void MainWindow::checkPolicies()
{
    if (controller_.isBusy() || updatesActive()) return;
    startPolicies(true); startApplicationUpdates(true);
}
void MainWindow::startPolicies(bool includeDbx)
{
    if (controller_.isBusy() || policyUpdates_.isActive()) return;
    checkingDbx_ = includeDbx;
    PolicyUpdateSources sources = policySources_;
    if (!includeDbx) sources.dbx.clear();
    const int error = policyUpdates_.start(ApplicationFilesDirectory(),controller_.secureBootPolicy(),sources);
    if (error != 0) policyProgress(tr("Cannot start policy update: %1").arg(QString::fromLocal8Bit(strerror(error))));
}
void MainWindow::policyProgress(QString message)
{
    policyStatus_ = message;
    refreshUpdateStatus();
    ui_->statusBar->showMessage(message);
}
void MainWindow::confirmDbxUpdate()
{
    if (!policyUpdates_.waitingForDbx() || dbxUpdateDialog_) return;
    if (controller_.isBusy()) { QTimer::singleShot(1000,this,&MainWindow::confirmDbxUpdate); return; }
    dbxUpdateDialog_ = new QMessageBox(QMessageBox::Warning,tr("DBX update available"),
        tr("Rufus has found updated DBX files for Secure Boot revocation checks. Download this update?"),QMessageBox::Yes | QMessageBox::No,this);
    dbxUpdateDialog_->setObjectName(QStringLiteral("dbxUpdateAvailable"));
    dbxUpdateDialog_->setInformativeText(tr("The files will be cached and reused automatically. Firmware and connected devices are not changed."));
    dbxUpdateDialog_->setDefaultButton(QMessageBox::No); dbxUpdateDialog_->setAttribute(Qt::WA_DeleteOnClose);
    connect(dbxUpdateDialog_, &QDialog::finished,this,&MainWindow::dbxUpdateFinished); dbxUpdateDialog_->open();
}
void MainWindow::dbxUpdateFinished(int result)
{
    dbxUpdateDialog_ = nullptr; policyUpdates_.answerDbx(result == QMessageBox::Yes);
}
void MainWindow::policyCompleted(PolicyUpdateResult result)
{
    if (dbxUpdateDialog_) dbxUpdateDialog_->reject();
    controller_.setSecureBootPolicy(result.policy);
    if (result.error == ECANCELED) policyStatus_ = tr("Policy update cancelled.");
    else if (result.failures != 0 || result.error != 0) policyStatus_ = tr("Policy check finished with unavailable or invalid responses; valid existing policies were kept.");
    else policyStatus_ = tr("Policy check finished: %1 lists and %2 DBX files updated.").arg(result.listUpdates).arg(result.dbxUpdates);
    if (result.listUpdates != 0 || result.dbxUpdates != 0) policyStatus_ += tr(" Select the image again to recheck its bootloaders.");
    appendLog(policyStatus_); policyProgress(policyStatus_);
    ApplicationSettings settings;
    if (checkingDbx_) settings.setValue(QStringLiteral("LastPolicyUpdateCheck"),QString::number(QDateTime::currentSecsSinceEpoch()));
    schedulePolicyUpdates(settings.value(QStringLiteral("UpdateCheckInterval"),-1).toInt());
}

void MainWindow::showAbout()
{
    AboutDialog* dialog = findChild<AboutDialog*>(QStringLiteral("aboutDialog"));
    if (!dialog)
    {
        dialog = new AboutDialog(this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
    }
    dialog->open();
}

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
    if (persistentLog_) appendLog(QDateTime::currentDateTime().toString(QStringLiteral("yyyy.MM.dd HH:mm:ss")));
    appendLog(tr("Rufus %1. No media operations have been started.")
        .arg(QLatin1String(RUFUS_LINUX_VERSION)));
    QDialogButtonBox* buttons = new QDialogButtonBox(logWindow_);
    QPushButton* clear = buttons->addButton(tr("Clear"), QDialogButtonBox::ActionRole);
    clear->setObjectName(QStringLiteral("clearLogButton"));
    QPushButton* save = buttons->addButton(tr("Save"), QDialogButtonBox::ActionRole);
    save->setObjectName(QStringLiteral("saveLogButton"));
    buttons->addButton(QDialogButtonBox::Close);
    layout->addWidget(log_);
    layout->addWidget(buttons);
    connect(clear, &QPushButton::clicked, this, &MainWindow::clearLog);
    connect(save, &QPushButton::clicked, this, &MainWindow::chooseLogFile);
    connect(buttons, &QDialogButtonBox::rejected, logWindow_, &QDialog::reject);
    connect(&controller_, &SelectionController::logMessage,
            this, &MainWindow::appendLog);
    logWindow_->resize(640, 500);
}

void MainWindow::refreshSelection()
{
    const Selection& selection = controller_.selection();
    if (selection.imageState != ImageState::ChoosingStore)
    {
        auto* dialog = findChild<QInputDialog*>(QStringLiteral("ffuStoreDialog"));
        if (dialog && dialog->isVisible()) dialog->reject();
    }
    const QSignalBlocker blocker(ui_->deviceCombo);
    ui_->deviceCombo->clear();
    bool readOnly = false;
    for (const Device& device : selection.devices)
    {
        QString text = DeviceCaption(device, properSizeUnits_);
        if (device.readOnly) text = tr("%1 [Read-only]").arg(text);
        if (device.id == selection.deviceId) readOnly = device.readOnly;
        ui_->deviceCombo->addItem(text, device.id);
    }
    ui_->deviceCombo->setCurrentIndex(ui_->deviceCombo->findData(selection.deviceId));
    const bool analyzing = controller_.isAnalyzing();
    const bool busy = controller_.isBusy();
    const IMAGE_WRITE_CHOICES choices = controller_.isoWriteChoices(selection.persistenceSize,
        selection.partition == PartitionScheme::Gpt, selection.fileSystem != FS_NTFS);
    const bool sourceFree = selection.bootType != BT_IMAGE;
    const bool ddOnly = !sourceFree && (selection.rawImage
        || (choices.count == 1 && choices.mode[0] == IMAGE_WRITE_DD));
    // Preserve the image's ISO choices/defaults even if only DD can run with
    // the installed components. Choosing ISO reports its missing helper.
    const bool isoReady = controller_.isoWriter().isAvailable(IMAGE_WRITE_DD) && choices.count > 0;
    const bool isoControls = !ddOnly && isoReady;
    const bool formatControls = isoControls || sourceFree;
    // Win32 shows the target's format defaults before an image is chosen,
    // while leaving the fields disabled through EnableControls.
    const bool idleWithoutImage = !busy && selection.imagePath.isEmpty();
    const bool showFormatChoices = formatControls || (idleWithoutImage && !selection.deviceId.isEmpty());
    const bool formatEnabled = formatControls && !busy && !selection.deviceId.isEmpty() && !readOnly;
    const bool rawReady = !sourceFree && selection.rawImage && selection.imageState == ImageState::Complete
        && controller_.writer().isAvailable();
    const bool usableImage = isoReady || rawReady;
    if (analyzing || sourceFree)
        imageNotice_->hide();
    ui_->languageButton->setEnabled(!busy && !updatesActive());
    ui_->deviceCombo->setEnabled(!busy);
    ui_->selectImageButton->setEnabled(!busy); // Win32 permits selecting a new image in every boot mode.
    ui_->selectImageButton->setToolTip(ui_->selectImageButton->downloadMode()
        ? tr("Download an image. Use the arrow to switch to SELECT. Hold Ctrl to add a ZIP archive.")
        : selection.archivePath.isEmpty()
        ? tr("Select a boot image. Hold Ctrl while selecting to add a ZIP archive.")
        : tr("Additional ZIP: %1\nHold Ctrl while selecting to replace or clear it.").arg(selection.archivePath));
    ui_->hashButton->setEnabled(controller_.canHashImage());
    ui_->saveDeviceButton->setEnabled(controller_.canSaveDevice());
    ui_->bootCombo->setEnabled(!busy);
    ui_->advancedDriveButton->setEnabled(!busy);
    ui_->listUsbHardDrives->setEnabled(!busy);
    ui_->listUsbHardDrives->setChecked(selection.allowFixedUsb);
    ui_->advancedFormatButton->setEnabled(!busy);
    ui_->closeButton->setText(busy ? tr("CANCEL") : tr("CLOSE"));
    ui_->closeButton->setEnabled(selection.imageState != ImageState::Cancelling
        && controller_.writer().status().state != WriteState::Cancelling
        && controller_.isoWriter().state() != IsoWriteState::Cancelling
        && controller_.hasher().state() != ImageHashState::Cancelling
        && controller_.saver().state() != ImageSaveState::Cancelling
        && controller_.downloader().state()!=ImageDownloadState::Cancelling);
    ui_->startButton->setEnabled(controller_.canStartWrite());
    ui_->startButton->setToolTip(readOnly ? tr("This device is read-only. Use the save icon to create an image.") : QString());
    // Rufus clears these checks for DD-only images and forces quick for large FAT32.
    ui_->advancedFormatOptions->setEnabled(!busy);
    ui_->quickFormat->setEnabled(formatEnabled && !ddOnly && controller_.canFullFormat());
    ui_->quickFormat->setChecked(!ddOnly && controller_.quickFormat());
    ui_->extendedLabel->setEnabled(formatEnabled && !ddOnly && controller_.canExtendedLabel());
    ui_->uefiValidation->setEnabled(formatEnabled && !ddOnly && controller_.canRuntimeValidate());
    ui_->uefiValidation->setChecked(!ddOnly && controller_.runtimeValidationEnabled());
    ui_->oldBiosFixes->setEnabled(formatEnabled && !ddOnly && controller_.canOldBiosFixes());
    ui_->oldBiosFixes->setChecked(!ddOnly && controller_.canOldBiosFixes() && controller_.oldBiosFixesEnabled());
    const bool testEnabled = !busy && !selection.deviceId.isEmpty() && !readOnly;
    ui_->badBlocks->setEnabled(testEnabled);
    ui_->badBlocks->setChecked(controller_.badBlocksEnabled());
    ui_->passesCombo->setEnabled(testEnabled && controller_.badBlocksEnabled());
    ui_->passesCombo->setCurrentIndex(controller_.badBlocksPattern());
    ui_->badBlocks->setToolTip(controller_.detectFakes()
        ? tr("Test the entire device before creation, including fake-capacity detection. Alt+B toggles fake detection.")
        : tr("Test the entire device before creation. Fake-capacity detection is disabled; Alt+B enables it."));
    if (badBlocksReview_ && !controller_.isWritingActive()) badBlocksReview_->reject();
    ui_->extendedLabel->setChecked(!ddOnly && selection.fileSystem != FS_EXT2 && selection.fileSystem != FS_EXT3
        && selection.fileSystem != FS_EXT4 && controller_.extendedLabelEnabled());
    ui_->extendedLabel->setToolTip(tr("Create autorun.inf and the Rufus icon for the original Unicode label in Windows Explorer. Keep an existing autorun.inf."));
    ui_->volumeEdit->setEnabled(formatEnabled);
    const bool windowsOptions = !sourceFree && !selection.imagePath.isEmpty() && !analyzing && !ddOnly
        && HAS_WINTOGO(selection.imageAnalysis.report);
    const bool persistence = controller_.canSetPersistence();
    ui_->imageOptions->setVisible(windowsOptions || persistence);
    ui_->imageOptions->setEnabled(formatEnabled);
    ui_->imageOptionCombo->setVisible(windowsOptions);
    ui_->imageOptionCombo->clear();
    if (!selection.imageAnalysis.report.is_windows_img)
        ui_->imageOptionCombo->addItem(tr("Standard Windows installation"), false);
    if (controller_.canSelectWindowsToGo()) ui_->imageOptionCombo->addItem(tr("Windows To Go"), true);
    ui_->imageOptionCombo->setCurrentIndex(ui_->imageOptionCombo->findData(selection.windowsToGo));
    ui_->imageOptionLabel->setText(persistence ? tr("Persistent partition size") : tr("Image option"));
    ui_->persistenceSlider->setVisible(persistence);
    ui_->persistenceSize->setVisible(persistence);
    ui_->persistenceUnits->setVisible(persistence);
    refreshPersistence(formatEnabled && persistence);
    const bool ext = selection.fileSystem == FS_EXT2 || selection.fileSystem == FS_EXT3;
    const QString labelTip = ext
        ? tr("ext uses up to 15 UTF-8 bytes. The confirmation shows the label that will be written.")
        : selection.fileSystem == FS_EXFAT
        ? tr("exFAT uses up to 11 characters. The confirmation shows the label that will be written.")
        : selection.fileSystem == FS_NTFS
        ? tr("NTFS uses up to 32 characters. The confirmation shows the label that will be written.")
        : selection.fileSystem == FS_UDF
        ? tr("UDF uses up to 32 characters. The confirmation shows the label that will be written.")
        : tr("FAT%1 uses up to 11 characters. The confirmation shows the label that will be written.")
            .arg(selection.fileSystem == FS_FAT16 ? 16 : 32);
    ui_->volumeEdit->setToolTip(formatControls ? labelTip : QString());
    ui_->fileSystemCombo->setEnabled(formatEnabled && selection.fileSystems != 0);
    ui_->clusterCombo->setEnabled(formatEnabled && selection.clusterSizes.Allowed != 0);
    ui_->clusterCombo->clear();
    if (showFormatChoices)
    {
        quint32 candidates = selection.clusterSizes.Allowed;
        while (candidates != 0)
        {
            const quint32 bytes = candidates & (0u - candidates);
            candidates &= ~bytes;
            // Same unit switch and default marker as SetClusterSizeLabels.
            constexpr quint32 lastByteLabel = 8192;
            constexpr quint32 bytesPerKilobyte = 1024;
            QString text = bytes <= lastByteLabel ? tr("%1 bytes").arg(bytes)
                : tr("%1 kilobytes").arg(bytes / bytesPerKilobyte);
            if (ext || selection.fileSystem == FS_UDF)
                text = tr("Default");
            else if (bytes == selection.clusterSizes.Default)
                text = tr("%1 (Default)").arg(text);
            ui_->clusterCombo->addItem(text, bytes);
        }
    }
    ui_->partitionCombo->clear();
    ui_->targetCombo->clear();
    ui_->fileSystemCombo->clear();
    if (sourceFree && !selection.deviceId.isEmpty())
    {
        const bool biosOnly = selection.bootType == BT_FREEDOS || selection.bootType == BT_MSDOS || selection.bootType == BT_REACTOS || selection.bootType == BT_GRUB2
            || selection.bootType == BT_GRUB4DOS || selection.syslinux;
        if (biosOnly || controller_.canSelectFormatMbr())
            ui_->partitionCombo->addItem(QStringLiteral("MBR"), int(PartitionScheme::Mbr));
        if (!biosOnly)
            ui_->partitionCombo->addItem(QStringLiteral("GPT"), int(PartitionScheme::Gpt));
        if (controller_.canSelectSfd())
            ui_->partitionCombo->addItem(QStringLiteral("Super Floppy Disk"), int(PartitionScheme::Sfd));
        ui_->partitionCombo->setCurrentIndex(ui_->partitionCombo->findData(int(selection.partition)));
        const QString target = biosOnly ? tr("BIOS (or UEFI-CSM)")
            : selection.bootType == BT_UEFI_NTFS ? tr("UEFI (non CSM)") : tr("BIOS or UEFI");
        ui_->targetCombo->addItem(target);
    }
    else if (isoControls)
    {
        const bool biosAvailable = controller_.canSelectSyslinux() || controller_.canSelectGrub() || controller_.canSelectWindowsBios()
            || controller_.canSelectKolibri() || controller_.canSelectWindowsToGoMbr();
        const bool efi = IS_EFI_BOOTABLE(selection.imageAnalysis.report);
        if (biosAvailable)
            ui_->partitionCombo->addItem(QStringLiteral("MBR"), int(PartitionScheme::Mbr));
        if (!biosAvailable || efi)
            ui_->partitionCombo->addItem(QStringLiteral("GPT"), int(PartitionScheme::Gpt));
        QString target = tr("UEFI (non CSM)");
        if (selection.partition == PartitionScheme::Mbr)
        {
            const bool dual = efi && (!controller_.canSelectWindowsBios() || controller_.enhancedInstall());
            target = dual ? tr("BIOS or UEFI") : tr("BIOS (or UEFI-CSM)");
        }
        if (selection.windowsToGo && selection.partition == PartitionScheme::Mbr)
        {
            if (HAS_BOOTMGR_BIOS(selection.imageAnalysis.report)) ui_->targetCombo->addItem(tr("BIOS or UEFI"), TT_BIOS);
            ui_->targetCombo->addItem(tr("UEFI (non CSM)"), TT_UEFI);
            ui_->targetCombo->setCurrentIndex(ui_->targetCombo->findData(selection.windowsToGoTarget));
        }
        else
            ui_->targetCombo->addItem(target);
        ui_->partitionCombo->setCurrentIndex(ui_->partitionCombo->findData(int(selection.partition)));
    }
    else if (idleWithoutImage && !selection.deviceId.isEmpty())
    {
        // Win32 previews the existing table before an image is selected, with
        // MBR as the raw/unknown fallback. This is not an ISO creation choice.
        for (const Device& device : selection.devices)
        {
            if (device.id != selection.deviceId)
                continue;
            const bool gpt = device.partitionTableType == QStringLiteral("gpt");
            ui_->partitionCombo->addItem(QStringLiteral("MBR"), int(PartitionScheme::Mbr));
            ui_->partitionCombo->addItem(QStringLiteral("GPT"), int(PartitionScheme::Gpt));
            ui_->partitionCombo->setCurrentIndex(ui_->partitionCombo->findData(int(gpt ? PartitionScheme::Gpt : PartitionScheme::Mbr)));
            if (!gpt)
                ui_->targetCombo->addItem(tr("BIOS (or UEFI-CSM)"));
            ui_->targetCombo->addItem(tr("UEFI (non CSM)"));
            break;
        }
    }
    if (showFormatChoices)
    {
        for (int filesystem : {FS_FAT16, FS_FAT32, FS_NTFS, FS_UDF, FS_EXFAT, FS_EXT2, FS_EXT3})
        {
            if ((selection.fileSystems & FS_MASK(filesystem)) == 0)
                continue;
            QString text = fileSystemName(filesystem);
            if (filesystem == selection.defaultFileSystem)
                text = tr("%1 (Default)").arg(text);
            ui_->fileSystemCombo->addItem(text, filesystem);
        }
    }
    ui_->partitionCombo->setEnabled(formatEnabled);
    ui_->targetCombo->setEnabled(formatEnabled);
    ui_->fileSystemCombo->setCurrentIndex(ui_->fileSystemCombo->findData(selection.fileSystem));
    ui_->clusterCombo->setCurrentIndex(showFormatChoices ? ui_->clusterCombo->findData(selection.clusterSize) : -1);
    ui_->deviceCombo->setToolTip(ui_->deviceCombo->currentText());
    ui_->bootCombo->setItemText(imageBootIndex, selection.imagePath.isEmpty()
        ? tr("Disk or ISO image (Please select)") : QFileInfo(selection.imagePath).fileName());
    ui_->bootCombo->setItemData(imageBootIndex, selection.imagePath);
    ui_->bootCombo->setCurrentIndex(ui_->bootCombo->findData(selection.bootType, bootTypeRole));

    QString status = tr("Device discovery unavailable; writing disabled.");
    if (selection.discoveryAvailable)
        status = tr("%n device(s) found", nullptr, selection.devices.size());
    if (!selection.discoveryProblem.isEmpty())
        status = selection.discoveryProblem;
    QString imageDetails;
    if (!sourceFree && selection.imageState != ImageState::None)
        status = controller_.imageStatusText();
    const bool scanRejected = !sourceFree && (selection.imageState == ImageState::Failed
        || selection.imageState == ImageState::Cancelled);
    if (scanRejected)
        status = tr("Failed to scan image"); // ImageScanThread: MSG_203, then READY.
    if (!sourceFree && selection.imageState == ImageState::Unsupported)
        status = tr("No image selected"); // ImageScanThread: MSG_086.
    if (usableImage)
        status = tr("Using image: %1").arg(QFileInfo(selection.imagePath).fileName()); // Win32 MSG_205.
    if (!sourceFree && !selection.imagePath.isEmpty())
    {
        imageDetails = tr("%1\n%2\n%3").arg(selection.imagePath,
            QLocale().formattedDataSize(qint64(selection.imageSize)), controller_.imageStatusText());
    }
    if (ui_->volumeEdit->text() != selection.volumeLabel)
        ui_->volumeEdit->setText(selection.volumeLabel);
    ui_->progressBar->setRange(0, analyzing ? 0 : 100);
    ui_->progressBar->setValue(0);
    QString progress = tr("READY");
    if (selection.imageState == ImageState::Scanning)
        progress = tr("Scanning image...");
    else if (selection.imageState == ImageState::Cancelling)
        progress = tr("Cancelling...");
    else if (!sourceFree && selection.imageState == ImageState::Partial && !usableImage)
        progress = tr("INCOMPLETE");
    ui_->progressBar->setFormat(progress);
    ui_->bootCombo->setToolTip(imageDetails);
    statusLabel_->setText(status);
    statusLabel_->setToolTip(usableImage ? imageDetails : status);
    if (scanRejected || (!sourceFree && selection.imageState == ImageState::Unsupported))
        statusLabel_->setToolTip(controller_.imageStatusText());
    refreshElapsedTime();
    refreshWrite();
    refreshIsoWrite();
    refreshHashes();
    refreshSave();
    refreshDeviceOperation();
    refreshCompletion();
    refreshDownload();
    busyLabel_->setText(ui_->progressBar->format());
    busyLabel_->setVisible(ui_->progressBar->maximum() == 0);
}

void MainWindow::refreshPersistence(bool enabled)
{
    const quint64 maximum = controller_.maximumPersistenceSize();
    const quint64 selected = controller_.selection().persistenceSize;
    const quint64 unit = ui_->persistenceUnits->currentData().toULongLong();
    const QSignalBlocker blocker(ui_->persistenceSlider);
    ui_->persistenceSlider->setValue(maximum != 0 ? int(selected / PersistenceUnitBytes * 1000
        / (maximum / PersistenceUnitBytes)) : 0);
    ui_->persistenceSlider->setEnabled(enabled && maximum != 0);
    ui_->persistenceSize->setEnabled(enabled && maximum != 0);
    ui_->persistenceUnits->setEnabled(enabled && maximum != 0);
    persistenceText_ = QLocale().toString(unit != 0 ? double(selected) / double(unit) : 0, 'f', 3);
    ui_->persistenceSize->setText(persistenceText_);
}

void MainWindow::editPersistenceSize()
{
    // Changing units can round the displayed decimal; leaving an unchanged
    // field must preserve the exact MiB selection behind that display.
    if (ui_->persistenceSize->text() == persistenceText_)
        return;
    bool valid = false;
    const double amount = QLocale().toDouble(ui_->persistenceSize->text(), &valid);
    const quint64 unit = ui_->persistenceUnits->currentData().toULongLong();
    const double bytes = amount * double(unit);
    if (valid && std::isfinite(bytes) && bytes >= 0)
    {
        const double bounded = qMin(bytes, double(controller_.maximumPersistenceSize()));
        controller_.setPersistenceSize(quint64(bounded));
    }
    refreshPersistence(!controller_.isBusy() && controller_.canSetPersistence());
}

void MainWindow::movePersistenceSlider(int value)
{
    const quint64 maximumMiB = controller_.maximumPersistenceSize() / PersistenceUnitBytes;
    controller_.setPersistenceSize(maximumMiB * quint64(value) / 1000 * PersistenceUnitBytes);
}

void MainWindow::choosePersistenceUnits(int index)
{
    Q_UNUSED(index);
    refreshPersistence(!controller_.isBusy() && controller_.canSetPersistence());
}

void MainWindow::finishImageSelection()
{
    const Selection& selection = controller_.selection();
    if (selection.imageState == ImageState::ChoosingStore)
    {
        chooseFfuStore();
        return;
    }
    const bool rejected = selection.imageState == ImageState::Failed
        || selection.imageState == ImageState::Cancelled || selection.imageState == ImageState::Unsupported;
    const IMAGE_WRITE_CHOICES choices = controller_.isoWriteChoices(selection.persistenceSize,
        selection.partition == PartitionScheme::Gpt, selection.fileSystem != FS_NTFS);
    const bool forcedDd = !selection.rawImage && choices.count == 1
        && choices.mode[0] == IMAGE_WRITE_DD;
    imageNotice_->hide();
    if (rejected)
    {
        imageNotice_->setWindowTitle(tr("Unsupported image"));
        imageNotice_->setIcon(QMessageBox::Information);
        imageNotice_->setText(tr("This image is either non-bootable, or it uses a boot or compression method "
            "that is not supported by Rufus..."));
        imageNotice_->setDetailedText(controller_.imageStatusText());
        imageNotice_->open();
    }
    else if (forcedDd)
    {
        imageNotice_->setWindowTitle(tr("ISOHybrid image detected"));
        imageNotice_->setIcon(QMessageBox::Information);
        imageNotice_->setText(tr("The image you have selected is an ISOHybrid, but its creators have not made it "
            "compatible with ISO/File copy mode.\nAs a result, DD image writing mode will be enforced."));
        imageNotice_->setDetailedText(QString());
        imageNotice_->open();
    }
    else
        QTimer::singleShot(0, this, &MainWindow::focusStart);
}

void MainWindow::chooseFfuStore()
{
    if (findChild<QInputDialog*>(QStringLiteral("ffuStoreDialog"))) return;
    auto* dialog = new QInputDialog(this);
    dialog->setObjectName(QStringLiteral("ffuStoreDialog"));
    dialog->setWindowTitle(tr("Select FFU store"));
    dialog->setLabelText(tr("This image contains multiple stores. Choose the store to write to the selected device."));
    dialog->setComboBoxItems(controller_.selection().ffuStores);
    dialog->setComboBoxEditable(false);
    dialog->setWindowModality(Qt::WindowModal);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &QDialog::finished, this, &MainWindow::ffuStoreFinished);
    dialog->open();
}

void MainWindow::ffuStoreFinished(int result)
{
    auto* dialog = qobject_cast<QInputDialog*>(sender());
    if (!dialog) return;
    const qsizetype selected = controller_.selection().ffuStores.indexOf(dialog->textValue());
    const unsigned store = result == QDialog::Accepted && selected >= 0 ? unsigned(selected + 1) : 0;
    controller_.chooseFfuStore(store);
}

void MainWindow::focusStart()
{
    if (ui_->startButton->isEnabled())
        ui_->startButton->setFocus(Qt::OtherFocusReason);
}

void MainWindow::refreshElapsedTime()
{
    const WriteState raw = controller_.writer().status().state;
    const IsoWriteState iso = controller_.isoWriter().state();
    const bool hashing = controller_.hasher().isActive() || controller_.downloader().isActive();
    const ImageSaveState save = controller_.saver().state();
    const bool capturing = save == ImageSaveState::Saving || save == ImageSaveState::Verifying || save == ImageSaveState::Finalizing;
    const bool idle = raw == WriteState::Idle && iso == IsoWriteState::Idle && !hashing && save == ImageSaveState::Idle;
    const bool checking = raw == WriteState::Checking || iso == IsoWriteState::Checking
        || save == ImageSaveState::Checking || save == ImageSaveState::Authorizing;
    if (idle || checking)
        creationStarted_ = false;
    const bool writing = controller_.writer().isWriting() || controller_.isoWriter().isWriting() || capturing;
    if ((writing || hashing) && !elapsedTimer_.isActive())
    {
        creationStarted_ = writing;
        elapsed_.start();
        updateElapsedTime();
        elapsedTimer_.start();
        ui_->closeButton->setFocus(Qt::OtherFocusReason);
    }
    else if (!controller_.isWritingActive() && !hashing && elapsedTimer_.isActive())
    {
        updateElapsedTime();
        elapsedTimer_.stop();
    }
}

void MainWindow::updateElapsedTime()
{
    const qint64 seconds = elapsed_.elapsed() / clockTickMs;
    constexpr int secondsPerMinute = 60;
    constexpr int secondsPerHour = secondsPerMinute * 60;
    elapsedLabel_->setText(QStringLiteral("%1:%2:%3")
        .arg(seconds / secondsPerHour, 2, 10, QLatin1Char('0'))
        .arg(seconds / secondsPerMinute % secondsPerMinute, 2, 10, QLatin1Char('0'))
        .arg(seconds % secondsPerMinute, 2, 10, QLatin1Char('0')));
}

void MainWindow::refreshCompletion()
{
    const WriteState raw = controller_.writer().status().state;
    const IsoWriteState iso = controller_.isoWriter().state();
    const ImageSaveState save = controller_.saver().state();
    const bool complete = raw == WriteState::Complete || iso == IsoWriteState::Complete || save == ImageSaveState::Complete
        || (controller_.deviceOperation() && controller_.deviceOperation()->state() == PrepareImageProcess::State::Complete);
    const bool failed = raw == WriteState::Failed || iso == IsoWriteState::Failed
        || controller_.hasher().state() == ImageHashState::Failed || save == ImageSaveState::Failed
        || (controller_.deviceOperation() && controller_.deviceOperation()->state() == PrepareImageProcess::State::Failed);
    const bool cancelled = raw == WriteState::Cancelled || iso == IsoWriteState::Cancelled || save == ImageSaveState::Cancelled;
    // Rufus's PBST_NORMAL/ERROR/PAUSED colors from src/ui.h.
    QColor color(0x06, 0xB0, 0x25);
    if (failed)
        color = QColor(0xDA, 0x26, 0x26);
    else if ((cancelled && creationStarted_) || controller_.hasher().state() == ImageHashState::Cancelled)
        color = QColor(0xDA, 0xCB, 0x26);
    QPalette palette = ui_->progressBar->palette();
    palette.setColor(QPalette::Highlight, color);
    ui_->progressBar->setPalette(palette);
    if (complete && !creationCompleted_)
    {
        // Match UM_FORMAT_COMPLETED, once, only after the job has released I/O.
        QApplication::beep();
        QApplication::alert(this);
    }
    creationCompleted_ = complete;
}

void MainWindow::refreshHashes()
{
    const ImageHashController& hasher = controller_.hasher();
    if (hasher.state() == ImageHashState::Idle)
        return;
    statusLabel_->setText(hasher.statusText());
    statusLabel_->setToolTip(hasher.statusText());
    ui_->progressBar->setRange(0, MAX_PROGRESS);
    ui_->progressBar->setValue(hasher.progressPosition());
    QString text = hasher.statusText();
    if (hasher.state() == ImageHashState::Running)
        text = tr("Computing image hashes: %p%");
    else if (hasher.state() == ImageHashState::Cancelled)
        text = tr("CANCELLED");
    else if (hasher.state() == ImageHashState::Failed)
        text = tr("FAILED");
    setProgressFormat(text,QStringLiteral("hash"),hasher.processed(),hasher.total(),hasher.isActive());
}

void MainWindow::showHashResult()
{
    ImageHashController& hasher = controller_.hasher();
    if (hasher.state() == ImageHashState::Complete)
    {
        // HashThread also uses UM_FORMAT_COMPLETED's notification on Windows.
        // Here the reader has exited; Qt widgets stay on the GUI thread.
        QApplication::beep();
        QApplication::alert(this);
        ImageHashDialog* dialog = new ImageHashDialog(hasher.result(), this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->open();
    }
    else if (hasher.state() == ImageHashState::Failed)
    {
        QMessageBox* notice = new QMessageBox(QMessageBox::Critical, tr("Rufus - Error"),
            hasher.result().problem, QMessageBox::Close, this);
        notice->setObjectName(QStringLiteral("imageHashError"));
        notice->setTextFormat(Qt::PlainText);
        notice->setAttribute(Qt::WA_DeleteOnClose);
        notice->open();
    }
}

void MainWindow::toggleExtraHashes()
{
    if (controller_.isBusy())
        return;
    controller_.toggleExtraHashes();
    ui_->statusBar->showMessage(controller_.extraHashes()
        ? tr("Extra hashes (SHA512) enabled") : tr("Extra hashes (SHA512) disabled"), shortcutStatusTimeoutMs);
}

void MainWindow::chooseFileSystem(int index)
{
    if (index >= 0)
        controller_.setFileSystem(ui_->fileSystemCombo->itemData(index).toInt());
}

void MainWindow::choosePartition(int index)
{
    if (index >= 0)
        controller_.setPartitionScheme(PartitionScheme(ui_->partitionCombo->itemData(index).toInt()));
}

void MainWindow::chooseCluster(int index)
{
    controller_.setClusterSize(ui_->clusterCombo->itemData(index).toUInt());
}

void MainWindow::chooseIsoMode()
{
    modeDialog_ = new ImageModeDialog(controller_.isoWriter().check().choices, this);
    modeDialog_->setAttribute(Qt::WA_DeleteOnClose);
    connect(modeDialog_, &QDialog::finished, this, &MainWindow::isoModeFinished);
    modeDialog_->open();
}

void MainWindow::isoModeFinished()
{
    const IMAGE_WRITE_MODE mode = modeDialog_->selectedMode();
    modeDialog_ = nullptr;
    controller_.isoWriter().chooseMode(mode);
}

void MainWindow::toggleExpertMode()
{
    if (controller_.isBusy())
        return;
    IsoWriteController& writer = controller_.isoWriter();
    writer.toggleExpertMode();
    ui_->statusBar->showMessage(writer.expertMode() ? tr("Expert Mode enabled") : tr("Expert Mode disabled"), shortcutStatusTimeoutMs);
}

void MainWindow::toggleFixedUsb()
{
    controller_.setAllowFixedUsb(!controller_.selection().allowFixedUsb);
}

void MainWindow::showFixedUsbStatus(bool enabled)
{
    ui_->statusBar->showMessage(enabled ? tr("Hard disk detection enabled")
        : tr("Hard disk detection disabled"), shortcutStatusTimeoutMs);
}

void MainWindow::toggleEnhancedInstall()
{
    if (controller_.isBusy())
        return;
    controller_.toggleEnhancedInstall();
    ui_->statusBar->showMessage(controller_.enhancedInstall()
        ? tr("Dual UEFI/BIOS mode enabled")
        : tr("Dual UEFI/BIOS mode disabled"), shortcutStatusTimeoutMs);
}

void MainWindow::toggleWindowsMbr()
{
    if (controller_.isBusy())
        return;
    controller_.toggleWindowsMbr();
    ui_->statusBar->showMessage(controller_.windowsRufusMbr()
        ? tr("Use Rufus MBR enabled") : tr("Use Rufus MBR disabled"), shortcutStatusTimeoutMs);
}

void MainWindow::chooseWindowsOptions()
{
    if (windowsDialog_)
        return;
    const IsoWriteController& writer = controller_.isoWriter();
    windowsDialog_ = new WindowsUserDialog(writer.check().analysis.report, writer.windowsOptions(), writer.expertMode(), this,
        writer.request().windowsToGo);
    windowsDialog_->setInstallOptions(writer.check());
    connect(windowsDialog_, &QDialog::finished, this, &MainWindow::windowsOptionsFinished);
    windowsDialog_->open();
}

void MainWindow::chooseWindowsImageOption(int index)
{
    if (index >= 0) controller_.setWindowsToGo(ui_->imageOptionCombo->itemData(index).toBool());
}

void MainWindow::chooseWindowsTarget(int index)
{
    if (index >= 0 && controller_.selection().windowsToGo)
        controller_.setWindowsToGoTarget(ui_->targetCombo->itemData(index).toInt());
}

void MainWindow::chooseWindowsArchive()
{
    if (windowsImageDialog_) return;
    windowsImageDialog_ = new WindowsImageDialog(controller_.isoWriter().check().windowsArchives, this);
    connect(windowsImageDialog_, &QDialog::finished, this, &MainWindow::windowsImageFinished);
    windowsImageDialog_->open();
}

void MainWindow::chooseWindowsEdition()
{
    if (windowsImageDialog_) return;
    windowsImageDialog_ = new WindowsImageDialog(controller_.isoWriter().check().windowsInstall.editions, this);
    connect(windowsImageDialog_, &QDialog::finished, this, &MainWindow::windowsImageFinished);
    windowsImageDialog_->open();
}

void MainWindow::windowsImageFinished(int result)
{
    if (!windowsImageDialog_) return;
    const bool archive = windowsImageDialog_->choosingArchive();
    const QString path = windowsImageDialog_->selectedArchive();
    const int image = windowsImageDialog_->selectedEdition();
    windowsImageDialog_->deleteLater();
    windowsImageDialog_ = nullptr;
    if (result != QDialog::Accepted) controller_.isoWriter().cancel();
    else if (archive) controller_.isoWriter().chooseWindowsArchive(path);
    else controller_.isoWriter().chooseWindowsEdition(image);
}

void MainWindow::windowsOptionsFinished(int result)
{
    if (!windowsDialog_)
        return;
    const PrepareImageRequest options = windowsDialog_->options();
    windowsDialog_->deleteLater();
    windowsDialog_ = nullptr;
    if (result == QDialog::Accepted)
        controller_.isoWriter().chooseWindowsOptions(options);
    else
        controller_.isoWriter().cancel();
}

void MainWindow::chooseBootMode(int index)
{
    if (index >= 0)
        controller_.setBootType(ui_->bootCombo->itemData(index, bootTypeRole).toInt());
}

void MainWindow::chooseDosOptions()
{
    if (dosDialog_)
        return;
    dosDialog_ = new FreeDosLocaleDialog(QStringLiteral("us"), 437, this, !controller_.isoWriter().request().msDos);
    connect(dosDialog_, &QDialog::finished, this, &MainWindow::dosOptionsFinished);
    dosDialog_->open();
}

void MainWindow::dosOptionsFinished(int result)
{
    if (!dosDialog_)
        return;
    const DOS_LOCALE locale = dosDialog_->selection();
    dosDialog_->deleteLater();
    dosDialog_ = nullptr;
    if (result == QDialog::Accepted)
        controller_.isoWriter().chooseDosOptions(QString::fromLatin1(locale.keyboard), locale.codepage);
    else
        controller_.isoWriter().cancel();
}

void MainWindow::chooseMsDosDownload()
{
    if (grubDownloadDialog_) return;
    grubDownloadDialog_ = new QMessageBox(QMessageBox::Question,tr("MS-DOS files"),
        tr("MS-DOS requires files from Microsoft that Rufus must download. Download diskcopy.dll now? The file will be cached for reuse."),
        QMessageBox::Yes | QMessageBox::No,this);
    grubDownloadDialog_->setObjectName(QStringLiteral("msDosDownloadDialog"));
    grubDownloadDialog_->setDefaultButton(QMessageBox::No);
    grubDownloadDialog_->setAttribute(Qt::WA_DeleteOnClose);
    connect(grubDownloadDialog_,&QDialog::finished,this,&MainWindow::msDosDownloadFinished);
    grubDownloadDialog_->open();
}
void MainWindow::msDosDownloadFinished(int result)
{
    grubDownloadDialog_ = nullptr;
    controller_.isoWriter().chooseMsDosDownload(result == QMessageBox::Yes);
}

void MainWindow::chooseGrubDownload(const QString& detectedVersion)
{
    if (grubDownloadDialog_)
        return;
    QString message = tr("This image uses Grub %1 but the application only includes the installation files for "
        "Grub %2.\n\nAs different versions of Grub may not be compatible with one another, and it is "
        "not possible to include them all, Rufus will attempt to locate a version of the Grub "
        "installation file ('core.img') that matches the one from your image:\n"
        "- Select 'Yes' to connect to the Internet and attempt to download it\n"
        "- Select 'No' to use the default version from Rufus\n"
        "- Select 'Cancel' to abort the operation\n\n"
        "Note: The file will be downloaded in '%3' and will be reused "
        "automatically if present. If no match can be found online, then the default version will be used.")
        .arg(detectedVersion, QStringLiteral(GRUB2_PACKAGE_VERSION), controller_.isoWriter().grubFilesDirectory());
    const bool grub4dos = controller_.isoWriter().request().grub4dos;
    if (grub4dos)
        message = tr("Grub4DOS 0.4 or later requires a 'grldr' file to be installed.\n"
            "Because this file is more than 100 KB in size, and always present on Grub4DOS ISO images, "
            "it is not embedded in Rufus.\n\nRufus can download the missing file for you:\n"
            "- Select 'Yes' to connect to the internet and download the file\n"
            "- Select 'No' to stop and provide this file manually before starting again\n"
            "- Select 'Cancel' to abort the operation\n\n"
            "Note: The file will be downloaded as '%1' and will be reused automatically if present.")
            .arg(controller_.isoWriter().grubFilesDirectory() + QStringLiteral("/grub4dos-" GRUB4DOS_VERSION "/grldr"));
    if (controller_.isoWriter().isSelectingSyslinux())
        message = tr("This image needs matching %1 installation files or replacements for obsolete COM32 modules.\n\n"
            "Select Yes to download the signed files and reuse them from '%2'. Select No to keep optional obsolete modules; "
            "a required missing loader pair stops creation. Cancel aborts this job.").arg(detectedVersion,controller_.isoWriter().grubFilesDirectory());
    const QString title = grub4dos ? tr("Download grldr?") : tr("Download required");
    grubDownloadDialog_ = new QMessageBox(QMessageBox::Warning, title, message,
        QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel, this);
    grubDownloadDialog_->setObjectName(QStringLiteral("grubDownloadDialog"));
    grubDownloadDialog_->setTextFormat(Qt::PlainText);
    grubDownloadDialog_->setDefaultButton(QMessageBox::Cancel);
    grubDownloadDialog_->setEscapeButton(QMessageBox::Cancel);
    grubDownloadDialog_->setAttribute(Qt::WA_DeleteOnClose);
    connect(grubDownloadDialog_, &QDialog::finished, this, &MainWindow::grubDownloadFinished);
    grubDownloadDialog_->open();
}

void MainWindow::grubDownloadFinished(int result)
{
    grubDownloadDialog_ = nullptr;
    const Grub2DownloadChoice choice = result == QMessageBox::Yes ? Grub2DownloadChoice::Yes
        : result == QMessageBox::No ? Grub2DownloadChoice::No : Grub2DownloadChoice::Cancel;
    controller_.isoWriter().chooseGrubDownload(choice);
}

void MainWindow::confirmIsoWrite()
{
    const IsoWriteController& writer = controller_.isoWriter();
    const Device& device = writer.target();
    const QString target = DeviceCaption(device, properSizeUnits_);
    isoConfirmation_ = new QMessageBox(QMessageBox::Warning, tr("Rufus"),
        tr("WARNING: ALL DATA ON DEVICE '%1' WILL BE DESTROYED.\n"
           "To continue with this operation, click OK. To quit click CANCEL.").arg(target),
        QMessageBox::Ok | QMessageBox::Cancel, this);
    isoConfirmation_->setObjectName(QStringLiteral("isoWriteConfirmation"));
    isoConfirmation_->setTextFormat(Qt::PlainText);
    const int fileSystem = writer.request().fileSystem;
    const bool ext = fileSystem == FS_EXT2 || fileSystem == FS_EXT3;
    const QString filesystemName = fileSystemName(fileSystem);
    QString layout = tr("GPT, %1, UEFI").arg(filesystemName);
    if (writer.request().syslinux || writer.request().grub2 || writer.request().grub4dos || writer.request().windowsBios)
    {
        const bool efi = IS_EFI_BOOTABLE(writer.check().analysis.report)
            && (!writer.request().windowsBios || writer.request().enhancedInstall);
        layout = (efi ? tr("MBR, %1, BIOS or UEFI") : tr("MBR, %1, BIOS")).arg(filesystemName);
    }
    isoConfirmation_->setInformativeText(tr("Image: %1\nISO Image — %2\nVolume label: %3\nCluster size: %4 bytes")
        .arg(writer.request().imagePath, layout,
            writer.request().label).arg(writer.request().clusterSize));
    if (writer.request().windowsToGo)
    {
        if (writer.request().mbr)
            layout = writer.request().windowsToGoTarget == TT_BIOS ? tr("MBR, NTFS, BIOS or UEFI") : tr("MBR, NTFS, UEFI");
        QString edition;
        for (const WindowsEdition& item : writer.check().windowsInstall.editions)
            if (item.image == writer.request().windowsEdition) edition = item.name;
        isoConfirmation_->setInformativeText(tr("Image: %1\nWindows To Go — %2\nEdition: %3\nVolume label: %4\nCluster size: %5 bytes")
            .arg(writer.request().imagePath, layout, edition, writer.request().label).arg(writer.request().clusterSize));
    }
    if (writer.request().isoEsp)
        isoConfirmation_->setInformativeText(tr("Image: %1\nISO → ESP — %2\nVolume label: %3\nCluster size: Default (%4 bytes)")
            .arg(writer.request().imagePath, layout, writer.request().label)
            .arg(FormatFatClusterSize(writer.request(), writer.check().partition)));
    const bool standaloneSyslinux = writer.request().isStandaloneSyslinux();
    if (writer.request().isSourceFree())
    {
        QString scheme = writer.request().mbr || standaloneSyslinux ? QStringLiteral("MBR") : QStringLiteral("GPT");
        if (writer.request().sfd)
            scheme = QStringLiteral("Super Floppy Disk");
        const QLatin1String syslinuxName(writer.request().syslinuxSelection == BT_SYSLINUX_V4 ? nativeSyslinux4Name : nativeSyslinuxName);
        const QString boot = standaloneSyslinux ? syslinuxName
            : writer.request().msDos ? QStringLiteral("MS-DOS")
            : writer.request().reactOs ? QStringLiteral("ReactOS")
            : writer.request().freeDos ? QStringLiteral("FreeDOS")
            : writer.request().grub2 ? QStringLiteral("Grub " GRUB2_PACKAGE_VERSION)
            : writer.request().grub4dos ? QStringLiteral("Grub4DOS " GRUB4DOS_VERSION)
            : writer.request().isStandaloneUefiNtfs() ? QStringLiteral("UEFI:NTFS") : tr("Non bootable");
        const QString cluster = ext || fileSystem == FS_UDF ? tr("Default") : tr("%1 bytes").arg(writer.request().clusterSize);
        isoConfirmation_->setInformativeText(tr("%1 — %2, %3\nVolume label: %4\nCluster size: %5")
            .arg(boot, scheme, filesystemName, writer.request().label, cluster));
    }
    if (writer.request().runtimeValidation)
        isoConfirmation_->setInformativeText(isoConfirmation_->informativeText() + tr("\nRuntime UEFI media validation"));
    if (writer.request().archive.present())
        isoConfirmation_->setInformativeText(isoConfirmation_->informativeText()
            + tr("\nAdditional ZIP: %1\n%2 files, %3 unpacked")
                .arg(writer.request().archive.path).arg(writer.check().archiveFiles)
                .arg(QLocale().formattedDataSize(qint64(writer.check().archiveBytes))));
    isoConfirmation_->setDefaultButton(QMessageBox::Cancel);
    isoConfirmation_->setAttribute(Qt::WA_DeleteOnClose);
    connect(isoConfirmation_, &QDialog::finished, this, &MainWindow::isoConfirmationFinished);
    isoConfirmation_->open();
}

void MainWindow::isoConfirmationFinished(int result)
{
    isoConfirmation_ = nullptr;
    if (result == QMessageBox::Ok)
        confirmPartitions(false);
    else
        controller_.isoWriter().cancel();
}

void MainWindow::refreshIsoWrite()
{
    IsoWriteController& writer = controller_.isoWriter();
    const IsoWriteState state = writer.state();
    if (state != IsoWriteState::ChoosingMode && modeDialog_)
        modeDialog_->reject();
    if (state != IsoWriteState::ChoosingWindowsOptions && windowsDialog_)
        windowsDialog_->reject();
    if (state != IsoWriteState::ChoosingWindowsArchive && state != IsoWriteState::ChoosingWindowsEdition && windowsImageDialog_)
        windowsImageDialog_->reject();
    if (state != IsoWriteState::ChoosingDosOptions && dosDialog_)
        dosDialog_->reject();
    if (state != IsoWriteState::SelectingGrubCore && grubDownloadDialog_)
        grubDownloadDialog_->reject();
    if (state != IsoWriteState::AwaitingConfirmation && isoConfirmation_)
        isoConfirmation_->reject();
    if (state != IsoWriteState::AwaitingConfirmation && partitionConfirmation_ && !partitionConfirmationRaw_)
        partitionConfirmation_->reject();
    if (state == IsoWriteState::Idle)
        return;
    if (!writer.isWriting() && cancelConfirmation_)
        cancelConfirmation_->reject();
    const bool cancelledCreation = state == IsoWriteState::Cancelled && creationStarted_;
    statusLabel_->setText(cancelledCreation ? tr("Operation cancelled by the user") : writer.statusText());
    statusLabel_->setToolTip(writer.statusText());
    const bool waitingForCore = state == IsoWriteState::SelectingGrubCore && !writer.hasGrubDownloadProgress();
    const bool waiting = state == IsoWriteState::Checking || waitingForCore || state == IsoWriteState::Authorizing
        || state == IsoWriteState::Cancelling;
    ui_->progressBar->setRange(0, waiting ? 0 : MAX_PROGRESS);
    ui_->progressBar->setValue(writer.progressPosition());
    QString text = writer.statusText();
    if (state == IsoWriteState::Checking)
        text = writer.request().imagePath.isEmpty() ? tr("Checking device...") : tr("Checking image...");
    if (state == IsoWriteState::Authorizing) text = tr("Waiting for authorization...");
    if (state == IsoWriteState::Cancelling) text = tr("Cancelling...");
    if (state == IsoWriteState::Complete) text = tr("READY");
    if (state == IsoWriteState::Failed) text = tr("FAILED");
    if (state == IsoWriteState::Cancelled)
        text = creationStarted_ ? tr("CANCELLED") : tr("READY");
    setProgressFormat(text,QStringLiteral("iso-")+writer.progressPhase(),writer.processedBytes(),writer.totalBytes(),writer.isActive() && !waiting);
}

void MainWindow::refreshWrite()
{
    RawWriteController& writer = controller_.writer();
    const WriteState state = writer.status().state;
    if (state != WriteState::AwaitingConfirmation && writeConfirmation_)
        writeConfirmation_->reject();
    if (state != WriteState::AwaitingConfirmation && partitionConfirmation_ && partitionConfirmationRaw_)
        partitionConfirmation_->reject();
    if (!writer.isWriting() && !controller_.isoWriter().isWriting() && cancelConfirmation_)
        cancelConfirmation_->reject();
    if (state == WriteState::Idle)
        return;
    const bool cancelledCreation = state == WriteState::Cancelled && creationStarted_;
    statusLabel_->setText(cancelledCreation ? tr("Operation cancelled by the user") : writer.statusText());
    statusLabel_->setToolTip(writer.statusText());
    const bool waiting = state == WriteState::Checking || state == WriteState::Authorizing
        || (writer.isWriting() && writer.status().outcome.io.phase == RAW_WRITE_SYNC);
    ui_->progressBar->setRange(0, waiting ? 0 : 100);
    ui_->progressBar->setValue(writer.percent());
    QString text = writer.statusText();
    const bool zero = writer.plan() && writer.plan()->imageMetadata().zeroMode != ZERO_NONE;
    if (state == WriteState::Checking) text = zero ? tr("Checking device...") : tr("Checking image...");
    if (state == WriteState::Authorizing) text = tr("Waiting for authorization...");
    if (state == WriteState::Cancelling) text = tr("Cancelling...");
    if (state == WriteState::Complete)
        text = tr("READY");
    else if (state == WriteState::Cancelled)
        text = creationStarted_ ? tr("CANCELLED") : tr("READY");
    else if (state == WriteState::Failed)
        text = tr("FAILED");
    else if (state == WriteState::Writing && !waiting)
    {
        if (zero)
            text = writer.status().outcome.io.phase == RAW_WRITE_VERIFY ? tr("Verifying device: %p%") : tr("Zeroing device: %p%");
        else
            text = writer.status().outcome.io.phase == RAW_WRITE_VERIFY ? tr("Verifying image: %p%") : tr("Writing image: %p%");
    }
    setProgressFormat(text,QStringLiteral("raw-%1").arg(writer.status().outcome.io.phase),writer.status().outcome.io.phase == RAW_WRITE_VERIFY ? writer.status().outcome.io.bytes_verified : writer.status().outcome.io.bytes_written,writer.totalBytes(),state == WriteState::Writing && !waiting);
}

void MainWindow::confirmRevokedImage(int mask)
{
    if (revocationWarning_) return;
    const QString failure = mask == 4
        ? tr("a Windows Recovery Screen with error code 0xc0000428")
        : tr("a \"Security Violation\" screen");
    revocationWarning_ = new QMessageBox(QMessageBox::Warning, tr("Revoked UEFI bootloader detected"),
        tr("Rufus detected that the selected ISO contains a revoked UEFI bootloader that will produce %1 "
           "when Secure Boot is enabled on a fully up-to-date UEFI system.").arg(failure),
        QMessageBox::Ok | QMessageBox::Cancel, this);
    revocationWarning_->setObjectName(QStringLiteral("revocationWarning"));
    revocationWarning_->setTextFormat(Qt::PlainText);
    revocationWarning_->setInformativeText(tr(
        "If you obtained this image from an untrusted source, consider the possibility of UEFI malware and avoid booting it.\n\n"
        "If you obtained it from a trusted source, try to find a more recent version that does not produce this warning."));
    revocationWarning_->setDefaultButton(QMessageBox::Cancel);
    revocationWarning_->setAttribute(Qt::WA_DeleteOnClose);
    connect(revocationWarning_, &QDialog::finished, this, &MainWindow::revocationWarningFinished);
    revocationWarning_->open();
}

void MainWindow::revocationWarningFinished(int result)
{
    revocationWarning_ = nullptr;
    controller_.answerRevocationWarning(result == QMessageBox::Ok);
}

void MainWindow::confirmWrite()
{
    const RawWritePlan* plan = controller_.writer().plan();
    if (!plan || controller_.writer().status().state != WriteState::AwaitingConfirmation)
        return;
    const Device& device = plan->target();
    const QString target = DeviceCaption(device, properSizeUnits_);
    writeConfirmation_ = new QMessageBox(QMessageBox::Warning, tr("Rufus"),
        tr("WARNING: ALL DATA ON DEVICE '%1' WILL BE DESTROYED.\n"
           "To continue with this operation, click OK. To quit click CANCEL.").arg(target),
        QMessageBox::Ok | QMessageBox::Cancel, this);
    writeConfirmation_->setObjectName(QStringLiteral("writeConfirmation"));
    writeConfirmation_->setTextFormat(Qt::PlainText);
    QString image = tr("Image: %1\n%2 — DD Image")
        .arg(plan->sourcePath(), QLocale().formattedDataSize(qint64(plan->writeBytes())));
    if (plan->imageMetadata().zeroMode != ZERO_NONE)
        image = plan->imageMetadata().zeroMode == ZERO_FAST
            ? tr("Fast zeroing: leave blank regions unchanged and fill other regions with FF. No filesystem will be created.")
            : tr("Zero the entire device and verify the result. No filesystem will be created.");
    if (plan->imageMetadata().ffuMinimumSize != 0)
        image = tr("Image: %1\nOptimized FFU — captured disk: %2\nMinimum device: %3\n"
            "Resize the image's expandable partition to fit this device.")
            .arg(plan->sourcePath(), QLocale().formattedDataSize(qint64(plan->imageMetadata().decodedImageSize)),
                QLocale().formattedDataSize(qint64(plan->imageMetadata().ffuMinimumSize)));
    if (plan->imageMetadata().hasCompressedImage())
        image += tr("\nCompressed file: %1").arg(QLocale().formattedDataSize(qint64(plan->source().info.st_size)));
    if (plan->imageMetadata().ffuStore != 0)
        image += tr("\nFFU store: %1").arg(plan->imageMetadata().ffuStore);
    if (plan->imageMetadata().badBlocksPattern >= 0)
        image += tr("\nTest the entire device for bad blocks before writing (%1).").arg(ui_->passesCombo->itemText(plan->imageMetadata().badBlocksPattern));
    writeConfirmation_->setInformativeText(image);
    writeConfirmation_->setDefaultButton(QMessageBox::Cancel);
    writeConfirmation_->setAttribute(Qt::WA_DeleteOnClose);
    connect(writeConfirmation_, &QDialog::finished, this, &MainWindow::writeConfirmationFinished);
    writeConfirmation_->open();
}

void MainWindow::writeConfirmationFinished(int result)
{
    writeConfirmation_ = nullptr;
    if (result == QMessageBox::Ok)
        confirmPartitions(true);
    else if (controller_.writer().status().state == WriteState::AwaitingConfirmation)
        controller_.writer().cancel();
}

void MainWindow::confirmPartitions(bool raw)
{
    const int count = raw ? controller_.writer().partitionCount() : controller_.isoWriter().check().existingPartitions;
    if (count <= 1)
    {
        if (raw) controller_.writer().confirm();
        else controller_.isoWriter().confirm();
        return;
    }
    partitionConfirmationRaw_ = raw;
    partitionConfirmation_ = new QMessageBox(QMessageBox::Warning, tr("Multiple partitions detected"),
        tr("IMPORTANT: THIS DRIVE CONTAINS MULTIPLE PARTITIONS!!\n\n"
           "This may include partitions/volumes that aren't listed or even visible from Linux. "
           "Should you wish to proceed, you are responsible for any data loss on these partitions."),
        QMessageBox::Ok | QMessageBox::Cancel, this);
    partitionConfirmation_->setObjectName(QStringLiteral("partitionConfirmation"));
    partitionConfirmation_->setTextFormat(Qt::PlainText);
    partitionConfirmation_->setDefaultButton(QMessageBox::Cancel);
    partitionConfirmation_->setAttribute(Qt::WA_DeleteOnClose);
    connect(partitionConfirmation_, &QDialog::finished, this, &MainWindow::partitionConfirmationFinished);
    partitionConfirmation_->open();
}

void MainWindow::partitionConfirmationFinished(int result)
{
    partitionConfirmation_ = nullptr;
    if (partitionConfirmationRaw_)
    {
        if (result == QMessageBox::Ok) controller_.writer().confirm();
        else if (controller_.writer().status().state == WriteState::AwaitingConfirmation)
            controller_.writer().cancel();
    }
    else
    {
        if (result == QMessageBox::Ok) controller_.isoWriter().confirm();
        else if (controller_.isoWriter().state() == IsoWriteState::AwaitingConfirmation)
            controller_.isoWriter().cancel();
    }
}

void MainWindow::confirmCancel()
{
    if (cancelConfirmation_)
        return;
    const QString warning = controller_.saver().isActive()
        ? tr("Cancel saving the device image? The incomplete image will be removed.")
        : tr("Cancelling may leave the device in an UNUSABLE state.\n"
             "If you are sure you want to cancel, click YES. Otherwise, click NO.");
    cancelConfirmation_ = new QMessageBox(QMessageBox::Warning, tr("Rufus - Cancellation"), warning,
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
    {
        if (controller_.deviceOperation() && controller_.deviceOperation()->isActive())
            controller_.deviceOperation()->cancel();
        else if (controller_.saver().isActive())
            controller_.saver().cancel();
        else if (controller_.isoWriter().isActive())
            controller_.isoWriter().cancel();
        else
            controller_.writer().cancel();
    }
}

void MainWindow::chooseDevice(int index)
{
    controller_.selectDevice(ui_->deviceCombo->itemData(index).toString());
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event)
{
    const QList<QUrl> paths = event->mimeData()->urls();
    const bool available = !controller_.isBusy();
    const bool localFile = !paths.isEmpty() && paths.first().isLocalFile()
        && QFileInfo(paths.first().toLocalFile()).isFile();
    if (available && localFile && event->possibleActions().testFlag(Qt::CopyAction))
    {
        event->setDropAction(Qt::CopyAction);
        event->accept();
    }
}

void MainWindow::dropEvent(QDropEvent* event)
{
    const QList<QUrl> paths = event->mimeData()->urls();
    if (controller_.isBusy() || paths.isEmpty()) return;
    const QUrl path = paths.first();
    if (!path.isLocalFile() || !QFileInfo(path.toLocalFile()).isFile()
        || !event->possibleActions().testFlag(Qt::CopyAction)) return;
    controller_.selectImage(path.toLocalFile());
    event->setDropAction(Qt::CopyAction);
    event->accept();
}

void MainWindow::toggleFakeDetection()
{
    if (controller_.isBusy()) return;
    controller_.toggleFakeDetection();
    statusLabel_->setText(controller_.detectFakes() ? tr("Fake drive detection enabled") : tr("Fake drive detection disabled"));
}

void MainWindow::showBadBlocksReport(BadBlocksReport report, QString path)
{
    if (badBlocksReview_) return;
    badBlocksReview_ = new QMessageBox(QMessageBox::Warning, tr("Bad blocks detected"),
        tr("%1 bad blocks found: %2 read errors, %3 write errors and %4 corruption errors.")
            .arg(report.count).arg(report.readErrors).arg(report.writeErrors).arg(report.corruptionErrors),
        QMessageBox::Abort | QMessageBox::Retry | QMessageBox::Ignore, this);
    badBlocksReview_->setObjectName(QStringLiteral("badBlocksReview"));
    badBlocksReview_->setTextFormat(Qt::PlainText);
    badBlocksReview_->setInformativeText(tr("Log: %1\nRetry repeats the device test. Ignore continues creation on this device.").arg(path));
    badBlocksReview_->setDefaultButton(QMessageBox::Abort);
    badBlocksReview_->setEscapeButton(QMessageBox::Abort);
    badBlocksReview_->setAttribute(Qt::WA_DeleteOnClose);
    connect(badBlocksReview_, &QDialog::finished, this, &MainWindow::badBlocksReviewFinished);
    badBlocksReview_->open();
}

void MainWindow::badBlocksReviewFinished(int result)
{
    badBlocksReview_ = nullptr;
    const int choice = result == QMessageBox::Retry ? BadBlocksRetry : result == QMessageBox::Ignore ? BadBlocksIgnore : BadBlocksAbort;
    controller_.answerBadBlocks(choice);
}

void MainWindow::zeroDevice()
{
    controller_.startZero(false);
}

void MainWindow::fastZeroDevice()
{
    controller_.startZero(true);
}

void MainWindow::toggleBootMarker()
{
    if (controller_.isBusy()) return;
    controller_.toggleBootMarker();
    ui_->statusBar->showMessage(controller_.ignoreBootMarker()
        ? tr("Boot marker check disabled (Alt+M)") : tr("Boot marker check enabled (Alt+M)"), shortcutStatusTimeoutMs);
}

void MainWindow::chooseImage()
{
    if (controller_.isBusy()) return;
    if (QApplication::keyboardModifiers().testFlag(Qt::ControlModifier)) {
        controller_.selectArchive({}); // Windows also clears a prior archive on Cancel.
        if (!archiveDialog_) {
            archiveDialog_ = new QFileDialog(this, tr("Select an additional ZIP archive"));
            archiveDialog_->setObjectName(QStringLiteral("archiveDialog"));
            archiveDialog_->setFileMode(QFileDialog::ExistingFile);
            archiveDialog_->setAcceptMode(QFileDialog::AcceptOpen);
            archiveDialog_->setNameFilter(tr("ZIP archives (*.zip *.ZIP)"));
            connect(archiveDialog_, &QFileDialog::fileSelected, &controller_, &SelectionController::selectArchive);
        }
        archiveDialog_->open();
        return;
    }
    if (ui_->selectImageButton->downloadMode()) {
        if (!downloadDialog_) {
            downloadDialog_=new ImageDownloadDialog(this);
            connect(downloadDialog_,&QDialog::finished,this,&MainWindow::downloadChoiceFinished);
        }
        downloadDialog_->open(); downloadDialog_->begin(); return;
    }
    if (!imageDialog_)
    {
        imageDialog_ = new QFileDialog(this, tr("Select a boot image"));
        imageDialog_->setObjectName(QStringLiteral("imageDialog"));
        imageDialog_->setFileMode(QFileDialog::ExistingFile);
        imageDialog_->setAcceptMode(QFileDialog::AcceptOpen);
        imageDialog_->setNameFilter(tr("Disk images (*.iso *.ISO *.img *.IMG *.usb *.USB *.wic *.WIC *.gz *.GZ *.xz *.XZ *.bz2 *.BZ2 *.bzip2 *.BZIP2 *.lzma *.LZMA *.Z *.z *.zip *.ZIP *.zst *.ZST *.vtsi *.VTSI *.vhd *.VHD *.vhdx *.VHDX *.ffu *.FFU *.wim *.WIM *.esd *.ESD);;All files (*)"));
        connect(imageDialog_, &QFileDialog::fileSelected,
                &controller_, &SelectionController::selectImage);
    }
    if (!controller_.selection().imagePath.isEmpty())
        imageDialog_->setDirectory(QFileInfo(controller_.selection().imagePath).absolutePath());
    imageDialog_->open();
}

void MainWindow::downloadChoiceFinished(int result)
{
    if (result!=QDialog::Accepted) return;
    downloadUrl_=downloadDialog_->selectedUrl();
    if (downloadDialog_->useBrowser()) {
        if (!QDesktopServices::openUrl(downloadUrl_)) appendLog(tr("Could not open the download link in your browser."));
        return;
    }
    if (!downloadFileDialog_) {
        downloadFileDialog_=new QFileDialog(this,tr("Save downloaded image"));
        downloadFileDialog_->setObjectName(QStringLiteral("downloadFileDialog"));
        downloadFileDialog_->setAcceptMode(QFileDialog::AcceptSave); downloadFileDialog_->setFileMode(QFileDialog::AnyFile);
        downloadFileDialog_->setNameFilter(tr("ISO images (*.iso)")); downloadFileDialog_->setDefaultSuffix(QStringLiteral("iso"));
        downloadFileDialog_->setWindowModality(Qt::WindowModal);
        connect(downloadFileDialog_,&QFileDialog::fileSelected,this,&MainWindow::startDownload);
    }
    QString name=QFileInfo(downloadUrl_.path()).fileName();
    if (!name.endsWith(QStringLiteral(".iso"),Qt::CaseInsensitive)) name=QStringLiteral("image.iso");
    downloadFileDialog_->selectFile(name); downloadFileDialog_->open();
}
void MainWindow::startDownload(const QString& path)
{
    if (!controller_.downloadImage(downloadUrl_,path)) appendLog(tr("Cannot start the image download."));
}
void MainWindow::refreshDownload()
{
    const auto& download=controller_.downloader(); const auto state=download.state();
    if (state==ImageDownloadState::Idle || state==ImageDownloadState::Complete) return;
    const bool active=download.isActive();
    QString message=state==ImageDownloadState::Running ? tr("Downloading image...")
        : state==ImageDownloadState::Cancelling ? tr("Cancelling...") : download.result().problem;
    ui_->progressBar->setRange(0,active && download.total()==0 ? 0 : MAX_PROGRESS);
    const int position=download.total()==0 ? 0 : qMin(MAX_PROGRESS-1,int(double(download.received())/double(download.total())*MAX_PROGRESS));
    ui_->progressBar->setValue(position);
    setProgressFormat(state==ImageDownloadState::Running ? tr("Downloading image: %p%") : state==ImageDownloadState::Cancelled ? tr("CANCELLED") : state==ImageDownloadState::Failed ? tr("FAILED") : message,QStringLiteral("download"),download.received(),download.total(),state==ImageDownloadState::Running);
    statusLabel_->setText(message); statusLabel_->setToolTip(message);
    QPalette palette=ui_->progressBar->palette();
    palette.setColor(QPalette::Highlight,state==ImageDownloadState::Failed ? QColor(0xDA,0x26,0x26)
        : state==ImageDownloadState::Cancelled ? QColor(0xDA,0xCB,0x26) : QColor(0x06,0xB0,0x25));
    ui_->progressBar->setPalette(palette);
}

void MainWindow::resetUsbPort()
{
    if (!controller_.startDeviceOperation(ResetUsbPort) && !controller_.isBusy()) appendLog(tr("Select an eligible USB device to reset its port."));
}

void MainWindow::toggleEspType()
{
    if (!controller_.startDeviceOperation(ToggleEspType) && !controller_.isBusy()) appendLog(tr("Select an eligible device to toggle its ESP type."));
}

void MainWindow::refreshDeviceOperation()
{
    const PrepareImageProcess* operation = controller_.deviceOperation();
    if (!operation) return;
    using State = PrepareImageProcess::State;
    const State state = operation->state();
    const bool complete = state == State::Complete;
    const bool cancelled = state == State::Cancelled;
    const bool failed = state == State::Failed;
    ui_->progressBar->setRange(0, operation->isActive() ? 0 : MAX_PROGRESS);
    ui_->progressBar->setValue(complete ? MAX_PROGRESS : 0);
    ui_->progressBar->setFormat(complete ? tr("READY") : failed ? tr("FAILED") : cancelled ? tr("CANCELLED")
        : state == State::Cancelling ? tr("Cancelling...") : tr("Device operation..."));
    statusLabel_->setText(complete ? tr("Device operation completed") : failed ? operation->problem() : ui_->progressBar->format());
}

void MainWindow::chooseOpticalImage()
{
    if (controller_.isBusy()) return;
    for (const Device& device : controller_.selection().devices) {
        if (device.deviceClass != DeviceClass::Optical) continue;
        controller_.selectDevice(device.id);
        chooseSaveImage(); return;
    }
    appendLog(tr("No dumpable optical media found."));
}

void MainWindow::chooseSaveImage()
{
    if (!controller_.canSaveDevice()) return;
    const Selection& selection = controller_.selection();
    bool gpt = false, filesystem = false, optical = false;
    for (const Device& device : selection.devices)
        if (device.id == selection.deviceId) {
            optical = device.deviceClass == DeviceClass::Optical;
            gpt = device.partitionTableType == QLatin1String("gpt");
            filesystem = !device.fileSystemType.isEmpty();
        }
    if (!saveImageDialog_) {
        saveImageDialog_ = new QFileDialog(this, tr("Save device to image"));
        saveImageDialog_->setObjectName(QStringLiteral("saveImageDialog"));
        saveImageDialog_->setAcceptMode(QFileDialog::AcceptSave);
        saveImageDialog_->setWindowModality(Qt::WindowModal);
        connect(saveImageDialog_, &QFileDialog::fileSelected, this, &MainWindow::saveDeviceImage);
        connect(saveImageDialog_, &QFileDialog::filterSelected, this, &MainWindow::updateSaveImageFilter);
    }
    QStringList filters{tr("VHDX image (*.vhdx)"), tr("Uncompressed VHD image (*.vhd)")};
    if (optical) filters = {tr("ISO image — raw optical disc (*.iso)")};
    if (!optical && gpt) filters.append(tr("Full Flash Update image (*.ffu)"));
    if (!optical && filesystem) filters.append(tr("ISO image — filesystem files (*.iso)"));
    saveImageDialog_->setNameFilters(filters);
    ApplicationSettings settings;
    const QString preferred = settings.value(QStringLiteral("PreferredSaveImageType"), QStringLiteral("vhdx")).toString();
    int selected = 0;
    for (int i = 0; i < filters.size(); i++)
        if (filters.at(i).contains(QStringLiteral("*.%1)").arg(preferred))) selected = i;
    saveImageDialog_->selectNameFilter(filters.at(selected));
    updateSaveImageFilter(filters.at(selected));
    saveImageDialog_->selectFile(QStringLiteral("rufus-device.%1").arg(saveImageDialog_->defaultSuffix()));
    saveImageDialog_->open();
}

void MainWindow::updateSaveImageFilter(const QString& filter)
{
    if (!saveImageDialog_) return;
    const QString suffix = filter.contains(QStringLiteral("*.vhdx")) ? QStringLiteral("vhdx")
        : filter.contains(QStringLiteral("*.ffu")) ? QStringLiteral("ffu") : filter.contains(QStringLiteral("*.iso")) ? QStringLiteral("iso") : QStringLiteral("vhd");
    saveImageDialog_->setDefaultSuffix(suffix);
    QString path = saveImageDialog_->selectedFiles().value(0);
    const int previous = GetImageSaveFormat(path);
    if (previous == IMG_COMPRESSION_VHD || previous == IMG_COMPRESSION_VHDX || previous == IMG_COMPRESSION_FFU || previous == ImageSaveIso)
        saveImageDialog_->selectFile(path.left(path.lastIndexOf(QLatin1Char('.')) + 1) + suffix);
}

void MainWindow::saveDeviceImage(const QString& path)
{
    if (!controller_.canSaveDevice()) return;
    if (!controller_.startSaveDevice(path)) {
        imageNotice_->setWindowTitle(tr("Cannot save device image"));
        imageNotice_->setText(tr("Choose a filename ending in .vhd, .vhdx, .ffu or .iso."));
        imageNotice_->open(); return;
    }
    ApplicationSettings settings;
    settings.setValue(QStringLiteral("PreferredSaveImageType"), QFileInfo(path).suffix().toLower());
}

void MainWindow::refreshSave()
{
    const ImageSaveController& saver = controller_.saver();
    if (saver.state() == ImageSaveState::Idle) return;
    const bool waiting = saver.state() == ImageSaveState::Checking || saver.state() == ImageSaveState::Authorizing
        || saver.state() == ImageSaveState::Cancelling;
    ui_->progressBar->setRange(0, waiting ? 0 : MAX_PROGRESS);
    ui_->progressBar->setValue(saver.progressPosition());
    QString text = saver.statusText();
    if (saver.state() == ImageSaveState::Complete) text = tr("READY");
    if (saver.state() == ImageSaveState::Failed) text = tr("FAILED");
    if (saver.state() == ImageSaveState::Cancelled) text = tr("CANCELLED");
    setProgressFormat(text,QStringLiteral("save-%1").arg(int(saver.state())),saver.processedBytes(),saver.totalBytes(),saver.isActive() && !waiting);
    statusLabel_->setText(saver.state() == ImageSaveState::Complete ? tr("Saved '%1'").arg(saver.path()) : saver.statusText());
    statusLabel_->setToolTip(saver.statusText());
    if (!saver.isActive() && cancelConfirmation_) cancelConfirmation_->reject();
}

void MainWindow::showLog()
{
    logWindow_->show();
    logWindow_->raise();
    logWindow_->activateWindow();
}

void MainWindow::toggleLog()
{
    if (logWindow_->isVisible()) logWindow_->hide();
    else showLog();
}

void MainWindow::appendLog(const QString& message)
{
    if (!sessionLog_.append(message)) qWarning("Cannot retain the complete Rufus session log.");
    log_->appendPlainText(message);
}

void MainWindow::clearLog()
{
    if (!sessionLog_.clear()) { appendLog(tr("Cannot clear the session log.")); return; }
    log_->clear();
}

void MainWindow::togglePersistentLog()
{
    persistentLog_ = !persistentLog_;
    ApplicationSettings settings;
    settings.setValue(QStringLiteral("PersistentLog"), persistentLog_);
    const QString message = persistentLog_ ? tr("Persistent logging enabled") : tr("Persistent logging disabled");
    appendLog(message);
    ui_->statusBar->showMessage(message, shortcutStatusTimeoutMs);
}

void MainWindow::toggleSizeUnits()
{
    properSizeUnits_ = !properSizeUnits_;
    ApplicationSettings settings;
    settings.setValue(QStringLiteral("UseProperSizeUnits"), properSizeUnits_);
    refreshSelection();
    const QString message = properSizeUnits_ ? tr("Device sizes use 1024 bytes per KB") : tr("Device sizes use nominal decimal units");
    appendLog(message);
    ui_->statusBar->showMessage(message, shortcutStatusTimeoutMs);
}

void MainWindow::resetUserSettings()
{
    const bool saved = ResetUserApplicationSettings() == 0;
    const QString message = saved ? tr("User settings deleted") : tr("Cannot delete user settings");
    appendLog(message);
    ui_->statusBar->showMessage(message, shortcutStatusTimeoutMs);
}

void MainWindow::saveAutomaticLog()
{
    const QString directory = ApplicationFilesDirectory();
    QString problem;
    if (!QDir().mkpath(directory)) problem = tr("Cannot create the log directory: %1").arg(directory);
    else sessionLog_.save(QDir(directory).filePath(QStringLiteral("rufus.log")), persistentLog_, problem);
    if (!problem.isEmpty()) qWarning().noquote() << tr("Cannot save the Rufus log: %1").arg(problem);
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
    QString problem;
    if (!sessionLog_.save(path, false, problem))
    {
        const QString message = tr("Cannot save the log: %1").arg(problem);
        appendLog(message);
        statusLabel_->setText(message);
        statusLabel_->setToolTip(message);
    }
}

void MainWindow::toggleAdvancedDrive()
{
    const bool expanded = ui_->advancedDriveOptions->isHidden();
    ApplicationSettings settings;
    settings.setValue(QStringLiteral("ShowAdvancedDriveProperties"), expanded);
    ui_->advancedDriveOptions->setVisible(expanded);
    ui_->advancedDriveButton->setText(expanded
        ? tr("Hide advanced drive properties") : tr("Show advanced drive properties"));
    ui_->advancedDriveButton->setArrowType(expanded ? Qt::UpArrow : Qt::DownArrow);
    if (expanded)
    {
        ui_->bootCombo->addItem(QLatin1String(nativeSyslinux4Name));
        ui_->bootCombo->setItemData(ui_->bootCombo->count() - 1, BT_SYSLINUX_V4, bootTypeRole);
        ui_->bootCombo->addItem(QLatin1String(nativeSyslinuxName));
        ui_->bootCombo->setItemData(ui_->bootCombo->count() - 1, BT_SYSLINUX_V6, bootTypeRole);
        ui_->bootCombo->addItem(QStringLiteral("ReactOS"));
        ui_->bootCombo->setItemData(ui_->bootCombo->count() - 1, BT_REACTOS, bootTypeRole);
        ui_->bootCombo->addItem(QStringLiteral("Grub " GRUB2_PACKAGE_VERSION));
        ui_->bootCombo->setItemData(ui_->bootCombo->count() - 1, BT_GRUB2, bootTypeRole);
        ui_->bootCombo->addItem(QStringLiteral("Grub4DOS " GRUB4DOS_VERSION));
        ui_->bootCombo->setItemData(ui_->bootCombo->count() - 1, BT_GRUB4DOS, bootTypeRole);
        ui_->bootCombo->addItem(QStringLiteral("UEFI:NTFS"));
        ui_->bootCombo->setItemData(ui_->bootCombo->count() - 1, BT_UEFI_NTFS, bootTypeRole);
    }
    else
    {
        // SetBootOptions restores the retained image when hiding advanced choices.
        if (controller_.selection().bootType >= BT_SYSLINUX_V4)
            controller_.setBootType(BT_IMAGE);
        ui_->bootCombo->removeItem(ui_->bootCombo->findData(BT_REACTOS, bootTypeRole));
        ui_->bootCombo->removeItem(ui_->bootCombo->findData(BT_SYSLINUX_V4, bootTypeRole));
        ui_->bootCombo->removeItem(ui_->bootCombo->findData(BT_SYSLINUX_V6, bootTypeRole));
        ui_->bootCombo->removeItem(ui_->bootCombo->findData(BT_GRUB2, bootTypeRole));
        ui_->bootCombo->removeItem(ui_->bootCombo->findData(BT_GRUB4DOS, bootTypeRole));
        ui_->bootCombo->removeItem(ui_->bootCombo->findData(BT_UEFI_NTFS, bootTypeRole));
    }
    adjustSize();
}

void MainWindow::toggleAdvancedFormat()
{
    const bool expanded = ui_->advancedFormatOptions->isHidden();
    ApplicationSettings settings;
    settings.setValue(QStringLiteral("ShowAdvancedFormatOptions"), expanded);
    ui_->advancedFormatOptions->setVisible(expanded);
    ui_->advancedFormatButton->setText(expanded
        ? tr("Hide advanced format options") : tr("Show advanced format options"));
    ui_->advancedFormatButton->setArrowType(expanded ? Qt::UpArrow : Qt::DownArrow);
    controller_.setAdvancedFormat(expanded);
    adjustSize();
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    if (controller_.deviceOperation() && controller_.deviceOperation()->isActive()) { confirmCancel(); event->ignore(); return; }
    if (controller_.downloader().isActive()) { controller_.downloader().cancel(); event->ignore(); return; }
    if (controller_.saver().isActive())
    {
        if (controller_.saver().state() != ImageSaveState::Cancelling) confirmCancel();
        event->ignore();
        return;
    }
    if (controller_.hasher().isActive())
    {
        controller_.hasher().cancel();
        event->ignore();
        return;
    }
    if (controller_.isoWriter().isActive())
    {
        if (controller_.isoWriter().state() == IsoWriteState::Cancelling)
        {
            event->ignore();
            return;
        }
        if (controller_.isoWriter().isWriting())
            confirmCancel();
        else
            controller_.isoWriter().cancel();
        event->ignore();
        return;
    }
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
    if (saveImageDialog_)
        saveImageDialog_->close();
    if (saveLogDialog_)
        saveLogDialog_->close();
    saveAutomaticLog();
    logWindow_->close();
    QMainWindow::closeEvent(event);
}

void MainWindow::setProgressFormat(const QString& format, const QString& phase, quint64 processed, quint64 total, bool active)
{
    progressFormat_ = format;
    if (active) progressDisplay_.observe(phase,processed,total);
    else progressDisplay_.clear();
    ui_->progressBar->setFormat(progressDisplay_.format(format));
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event)
{
    const auto* widget = qobject_cast<QWidget*>(watched);
    const bool owned = widget && (widget == this || isAncestorOf(widget)
        || widget == logWindow_ || (logWindow_ && logWindow_->isAncestorOf(widget)));
    if (!owned) return QMainWindow::eventFilter(watched,event);
    if (event->type() == QEvent::ShortcutOverride) {
        if (static_cast<QKeyEvent*>(event)->key() != Qt::Key_Alt) bareAlt_ = false;
    } else if (event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        bareAlt_ = key->key() == Qt::Key_Alt && !key->isAutoRepeat();
        const bool selectLog = key->key() == Qt::Key_A && key->modifiers() == Qt::ControlModifier
            && logWindow_->isVisible();
        if (selectLog) { log_->selectAll(); return true; }
    } else if (event->type() == QEvent::KeyRelease) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Alt && bareAlt_ && !key->isAutoRepeat()) {
            bareAlt_ = false; progressDisplay_.cycle();
            if (controller_.isBusy() && ui_->progressBar->maximum() > 0 && !progressFormat_.isEmpty())
                ui_->progressBar->setFormat(progressDisplay_.format(progressFormat_));
        }
    } else if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::WindowDeactivate) bareAlt_ = false;
    return QMainWindow::eventFilter(watched,event);
}

void MainWindow::clearDownloadedResources()
{
    if (controller_.isBusy() || updatesActive()) return;
    const auto& selected = controller_.selection();
    QString problem;
    appendLog(tr("Deleting directory '%1'").arg(ApplicationFilesDirectory()));
    const int error = ClearApplicationResources({selected.imagePath,selected.archivePath},problem);
    controller_.isoWriter().discardCachedBootResources();
    const QString message = error == 0 ? tr("Downloaded resources cleared.")
        : tr("Cannot clear downloaded resources: %1").arg(problem.isEmpty() ? QString::fromLocal8Bit(strerror(error)) : problem);
    appendLog(message); ui_->statusBar->showMessage(message,shortcutStatusTimeoutMs);
}

void MainWindow::toggleDeviceLocking()
{
    if (controller_.isBusy()) return;
    SetDeviceLocking(!DeviceLocking());
    appendLog(DeviceLocking() ? tr("Drive locking enabled (Alt+,)") : tr("Drive locking disabled (Alt+,). Identity and mounted-source checks remain active."));
}

void MainWindow::toggleDarkAppearance()
{
    if (controller_.isBusy() || updatesActive()) return;
    const int mode = ApplicationIsDark() ? 2 : 1;
    ApplicationSettings settings; settings.setValue(QStringLiteral("DarkMode"),mode);
    ApplyApplicationAppearance(mode); configureToolbarIcons();
}

void MainWindow::increaseJobPriority() { changeJobPriority(1); }
void MainWindow::decreaseJobPriority() { changeJobPriority(-1); }
void MainWindow::changeJobPriority(int delta)
{
    const int level = qBound(MinimumJobPriority,JobPriority()+delta,MaximumJobPriority);
    const int error = SetJobPriority(level);
    controller_.writer().setJobPriority(level); controller_.isoWriter().setJobPriority(level);
    if (controller_.deviceOperation()) controller_.deviceOperation()->setJobPriority(level);
    ApplicationSettings settings; settings.setValue(QStringLiteral("DefaultThreadPriority"),level-1);
    const QString message = tr("Default thread priority: %1").arg(level);
    appendLog(message); ui_->statusBar->showMessage(message,shortcutStatusTimeoutMs);
    if (error != 0) appendLog(QStringLiteral("Cannot apply all native job scheduling preferences: %1").arg(error));
}

void MainWindow::toggleForceTimestampUpdate()
{
    if (controller_.isBusy() || updatesActive()) return;
    forceWindowsTimestamp_ = !forceUpdate_;
    forceUpdate_ = forceWindowsTimestamp_;
    const QString message = (forceUpdate_ ? tr("%1 enabled") : tr("%1 disabled")).arg(tr("Force update"));
    appendLog(message); ui_->statusBar->showMessage(message,shortcutStatusTimeoutMs);
}

} // namespace rufus
