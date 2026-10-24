// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "image_source.h"
#include <QString>

namespace rufus
{
struct ZipExtractResult
{
    quint64 files = 0, directories = 0, bytes = 0;
    QString path;
    int syncError = 0;
};

// Borrow a retained source and an optional private destination directory.
// Preflight validates all names, sizes and CRCs without writes. Extraction
// repeats preflight, copies in CDF order and overwrites ordinary single-link
// files, as Windows Rufus does. Reject path escape and target links/specials;
// keep completed entries and remove the current incomplete output on failure.
// maximumFile/maximumTotal are explicit output bounds; zero file limit means
// the total limit. No mounts, device selection or authorization are performed.
int CheckZipSupplement(const IMAGE_FILE_SOURCE& source, const QString& path,
    quint64 maximumFile, quint64 maximumTotal, ZipExtractResult& result);
int ExtractZipSupplement(const IMAGE_FILE_SOURCE& source, const QString& path, int directory,
    quint64 maximumFile, quint64 maximumTotal, ZipExtractResult& result);
}
