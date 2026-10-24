# Windows To Go and standalone WIM/ESD (CP58)

Preview 27 adds Windows To Go creation from a Windows ISO or a complete standalone
WIM/ESD. The ordinary archive/edition chooser, NTFS deployment, GPT ESP/MSR or MBR
BIOS/UEFI layout, native BCD/boot files and installed customization run in one
authorized helper. READY waits for writes, sync, private cleanup and helper exit.

Native deployment preserves ACLs, named streams, hard links, DOS names, reparse
points, timestamps, Windows EAs and EFS encrypted content. Offline SAN and S-mode
policy are implemented without running Windows tools. Normal/sanitizer components,
combined disposable Linux guests and the full Qt gate pass. Qualification covers
native bytes and a test EFI handoff; Windows execution/installation and decryption
remain deferred under the user's testing limit.

## Windows source contract

- `src/rufus.c:UpdateImage` offers only Windows To Go for standalone Windows
  images. `ImageScanThread` reads image 1's version and
  `Windows/Boot/EFI/bootmgr.efi` to report EFI architecture.
- `src/wue.c:SetWinToGoIndex` chooses an install archive and then its native
  image index before formatting. Labels use DISPLAYNAME, DESCRIPTION, then
  “Unknown Windows Version”. Linux keeps the explicit choice and first-index
  default; it must not infer an edition from this host.
- `src/format.c:FormatThread` uses the Windows To Go branch instead of ISO tree
  extraction. `SetupWinToGo` applies the selected WIM/ESD image to NTFS, formats
  the ESP when selected, invokes bcdboot, applies the requested internal-drive
  offline policy and disables recovery in the BCD store.
- `src/format_policy.c:GetExtraPartitions` supplies the shared layout policy.
  Keep its Windows To Go ESP/MSR and BIOS/UEFI decisions when integrating the
  native writer. Standalone WIM/ESD and ISO-based Windows To Go share the same
  deployment and boot setup.

Linux uses native BCD/boot-file adapters for those Windows command operations.
Their native integration is covered by the combined delivery gate below.
No Windows application or Windows installation test is scheduled.

