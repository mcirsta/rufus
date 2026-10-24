// SPDX-License-Identifier: GPL-3.0-or-later
#include "image_hash_dialog.h"
#include "hash_core.h"

#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScreen>
#include <QStyle>
#include <QTextDocument>
#include <QtMath>

namespace rufus
{

ImageHashDialog::ImageHashDialog(const ImageHashResult& result, QWidget* parent)
    : QDialog(parent)
{
    Q_ASSERT(result.error == 0);
    Q_ASSERT(result.digests.size() == 3 || result.digests.size() == 4);
    setObjectName(QStringLiteral("imageHashDialog"));
    setWindowTitle(QFileInfo(result.path).fileName());
    setWindowModality(Qt::WindowModal);
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    QGridLayout* layout = new QGridLayout(this);
    layout->setSizeConstraint(QLayout::SetFixedSize);
    const char* names[] = {"MD5", "SHA1", "SHA256", "SHA512"};
    QFont fixedFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    if (font().pixelSize() > 0)
        fixedFont.setPixelSize(font().pixelSize());
    else
        fixedFont.setPointSizeF(font().pointSizeF());
    for (int i = 0; i < HASH_MAX; i++)
    {
        const QString name = QLatin1String(names[i]);
        const bool hasDigest = i < result.digests.size();
        const QString text = hasDigest ? result.digests[i]
            : tr("Use Alt+H (in the main application window) to enable."); // MSG_311
        QLabel* label = new QLabel(name + QLatin1Char(':'), this);
        QWidget* field;
        if (i < HASH_SHA256)
        {
            QLineEdit* edit = new QLineEdit(text, this);
            edit->setReadOnly(true);
            edit->setFont(fixedFont);
            edit->setCursorPosition(0);
            field = edit;
        }
        else
        {
            QPlainTextEdit* edit = new QPlainTextEdit(text, this);
            edit->setReadOnly(true);
            edit->setFont(fixedFont);
            edit->setTabChangesFocus(true);
            edit->setLineWrapMode(QPlainTextEdit::WidgetWidth);
            edit->setWordWrapMode(hasDigest ? QTextOption::WrapAnywhere : QTextOption::WordWrap);
            edit->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
            const int lines = i == HASH_SHA256 ? 2 : 3;
            const QFontMetricsF metrics(edit->font());
            const int margin = int(edit->document()->documentMargin()) * 2 + edit->frameWidth() * 2;
            const QMargins outer = layout->contentsMargins();
            const int labelWidth = fontMetrics().horizontalAdvance(QStringLiteral("SHA512:"));
            const int spacing = style()->pixelMetric(QStyle::PM_LayoutHorizontalSpacing);
            const int availableWidth = screen()->availableGeometry().width() - outer.left() - outer.right()
                - labelWidth - qMax(0, spacing) - edit->frameWidth() * 2;
            const int preferredWidth = qCeil(metrics.horizontalAdvance(QString(48, QLatin1Char('0')))) + margin;
            edit->setMinimumWidth(qMin(preferredWidth, qMax(1, availableWidth)));
            edit->setFixedHeight(qCeil(metrics.lineSpacing() * lines + metrics.descent()) + margin);
            field = edit;
        }
        field->setObjectName(name.toLower() + QStringLiteral("Hash"));
        field->setAccessibleName(name);
        label->setBuddy(field);
        layout->addWidget(label, i, 0, Qt::AlignTop);
        layout->addWidget(field, i, 1);
    }
    QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Ok, this);
    buttons->button(QDialogButtonBox::Ok)->setDefault(true);
    layout->addWidget(buttons, HASH_MAX, 0, 1, 2);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    buttons->button(QDialogButtonBox::Ok)->setFocus();
}

} // namespace rufus
