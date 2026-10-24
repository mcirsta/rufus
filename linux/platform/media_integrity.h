// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QByteArray>
#include <QSet>
#include <QMap>
#include <QString>
#include <QTemporaryFile>
#include <memory>
#include "iso_analyze.h"

namespace rufus
{
struct MediaIntegrityResult
{
    quint64 files = 0, bytes = 0;
    unsigned updated = 0, validators = 0;
    int syncError = 0;
};

// One file-based creation job. Source hashes are supplied while copying,
// before any Rufus config/customization edits. Only explicitly modified files
// have existing checksums replaced. Unchanged source checksums retain their
// ability to detect a bad image or a bad copy.
class MediaIntegrity final
{
public:
    MediaIntegrity() = default;
    ~MediaIntegrity();
    MediaIntegrity(const MediaIntegrity&) = delete;
    MediaIntegrity& operator=(const MediaIntegrity&) = delete;
    // Borrow a private destination directory; duplicate it until finish/close.
    // manifestKind is the original report's has_md5sum: 0, md5sum.txt=1,
    // MD5SUMS=2. Runtime uses the bundled validators and md5sum.txt.
    int begin(int root, unsigned manifestKind, bool runtime);
    // Paths are relative UTF-8 output names. The digest is the 16-byte source
    // MD5. Generated manifests are streamed and bounded to 64 MiB.
    int copied(const char* path, quint64 size, const unsigned char* md5);
    int modified(const char* path);
    int relocated(const char* original, const char* destination);
    int linked(const char* path, const char* target);
    // Hash a newly generated SWM in its private stage before publication.
    int splitPart(const char* original, int stage, const char* name);
    // Complete metadata updates and validator installation, sync, and release
    // all descriptors. On any failure, release private stages too. No rollback
    // of an already prepared target; callers must retain FAILED.
    int finish(MediaIntegrityResult& result);
    int close();
    bool active() const { return root_ >= 0; }
    bool generatesManifest() const { return generated_ != nullptr; }

private:
    int updateManifest(QByteArray& bytes, const QSet<QString>& renamed, unsigned& updates);
    int publish(const QString& name, const QByteArray& bytes);
    int installValidators(QSet<QString>& renamed, MediaIntegrityResult& result);
    int root_ = -1;
    unsigned kind_ = 0;
    bool runtime_ = false;
    int syncError_ = 0;
    quint64 generatedBytes_ = 0, files_ = 0, bytes_ = 0, modifiedNameBytes_ = 0;
    QSet<QString> modified_;
    QMap<QString,QByteArray> relocations_, aliases_;
    QSet<QString> replaced_;
    QByteArray replacementLines_;
    std::unique_ptr<QTemporaryFile> generated_;
};
// Read-only resource lookup, with the same architecture names as Windows.
int ReadMediaValidator(int architecture, QByteArray& bytes);
// Retained read-only source. Validate reserved names/budgets and return extra
// staging/payload space before target writes. splitParts comes from WIM planning.
int CheckMediaIntegritySource(const ISO_ANALYSIS_SOURCE& source, const ISO_ANALYSIS& analysis,
    bool runtime, unsigned splitParts, bool rockRidge, quint64& reserve, QString& problem);
}
