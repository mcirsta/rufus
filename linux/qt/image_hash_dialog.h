// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "image_hash_job.h"
#include <QDialog>

namespace rufus
{

class ImageHashDialog final : public QDialog
{
    Q_OBJECT
public:
    explicit ImageHashDialog(const ImageHashResult& result, QWidget* parent = nullptr);
};

} // namespace rufus
