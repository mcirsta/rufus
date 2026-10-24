# Filesystem metadata policy — CP77 / Preview46

This release work preserves the saved **Alt+Q** file-indexing choice for supported
UDF data formatting and records provider limitations explicitly. Existing UDF
1.02, 1.50, 2.00 and 2.01 formatting, labels/icons, supplementary ZIP contents,
geometry and completion rules remain supported. UDF 2.50/2.60 metadata partitions
and ReFS formatting remain deferred.

## UDF indexing is an optional unsupported operation

Windows Rufus `src/format.c` attempts `FILE_ATTRIBUTE_NOT_CONTENT_INDEXED` on
NTFS, UDF and ReFS when file indexing is disabled. Failure logs a diagnostic and
does not fail the format. When indexing is enabled, it skips this disabling
attempt; it does not enable or run an indexing service.

Linux now freezes the saved choice with a UDF job separately from NTFS
compression. With indexing disabled, a successful UDF format reports that the
Linux UDF provider does not expose the Windows attribute and continues normally.
With indexing enabled, that unsupported disabling operation is skipped. NTFS
keeps its existing real root-attribute implementation. Other filesystems and DD
jobs do not acquire UDF indexing options.

**The Windows UDF indexing attribute is not implemented on disk.** This is an
explicit native provider limitation, not a claim of equivalent metadata or a
claim that the Windows UDF driver rejects the request. Its actual Windows driver
behavior remains unverified. Linux desktop search settings are not substituted.
UDF formatting/readback/sync failures still fail the job; only this optional
attribute operation is nonfatal.

The [UDF 2.01 specification](https://13thmonkey.org/documentation/UDF/udf201.pdf),
sections 3.3.1–3.3.4, defines Windows hidden/system/permission mappings without a
NOT_CONTENT_INDEXED mapping. The [Linux UDF implementation](https://raw.githubusercontent.com/torvalds/linux/master/fs/udf/file.c)
exposes no corresponding attribute-write interface. Windows's numeric 0x2000
must never be copied into UDF ICB flags: that bit means STREAM in UDF.

## Dev Drive protection deferred by the user

After the provider audit, the user explicitly chose to skip Dev Drive detection
for this release (2026-10-24). **Actual Dev Drive recognition is not implemented.**
There is no blanket refusal of ReFS virtual disks and no label-based substitute.
Existing system/application/source/home protections and configured exclusions
remain in force. They do not establish that a selected virtual disk is not a
Windows Dev Drive; verify the chosen disk before authorizing destructive work.

Windows Rufus excludes detected Dev Drives under its virtual-drive rule by
querying `FSCTL_QUERY_PERSISTENT_VOLUME_STATE`. The
[Microsoft SDK](https://github.com/microsoft/win32metadata/blob/main/generation/WinSDK/RecompiledIdlHeaders/um/winioctl.h)
distinguishes the format-time DEV_VOLUME state (0x2000) from machine-local
TRUSTED_VOLUME state (0x4000). A Windows API flag value does not specify its raw
ReFS location. The audited refsprogs, libfsrefs and forefst sources did not
provide a verified mapping. No invented raw bit or synthetic positive fixture
is presented as actual Dev Drive evidence.

Exact-commit/hash provider research is retained in
`../../.rufus-test-artifacts/cp77/dev-drive-provider-research.json`. Full Windows
parity remains incomplete. See the [release plan](linux-release-plan.md).

## Batched qualification

Three grouped component suites pass 163 substantive cases in each normal/sanitizer build, with zero failures/skips; two focused UDF selection rows pass in each build. One combined native route per build qualifies two ordinary UDF jobs (GPT/SFD, indexing disabled/enabled), exact optional diagnostics, saved/frozen preferences, independent Unicode label/icon/ZIP readback, one authorization and sync/cleanup/helper exit before full READY. One full nine-suite Qt gate passes 2,217 substantive cases with 2 existing optional skips and zero failures. Unchanged CP76 geometry, writer/helper faults and boot evidence are reused.

Protocol cases cover all four supported revisions and MBR/GPT/SFD, canonical
true/implicit false, malformed fields, conflicting NTFS options and ineligible
operations. Focused UI cases preserve saved state across NTFS/FAT32/UDF and
freeze it before authorization. Native media checks cover both values, exact
diagnostics, labels/icons/ZIP, cleanup and READY after helper exit. Existing
full-format, sector geometry and boot/fault evidence is reused.

The initial runner's inherited NTFS-only package flags were refused before guest
startup; corrected runner flags pass. Receipts retain that attempt. No Windows
execution or host physical-media writes were added. The A03 actual-metadata
parity gap stays open; its useful-release native limitation is qualified.
