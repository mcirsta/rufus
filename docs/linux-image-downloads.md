# Native image downloads

CP66 adds the Windows Rufus SELECT/DOWNLOAD workflow to the Linux Qt application.
Choose **DOWNLOAD** from the SELECT button's arrow menu, then press the button.
Choose version, release, edition, language and architecture, and a destination.
The completed file is selected and analyzed automatically. **Download using a
browser** opens the chosen link instead. Ctrl+SELECT still chooses the additional
ZIP archive in either button mode. Button mode is session-only.

## Source and behavior

The Windows references are `src/rufus.c`'s IDC_SELECT/IDM_DOWNLOAD dispatch,
`src/stdlg.c:FidoCheck` and `src/net.c:DownloadISOThread`. The provider is
[Fido v1.71](https://github.com/pbatard/Fido/tree/v1.71), Copyright 2019–2026 Pete
Batard, GPL-3.0-or-later. The native catalog/parser and Microsoft/UEFI Shell
protocol adapter derive from that source. The icon conversion and Windows GUI
code are not reused.

The native client reads the same bounded `Fido.ver` manifest, using the shared
FIDO_VERSION selector, then verifies the selected compressed provider with
Rufus's original detached-signature key and the existing signed transport. It
reuses the original bounded LZMA decoder. Only catalog literals and protocol
identifiers are parsed; PowerShell code is never evaluated, loaded or executed.
The verified catalog is retained for the window's session.

The chooser includes Windows 11, Windows 10, UEFI Shell 2.2 and UEFI Shell 2.0,
with the provider's releases and editions. Windows requests use separate vendor
sessions for the edition IDs, obtain languages, then obtain the chosen language's
architecture links. The source's bounded empty/error-response retries, request
ordering and required Referer are retained. HTTP/service refusals remain errors;
there is no alternative mirror or transport downgrade. Shell architectures come
from the release's Version.xml; Release/Debug ISO names follow the original rules.

Choices start at the first entry; no Linux language, locale or CPU detection is
added. The metadata language is English (US), and all provider editions remain
available explicitly, including China editions. This follows the user's explicit
Linux rule to choose from Rufus's lists rather than infer settings from the host.

## Native adaptations

Windows enables the split button after its background Fido availability check,
which is coupled to its update policy. Linux exposes DOWNLOAD as an explicit
on-demand action: switching the button mode performs no network request, and
pressing DOWNLOAD loads the provider. Linux's existing Settings policy controls
Secure Boot data, so it does not silently serve as consent for image discovery.
Local SELECT remains usable when the provider is unavailable.

Windows downloads a script, validates it again before PowerShell execution and
receives its result through a named pipe. Linux verifies the immutable compressed
bytes and parses bounded data directly, avoiding script execution and the mutable
script/pipe boundary. The native protocol adapter is audited against 1.71. An
unsupported provider version/schema fails explicitly and asks for an updated
Linux adapter; it is not executed or replaced by a guessed fixed catalog.

Metadata is bounded to 4 MiB per response, 512 choices per vendor result, a
1-MiB decompressed provider and finite literal nesting/items. HTTPS certificates
and redirects use Qt's validation. The provider signature authenticates the
provider data. The ISO transfer uses the vendor's HTTPS link; its calculated
SHA256 is logged, without claiming an independently published ISO digest was
verified. The existing checksum button remains available afterward.

## Saving and cancellation

The GUI stays unprivileged. A worker streams bounded 64-KiB chunks to a private
file in the selected directory. It requires a positive Content-Length, checks
actual byte counts and keeps 64-bit lengths/progress for images larger than
4 GiB. Disk writes, sync and cleanup run in the worker. Progress is throttled;
full completion and automatic selection require worker exit. Closing/CANCEL
interrupts the download and joins cleanup; it does not authorize a device job.

The same retained-directory transaction used for device-image saving preserves
an existing destination until publication. It refuses symlinks and changed
entries. Where supported, an atomic exchange replaces an existing file. Native
NTFS on the qualified kernel rejects that exchange operation. The fallback moves
the old entry to a fresh private backup with a no-replace rename, verifies and
syncs it, then publishes the new entry with another no-replace rename. The old
backup is removed only after verified, synced publication. A concurrent entry
is not overwritten. A failed rollback retains the old file under its reported
recovery name. I/O/permission errors do not trigger a different filesystem/provider.

This fallback has a short interval where the original name is absent. A crash
in that interval can leave the original under the backup name, rather than
losing its data. Normal failure/cancellation restores the name before completion.
Late cancellation after successful publication can leave the complete file but
suppresses automatic selection. These differences provide coherent preservation
on a filesystem without exchange support; Windows's direct download can replace
the destination's contents during transfer. The same fallback also applies to
captured VHD/VHDX/FFU/ISO files without changing their content writers.

## Qualification

The grouped normal/ASan/UBSan/LSan checks cover signed provider loading, catalog
syntax/bounds, both Windows edition sessions, language/architecture requests,
Shell chooser/backtracking/browser choice, transport errors, actual 5-GiB+17-byte
transfer/full-file readback, disk-full/sync/cancel/shutdown behavior and automatic
image analysis. Five unsupported-exchange cases exercise successful replacement,
publication and directory-sync rollback, and concurrent-entry preservation.

A live native metadata check retrieved the signed 1.71 provider and Microsoft's
38 Windows 10 languages plus x86/x64 HTTPS download options. It did not download
an ISO. One combined networkless guest per build runs representative signed
provider/chooser/save/cancel/fault/automatic-selection cases as UID 1000 on real
native NTFS. The native exchange refusal is recorded directly. Large host and
guest artifacts are removed after compact evidence export. The existing shared
writer/helper/boot/polkit and agreed customization evidence is reused; no Windows
installation matrix or physical host device writing is added. One full nine-suite
Qt gate and inspection of both packages complete the delivery record.
