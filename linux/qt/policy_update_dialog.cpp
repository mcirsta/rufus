// SPDX-License-Identifier: GPL-3.0-or-later
#include "policy_update_dialog.h"
#include "application_storage.h"
#include "rufus_build_info.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace rufus
{
PolicyUpdateDialog::PolicyUpdateDialog(QWidget* parent) : QDialog(parent)
{
    setObjectName(QStringLiteral("policyUpdateDialog")); setWindowTitle(tr("Rufus update policy"));
    setWindowModality(Qt::WindowModal); setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    auto* layout = new QVBoxLayout(this);
    auto* description = new QLabel(tr("Check for Rufus Linux releases and refresh Secure Boot revocation lists from Rufus and Microsoft. "
        "Application updates open the release page for installation with your package manager. "
        "Newer DBX files are downloaded only after confirmation; firmware is not changed."),this);
    description->setWordWrap(true); layout->addWidget(description);
    layout->addWidget(new QLabel(tr("Check for policy updates:"),this));
    frequency_ = new QComboBox(this); frequency_->setObjectName(QStringLiteral("policyUpdateFrequency"));
    frequency_->addItem(tr("Disabled"), -1); frequency_->addItem(tr("Daily (Default)"),86400);
    frequency_->addItem(tr("Weekly"),604800); frequency_->addItem(tr("Monthly"),2629800);
    ApplicationSettings settings; const int interval = settings.value(QStringLiteral("UpdateCheckInterval"),0).toInt();
    int index = frequency_->findData(interval == 0 ? 86400 : interval);
    if (index < 0) { frequency_->addItem(tr("Custom (%1 seconds)").arg(interval),interval); index = frequency_->count()-1; }
    frequency_->setCurrentIndex(index); layout->addWidget(frequency_);
    connect(frequency_, &QComboBox::currentIndexChanged,this,&PolicyUpdateDialog::frequencySelected);
    application_ = new QCheckBox(tr("Automatically check for application updates"),this);
    application_->setObjectName(QStringLiteral("applicationUpdateConsent"));
    application_->setChecked(settings.value(QStringLiteral("ApplicationUpdateConsent"),false).toBool());
    application_->setEnabled(interval >= 0); layout->addWidget(application_);
    connect(application_,&QCheckBox::toggled,this,&PolicyUpdateDialog::applicationPreferenceSelected);
    layout->addWidget(new QLabel(tr("Include beta versions"),this));
    beta_ = new QComboBox(this); beta_->setObjectName(QStringLiteral("applicationUpdateBetas"));
    beta_->addItems({tr("Yes"),tr("No")});
    beta_->setCurrentIndex(settings.value(QStringLiteral("CheckForBetas"),false).toBool() ? 0 : 1);
    beta_->setEnabled(interval >= 0 && QLatin1String(RUFUS_NATIVE_ARCH) == QLatin1String("x86_64"));
    layout->addWidget(beta_); connect(beta_,&QComboBox::activated,this,&PolicyUpdateDialog::betaSelected);
    status_ = new QLabel(this); status_->setObjectName(QStringLiteral("policyUpdateStatus")); status_->setWordWrap(true); layout->addWidget(status_);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close,this);
    check_ = buttons->addButton(tr("Check now"),QDialogButtonBox::ActionRole); check_->setObjectName(QStringLiteral("policyCheckNow"));
    cancel_ = buttons->addButton(tr("Cancel check"),QDialogButtonBox::ActionRole); cancel_->setObjectName(QStringLiteral("policyCancelCheck")); cancel_->setEnabled(false);
    release_ = buttons->addButton(tr("Open release page"),QDialogButtonBox::ActionRole);
    release_->setObjectName(QStringLiteral("applicationReleasePage")); release_->hide();
    connect(release_,&QPushButton::clicked,this,&PolicyUpdateDialog::openReleasePage);
    connect(check_, &QPushButton::clicked,this,&PolicyUpdateDialog::checkRequested);
    connect(cancel_, &QPushButton::clicked,this,&PolicyUpdateDialog::cancelRequested);
    connect(buttons, &QDialogButtonBox::rejected,this,&QDialog::reject); layout->addWidget(buttons); resize(480,260);
}
void PolicyUpdateDialog::frequencySelected(int index)
{
    const int seconds = frequency_->itemData(index).toInt();
    ApplicationSettings settings; settings.setValue(QStringLiteral("UpdateCheckInterval"),seconds);
    application_->setEnabled(seconds >= 0);
    beta_->setEnabled(seconds >= 0 && QLatin1String(RUFUS_NATIVE_ARCH) == QLatin1String("x86_64"));
    emit frequencyChanged(seconds);
}
void PolicyUpdateDialog::applicationPreferenceSelected(bool enabled)
{
    ApplicationSettings settings; settings.setValue(QStringLiteral("ApplicationUpdateConsent"),enabled);
    emit applicationPreferenceChanged();
}
void PolicyUpdateDialog::betaSelected(int index)
{
    ApplicationSettings settings; settings.setValue(QStringLiteral("CheckForBetas"),index == 0);
}
void PolicyUpdateDialog::setApplicationRelease(const QUrl& page)
{
    releasePage_ = page; release_->setVisible(!page.isEmpty());
}
void PolicyUpdateDialog::openReleasePage() { if (!releasePage_.isEmpty()) emit releasePageRequested(releasePage_); }
void PolicyUpdateDialog::setProgress(const QString& message, bool active)
{
    status_->setText(message); check_->setEnabled(!active); cancel_->setEnabled(active);
    release_->setEnabled(!active);
}
}
