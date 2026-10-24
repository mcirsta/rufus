// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "iso_analyze.h"
#include <QByteArray>
#include <QString>

namespace rufus
{
struct LegacyIsoResult
{
    quint64 files = 0, bytes = 0;
    int syncError = 0;
    QString path;
};

// Original embedded Syslinux 4.07 mboot.c32 and ReactOS config, selected even
// when the ISO contains no Syslinux config. Does not change scan facts.
int ReadReactOsIsoFiles(const RUFUS_IMG_REPORT& report, QByteArray& mboot, QByteArray& config);

// Read-only source eligibility in the analysis's selected ISO/UDF view. Check
// the actual loader and generated root paths before target access. Caller owns
// source identity, complete extraction preflight and original primary dispatch.
int CheckLegacyIsoFiles(const ISO_ANALYSIS_SOURCE& source, const RUFUS_IMG_REPORT& report,
    bool reactosSyslinux, QString& problem, bool rockRidge = true, quint64* extraBytes = nullptr);

// Match original ordering: ReactOS files precede ISO extraction, whose source
// files may replace them. Kolibri's root loader follows extraction. Borrow a
// checked FAT/NTFS private mount descriptor. No links or mount crossings; exact
// byte/attribute readback and fsync are included. Caller owns final device sync,
// mount cleanup and joined helper exit, including on incomplete-file errors.
int InstallReactOsIsoFiles(const RUFUS_IMG_REPORT& report, int directory, LegacyIsoResult& result);
int InstallKolibriIsoLoader(const ISO_ANALYSIS_SOURCE& source, int directory, LegacyIsoResult& result);
}
