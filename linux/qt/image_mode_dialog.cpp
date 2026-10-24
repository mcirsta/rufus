// SPDX-License-Identifier: GPL-3.0-or-later
#include "image_mode_dialog.h"

#include <QButtonGroup>
#include <QDialogButtonBox>
#include <QFrame>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QRadioButton>
#include <QStyle>

namespace rufus
{

ImageModeDialog::ImageModeDialog(const IMAGE_WRITE_CHOICES& choices, QWidget* parent)
    : QDialog(parent), choices_(choices), buttons_(new QButtonGroup(this))
{
    Q_ASSERT(choices.count >= 2 && choices.count <= 3);
    Q_ASSERT(choices.mode[0] == IMAGE_WRITE_ISO && (choices.mode[choices.count - 1] == IMAGE_WRITE_DD
        || (choices.count == 2 && choices.mode[1] == IMAGE_WRITE_ESP)));
    const bool espQuestion = choices.count == 2 && choices.mode[1] == IMAGE_WRITE_ESP;
    setObjectName(QStringLiteral("imageModeDialog"));
    setWindowTitle(tr("%1 image detected").arg(espQuestion ? QStringLiteral("ESP") : QStringLiteral("ISOHybrid"))); // MSG_274
    setWindowModality(Qt::WindowModal);
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);

    QGridLayout* layout = new QGridLayout(this);
    layout->setSizeConstraint(QLayout::SetFixedSize);
    QLabel* icon = new QLabel(this);
    const int iconSize = style()->pixelMetric(QStyle::PM_MessageBoxIconSize);
    icon->setPixmap(style()->standardIcon(QStyle::SP_MessageBoxQuestion).pixmap(iconSize));
    layout->addWidget(icon, 0, 0, Qt::AlignTop);

    const QString iso = tr("ISO Image"); // MSG_036
    const QString dd = tr("DD Image"); // MSG_095
    const QString hybridMessage = tr(
        "The image you have selected is an 'ISOHybrid' image. This means it can be written either in "
        "%1 (file copy) mode or %2 (disk image) mode.\n"
        "Rufus recommends using %1 mode, so that you always have full access to the drive after writing it.\n"
        "However, if you encounter issues during boot, you can try writing this image again in %2 mode.\n\n"
        "Please select the mode that you want to use to write this image:").arg(iso, dd); // MSG_275
    const QString espMessage = tr(
        "The ISO you have selected uses UEFI and is small enough to be written as an EFI System Partition (ESP). "
        "Writing to an ESP, instead of writing to a generic data partition occupying the whole disk, can be preferable "
        "for some types of installations.\n\nPlease select the mode that you want to use to write this image:"); // MSG_310
    QLabel* message = new QLabel(espQuestion ? espMessage : hybridMessage, this);
    message->setObjectName(QStringLiteral("imageModeMessage"));
    message->setTextFormat(Qt::PlainText);
    message->setWordWrap(true);
    message->setMinimumWidth(fontMetrics().averageCharWidth() * 60);
    layout->addWidget(message, 0, 1);

    for (int i = 0; i < choices.count; i++)
    {
        const IMAGE_WRITE_MODE mode = choices.mode[i];
        const QString text = mode == IMAGE_WRITE_ISO
            ? tr("Write in %1 mode (Recommended)").arg(iso) // MSG_276
            : tr("Write in %1 mode").arg(mode == IMAGE_WRITE_ESP ? QStringLiteral("ISO → ESP") : dd); // MSG_277
        QRadioButton* button = new QRadioButton(text, this);
        button->setObjectName(QStringLiteral("imageModeChoice%1").arg(i + 1));
        buttons_->addButton(button, 1 << i);
        layout->addWidget(button, i + 1, 1);
    }
    buttons_->button(1)->setChecked(true);

    QFrame* line = new QFrame(this);
    line->setFrameShape(QFrame::HLine);
    layout->addWidget(line, choices.count + 2, 0, 1, 2);
    QDialogButtonBox* actions = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    actions->button(QDialogButtonBox::Ok)->setDefault(true);
    layout->addWidget(actions, choices.count + 3, 0, 1, 2);
    connect(actions, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(actions, &QDialogButtonBox::rejected, this, &QDialog::reject);
    buttons_->button(1)->setFocus();
}

IMAGE_WRITE_MODE ImageModeDialog::selectedMode() const
{
    const QAbstractButton* selected = buttons_->checkedButton();
    const bool accepted = result() == QDialog::Accepted && selected && selected->isEnabled();
    return accepted ? GetImageWriteMode(&choices_, buttons_->checkedId()) : IMAGE_WRITE_NONE;
}

} // namespace rufus
