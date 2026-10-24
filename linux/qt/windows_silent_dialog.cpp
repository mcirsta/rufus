// SPDX-License-Identifier: GPL-3.0-or-later
#include "windows_silent_dialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QStyle>

namespace rufus
{
WindowsSilentDialog::WindowsSilentDialog(QWidget* parent) : QDialog(parent)
{
    setObjectName(QStringLiteral("windowsSilentWarning"));
    setWindowTitle(tr("Rufus"));
    setWindowModality(Qt::WindowModal);
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    auto* layout = new QGridLayout(this);
    layout->setSizeConstraint(QLayout::SetFixedSize);
    auto* icon = new QLabel(this);
    const int iconSize = style()->pixelMetric(QStyle::PM_MessageBoxIconSize);
    icon->setPixmap(style()->standardIcon(QStyle::SP_MessageBoxWarning).pixmap(iconSize));
    layout->addWidget(icon, 0, 0, Qt::AlignTop);
    // Original MSG_356 and MSG_372..374; all three start unchecked on every use.
    auto* warning = new QLabel(tr("You have selected to use the \"silent\" Windows installation option.\n\n"
        "This is an advanced option, reserved for people who want to create media that does not prompt the user during Windows installation and therefore UNCONDITIONALLY ERASES the first detected disk on "
        "the target system. If you are not careful, using this option can result in IRREVERSIBLE DATA LOSS!\n\nYou MUST read and accept all of the following propositions to proceed:"), this);
    warning->setTextFormat(Qt::PlainText);
    warning->setWordWrap(true);
    layout->addWidget(warning, 0, 1);
    const QStringList texts = {
        tr("I WILL ensure that all disks, except the one I want to install Windows on, are disconnected."),
        tr("I WILL ensure that I don't leave this media plugged on a PC I do not plan to erase."),
        tr("I AGREE that any data loss resulting from the use of this option lies entirely with me.")};
    for (const QString& text : texts)
    {
        auto* confirmation = new QCheckBox(text, this);
        confirmation->setObjectName(QStringLiteral("windowsSilentAcknowledgement%1").arg(confirmations_.size() + 1));
        confirmations_.append(confirmation);
        layout->addWidget(confirmation, confirmations_.size(), 1);
        connect(confirmation, &QCheckBox::toggled, this, &WindowsSilentDialog::checkConfirmations);
    }
    auto* actions = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    ok_ = actions->button(QDialogButtonBox::Ok);
    ok_->setDefault(true);
    layout->addWidget(actions, confirmations_.size() + 1, 0, 1, 2);
    connect(actions, &QDialogButtonBox::accepted, this, &WindowsSilentDialog::accept);
    connect(actions, &QDialogButtonBox::rejected, this, &QDialog::reject);
    checkConfirmations();
}

bool WindowsSilentDialog::allConfirmed() const
{
    for (const QCheckBox* confirmation : confirmations_)
    {
        if (!confirmation->isChecked())
            return false;
    }
    return true;
}

void WindowsSilentDialog::checkConfirmations()
{
    ok_->setEnabled(allConfirmed());
}

void WindowsSilentDialog::accept()
{
    if (allConfirmed())
        QDialog::accept();
}
}
