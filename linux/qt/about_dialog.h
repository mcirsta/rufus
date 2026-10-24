// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QDialog>

namespace rufus
{

class AboutDialog : public QDialog
{
    Q_OBJECT

public:
    explicit AboutDialog(QWidget* parent = nullptr);
};

} // namespace rufus
