// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "platform/windows_region_options.h"
#include <QDialog>

class QComboBox;
class QFormLayout;
class QLabel;
class QPushButton;

namespace rufus
{
// Input editor for the silent-install flow. It does not authorize or apply an
// installation. The caller must still obtain Rufus's destructive confirmation.
class WindowsRegionDialog : public QDialog
{
    Q_OBJECT
public:
    explicit WindowsRegionDialog(const WindowsRegionOptions& initial = {}, QWidget* parent = nullptr);
    WindowsRegionOptions options() const;
    void accept() override;

private:
    QComboBox* addLocale(QFormLayout* form, const QStringList& tags,
        const QString& label, const char* name, const QString& value);
    QComboBox* addTimezone(QFormLayout* form, const QString& windowsZone);
    void checkOptions();

    QComboBox* input_;
    QComboBox* system_;
    QComboBox* user_;
    QComboBox* language_;
    QComboBox* fallback_;
    QComboBox* timezone_;
    QLabel* problem_;
    QPushButton* ok_;
};
}
