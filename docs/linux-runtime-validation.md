# Runtime UEFI media validation

CP61 / Preview 30 groups runtime validation, source checksum maintenance and
actual Windows/SWM change reporting in one creation job. Full Windows parity
remains active; Secure Boot/revocation is a separate remaining work group.

## Windows reference

`src/rufus.c:EnableUefiValidation` admits an EFI-capable ISO copy, excluding
DD-only images, Windows To Go and forced BIOS-only Windows installation. The
eligibility predicate is now shared as `CanUseRuntimeUefiValidation` in
`src/format_policy.c`; the Windows wrapper preserves its original guarded
control access and check-state behavior. Native focused cases cover its
selection boundaries without executing the Windows application. The
checkbox retains a session preference while its visible check is cleared for
ineligible selections. Reading/writing `EnableRuntimeValidation` is commented
out in the checked Windows source; do not introduce a persisted Linux setting.

`src/iso.c` hashes ordinary source data during copying when generating a new
manifest. It records config changes in `modified_files`. `src/hash.c:UpdateMD5Sum`
updates only those entries, installs the bundled `res/md5` validators and
renames original EFI entries to `boot<arch>_original.efi`. The checksum paths
follow the renamed originals, preserving their original source digest. The
validator itself is not added to the manifest. This happens before extended
label/icon creation and the final supplementary ZIP overlay.

The [upstream validator](https://github.com/pbatard/uefi-md5sum) consumes
`md5sum.txt` and optionally its hexadecimal `md5sum_totalbytes` comment, then
chain-loads the preserved EFI entry. This is boot-time corruption detection,
separate from Secure Boot revocation/trust analysis. MD5 does not authenticate
an image's publisher.

## Native implementation

`linux/platform/media_integrity.*` streams a generated list into a private
stage, repairs explicitly modified entries, preserves comments/line endings
and unchanged source digests, and installs the six unchanged bundled EFI
validators. Original loaders have no-replace backups; an existing backup is
an error. IA64/EBC entries have no bundled validator and remain unchanged.
Case-insensitive reserved-path lookup refuses ambiguous aliases, links and
special entries. Atomic list publication, file/directory sync and byte readback
precede success; close removes stages and releases the retained directory.

`ISO_EXTRACT_EVENTS` connects source hashing to the existing transfer loop
without rereading large files. It starts only after extraction preflight,
reports sanitized output paths and records successful config edits. Existing
callers use a null observer. Source hashes are captured before config edits;
rehashing every output would conceal incorrect original checksum entries.

The bounded manifest budget is 64 MiB, 262144 generated entries and 4095 UTF-8
path bytes. Generated lists omit both checksum filenames: including a second
checksum file that Rufus must subsequently edit would leave a stale digest.
An existing `MD5SUMS` is repaired separately while runtime validation receives
its required `md5sum.txt`. This resolves the checked Windows path's inconsistent
choice between the two filenames without discarding the original list.

The focused normal and ASan/UBSan/LSan integration groups pass 203 real cases
each across five suites, using owned temporary directories and generated ISO/UDF
and WIM files, private D-Bus and simulated device metadata.
They group source streaming/config edits, all six resource identities and
loader backups, generated and existing lists, unchanged incorrect source hashes,
Unicode paths, source/backup refusal, cancellation, sync errors and cleanup.
The device/firmware batch below qualifies the integrated x64 path.

The strict optional request flag is frozen before authorization and cleared on
DD selection. Both controller and helper use the original eligibility. Source
preflight rejects conflicting original-loader backups, oversized checksum lists
and empty runtime lists, and reserves space for lists, stages and EFI payloads.
An empty existing runtime list is refused instead of silently enabling a check
that validates no files. All collector handles close before mount cleanup.

Generated SWMs are hashed in their private stage before publication. Their
checksum rows replace the now-missing `install.wim` reference in an existing
list, and are included in a generated list. Actual Windows upgrade, Setup-WIM
and CA2023 boot-file replacements report their paths to the collector; creating
an OEM answer alone does not rehash unchanged source files. Existing synthetic
WIM/customization coverage checks this without Windows installation tests.

As on Windows, the final supplementary ZIP is applied after validation setup.
An archive that overwrites validated content must supply a corresponding list;
it can also replace the validator itself. The ordinary confirmation identifies
the requested runtime option and any ZIP attachment.

## Batch qualification and delivery

One networkless Linux guest per normal/sanitizer build creates GPT FAT16,
FAT32 and NTFS media through the ordinary Qt selection/confirmation path.
Independent kernel mounts verify repaired configs, all manifest entries,
preserved original EFI bytes and byte identity with the bundled validator.
An intentionally wrong source digest remains wrong in the existing MD5SUMS;
the fresh runtime list is independently checked against copied source bytes.
Every successful job checks one authorization, held progress until launcher
exit, full joined READY and released claims. A backup-name conflict is refused
by the real helper with the complete disk digest unchanged.

The final NTFS media boots UEFI:NTFS, the unchanged bundled validator and a bare
native EFI test application. All five entries pass. A disposable copy with one
changed file-data byte reports one failure and waits at the unanswered continue
prompt; the original loader never starts. No Windows program or installation
runs. The source-size progress hint follows Windows and can differ after a
config changes length; the validator's per-file digest result is authoritative.

The batch uses one full normal Qt regression gate after a complete build and
reuses unchanged claim/polkit/helper-fault and earlier customization evidence.
The full gate has two older legacy-GRUB row failures; both pass targeted normal
and sanitizer reruns without code changes. Their cause remains unestablished;
the [checkpoint record](testing-and-checkpoints.md#cp61--runtime-uefi-validation-and-checksum-maintenance-preview-30-2026-10-21)
retains this qualification limit.
Preview 30 packaging includes the validator GPL license and attribution.
Compact commands, hashes, transcripts, package inspection and cleanup records
are in `../.rufus-test-artifacts/cp61/checks.json`. Guest disks and package stages
are removed after evidence export. No host installation or host-device writes
are performed.