For the native BCD adapter, use the
[Microsoft boot manager/loader settings](https://learn.microsoft.com/en-us/windows-hardware/manufacture/desktop/bcd-system-store-settings-for-uefi)
and [qualified partition identity](https://learn.microsoft.com/en-us/previous-versions/windows/desktop/bcd/bcddevicequalifiedpartitiondata)
contracts: boot manager device/path identify the system partition and EFI
manager; the loader's device/osdevice identify the Windows partition and its
path identifies winload. Template object GUIDs vary across Windows versions;
derive the selected loader from the template's role/path rather than assume
one fixed GUID. The current user's template contains separate BIOS/EFI loader
and PE factory objects, so its default PE object cannot simply be reused.

## Native NTFS deployment

The pinned upstream wimlib build now includes its offline NTFS backend,
preferring libntfs from ntfsprogs-plus and otherwise libntfs-3g. Both additional
upstream source files are hash pinned. Only their include prefixes are adapted
in build copies; the algorithms stay upstream. The selected runtime library is
also included in generated package dependencies.

This is direct access to an unmounted NTFS volume. It does not choose or switch
the kernel/FUSE mount driver. Subsequent private mounts continue to prefer the
kernel `ntfs` driver under the existing policy.

The Unix directory backend cannot preserve the complete Windows metadata
needed for deployment. The offline NTFS backend preserves ACLs, alternate data
streams, hard links, DOS names, reparse points, attributes and NTFS timestamps.
The [wimlib apply documentation](https://wimlib.net/man1/wimapply.html) describes
that supported installation workflow and its upstream encrypted-file/EA
limitations. The native adapter now restores standard Windows WIM extended
attributes as bounded, aligned NTFS EA records, including binary and empty
values. Independent NTFS readback verifies the result.

`wimlib/native_ntfs_efs.*` restores the WIM's serialized EFS raw stream directly
to unmounted NTFS. It preserves ciphertext, `$EFS` metadata, encrypted and plain
named streams, hard links, sparse holes, empty streams/files/directories, logical
size and valid-data length. It does not need encryption keys or decrypt files.
The bounded streaming parser follows Microsoft's
[MS-EFSR raw format](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-efsr/a38459f2-40e9-418d-866c-d33789886a91)
and the logical-unit indexing described in the
[Windows SDK's ENCRYPTED_DATA_INFO](https://github.com/microsoft/win32metadata/blob/main/generation/WinSDK/RecompiledIdlHeaders/um/winioctl.h).
Serialized block lengths do not rescale logical unit offsets; oversized blocks
across VDL and a sparse hole are covered in the shared fixture. Serialized
padding beyond a logical unit is consumed without allocating the next unit's
hole. The source plan derives the largest NTFS cluster that can represent the
holes. Larger requested clusters are refused before target access; direct apply
also checks the formatted volume before writes. This follows the SDK's sparse
unit restriction and prevents replacing plaintext-zero holes with allocated
zero ciphertext. The format's reserved zero WORD
is not an additional supported compression format. Impossible encrypted and
compressed/reparse WIM attribute combinations are refused.

The same parser validates every selected-image EFS blob and its hash before
target access. Truncation, bad lengths/metadata, duplicate streams and trailing
bytes fail before writes. Actual apply checks cover XPRESS/LZX WIM and solid
LZMS ESD with independent raw-runlist NTFS readback, plus short writes, partial
failure and cancellation. Raw-runlist readback leaves the saved length fields
unchanged; the provider's ordinary raw-read API refuses streams with VDL below
logical size. Ciphertext and metadata are synthetic test bytes; decrypting an
installed Windows file remains unverified. This component evidence does not
qualify the complete device route.

`platform/windows_apply.*` checks selected-image resources and output size,
counts hardlinked payloads once, refuses unsupported device entries, checks the
retained outer source around work and progress, and applies through a borrowed
target descriptor. It does not discover devices or authorize a job. Cancellation
and errors leave a partial filesystem; flushing is attempted on every started
apply. The caller must still finish boot setup, sync and private cleanup, and
join the helper before successful full READY.

## Native boot configuration

`platform/windows_bcd.*` generates BIOS or EFI stores from the selected image's
BCD template using Hivex. It selects a non-PE loader by role and path, preserves
inherited settings, and encodes the actual MBR signature/partition offset or GPT
disk/partition GUIDs. Boot manager, loader, resume and memory diagnostic devices
and paths are configured for the selected firmware route; automatic recovery is
disabled as in `SetupWinToGo`. The borrowed template remains unchanged.

`platform/windows_bootloaders.*` stages the selected image's PCAT/EFI boot files,
fonts, localized resources and generated BCD in a private directory. It supports
the explicit CA2023 file set and architecture-specific EFI fallback path. Copying
uses checked atomic replacement, synchronization and source identity checks;
links and a mismatched EFI architecture are refused. Staging before unmount
allows the helper to transfer files to a separate ESP using its single private
mount at a time. The helper now calls this sequence and the FAT/NTFS DOS-attribute
adapter, then finishes filesystem/device sync and cleanup. That complete device
route passes the combined guest qualification below.

The BCD and boot-tree component group passes in normal and sanitizer builds,
including BIOS, dual mode, ARM64 EFI, failure handling and the user's real
Windows template. These checks validate generated files; they do not establish
firmware boot or the completed ordinary creation workflow.

## Layout and installed customization

The native GPT writer now supports the original `drive.c` Windows To Go layout:
a 260-MiB EFI System Partition, a 128-MiB Microsoft Reserved Partition, then
the Windows data partition. Track/cluster alignment, types, names, both tables,
CRCs and generated GUIDs are verified. This layout excludes persistence, the
small UEFI:NTFS payload and the main-partition ESP option. Its Windows partition
is number 3; the helper now uses that number for formatting, offline apply and
private mounting, and number 1 for the ESP. BIOS/MBR and MBR/UEFI keep their
existing data/payload layout policy.

`ApplyWindowsToGoCustomization` writes the shared answer to `Windows/Panther`
in the applied image, preserving an existing answer. The selected internal-drive
offline policy edits this image's SYSTEM hive through Hivex, setting
`Services/partmgr/Parameters/SanPolicy` to DWORD 4 in the control set named by
`Select/Default`. This follows Microsoft's
[offline-internal policy](https://learn.microsoft.com/en-gb/previous-versions/windows/it-pro/windows-10/deployment/windows-to-go/deploy-windows-to-go)
and [offline next-boot control-set selection](https://learn.microsoft.com/en-us/troubleshoot/windows-client/performance/stop-error-7b-or-inaccessible-boot-device-troubleshooting).
No host registry or regional settings are read. The prepared hive is copied
back through its original descriptor, retaining its inode, ACLs, streams and
hard links. A partial write or failed sync fails the operation; there is no
rollback. The caller still owns final filesystem/device sync and cleanup.

The native offline S-mode adapter applies the selected policy to
`Control/CI/Policy/SkuPolicyRequired` as DWORD 1 in the SYSTEM hive's next-boot
control set. SAN and S-mode selections share one checked hive edit and preserve
unselected values and control sets. The original Windows operation applies the
answer with DISM; copying its XML alone would not perform this step. Microsoft's
[S-mode setting contract](https://learn.microsoft.com/en-us/windows-hardware/customize/desktop/unattend/microsoft-windows-codeintegrity-skupolicyrequired)
requires offline servicing. Read-only inspection of both CodeIntegrity component
manifests in the supplied ISO (10.0.26100.1591 and 10.0.26100.8036) confirms the
setting's `REG_DWORD` binding to that registry key. A native Linux PA30 decoder
and the image's WCP dictionary expanded these DCM1 manifests; no Windows code was
executed. The expanded XML has no intrinsic PA30 target hash and was used only
for read-only contract inspection, not as authenticated deployment input.
The expert To Go dialog now offers S-mode with Rufus's existing warning and
default-off behavior. The original installer implementation stays available.
CA2023 selection belongs to the boot-file stage above; this answer
adapter does not independently install its boot files.

## Retained source and authorized job

`WindowsToGoImage` retains the complete standalone WIM/ESD or embedded ISO/UDF
reader chain. The frozen request names the archive and native image index, plus
the firmware and partition choices. An embedded archive must occur in the fresh
ISO scan's list; a standalone archive uses an empty internal path. Before target
access, the helper checks geometry, selected resources, architecture, boot-tree
types, BCD-template suitability, customization eligibility and existing answers.
It repeats outer-source identity checks through apply and final cleanup.

The two native To Go operations use the existing authorized helper. It formats
NTFS, applies the image while unmounted, stages boot files from the private
Windows mount, applies selected customization, then unmounts it. GPT jobs mount
the freshly formatted ESP and install the stage there. MBR jobs use the main
volume and the shared UEFI:NTFS policy. The helper checks DOS-attribute readback,
sync, private-mount cleanup, stage removal and explicit source/target closes
before reporting To Go completion. The ordinary controller must still join the
helper and launcher before successful READY.

BCD uses the actual GPT disk/partition GUIDs or MBR disk signature and partition
offset. UEFI/MBR To Go retains a generated nonzero signature while keeping zero
bootstrap code. Ordinary zero-code formatting still clears the signature. This
native To Go distinction provides a stable qualified BCD identity without relying
on Windows mounting or assigning an identity during setup. Independent file-target
checks verify both that signature and the zero bootstrap bytes.

`test_windows_to_go` passes 14 grouped source/protocol/identity cases in each
build: WIM, solid ESD, both embedded in ISO, source/selection/architecture/answer
refusal, and GPT/UEFI plus MBR BIOS/UEFI identities. The shared protocol/helper
regression adds 19 focused cases, including existing completion, cleanup and
sync-failure paths. The combined delivery gate below separately qualifies the
actual device writes, private mounts, boot attributes and ordinary controller.

## Ordinary GUI workflow

Standalone Windows WIM/ESD selection now reads image 1's metadata and EFI
architecture through the retained descriptor and offers Windows To Go. Windows
ISOs retain the standard-installation choice and offer To Go separately. To Go
uses NTFS and the native GPT ESP/MSR layout or explicit MBR BIOS/UEFI selection.

Before confirmation and authorization, the controller offers an archive chooser
when the ISO contains several installation archives, then an edition chooser.
Native image indices remain distinct even when labels repeat; the first image is
the default. Inspection does not require image 1 to fit or have all deployment
resources before the user can choose another edition. The selected image then
undergoes the strict deployment preflight. No host edition is inferred.

The existing Windows User Experience dialog uses the To Go option set, including
the internal-drive offline policy. Windows regional-settings copying stays
visible and disabled. S-mode is available only through the existing expert mode.
Confirmation names Windows To Go, the selected edition and actual target layout.
The confirmed preflight rechecks retained sources and the frozen apply/layout
plan. Successful completion requires the exact applied byte count and explicit
To Go marker, followed by the existing helper/launcher join.

The focused normal/sanitizer group covers 14 source/protocol/identity cases,
three dialog cases and five GUI cases: standalone WIM, ISO WIM, multiple-archive
selection followed by edition cancellation, a missing completion marker and the
ordinary Windows-selection regression. The protocol-only GUI child never opens
the target. Success stays below full READY while that child remains alive. This
does not replace the actual-device combined guest gate.

## Batch qualification

CP58 is one delivery batch, packaged as Preview 27. Normal and sanitizer results
cover 284 component cases per build, excluding lifecycle hooks. The sanitizer
nine-suite gate is followed by the three affected suites after the final EFS,
geometry and boot-attribute fixes; the other six suites reuse their passed results.
The selected user's real BCD template is included in the final BCD group.

Each combined Linux guest performs four ordinary controller/device jobs: standalone
WIM and solid ESD on GPT ESP/MSR, ISO-WIM on MBR/BIOS and ISO-ESD on MBR/UEFI.
Independent NTFS3/ESP readback checks edition 2, payloads, SAN policy, actual
partition identities in BCD and stored DOS protection flags. Every job has one
guest authorization stand-in, an explicit completion marker and READY after
helper/launcher exit. Existing real desktop polkit qualification is reused.
One native EFI marker boots the final USB through the original UEFI:NTFS payload
in each build; its backing disk stays unchanged. No Windows program is executed.

The full nine-suite Qt gate passes 2,190 cases excluding lifecycle hooks, with five
unselected optional real-media/unknown-companion cases skipped. It runs once for
this batch. The EFS group passes 19 cases per build, including holes inside VDL,
ciphertext after a hole, oversized serialized padding, short/failed writes,
cancellation and prewrite refusal when the target cluster is too large.
EFS ciphertext/key metadata are synthetic and its independent readback uses file
targets; the combined guest images are Windows-shaped synthetic sources.
Native 512-byte device sectors, 4-KiB NTFS clusters and the preferred kernel driver
are the guest configuration. Other existing provider/geometry limits still apply.

Integration found and repaired the ISO-oriented phase sequence: To Go applies
before mounting, then stages boot files and emits one finalization phase through
ESP installation. A skipped operation cannot produce successful READY. Fresh NTFS
directories can initially omit DIRECTORY in their exposed DOS flags; protection
now explicitly retains the target inode's directory type and checks every flag.
NTFS xattrs are encoded little endian. Independent NTFS3 readback checks the result.

Compact evidence is in `.rufus-test-artifacts/cp58`: `native-guest-checks.json`,
`qt-run.json`, grouped transcripts, guest manifests/serial logs and
`checks-packages.json`. Earlier native/boot/layout/helper/GUI/offline-policy/EFS
receipts retain their original source hashes. The retained 28-KiB BCD template,
16-KiB installer BCD and CodeIntegrity manifests are owner-provided read-only
analysis inputs, not redistributable fixtures. Preserve the user's source ISO.
Disposable guest disks and package stages are removed after evidence export.
