// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "image_compression.h"
#include <QByteArray>
#include <QString>
#include <sys/stat.h>

namespace rufus
{
enum class ImageSavePhase { Copy, VerifyImage, VerifySource, Sync };
using ImageSaveProgress = void (*)(void*, ImageSavePhase, quint64, quint64);

struct ImageSaveResult
{
    int error = 0;
    bool committed = false;
    quint64 bytes = 0;
    QByteArray sha256;
};

// Bounds/format checks without reading or changing either descriptor.
int ImageSaveMaximumBytes(quint64 size, unsigned sector, int format, quint64& maximum);
int CheckImageSaveDevice(int descriptor, quint64 device, quint64 size, quint64 sequence, unsigned& sector);
// Borrow a read-only source and an empty, private, read/write regular output.
// All I/O is positioned. Caller retains the claim, closes both descriptors and
// atomically publishes the destination only after this returns verified success.
// The output path must name that inode and end in the selected format suffix.
int SaveDeviceImage(int source, quint64 size, unsigned sector, int output,
    const QString& outputPath, int format, ImageSaveProgress progress, void* context,
    ImageSaveResult& result);
}
