// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "signed_download.h"
#include "platform/grub2_cache.h"
#include "platform/prepare_source.h"
#include <memory>

namespace rufus
{
enum class Grub2CoreOrigin { Embedded, Cache, Download };
enum class Grub2DownloadChoice { Yes, No, Cancel };
struct Grub2CoreSelectionResult
{
    int error = 0; // Nonzero stops preparation; failures contain no selected bytes.
    int candidateError = 0; // GRUB2 may fall back; a required standalone loader cannot.
    Grub2CoreOrigin origin = Grub2CoreOrigin::Embedded;
    QString cachePath;
    QString downloadVersion; // Server candidate, never a replacement for image detection.
    QByteArray bytes;
    QByteArray sha256;
};

// Unprivileged boot-file selection on the object's event-loop thread. The image
// mode chooses GRUB2 core bytes, not an installer/module ABI or SBR precedence.
// An Embedded success is GRUB2's existing resource fallback. Standalone GRUB4DOS
// instead requires owned grldr bytes; it never succeeds with an empty fallback.
// No helper launch, device I/O, privilege request or image report mutation.
class Grub2CoreSelection final : public QObject
{
    Q_OBJECT
public:
    explicit Grub2CoreSelection(QObject* parent = nullptr);
    ~Grub2CoreSelection() override;
    // source is the captured ISO identity. server is the caller's FILES_URL root
    // (or a loopback fixture). Accepted starts emit one completed result, possibly
    // synchronously. EINVAL/EBUSY starts emit nothing. Destruction silently aborts.
    int start(const PrepareImageRequest& source, const RUFUS_IMG_REPORT& report,
        const QString& filesDirectory, bool mbr, bool bios, quint64 partitionOffset,
        const QUrl& server);
    // Select standalone grldr through the same cache/download state machine.
    // Fixed bundled version/path; no ISO, provider inference or prefix fallback.
    // No/Cancel and unusable/missing/download/save/readback errors stop the job.
    int startGrub4dos(const QString& filesDirectory, const QUrl& server);
    int choose(Grub2DownloadChoice choice);
    void cancel();
    void invalidate(); // Changed image/target selection, even if the old ISO is intact.
    bool isActive() const { return active_; }
    bool isAwaitingChoice() const { return waiting_; }

signals:
    void downloadRequired(QString detectedVersion); // GRUB2 detected or standalone GRUB4DOS bundled version.
    void progress(quint64 received, quint64 total); // Per-candidate payload only.
    void completed(rufus::Grub2CoreSelectionResult result);

private:
    void nextCandidate();
    void downloadFinished(rufus::SignedDownloadResult result);
    void finish(int error);
    int checkSource() const;

    SignedDownload download_;
    PrepareImageRequest request_;
    RUFUS_IMG_REPORT report_ = {};
    std::unique_ptr<PrepareIsoSource> source_;
    Grub2CoreSelectionResult result_;
    QString filesDirectory_;
    QUrl server_;
    quint64 partitionOffset_ = 0;
    size_t before_ = 0;
    bool mbr_ = false;
    bool bios_ = false;
    bool active_ = false;
    bool waiting_ = false;
    bool signatureRejected_ = false;
    bool grub4dos_ = false;
};
}
Q_DECLARE_METATYPE(rufus::Grub2CoreSelectionResult)
