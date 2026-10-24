// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "download_provider.h"
#include <QDialog>
class QComboBox;
class QDialogButtonBox;
class QFormLayout;
class QLabel;
class QCheckBox;
class QPushButton;
namespace rufus
{
class ImageDownloadDialog final : public QDialog
{
    Q_OBJECT
public:
    explicit ImageDownloadDialog(QWidget* parent=nullptr,const DownloadProviderSources& sources={});
    QUrl selectedUrl() const;
    bool useBrowser() const;
    void begin();
protected:
    void reject() override;
private:
    void advance();
    void back();
    void loaded();
    void languagesReady();
    void linksReady();
    void failed(int error,const QString& problem);
    void refresh();
    void showStage(int stage,const QString& title,const QStringList& items);
    DownloadProvider provider_;
    QComboBox* choices_[5]={};
    QLabel* labels_[5]={};
    QLabel* status_=nullptr;
    QCheckBox* browser_=nullptr;
    QPushButton *next_=nullptr,*back_=nullptr;
    int stage_=-1;
};
}
