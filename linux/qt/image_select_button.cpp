// SPDX-License-Identifier: GPL-3.0-or-later
#include "image_select_button.h"
#include <QKeyEvent>
#include <QEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QStyleOptionButton>
#include <QStylePainter>
namespace rufus
{
ImageSelectButton::ImageSelectButton(QWidget* parent) : QPushButton(parent)
{
    setAccessibleDescription(tr("Use the Down arrow to choose SELECT or DOWNLOAD."));
    menu_=new QMenu(this);
    auto* select=menu_->addAction(tr("SELECT")); select->setObjectName(QStringLiteral("selectImageAction"));
    auto* download=menu_->addAction(tr("DOWNLOAD")); download->setObjectName(QStringLiteral("downloadImageAction"));
    connect(select,&QAction::triggered,this,&ImageSelectButton::selectMode);
    connect(download,&QAction::triggered,this,&ImageSelectButton::downloadModeChosen);
    setText(tr("SELECT"));
}
void ImageSelectButton::selectMode() { download_=false; setText(tr("SELECT")); emit modeChanged(); }
void ImageSelectButton::downloadModeChosen() { download_=true; setText(tr("DOWNLOAD")); emit modeChanged(); }
void ImageSelectButton::changeEvent(QEvent* event)
{
    QPushButton::changeEvent(event);
    if (event->type() != QEvent::LanguageChange) return;
    menu_->actions()[0]->setText(tr("SELECT"));
    menu_->actions()[1]->setText(tr("DOWNLOAD"));
    setText(download_ ? tr("DOWNLOAD") : tr("SELECT"));
    setAccessibleDescription(tr("Use the Down arrow to choose SELECT or DOWNLOAD."));
}
QSize ImageSelectButton::sizeHint() const
{
    QSize size=QPushButton::sizeHint(); size.rwidth()+=style()->pixelMetric(QStyle::PM_MenuButtonIndicator); return size;
}
void ImageSelectButton::paintEvent(QPaintEvent*)
{
    QStylePainter painter(this); QStyleOptionButton option; initStyleOption(&option);
    option.features|=QStyleOptionButton::HasMenu; painter.drawControl(QStyle::CE_PushButton,option);
}
void ImageSelectButton::showMenu() { menu_->popup(mapToGlobal(QPoint(0,height()))); }
void ImageSelectButton::mousePressEvent(QMouseEvent* event)
{
    const int width=style()->pixelMetric(QStyle::PM_MenuButtonIndicator)+6;
    const bool arrow=layoutDirection()==Qt::RightToLeft ? event->position().x()<width : event->position().x()>=this->width()-width;
    if (event->button()==Qt::LeftButton && arrow) { showMenu(); event->accept(); return; }
    QPushButton::mousePressEvent(event);
}
void ImageSelectButton::keyPressEvent(QKeyEvent* event)
{
    if (event->key()==Qt::Key_Down || event->key()==Qt::Key_Menu) { showMenu(); event->accept(); return; }
    QPushButton::keyPressEvent(event);
}
}
