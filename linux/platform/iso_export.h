// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "image_save.h"
namespace rufus {
constexpr int ImageSaveIso = 256;
int GetImageSaveFormat(const QString& path);
// Borrow a retained read-only source tree and the private output descriptor.
// Encode as UDF 1.02 only, verify every filename/directory/file and recheck the
// source. Publication, source-mount cleanup and checked closes belong to caller.
int ExportIsoImage(int directory, int output, const QString& outputPath, const QString& label,
    const QString& encoder, ImageSaveProgress progress, void* context, ImageSaveResult& result);
}
