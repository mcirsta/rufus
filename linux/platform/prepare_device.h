// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "fat_format.h"
#include "qt/prepare_image_protocol.h"

namespace rufus
{
// Read-only sysfs identity, captured by the ordinary caller before authorization.
quint64 ReadDiskSequence(quint64 device);

// One native target for the helper's GPT/FAT32 operation. Owns the exclusive
// whole-disk descriptor, releases it for mkfs, then reacquires and revalidates.
class PrepareDeviceTarget
{
public:
    explicit PrepareDeviceTarget(const PrepareImageRequest& request);
    ~PrepareDeviceTarget();
    PrepareDeviceTarget(const PrepareDeviceTarget&) = delete;
    PrepareDeviceTarget& operator=(const PrepareDeviceTarget&) = delete;
    int open();
    int check();
    int descriptor() const { return fd_; }
    int format(const FAT32_FORMAT_OPTIONS& options, const char* mkfs, FAT32_FORMAT_RESULT& result);

private:
    int checkSnapshot();
    int checkDescriptor(int fd, quint64 device, quint64 size);
    int openPartition(const FAT32_FORMAT_OPTIONS& options, quint64& device);

    const PrepareImageRequest& request_;
    int fd_ = -1;
};
}
