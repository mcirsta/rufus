// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "image_report.h"
#include "prepare_image_protocol.h"
#include <QDialog>
#include <QList>

class QCheckBox;
class QComboBox;
class QGridLayout;
class QLabel;
class QLineEdit;
class QPushButton;

namespace rufus
{
struct IsoWriteCheck;
class WindowsRegionDialog;
class WindowsSilentDialog;
class WindowsUserDialog : public QDialog
{
    Q_OBJECT
public:
    WindowsUserDialog(const RUFUS_IMG_REPORT& report, int mask, bool expertMode = false, QWidget* parent = nullptr, bool windowsToGo = false);
    int selectedFlags() const;
    QString username() const;
    void setInstallOptions(const IsoWriteCheck& checked);
    const PrepareImageRequest& options() const { return options_; }
    void accept() override;
    void done(int result) override;

private:
    void addChoice(int flag, const QString& text, const QString& tooltip);
    void checkOptions();
    PrepareImageRequest currentOptions() const;
    void chooseRegion();
    void regionFinished(int result);
    void warningFinished(int result);
    void closeChildDialogs();

    int available_;
    int mask_;
    bool windowsToGo_;
    QGridLayout* layout_;
    QList<QCheckBox*> choices_;
    QLineEdit* username_ = nullptr;
    QComboBox* edition_ = nullptr;
    QCheckBox* silent_ = nullptr;
    QPushButton* regionButton_ = nullptr;
    WindowsRegionDialog* regionDialog_ = nullptr;
    WindowsSilentDialog* warningDialog_ = nullptr;
    WindowsRegionOptions region_;
    QString installPath_;
    QString installProblem_;
    bool installAvailable_ = false;
    PrepareImageRequest options_;
    QLabel* problem_ = nullptr;
    QPushButton* ok_ = nullptr;
};
}
