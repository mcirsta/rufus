// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QDialog>
#include <QList>

class QCheckBox;
class QPushButton;

namespace rufus
{
class WindowsSilentDialog : public QDialog
{
    Q_OBJECT
public:
    explicit WindowsSilentDialog(QWidget* parent = nullptr);
    bool allConfirmed() const;
    void accept() override;

private:
    void checkConfirmations();
    QList<QCheckBox*> confirmations_;
    QPushButton* ok_;
};
}
