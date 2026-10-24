/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Read a private extracted SYSTEM hive and write Rufus's three Setup/LabConfig
 * DWORD values into a new hive file. Reuse the shared bypass_name array. Setup
 * must already exist, matching Windows's RegOpenKeyEx; LabConfig may be created.
 * Other values/subkeys are preserved. No source modification or hive mounting.
 *
 * Return 0 or errno. The source must be a regular file, not a symlink. The
 * destination must not exist, and both paths must stay in the caller's private
 * workspace. Library I/O uses retained descriptors through /proc/self/fd.
 * Flush and close failures or cancellation remove our newly created output;
 * cleanup failure is logged. Hivex commit itself cannot be interrupted midway.
 * Only a successful output may be passed to the WIM update stage. No elevation,
 * authorization or Windows API emulation is performed here.
 */
int PrepareWindowsSetupHive(const char* source, const char* destination);

/* Same private-source/new-output contract. Set the next-boot control set's
 * Services/partmgr/Parameters/SanPolicy DWORD to 4 (offline internal disks).
 * Read Select/Default from this SYSTEM hive; never assume ControlSet001 or use
 * host registry settings. FORCE_S_MODE sets Control/CI/Policy/SkuPolicyRequired
 * to DWORD 1 in that same control set. flags selects either or both policies;
 * no other flags are accepted. Missing/malformed selection or required parent
 * service/control key is an error. Existing values and unselected sets survive.
 */
int PrepareWindowsToGoHive(const char* source, const char* destination, int flags);

#ifdef __cplusplus
}
#endif
