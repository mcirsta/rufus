// SPDX-License-Identifier: GPL-3.0-or-later
#include "windows_region_dialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QTimeZone>
#include <QVBoxLayout>

namespace rufus
{
namespace
{
constexpr int editorTextLimit = 256;

QString localeValue(const QComboBox* combo)
{
    // An edited tag is kept verbatim; Qt's locale normalization can discard
    // script subtags. Item labels are only for display, never answer-file data.
    const bool selectedEntry = combo->currentIndex() >= 0
        && combo->currentText() == combo->itemText(combo->currentIndex());
    if (selectedEntry)
        return combo->currentData().toString();
    return combo->currentText().trimmed();
}
}

WindowsRegionDialog::WindowsRegionDialog(const WindowsRegionOptions& initial, QWidget* parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("windowsRegionDialog"));
    setWindowTitle(tr("Windows regional settings"));
    setWindowModality(Qt::WindowModal);
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    auto* layout = new QVBoxLayout(this);
    layout->setSizeConstraint(QLayout::SetFixedSize);
    auto* explanation = new QLabel(tr("Choose settings for the Windows installation.\nLanguages must be available in the selected Windows edition."), this);
    explanation->setTextFormat(Qt::PlainText);
    layout->addWidget(explanation);
    auto* form = new QFormLayout;
    layout->addLayout(form);

    QStringList tags;
    const QList<QLocale> locales = QLocale::matchingLocales(QLocale::AnyLanguage, QLocale::AnyScript, QLocale::AnyTerritory);
    for (const QLocale& locale : locales)
    {
        if (locale.language() != QLocale::C)
            tags.append(QString(locale.name()).replace(QLatin1Char('_'), QLatin1Char('-')));
    }
    tags.removeDuplicates();
    tags.sort();
    language_ = addLocale(form, tags, tr("Display language:"), "windowsDisplayLanguage", initial.uiLanguage);
    fallback_ = addLocale(form, tags, tr("Fallback language (optional):"), "windowsFallbackLanguage", initial.uiLanguageFallback);
    user_ = addLocale(form, tags, tr("Regional format:"), "windowsUserLocale", initial.userLocale);
    system_ = addLocale(form, tags, tr("Language for non-Unicode programs:"), "windowsSystemLocale", initial.systemLocale);
    input_ = addLocale(form, tags, tr("Keyboard / input language:"), "windowsInputLocale", initial.inputLocale);
    input_->setToolTip(tr("Select a language for its default Windows keyboard, or enter a layout such as 0409:00020409 (US International). Separate multiple inputs with semicolons; the first is the default."));
    fallback_->setToolTip(tr("Only needed for a partially localized Windows display language. Leave empty to use Windows Setup's default."));

    timezone_ = addTimezone(form, initial.timezone);

    problem_ = new QLabel(tr("Choose each required setting. Use language tags such as en-US or ro-RO."), this);
    problem_->setObjectName(QStringLiteral("windowsRegionProblem"));
    problem_->setTextFormat(Qt::PlainText);
    layout->addWidget(problem_);
    auto* actions = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    ok_ = actions->button(QDialogButtonBox::Ok);
    ok_->setDefault(true);
    layout->addWidget(actions);
    connect(actions, &QDialogButtonBox::accepted, this, &WindowsRegionDialog::accept);
    connect(actions, &QDialogButtonBox::rejected, this, &QDialog::reject);
    const QList<QComboBox*> fields = {language_, fallback_, user_, system_, input_, timezone_};
    for (QComboBox* field : fields)
        connect(field, &QComboBox::currentTextChanged, this, &WindowsRegionDialog::checkOptions);
    checkOptions();
}

QComboBox* WindowsRegionDialog::addLocale(QFormLayout* form, const QStringList& tags,
    const QString& label, const char* name, const QString& value)
{
    auto* combo = new QComboBox(this);
    combo->setObjectName(QString::fromLatin1(name));
    combo->setEditable(true);
    combo->setInsertPolicy(QComboBox::NoInsert);
    combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    combo->setMinimumContentsLength(28);
    combo->lineEdit()->setMaxLength(editorTextLimit);
    combo->lineEdit()->setPlaceholderText(tr("Select or enter a language tag"));
    for (const QString& tag : tags)
        combo->addItem(tr("%1 (%2)").arg(QLocale::languageToString(QLocale(tag).language()), tag), tag);
    combo->setCurrentIndex(-1);
    combo->setEditText(value);
    form->addRow(label, combo);
    return combo;
}

QComboBox* WindowsRegionDialog::addTimezone(QFormLayout* form, const QString& windowsZone)
{
    auto* combo = new QComboBox(this);
    combo->setObjectName(QStringLiteral("windowsTimezone"));
    combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    combo->setMinimumContentsLength(28);
    combo->setPlaceholderText(tr("Select a time zone"));
    QStringList windowsZones;
    const QList<QByteArray> zones = QTimeZone::availableTimeZoneIds();
    for (const QByteArray& zone : zones)
    {
        const QByteArray windowsId = QTimeZone::ianaIdToWindowsId(zone);
        if (!windowsId.isEmpty())
            windowsZones.append(QString::fromUtf8(windowsId));
    }
    windowsZones.removeDuplicates();
    windowsZones.sort();
    for (const QString& windowsId : windowsZones)
    {
        // Windows groups several IANA zones. Show one Windows choice with its
        // representative city, so reopening does not display an arbitrary alias.
        const QString city = QString::fromUtf8(QTimeZone::windowsIdToDefaultIanaId(windowsId.toUtf8()));
        combo->addItem(tr("%1 (%2)").arg(windowsId, city), windowsId);
    }
    combo->setCurrentIndex(windowsZone.isEmpty() ? -1 : combo->findData(windowsZone));
    form->addRow(tr("Time zone:"), combo);
    return combo;
}

WindowsRegionOptions WindowsRegionDialog::options() const
{
    return {localeValue(input_), localeValue(system_), localeValue(user_), localeValue(language_),
        localeValue(fallback_), timezone_->currentData().toString()};
}

void WindowsRegionDialog::checkOptions()
{
    timezone_->setToolTip(timezone_->currentText());
    const bool valid = CheckWindowsRegionOptions(options()) == 0;
    ok_->setEnabled(valid);
    problem_->setVisible(!valid);
}

void WindowsRegionDialog::accept()
{
    if (CheckWindowsRegionOptions(options()) == 0)
        QDialog::accept();
}
}
