// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "image_source.h"
#include "qt/prepare_image_protocol.h"
#include <QByteArray>

namespace rufus
{
// Called by the ordinary caller when accepting its source selection.
int CapturePrepareSource(const QString& path, PrepareImageRequest& request);
int CapturePrepareArchive(const QString& path, PrepareArchiveSource& archive);
PrepareImageRequest PrepareArchiveRequest(const PrepareArchiveSource& archive);
int CheckPrepareSourceIdentity(const PrepareImageRequest& request, const IMAGE_FILE_SOURCE& source);

class PrepareIsoSource
{
public:
    explicit PrepareIsoSource(const PrepareImageRequest& request) : request_(request) {}
    ~PrepareIsoSource();
    PrepareIsoSource(const PrepareIsoSource&) = delete;
    PrepareIsoSource& operator=(const PrepareIsoSource&) = delete;
    int open(uid_t caller);
    int check() const;
    bool hasParentSources() const;
    int checkParentBacking(int disk, int callerHome) const;
    // Explicit checked close for jobs that publish only after descriptor cleanup.
    int close();
    const char* retainedPath() const { return retainedPath_.constData(); }
    const IMAGE_FILE_SOURCE& retainedSource() const { return source_; }

private:
    const PrepareImageRequest& request_;
    IMAGE_FILE_SOURCE source_ = {-1, {}, false, false, nullptr, 0};
    QByteArray retainedPath_;
};

// Shared bounded cache/helper reader. fd and initial fstat info are borrowed;
// check identity around reads, keep seek position and publish bytes only on success.
int ReadPrepareCoreSource(IMAGE_FILE_SOURCE& source, const QByteArray& path,
    quint64 maximumBytes, QByteArray& bytes);
// Helper preflight before target access: require caller-owned regular single-link
// input, exact frozen size/hash and sufficient embedding gap. Existing source
// checking detects mutation during reads; owned bytes survive later file removal.
// No signature/ABI claim, device writes, authorization or cache lookup.
int LoadPrepareGrub2Core(const PrepareImageRequest& request, uid_t caller,
    quint64 partitionOffset, QByteArray& bytes);

// Same retained-file checks, but the standalone GRUB4DOS filesystem loader is
// bounded by memory, not by the partition embedding gap. Required metadata and
// the source-free FAT32 operation must be valid before any reads.
int LoadPrepareSyslinuxResources(const PrepareImageRequest& request, uid_t caller, QByteArray& bytes);
int LoadPrepareGrub4dosLoader(const PrepareImageRequest& request, uid_t caller, QByteArray& bytes);

}
