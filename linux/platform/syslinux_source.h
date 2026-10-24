// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "iso_analyze.h"
#include <QString>

namespace rufus
{
// Read-only eligibility for the qualified native root installer. Borrow the
// analysis's selected view and use the same Rock Ridge setting as extraction.
// Compare initial config lookup with Win32 Rufus, including its shared root
// redirect. Known versions use Win32's version/suffix selection for preserved
// modules. Unknown versions and any overwritten root companion need exact bytes
// from the pinned pair. Installed payload verification always stays exact.
// Returns errno with a diagnostic; never changes the analysis or opens a target.
// The caller retains/checks source identity and performs ordinary extraction
// preflight too. This does not parse config commands, certify other modules or
// prove bootability. Those, target ownership and post-install checks remain job
// responsibilities. Serialize all access to the borrowed reader handles.
int CheckNativeSyslinuxSource(const ISO_ANALYSIS_SOURCE& source, const ISO_ANALYSIS& analysis,
    bool rockRidge, int filesystem, quint32 sectorBytes, quint64 offsetBytes, QString& problem);
}
