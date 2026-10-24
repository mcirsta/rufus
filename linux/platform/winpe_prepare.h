// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>

namespace rufus
{
/* Prepare legacy WinPE files in the caller's retained, private extracted root.
 * Reuse SetupWinPE's directory selection and loader patches. All required
 * sources/text and destinations are checked before the first write. An existing
 * root NTDETECT.COM is retained unchanged, even with different content, as in
 * Rufus's no-replace copy. It must be a unique, nonempty, bounded regular file
 * and is rechecked across preparation; retention does not authenticate it.
 * An existing required root TXTSETUP.SIF is edited from its own content, keeping its spelling and
 * ownership/permissions. An existing root BOOTMGR must match the selected
 * directory SETUPLDR after one shared patch pass per input; patch mode edits
 * that root input, keeping its spelling/ownership/permissions, while keep-loader minint
 * retains it unchanged and requires unmodified equality. This permits ordinary
 * prepatched/partially patched inputs, not unrelated loaders. Repeated patching
 * need not be idempotent; equality establishes neither authenticity nor bootability.
 * Source paths match ASCII case-insensitively, with ambiguous names refused;
 * symlinks, hard-linked/special files and empty loaders are refused.
 * Each new output is verified/flushed and published without replacement;
 * root SIF/loader edits use identity-checked atomic replacement. Inputs and
 * committed root edits are retained/rechecked; directory sources are never edited.
 * Errors before replacement preserve that root file; a following sync error can
 * leave the complete edit committed but must fail the job. Returns 0 or errno.
 * Later failures may leave earlier complete outputs: this is not a transaction
 * for the whole set. Caller must abort the job on error and still perform final
 * filesystem/device sync, unmount and helper-exit checks before READY.
 * No privileges, partition/boot records, mounts or GUI enablement here. */
int PrepareWinPeRootFiles(int root, uint16_t winpe, bool usesMinint, bool optionsKnown);
}
