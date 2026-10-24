// SPDX-License-Identifier: GPL-3.0-or-later
#include "freedos_locale_dialog.h"
#include <QAbstractItemModel>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace rufus
{
FreeDosLocaleDialog::FreeDosLocaleDialog(const QString& keyboard, quint32 codepage, QWidget* parent, bool freedos)
    : QDialog(parent), freedos_(freedos)
{
    setObjectName(QStringLiteral("freeDosLocaleDialog"));
    setWindowTitle(freedos_ ? tr("FreeDOS settings") : tr("MS-DOS settings"));
    setWindowModality(Qt::WindowModal);
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    auto* layout = new QVBoxLayout(this);
    layout->setSizeConstraint(QLayout::SetFixedSize);
    auto* explanation = new QLabel(freedos_ ? tr("Choose the keyboard and character set to use in FreeDOS.") : tr("Choose the keyboard and character set to use in MS-DOS."), this);
    explanation->setTextFormat(Qt::PlainText);
    layout->addWidget(explanation);
    auto* form = new QFormLayout;
    layout->addLayout(form);
    keyboard_ = new QComboBox(this);
    keyboard_->setObjectName(QStringLiteral("freeDosKeyboard"));
    for (size_t index = 0; const char* id = GetDOSKeyboard(index,freedos_); index++)
    {
        const DOS_LOCALE locale = SelectDOSLocale(id, 437, freedos_);
        const QString key = QString::fromLatin1(id);
        if (keyboard_->findData(key) >= 0) continue;
        keyboard_->addItem(QStringLiteral("%1 [%2]").arg(QString::fromUtf8(locale.keyboard_name), key), key);
    }
    keyboard_->model()->sort(0);
    keyboard_->setCurrentIndex(keyboard_->findData(keyboard));
    form->addRow(tr("&Keyboard layout:"), keyboard_);

    codepage_ = new QComboBox(this);
    codepage_->setObjectName(QStringLiteral("freeDosCodepage"));
    for (size_t index = 0; const uint32_t cp = GetDOSCodepage(index,freedos_); index++)
        codepage_->addItem(QStringLiteral("%1 — %2").arg(cp).arg(QString::fromUtf8(GetDOSCodepageName(cp))), cp);
    const DOS_LOCALE initial = SelectDOSLocale("us", codepage, freedos_);
    codepage_->setCurrentIndex(initial.codepage_fallback ? -1 : codepage_->findData(initial.codepage));
    codepage_->setToolTip(tr("Code pages control which characters DOS can display. Choose one supported by your keyboard layout."));
    form->addRow(tr("&Code page:"), codepage_);
    note_ = new QLabel(this);
    note_->setObjectName(QStringLiteral("freeDosLocaleNote"));
    note_->setTextFormat(Qt::PlainText);
    layout->addWidget(note_);
    auto* actions = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    ok_ = actions->button(QDialogButtonBox::Ok);
    ok_->setDefault(true);
    layout->addWidget(actions);
    connect(actions, &QDialogButtonBox::accepted, this, &FreeDosLocaleDialog::accept);
    connect(actions, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(keyboard_, &QComboBox::currentIndexChanged, this, &FreeDosLocaleDialog::updateChoice);
    connect(codepage_, &QComboBox::currentIndexChanged, this, &FreeDosLocaleDialog::updateChoice);
    updateChoice();
}

DOS_LOCALE FreeDosLocaleDialog::selection() const
{
    if (keyboard_->currentIndex() < 0 || codepage_->currentIndex() < 0)
        return {};
    const QByteArray keyboard = keyboard_->currentData().toString().toLatin1();
    const uint32_t codepage = codepage_->currentData().toUInt();
    const DOS_LOCALE locale = SelectDOSLocale(keyboard.constData(), codepage, freedos_);
    const bool exactKeyboard = !locale.keyboard_fallback && keyboard == locale.keyboard;
    const bool exactCodepage = !locale.codepage_fallback && locale.codepage == codepage;
    return exactKeyboard && exactCodepage ? locale : DOS_LOCALE{};
}

void FreeDosLocaleDialog::updateChoice()
{
    const DOS_LOCALE locale = selection();
    ok_->setEnabled(locale.keyboard != nullptr);
    if (locale.keyboard == nullptr)
        note_->setText(tr("Choose a keyboard layout and code page."));
    else if (locale.use_menu)
        note_->setText(tr("The boot menu will also offer US-English with code page 437."));
    else
        note_->setText(freedos_ ? tr("FreeDOS will start with the US-English keyboard and code page 437.") : tr("MS-DOS will start with the US-English keyboard and code page 437."));
}

void FreeDosLocaleDialog::accept()
{
    if (selection().keyboard != nullptr)
        QDialog::accept();
}
}
