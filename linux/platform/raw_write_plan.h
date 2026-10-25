// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "device.h"
#include "image_source.h"
#include "udisks_client.h"
#include "qt/prepare_image_protocol.h"

namespace rufus
{

// Raw DD eligibility follows the inspected bytes, independently of the suffix.
// An explicit marker override retains the sector and WIM checks.
QString CheckRawImageSource(const IMAGE_FILE_SOURCE& source, const QString& path, bool ignoreBootMarker = false);

// Lightweight SELECT check. START retains and revalidates a new confirmed plan.
QString CheckRawImageFile(const QString& path, bool ignoreBootMarker = false);

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

    // Raw inputs retain whole 512-byte sectors, the marker (unless explicitly
    // overridden) and WIM checks. Eligibility does not establish bootability.
    // expected supplies a chosen ISOHybrid DD identity or a complete container
    // analysis. Freeze the observed target sector/sequence and required bytes
    // (the authenticated minimum for optimized FFU). The helper rechecks the
    // captured logical stream/hash and plans actual target geometry before writes.
    QString prepare(const QString& path, const Device& target, const PrepareImageRequest* expected = nullptr, bool allowFixedUsb = false);
    // A mount list opts into candidate-only preflight; the helper enforces native protections.
    QString checkSnapshot(const UDisksObjects& objects, const QString& owner, QStringList* mounts = nullptr, int* partitionCount = nullptr) const;
    QString checkSource() const;
    const IMAGE_FILE_SOURCE& source() const { return source_; }
    const QString& sourcePath() const { return path_; }
    const Device& target() const { return target_; }
    bool allowsFixedUsb() const { return allowFixedUsb_; }
    quint64 writeBytes() const { return writeBytes_; }
    quint64 targetDiskSequence() const { return diskSequence_; }
    const PrepareImageRequest& imageMetadata() const { return compressed_; }

private:
    IMAGE_FILE_SOURCE source_ = {-1, {}, false, false, nullptr, 0};
    QString path_;
    Device target_;
    bool allowFixedUsb_ = false;
    PrepareImageRequest compressed_;
    quint64 writeBytes_ = 0;
    quint64 diskSequence_ = 0;
    quint32 zeroSectorSize_ = 0;
};

} // namespace rufus
