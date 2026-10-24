// SPDX-License-Identifier: GPL-3.0-or-later
#include "image_download_dialog.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <cerrno>
namespace rufus
{
ImageDownloadDialog::ImageDownloadDialog(QWidget* parent,const DownloadProviderSources& sources)
    : QDialog(parent),provider_(this,sources)
{
    setObjectName(QStringLiteral("imageDownloadDialog")); setWindowTitle(tr("Download ISO Image")); setWindowModality(Qt::WindowModal);
    auto* layout=new QVBoxLayout(this); auto* form=new QFormLayout;
    const char* names[]={"version","release","edition","language","architecture"};
    for (int index=0;index<5;++index) {
        labels_[index]=new QLabel(this); choices_[index]=new QComboBox(this);
        choices_[index]->setObjectName(QString::fromLatin1(names[index])+QStringLiteral("Choice"));
        choices_[index]->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        choices_[index]->setMinimumContentsLength(32); form->addRow(labels_[index],choices_[index]);
        labels_[index]->hide(); choices_[index]->hide();
    }
    layout->addLayout(form);
    browser_=new QCheckBox(tr("Download using a browser"),this); browser_->setObjectName(QStringLiteral("downloadUsingBrowser")); browser_->hide(); layout->addWidget(browser_);
    status_=new QLabel(this); status_->setObjectName(QStringLiteral("downloadChoiceStatus")); status_->setTextFormat(Qt::PlainText); status_->setWordWrap(true); layout->addWidget(status_);
    auto* buttons=new QDialogButtonBox(this); next_=buttons->addButton(tr("Continue"),QDialogButtonBox::AcceptRole); next_->setDefault(true); next_->setObjectName(QStringLiteral("downloadContinue"));
    back_=buttons->addButton(tr("Back"),QDialogButtonBox::ActionRole); back_->setObjectName(QStringLiteral("downloadBack"));
    buttons->addButton(QDialogButtonBox::Cancel); layout->addWidget(buttons);
    connect(next_,&QPushButton::clicked,this,&ImageDownloadDialog::advance); connect(back_,&QPushButton::clicked,this,&ImageDownloadDialog::back);
    connect(buttons,&QDialogButtonBox::rejected,this,&ImageDownloadDialog::reject);
    connect(&provider_,&DownloadProvider::catalogReady,this,&ImageDownloadDialog::loaded);
    connect(&provider_,&DownloadProvider::languagesReady,this,&ImageDownloadDialog::languagesReady);
    connect(&provider_,&DownloadProvider::linksReady,this,&ImageDownloadDialog::linksReady);
    connect(&provider_,&DownloadProvider::failed,this,&ImageDownloadDialog::failed);
    refresh();
}
void ImageDownloadDialog::begin()
{
    if (provider_.isActive()) return;
    stage_=-1; browser_->setChecked(false);
    for (int index=0;index<5;++index) { choices_[index]->hide(); labels_[index]->hide(); choices_[index]->clear(); }
    const int error=provider_.load(); if (error!=0) failed(error,{}); refresh();
}
void ImageDownloadDialog::showStage(int stage,const QString& title,const QStringList& items)
{
    stage_=stage; labels_[stage]->setText(title); labels_[stage]->show(); choices_[stage]->clear(); choices_[stage]->addItems(items); choices_[stage]->show();
    status_->clear(); refresh();
}
void ImageDownloadDialog::loaded()
{
    QStringList choices; for (const auto& item : provider_.catalog().products) choices.append(item.name);
    showStage(0,tr("Version"),choices);
}
void ImageDownloadDialog::languagesReady()
{
    QStringList choices; for (const auto& item : provider_.languageChoices()) choices.append(item.display);
    showStage(3,tr("Language"),choices);
}
void ImageDownloadDialog::linksReady()
{
    QStringList choices; for (const auto& item : provider_.linkChoices()) choices.append(item.architecture);
    showStage(4,tr("Architecture"),choices);
}
void ImageDownloadDialog::advance()
{
    if (provider_.isActive()) return;
    const int product=choices_[0]->currentIndex(),release=choices_[1]->currentIndex(),edition=choices_[2]->currentIndex();
    int error=0;
    switch (stage_) {
    case -1: begin(); return;
    case 0: {
        QStringList choices; for (const auto& item : provider_.catalog().products[product].releases) choices.append(item.name);
        showStage(1,tr("Release"),choices); return;
    }
    case 1: {
        QStringList choices; for (const auto& item : provider_.catalog().products[product].releases[release].editions) choices.append(item.name);
        showStage(2,tr("Edition"),choices); return;
    }
    case 2: error=provider_.languages(product,release,edition); break;
    case 3: error=provider_.links(choices_[3]->currentIndex()); break;
    case 4: accept(); return;
    default: return;
    }
    if (error!=0) failed(error,tr("Cannot obtain the selected download choices."));
    refresh();
}
void ImageDownloadDialog::back()
{
    if (provider_.isActive() || stage_<=0) return;
    choices_[stage_]->hide(); labels_[stage_]->hide(); choices_[stage_]->clear(); --stage_; status_->clear(); refresh();
}
void ImageDownloadDialog::refresh()
{
    const bool busy=provider_.isActive();
    for (int index=0;index<5;++index) choices_[index]->setEnabled(!busy && index==stage_);
    next_->setEnabled(!busy); back_->setEnabled(!busy && stage_>0);
    next_->setText(stage_==4 ? tr("Download") : tr("Continue")); browser_->setVisible(stage_==4); browser_->setEnabled(!busy);
    if (busy) status_->setText(tr("Please wait..."));
}
void ImageDownloadDialog::failed(int error,const QString& problem)
{
    if (error==ECANCELED) return;
    status_->setText(problem.isEmpty() ? tr("Could not load the download provider. Try again.") : problem); refresh();
}
QUrl ImageDownloadDialog::selectedUrl() const
{
    const int index=choices_[4]->currentIndex();
    return stage_==4 && index>=0 && index<provider_.linkChoices().size() ? provider_.linkChoices()[index].url : QUrl();
}
bool ImageDownloadDialog::useBrowser() const { return browser_->isChecked(); }
void ImageDownloadDialog::reject() { provider_.cancel(); QDialog::reject(); }
}
