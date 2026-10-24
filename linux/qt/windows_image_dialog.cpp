// SPDX-License-Identifier: GPL-3.0-or-later
#include "windows_image_dialog.h"
#include <QComboBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace rufus
{
void WindowsImageDialog::initialize(const QString& question)
{
    setWindowTitle(tr("Windows To Go"));
    setWindowModality(Qt::WindowModal);
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    auto* layout = new QVBoxLayout(this);
    layout->setSizeConstraint(QLayout::SetFixedSize);
    auto* label = new QLabel(question, this);
    label->setTextFormat(Qt::PlainText);
    layout->addWidget(label);
    choice_ = new QComboBox(this);
    choice_->setObjectName(QStringLiteral("windowsImageChoice"));
    choice_->setAccessibleName(question);
    choice_->setMinimumContentsLength(32);
    choice_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    layout->addWidget(choice_);
    auto* actions = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    actions->button(QDialogButtonBox::Ok)->setDefault(true);
    layout->addWidget(actions);
    connect(actions, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(actions, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

WindowsImageDialog::WindowsImageDialog(const QStringList& archives, QWidget* parent) : QDialog(parent), archive_(true)
{
    setObjectName(QStringLiteral("windowsArchiveDialog"));
    initialize(tr("Select the Windows installation archive to use:"));
    for (const QString& archive : archives) choice_->addItem(archive, archive);
}

WindowsImageDialog::WindowsImageDialog(const QList<WindowsEdition>& editions, QWidget* parent) : QDialog(parent)
{
    setObjectName(QStringLiteral("windowsEditionDialog"));
    initialize(tr("Select the Windows version to install:"));
    for (const WindowsEdition& edition : editions) choice_->addItem(edition.name, edition.image);
}

QString WindowsImageDialog::selectedArchive() const { return choice_->currentData().toString(); }
int WindowsImageDialog::selectedEdition() const { return choice_->currentData().toInt(); }
}
