// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "platform/windows_install_options.h"
#include <QDialog>
#include <QStringList>
class QComboBox;

namespace rufus
{
class WindowsImageDialog : public QDialog
{
    Q_OBJECT
public:
    WindowsImageDialog(const QStringList& archives, QWidget* parent = nullptr);
    WindowsImageDialog(const QList<WindowsEdition>& editions, QWidget* parent = nullptr);
    bool choosingArchive() const { return archive_; }
    QString selectedArchive() const;
    int selectedEdition() const;
private:
    void initialize(const QString& question);
    QComboBox* choice_ = nullptr;
    bool archive_ = false;
};
}
