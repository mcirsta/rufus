// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QPushButton>
class QMenu;
namespace rufus
{
class ImageSelectButton final : public QPushButton
{
    Q_OBJECT
public:
    explicit ImageSelectButton(QWidget* parent=nullptr);
    bool downloadMode() const { return download_; }
    QSize sizeHint() const override;
signals:
    void modeChanged();
protected:
    void changeEvent(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
private:
    void selectMode();
    void downloadModeChosen();
    void showMenu();
    QMenu* menu_=nullptr;
    bool download_=false;
};
}
