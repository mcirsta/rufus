// SPDX-License-Identifier: GPL-3.0-or-later
#include "windows_user_dialog.h"
#include "windows_region_dialog.h"
#include "windows_silent_dialog.h"
#include "platform/iso_write_check.h"
#include "platform/windows_customization.h"
#include "platform/windows_install_options.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFrame>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStyle>
#include <QSignalBlocker>
#include <cstring>
#include <pwd.h>
#include <unistd.h>

namespace rufus
{
WindowsUserDialog::WindowsUserDialog(const RUFUS_IMG_REPORT& report, int mask, bool expertMode, QWidget* parent, bool windowsToGo)
    : QDialog(parent), available_(windowsToGo ? GetWindowsToGoUnattendFlags(&report, expertMode) : GetWindowsUnattendFlags(&report, expertMode)), mask_(mask),
      windowsToGo_(windowsToGo),
      layout_(new QGridLayout(this))
{
    setObjectName(QStringLiteral("windowsUserDialog"));
    setWindowTitle(tr("Windows User Experience")); // MSG_327
    setWindowModality(Qt::WindowModal);
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    layout_->setSizeConstraint(QLayout::SetFixedSize);
    QLabel* icon = new QLabel(this);
    const int iconSize = style()->pixelMetric(QStyle::PM_MessageBoxIconSize);
    icon->setPixmap(style()->standardIcon(QStyle::SP_MessageBoxQuestion).pixmap(iconSize));
    layout_->addWidget(icon, 0, 0, Qt::AlignTop);
    QLabel* question = new QLabel(tr("Customize Windows installation?"), this); // MSG_328
    question->setTextFormat(Qt::PlainText);
    layout_->addWidget(question, 0, 1, 1, 2);

    // Same installation-dialog order, text and eligibility as src/rufus.c.
    addChoice(UNATTEND_OFFLINE_INTERNAL_DRIVES, tr("Prevent Windows To Go from accessing internal disks"),
        tr("Keep the target PC's internal disks offline when running Windows To Go."));
    addChoice(UNATTEND_SECUREBOOT_TPM_MINRAM,
        tr("Remove requirement for 4GB+ RAM, Secure Boot and TPM 2.0"),
        tr("It is safe to leave this option enabled even if you have a TPM or more RAM, as this option only bypasses the setup requirements and does not actually prevent Windows from using all the hardware available."));
    addChoice(UNATTEND_NO_ONLINE_ACCOUNT, tr("Remove requirement for an online Microsoft account"),
        tr("For this option to work, network/Internet MUST be disconnected during installation!"));
    addChoice(UNATTEND_SET_USER, tr("Create a local account with username:"),
        tr("Automatically create a local account with the specified user name, using an empty password that will need to be changed on next logon."));
    addChoice(UNATTEND_DUPLICATE_LOCALE, tr("Set regional options to the same values as this user's"),
        tr("Unavailable on Linux: this option copies settings from a Windows host. Linux regional settings are not substituted."));
    addChoice(UNATTEND_NO_DATA_COLLECTION, tr("Disable data collection (Skip privacy questions)"),
        tr("Automatically answer 'no' to the Windows setup questions relating to the sharing of data with Microsoft, instead of prompting the user."));
    addChoice(UNATTEND_SILENT_INSTALL, tr("⚠SILENTLY⚠ erase disk and install:"),
        tr("If you use this option, please make sure to disconnect every disk from the target PC, except the one you want to install Windows on, as well as not leave the media plugged into any PC you don't want to erase."));
    addChoice(UNATTEND_DISABLE_BITLOCKER, tr("Disable BitLocker automatic device encryption"),
        tr("Don't encrypt the system disk, unless explicitly requested by the user."));
    addChoice(UNATTEND_QOL_ENHANCEMENTS,
        tr("QoL improvements (Don't force Copilot, OneDrive, Outlook, Fast Startup, etc.)"),
        tr("\"Quality of Life\" enhancements: Disable most of the unwanted features Microsoft is trying to force onto end users."));
    addChoice(UNATTEND_USE_MS2023_BOOTLOADERS,
        tr("Use 'Windows CA 2023' signed bootloaders (Requires a compatible target PC)"),
        tr("Use this option if the system you plan to install Windows on is using fully up-to-date Secure Boot certificates. If needed, you can look into using 'Mosby' to update your system."));
    addChoice(UNATTEND_APPLY_SKUSIPOLICY, tr("Apply SkuSiPolicy.p7b on installation (See KB5042562)"),
        tr("Use this option if you want to revoke additional potentially unsafe Windows bootloaders, but with the potential of also preventing standard Windows media from booting."));
    addChoice(UNATTEND_FORCE_S_MODE, tr("Restrict Windows to S-Mode (INCOMPATIBLE with online account bypass)"),
        tr("Use this option only if you know what S-Mode is and understand that your system may be locked into S-Mode even after you completely erase and reinstall Windows."));

    problem_ = new QLabel(this);
    problem_->setObjectName(QStringLiteral("windowsUsernameProblem"));
    problem_->setTextFormat(Qt::PlainText);
    layout_->addWidget(problem_, choices_.size() + 1, 1, 1, 2);
    QFrame* line = new QFrame(this);
    line->setFrameShape(QFrame::HLine);
    layout_->addWidget(line, choices_.size() + 2, 0, 1, 3);
    QDialogButtonBox* actions = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    ok_ = actions->button(QDialogButtonBox::Ok);
    ok_->setDefault(true);
    layout_->addWidget(actions, choices_.size() + 3, 0, 1, 3);
    connect(actions, &QDialogButtonBox::accepted, this, &WindowsUserDialog::accept);
    connect(actions, &QDialogButtonBox::rejected, this, &QDialog::reject);
    for (QCheckBox* choice : choices_)
    {
        connect(choice, &QCheckBox::toggled, this, &WindowsUserDialog::checkOptions);
    }
    if (username_)
        connect(username_, &QLineEdit::textChanged, this, &WindowsUserDialog::checkOptions);
    if (edition_)
        connect(edition_, &QComboBox::currentIndexChanged, this, &WindowsUserDialog::checkOptions);
    checkOptions();
}

void WindowsUserDialog::addChoice(int flag, const QString& text, const QString& tooltip)
{
    if (!(available_ & flag))
        return;
    QCheckBox* choice = new QCheckBox(text, this);
    choice->setObjectName(QStringLiteral("windowsOption%1").arg(flag));
    choice->setProperty("unattendFlag", flag);
    choice->setEnabled(((windowsToGo_ ? WindowsToGoCustomizationFlags : WindowsCustomizationFlags) & flag) != 0);
    choice->setChecked(choice->isEnabled() && (mask_ & flag));
    QString reason = tooltip;
    if (!choice->isEnabled() && flag != UNATTEND_DUPLICATE_LOCALE && flag != UNATTEND_SILENT_INSTALL)
        reason = tr("Not yet available on Linux.") + QLatin1Char('\n') + reason;
    choice->setToolTip(reason);
    choices_.append(choice);
    const int row = choices_.size();
    if (flag == UNATTEND_SILENT_INSTALL)
    {
        silent_ = choice;
        layout_->addWidget(choice, row, 1);
        edition_ = new QComboBox(this);
        edition_->setObjectName(QStringLiteral("windowsEdition"));
        edition_->setAccessibleName(tr("Windows edition"));
        edition_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        edition_->setMinimumContentsLength(24);
        edition_->setPlaceholderText(tr("Select a Windows edition"));
        edition_->setEnabled(false);
        edition_->setToolTip(reason);
        layout_->addWidget(edition_, row, 2);
        return;
    }
    if (flag == UNATTEND_DUPLICATE_LOCALE && (available_ & UNATTEND_SILENT_INSTALL))
    {
        layout_->addWidget(choice, row, 1);
        regionButton_ = new QPushButton(tr("Choose regional settings…"), this);
        regionButton_->setObjectName(QStringLiteral("windowsRegionButton"));
        regionButton_->setAutoDefault(false);
        regionButton_->setEnabled(false);
        regionButton_->setToolTip(tr("Required for silent installation. Choose Windows settings explicitly; no settings are copied from this PC."));
        layout_->addWidget(regionButton_, row, 2);
        connect(regionButton_, &QPushButton::clicked, this, &WindowsUserDialog::chooseRegion);
        return;
    }
    if (flag != UNATTEND_SET_USER)
    {
        layout_->addWidget(choice, row, 1, 1, 2);
        return;
    }
    layout_->addWidget(choice, row, 1);
    username_ = new QLineEdit(this);
    username_->setObjectName(QStringLiteral("windowsUsername"));
    username_->setAccessibleName(tr("Local account username"));
    username_->setToolTip(reason);
    username_->setMaxLength(MAX_USERNAME_LENGTH - 1);
    username_->setMinimumWidth(fontMetrics().horizontalAdvance(QStringLiteral("MOOOOOOOOOOO")));
    const passwd* account = getpwuid(getuid());
    const QString currentUser = account ? QString::fromUtf8(account->pw_name) : QString();
    username_->setText(currentUser.isEmpty() ? QStringLiteral("User") : currentUser);
    layout_->addWidget(username_, row, 2);
}

void WindowsUserDialog::setInstallOptions(const IsoWriteCheck& checked)
{
    if (!edition_)
        return;
    closeChildDialogs();
    const QSignalBlocker blocker(edition_);
    edition_->clear();
    region_ = {};
    installPath_ = checked.windowsInstallPath;
    installAvailable_ = CheckWindowsSilentInstallSource(checked, installProblem_) == 0;
    if (checked.windowsInstallError == 0 && !installPath_.isEmpty())
    {
        for (const WindowsEdition& edition : checked.windowsInstall.editions)
            edition_->addItem(edition.name, edition.image);
    }
    edition_->setCurrentIndex(-1); // Linux has no Windows host edition to match.
    edition_->setToolTip(installAvailable_ ? tr("Select the Windows edition to install.") : installProblem_);
    regionButton_->setEnabled(installAvailable_);
    if (!installAvailable_)
        regionButton_->setToolTip(installProblem_);
    else
        regionButton_->setToolTip(tr("Required for silent installation. Choose Windows settings explicitly; no settings are copied from this PC."));
    checkOptions();
}

int WindowsUserDialog::selectedFlags() const
{
    int flags = 0;
    for (const QCheckBox* choice : choices_)
    {
        if (choice->isEnabled() && !choice->isHidden() && choice->isChecked())
            flags |= choice->property("unattendFlag").toInt();
    }
    if (flags & UNATTEND_SILENT_INSTALL)
        flags |= UNATTEND_DUPLICATE_LOCALE; // Emit explicit fields, never copy host settings.
    return flags;
}

QString WindowsUserDialog::username() const
{
    if (!(selectedFlags() & UNATTEND_SET_USER) || !username_)
        return {};
    QByteArray name = username_->text().toUtf8();
    for (char& character : name)
    {
        if (character != '\0' && strchr(USERNAME_INVALID_CHARS, character) != nullptr)
            character = '_';
    }
    return QString::fromUtf8(name.trimmed());
}

void WindowsUserDialog::checkOptions()
{
    options_ = {};
    if (warningDialog_)
        warningDialog_->reject(); // A changed choice needs fresh acknowledgement.
    const int ordinaryFlags = selectedFlags() & (windowsToGo_ ? WindowsToGoCustomizationFlags : WindowsCustomizationFlags);
    const int prerequisites = UNATTEND_SET_USER | UNATTEND_NO_DATA_COLLECTION;
    const bool haveAccountAndPrivacy = (ordinaryFlags & prerequisites) == prerequisites;
    const bool haveRegion = CheckWindowsRegionOptions(region_) == 0;
    if (silent_)
    {
        const QSignalBlocker blocker(silent_);
        const bool showSilent = haveAccountAndPrivacy && haveRegion;
        silent_->setVisible(showSilent);
        edition_->setVisible(showSilent);
        silent_->setEnabled(showSilent && installAvailable_);
        if (!silent_->isEnabled())
            silent_->setChecked(false);
        edition_->setEnabled(silent_->isEnabled() && silent_->isChecked());
        regionButton_->setText(haveRegion ? tr("Edit regional settings…") : tr("Choose regional settings…"));
    }
    const QString rawName = (ordinaryFlags & UNATTEND_SET_USER) && username_ ? username_->text() : QString();
    QString problem;
    const int settingsError = windowsToGo_ ? CheckWindowsToGoCustomizationSettings(ordinaryFlags, rawName)
        : CheckWindowsCustomizationSettings(ordinaryFlags, rawName);
    if (settingsError != 0)
        problem = tr("The username is too long or contains unsupported characters.");
    else if (selectedFlags() & UNATTEND_SILENT_INSTALL)
    {
        const QString normalized = username();
        const QByteArray name = normalized.toUtf8();
        const bool usableName = !normalized.isEmpty() && normalized == normalized.trimmed()
            && !IsUnattendUserNameReserved(name.constData());
        if (!usableName)
            problem = tr("Silent installation requires a nonempty, non-reserved local account name without surrounding whitespace.");
        else if (edition_->currentData().toInt() < 1)
            problem = tr("Select a Windows edition.");
    }
    ok_->setEnabled(problem.isEmpty());
    problem_->setText(problem);
    problem_->setVisible(!problem.isEmpty());
}

PrepareImageRequest WindowsUserDialog::currentOptions() const
{
    PrepareImageRequest options;
    options.windowsFlags = selectedFlags();
    options.windowsUsername = username();
    if (options.windowsFlags & UNATTEND_SILENT_INSTALL)
    {
        options.windowsInstallPath = installPath_;
        options.windowsEdition = edition_->currentData().toInt();
        options.windowsRegion = region_;
    }
    return options;
}

void WindowsUserDialog::chooseRegion()
{
    if (!installAvailable_ || regionDialog_ || warningDialog_)
        return;
    options_ = {};
    regionDialog_ = new WindowsRegionDialog(region_, this);
    connect(regionDialog_, &QDialog::finished, this, &WindowsUserDialog::regionFinished);
    regionDialog_->open();
}

void WindowsUserDialog::regionFinished(int result)
{
    if (result == QDialog::Accepted)
        region_ = regionDialog_->options();
    regionDialog_->deleteLater();
    regionDialog_ = nullptr;
    checkOptions();
}

void WindowsUserDialog::accept()
{
    if (regionDialog_ || warningDialog_)
        return;
    checkOptions();
    if (!ok_->isEnabled())
        return;
    if (!(selectedFlags() & UNATTEND_SILENT_INSTALL))
    {
        options_ = currentOptions();
        QDialog::accept();
        return;
    }
    warningDialog_ = new WindowsSilentDialog(this);
    connect(warningDialog_, &QDialog::finished, this, &WindowsUserDialog::warningFinished);
    warningDialog_->open();
}

void WindowsUserDialog::warningFinished(int result)
{
    const bool accepted = result == QDialog::Accepted && warningDialog_->allConfirmed();
    warningDialog_->deleteLater();
    warningDialog_ = nullptr;
    checkOptions();
    if (!accepted || !ok_->isEnabled() || !(selectedFlags() & UNATTEND_SILENT_INSTALL))
        return;
    options_ = currentOptions();
    options_.windowsSilentConfirmed = true;
    QDialog::accept();
}

void WindowsUserDialog::closeChildDialogs()
{
    if (regionDialog_)
    {
        disconnect(regionDialog_, nullptr, this, nullptr);
        regionDialog_->reject();
        regionDialog_->deleteLater();
        regionDialog_ = nullptr;
    }
    if (warningDialog_)
    {
        disconnect(warningDialog_, nullptr, this, nullptr);
        warningDialog_->reject();
        warningDialog_->deleteLater();
        warningDialog_ = nullptr;
    }
}

void WindowsUserDialog::done(int result)
{
    closeChildDialogs();
    if (result != QDialog::Accepted)
        options_ = {};
    QDialog::done(result);
}
}
