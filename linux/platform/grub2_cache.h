// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "image_report.h"
#include <QByteArray>
#include <QString>

namespace rufus
{
enum class Grub2CacheState { NotNeeded, Missing, Unusable, Ready };

struct Grub2CachedCore
{
    Grub2CacheState state = Grub2CacheState::NotNeeded;
    QString path;
    QByteArray bytes;
    QByteArray sha256;
};

// Read the original embedded Rufus core into owned bytes, checking its fit in
// the planned embedding gap. Return errno and clear bytes on failure. No source
// version/module inference, network, target access or privilege request.
int ReadEmbeddedGrub2Core(quint64 partitionOffset, QByteArray& core);
// Borrow the complete original resource; its first MBR sector is skipped by
// WriteGrub4dosSbrImage. The helper retains these immutable bytes before target access.
int ReadEmbeddedGrub4dosSbr(quint64 partitionOffset, QByteArray& resource);

// The ordinary caller supplies its files directory, not a privileged helper.
// Apply the shared BIOS/MBR image gate, then read grub-<full detected name>/core.img.
// No lookup for an unknown or embedded version; no shortened-name cache fallback.
// Missing means open failed (Win32 offers a download). Unusable means an opened
// file failed (Win32 falls back without re-prompting), or invalid arguments.
// ECANCELED/ESTALE always stop selection, rather than silently choosing fallback.
// Return errno and clear bytes/hash on failure. Ready owns a bounded byte snapshot;
// later cache changes cannot change it. Hash identifies bytes, not signature,
// detected GRUB version, module compatibility or authorization to write a device.
// partitionOffset is the planned first partition start; only Rufus's supported
// 512-byte-sector embedding gaps up to 1 MiB are accepted. No filesystem writes,
// directory creation, download, privileges or helper protocol changes.
int ReadGrub2CoreCache(const QString& filesDirectory, const RUFUS_IMG_REPORT& report,
    bool mbr, bool bios, quint64 partitionOffset, Grub2CachedCore& core);

// Ordinary caller only, after SignedDownload completed with error == 0.
// This storage operation does not authenticate bytes: never pass a partial or
// rejected download. The full detected name controls the cache directory,
// even if a shorter download candidate supplied the verified bytes.
// Reuse the lookup/path/gap rules, then atomically replace core.img using
// QSaveFile with direct writes disabled. Errors or cancellation observed before
// commit leave the previous file intact; empty created directories may remain.
// Success is the commit point; later cancellation cannot undo the replacement.
// Input bytes/report stay unchanged. No privileges, helper or network activity.
int StoreGrub2CoreCache(const QString& filesDirectory, const RUFUS_IMG_REPORT& report,
    bool mbr, bool bios, quint64 partitionOffset, const QByteArray& verifiedBytes);

// Standalone GRUB4DOS uses the original fixed grub4dos-<GRUB4DOS_VERSION>/grldr
// path, with no ISO report or version inference. Reuse the owned cache snapshot,
// bounded reader and atomic writer above. This is a filesystem loader, not an SBR:
// the existing reader's maximum size bounds memory, not an embedding gap.
// Cache reuse matches Windows (no signature check); downloads must be verified
// by SignedDownload before storage. No usable loader means the job must stop.
int ReadGrub4dosLoaderCache(const QString& filesDirectory, Grub2CachedCore& loader);
int StoreGrub4dosLoaderCache(const QString& filesDirectory, const QByteArray& verifiedBytes);

}
