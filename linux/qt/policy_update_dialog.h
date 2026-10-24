// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QDialog>
#include <QUrl>
class QComboBox;
class QLabel;
class QPushButton;
class QCheckBox;
namespace rufus
{
class PolicyUpdateDialog final : public QDialog
{
    Q_OBJECT
public:
    explicit PolicyUpdateDialog(QWidget* parent = nullptr);
    void setProgress(const QString& message, bool active);
    void setApplicationRelease(const QUrl& page);
signals:
    void checkRequested();
    void cancelRequested();
    void frequencyChanged(int seconds);
    void applicationPreferenceChanged();
    void releasePageRequested(QUrl page);
private:
    void frequencySelected(int index);
    void applicationPreferenceSelected(bool enabled);
    void betaSelected(int index);
    void openReleasePage();
    QComboBox* frequency_;
    QComboBox* beta_;
    QCheckBox* application_;
    QLabel* status_;
    QPushButton *check_, *cancel_;
    QPushButton* release_;
    QUrl releasePage_;
};
}
