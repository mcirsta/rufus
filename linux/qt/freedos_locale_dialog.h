// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "dos_locale_policy.h"
#include <QDialog>

class QComboBox;
class QLabel;
class QPushButton;

namespace rufus
{
// Explicit DOS settings, independent of the Linux desktop and display language.
// The caller copies selection() only after Accepted, before authorizing a job.
// This dialog does not start a write.
class FreeDosLocaleDialog : public QDialog
{
    Q_OBJECT
public:
    explicit FreeDosLocaleDialog(const QString& keyboard = QStringLiteral("us"),
        quint32 codepage = 437, QWidget* parent = nullptr, bool freedos = true);
    DOS_LOCALE selection() const;
    void accept() override;

private:
    void updateChoice();
    bool freedos_ = true;
    QComboBox* keyboard_;
    QComboBox* codepage_;
    QLabel* note_;
    QPushButton* ok_;
};
}
