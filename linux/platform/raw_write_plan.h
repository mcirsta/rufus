// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "device.h"
#include "image_source.h"
#include "udisks_client.h"

namespace rufus
{

// Lightweight SELECT check. START retains and revalidates a new confirmed plan.
QString CheckRawImageFile(const QString& path);

// One retained source and a copy of the selected target. Prepare before asking
// for confirmation; keep this object alive and unchanged through authorization
// and the writer. Preparing a plan does not open a target or authorize anything.
class RawWritePlan
{
public:
    RawWritePlan() = default;
    ~RawWritePlan();
    RawWritePlan(const RawWritePlan&) = delete;
    RawWritePlan& operator=(const RawWritePlan&) = delete;

    // First supported input: .img, whole 512-byte sectors, Rufus's 55 AA boot
    // marker, no WIM candidate. This is raw-mode eligibility, not a boot test.
    QString prepare(const QString& path, const Device& target);
    QString checkSnapshot(const UDisksObjects& objects, const QString& owner) const;
    QString checkSource() const;
    const IMAGE_FILE_SOURCE& source() const { return source_; }
    const QString& sourcePath() const { return path_; }
    const Device& target() const { return target_; }

private:
    IMAGE_FILE_SOURCE source_ = {-1, {}, false, false};
    QString path_;
    Device target_;
};

// Kernel facts for a received descriptor: block type/dev_t, capacity, read-only
// state, access flags and sector alignment. Never opens or writes a device.
QString CheckRawTargetDescriptor(int fd, const RawWritePlan& plan);

} // namespace rufus
