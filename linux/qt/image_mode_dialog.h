// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "image_policy.h"
#include <QDialog>

class QButtonGroup;

namespace rufus
{

// The ISOHybrid part of Win32 SelectionDialog. The caller checks backend
// availability before opening it; accepting this dialog never starts a write.
class ImageModeDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ImageModeDialog(const IMAGE_WRITE_CHOICES& choices, QWidget* parent = nullptr);
    IMAGE_WRITE_MODE selectedMode() const;

private:
    IMAGE_WRITE_CHOICES choices_;
    QButtonGroup* buttons_;
};

} // namespace rufus
