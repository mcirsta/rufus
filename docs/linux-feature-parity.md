# Windows feature parity backlog

## Current release work — CP79 / Preview48 (2026-10-24)

Preview48 packages the qualified fixes following Preview47: preserved standalone
GRUB4DOS selection, older Qt/header compatibility, distro CI dependencies and
deterministic fixtures. Both Ubuntu CI jobs pass 252 suites with one optional
exFAT capability skip, staged integration and the separate ISO classification
fixture. Focused normal/sanitizer repairs and unchanged CP78 desktop/media
evidence are reused. The [evidence index](linux-release-evidence.md) records the
exact nine-file product delta. New package/publication results belong to CP79's
delivery receipt; `linux-preview.47` remains immutable.

B8 prepares the first useful x86-64 release: the original 38-language Rufus
catalog is reused wherever the native text is equivalent, with explicit English
fallback for Linux-specific messages. Full new translations remain a follow-up,
as accepted by the user. See [localization](linux-localization.md).

The [release evidence index](linux-release-evidence.md) reconciles existing
media, boot, geometry and helper results. Unchanged media code reuses those
results. Current-candidate desktop checks and the single Qt regression gate are
recorded with CP78. Debian 13 and Fedora 44 packages pass isolated native builds
and fresh installation, same-payload package-release upgrade and removal checks.
The AUR source recipe builds the tagged GitHub archive; source and binary
packages have distinct packaging instructions.

The user authorized the next GitHub release at `mcirsta/rufus`, tag
`linux-preview.48`. The AUR recipe must use the checksum of that actual downloaded
tag archive; the release owner will submit it manually.
Arch/CachyOS, Debian, Fedora and generic install trees depend on system libraries;
the generic archive is not self-contained.

Full Windows parity remains incomplete. UDF 2.50/2.60, ReFS formatting and Dev
Drive detection are user-deferred. Existing UDF 1.02–2.01 remains supported; the
actual Windows UDF indexing attribute is unavailable. Non-x64 To Go creation,
complete i686/aarch64 application hosts and unavailable physical USB/unplug/4Kn
checks remain explicitly unqualified. No additional installed-Windows or
customization matrix is scheduled.


## Existing explicit Linux decisions

- Keep copying the current user's **Windows regional settings** visible but
  disabled on Linux, with the existing explanation. Do not substitute Linux
  settings. Other Windows customization options remain in scope.
- For FreeDOS, ask for explicit keyboard/code-page choices from Rufus’s tables,
  starting at US/437; do not infer them from the Linux host.
- Use **FAT32** as the Linux label at all supported sizes; the separate Windows
  Large FAT32 formatter name does not apply to the native formatter.
- Keep the Qt GUI unprivileged and authorize a whole creation at most once.
  READY requires all writes, sync, cleanup and helper/worker exit. Do not eject
  the USB automatically.
- Native desktop dialogs/themes and platform services can differ for a concrete
  reason; record and test behavior differences. Implementation convenience is
  not a reason to remove functionality.
- B6 UDF 2.50/2.60 metadata partitions are explicitly deferred from useful-release
  work. Keep UDF 1.02–2.01 and its tested behavior; disclose the gap without
  claiming full Windows parity. Release preparation need not wait for B6.
- ReFS formatting (I15) is also excluded from first-release requirements by the
  user's **"remove just ReFS"** response. The user subsequently deferred Dev Drive detection and accepted original
  translations with Linux-specific English fallback for this release. CP78
  records release qualification; CP77 records the native UDF indexing limitation.

## Historical checkpoint record — through CP71

The following dated entries describe earlier states. Their pending/next-step
statements are historical; the completion register above is current. SFD is restored to active work: CP16o adds whole-device
metadata preparation and native FAT32 file qualification; CP16p qualifies native
FAT32 block-device handoff. CP16q qualifies the complete source-free FAT32
helper job; CP16r enables Qt selection, confirmation and completion. CP16s
qualifies FAT16 native/helper jobs; CP16t enables its Qt selection. CP16u
qualifies native/helper NTFS SFD with a retained exclusive claim and final sync.
CP16v exposes NTFS SFD in Qt. CP16w qualifies the exFAT SFD native/helper route
and CP16x exposes it in Qt. CP17b1 adds the shared read-only mounted-target
candidate checks. CP17b2 qualifies the isolated native mount/source checks and
unmount operation in simulated USB guests. CP17b3 moves helper mount isolation
into its worker after exclusive acquisition and qualifies child inheritance,
cleanup and failure before writes; mounted creation remains disabled.
CP17b4 qualifies exact UDisks/native matching and retained caller-home protection.
CP17b5 connects these checks and native unmounting to the preparation helper,
then waits for UDisks metadata and acquires O_EXCL before writes. CP17b6 enables
eligible mounted ISO/non-bootable/SFD preparation in Qt, with partitioned and
source-backed FAT32/Syslinux helper qualification, readback and boot. CP17b7 adds
mounted DD within one authorization; the later consolidation also routes
unmounted DD through that helper.
CP17b7a adds the qualified native DD operation using the existing claim/unmount
and copy/flush/readback code. CP17b7b connects it to the existing authorized
helper/client with distinct raw progress/results, EOF cancellation and exit
ordering. CP17b7c enables Qt mounted DD routing with a fresh snapshot before
one authorization, unchanged ISO/DD defaults and raw percentages, and completion
after helper exit. CP17c adds Windows's existing-multiple-partitions confirmation
and auxiliary-partition exclusions to all current Qt creation routes.
CP17d1 audits fixed USB eligibility and adds explicit candidate recognition.
CP17d2 carries opt-in through helper requests and fresh native checks, including
empty UDisks mount inventories; simulated fixed-USB formatting and DD jobs are
qualified. CP17d3 enables List USB Hard Drives/Alt+F and carries frozen job
policy through selection and preflight, routing every opted-in DD job through
the existing single helper. The original tooltip, temporary status message and
existing confirmation dialogs are retained. CP17d4 records
the [native fact mapping and original-function reference cases](linux-hdd-classification.md).
CP17d5a extracts the scorer and existing tables for reuse, with a thin Windows
facts/logging wrapper and reference comparisons. CP17d5b adds tested read-only
sysfs snapshots with explicit unknowns and identity rechecks. CP17d5c qualifies
native name/USB-ID mapping and UDisks card exceptions, sharing the existing
500-GiB card limit with Windows. CP17d5d connects that filter to discovery and
fresh GUI/helper checks. Default-visible fixed media retains native source/home/
system/holder protections even without opt-in and with empty UDisks mounts.
Qt and simulated fixed-USB qualification cover that expanded default list.
Native names are not proven identical to Windows friendly names; failed identity
snapshots remain excluded, including missing disk sequence.
CP17e1 accepts direct `fuseblk` targets through the existing native path, with
both NTFS-3G variants qualified in a generated-device VM. CP17e2 qualifies full
helper FAT32 recreation and DD from such mounts, including refusal/cancellation,
one authorization, exit ordering, independent readback and boot. Anonymous FUSE and unresolved layered source/home
backing remain work.
Remaining Windows filesystem/layout features stay planned.

For each feature, trace the Windows implementation, identify reusable rules and
native operations, test generated files first, then qualify device/helper
integration before enabling its Qt control. Include failures, cancellation and
final sync where writes occur. Keep UI controls unavailable until they perform
the advertised operation. Record the exact supported boundary, not just a count
of passing tests. Use a representative functional test where useful; do not
expand into a per-option Windows customization installation matrix. The existing
single-customization qualification remains the agreed scope. Remove completed
large test fixtures while retaining compact evidence.

For related formats that share the same write path, combine qualification in
one batch. Keep parser and source-identity tests specific to each format, but
reuse the existing helper fault suite when the protocol and writer are
unchanged. Use one normal and one sanitizer guest run for the batch: each
format gets real-device write/readback, and a representative full bootable
restore covers the shared writer. Add a separate full restore only for distinct
write semantics, such as VTSI sparse extents. Run focused Qt cases during
development and the full Qt regression suite once at the batch gate. This
reduces repeated guest boot, media setup and broad suite work without relaxing
prewrite, claim, sync, cleanup or READY checks.
Keep one checkpoint and evidence package per completed batch rather than
documenting every internal layer as a separate milestone. Choose batches by
user-visible parity and shared implementation, not by the order of this table.

Production consolidation, CP17d5d, CP17e1/e2 and CP18a/b (2026-09-27) are complete.
CP18a/b qualify source-free GPT/NTFS and GPT/exFAT UEFI:NTFS preparation with
shared embedded-payload handling, existing native formatters, final sync and
completion checks. Ordinary exFAT formatting keeps its original request meaning.
CP18c (2026-09-28) qualifies native MBR layout/preparation and shared payload
writing for NTFS/exFAT on files, retaining Windows's flags, types and placement.
CP18d connects explicit standalone MBR requests to the same helper and native
formatters, with complete NTFS/exFAT jobs qualified on simulated USB. It retains
one authorization, exact payload readback, final sync and exit-before-completion.
CP18e exposes the advanced Qt selector with Windows's eligible combinations,
GPT preference, UEFI target, retained-image behavior and shared progress rules.
ISO-backed MBR stays separate. CP19a inventories the existing FreeDOS payload and
verifies that the original FAT boot-record routines compile and run on Linux
with a test-only stdio adapter. The [FreeDOS reuse audit](linux-freedos-reuse.md)
defines the shared/native split. CP19b qualifies the bounded FAT16/FAT32 adapter
on formatted regular files, including both FAT32 records and failure/cancellation
paths. CP19c1 shares DOS locale policy/content with the existing Windows wrapper
and qualifies native byte generation. CP19c2 shares the payload manifest and
qualifies native copying/hidden-system attributes on kernel FAT16/FAT32. CP19c3a
publishes startup files through the shared writers and native copy I/O, with
explicit DOS inputs. CP19c3b supplies a tested Qt keyboard/code-page chooser,
using shared tables. Per the 2026-09-29 user decision there is no Linux host
detection; CP19e freezes accepted values before authorization. CP19d1 extends
the existing native MBR writer with original Rufus and Windows 7 code for active
FAT16/FAT32 partitions. CP19d2a freezes explicit DOS/MBR choices and validates
the helper phase/discrete-progress contract. CP19d2b assembles and qualifies
complete FAT16/FAT32 device-helper jobs with original Rufus/Windows 7 MBR code,
bundled files and explicitly selected startup content. Simulated USB tests cover
progress, cancellation and cleanup failures. CP19d2c verifies representative
BIOS boots on unchanged helper-created FAT32/Rufus-MBR and FAT16/Windows-7-MBR
USB media, including the selected DOS startup and shell commands. CP19e enables
FreeDOS in the normal Qt selector with MBR/BIOS/FAT settings, explicit US/437-based
locale choice, retained-image transitions, original discrete progress and the
existing sync/cleanup/helper-exit completion gate. Advanced BIOS fixes and
remaining geometries are still pending.
All DD modes now share one helper
route. Writable-target inspection and format planning are consolidated, and
read-only GUI selection no longer links helper device acquisition/mounting.
See [the consolidation record](linux-consolidation.md). HDD filtering changes
default visibility within the existing USB class; other device classes remain work.


CP20a's [standalone Syslinux audit](linux-syslinux-standalone.md) confirms that
the existing native v6 installer/payload can boot empty MBR/FAT16 and FAT32
media without an ISO or generated configuration. It adds no production route.
CP20b adds explicit source-free v6 selection to the existing adapter, separating
ISO eligibility from common geometry/companion checks while sharing its
runner/readback. FAT16/FAT32 normal/sanitized failure matrices and bare BIOS
boots pass. CP20c adds the explicit single-helper route and validates native
virtual-USB creation, failures, two copy events and final-sync/exit ordering.
CP20d exposes Syslinux 6.04 under advanced Qt drive properties, with MBR/BIOS,
FAT defaults, original progress allocation and the same one-helper completion gate.
Windows also supports NTFS for the v6 choice: investigate the upstream native extlinux path rather than treating that
as out of scope. CP20e now builds that separately verified 4.07 provider with a shared CMake
recipe, unchanged upstream sources and exact Rufus payload equality. Regular-file
runner/staging/FAT16/FAT32 bare BIOS qualification passes on x86-64. CP20f adds
explicit v4 selection and exact loader/ADV/VBR readback to the shared adapter;
274 normal/sanitized adapter cases and checked bare boots pass. CP20g connects
v4 to the same single-helper native-device job, with fixed provider dispatch,
original progress and final-sync/exit completion. Forty-four standalone guest
jobs, four bare boots and five source-backed ISO regressions pass. CP20h exposes
the advanced v4 choice before v6, reusing controller/preflight/status/progress
and the qualified helper. Normal and sanitized GUI tests cover both versions,
retained images, failure/cancellation and READY after helper exit. Switching
boot choices resets cluster size to the default, as Windows does. Image-version
compatibility remains unchanged. Non-x86 upstream accessor fixes and native v6
NTFS remain required work; investigate upstream extlinux before adding an installer.

CP20i's [NTFS audit and executable probe](linux-syslinux-ntfs.md) establishes
upstream extlinux as the v6 provider candidate: kernel NTFS/FIEMAP and
NTFS-3G/FIBMAP both install identical payloads, retain mount claims, remount and
boot as bare loaders. CP20j adds the checked NTFS adapter and shares the original
payload verifier with FAT. Its 96-case two-driver matrix passes normally and
under ASan/UBSan, followed by independent readback/remount and bare BIOS boots.
CP20k adds verified Windows file attributes through the common NTFS xattr ABI
and packages unchanged upstream `rufus-extlinux` in the main CMake build.
The extended 118-case matrix per build, remount/independent attribute readback
and four normal/sanitized bare boots pass with the staged provider. CP20l connects the
explicit standalone-v6 request to the existing helper and qualifies creation,
cancellation, attributes, sync/unmount errors, claim release and helper-exit
ordering on both drivers, normally and under sanitizers. Preserve the original Rufus VBR despite the recorded
ntfsprogs-plus jump-check incompatibility; no new installer algorithm is needed.

CP20m exposes standalone v6/MBR/NTFS in Qt using the shared Windows filesystem
and cluster policy. The existing controller checks the selected native provider
and formatter before authorization, preserves the original progress slots and
waits for cleanup/helper exit before successful READY. v4 remains FAT-only;
source-backed NTFS Syslinux and other image versions remain separate work.
Focused normal/sanitized Widgets checks cover all five NTFS cluster choices,
selection changes, missing tools and failure/cancellation. Native formatting,
attributes and BIOS boot qualification remains CP20l's, on 512-byte sectors and
4-KiB clusters; this GUI checkpoint does not broaden that native test claim.

CP20n proves shared ISO extraction followed by native Syslinux installation on
both NTFS drivers, including independent readback and configured BIOS handoff
with one matching v6 fixture. It fixes redundant metadata operations and missing
no-overwrite rename support in the NTFS-3G config path. Source-backed NTFS remains
unavailable in Qt/helper requests until checked-adapter source/payload validation
and lifecycle integration are qualified; see the [NTFS audit](linux-syslinux-ntfs.md#cp20n--iso-extraction-and-native-ntfs-installation).


CP20o adds checked NTFS Syslinux image installation by reusing the FAT source,
version, companion and config policy. Both native drivers pass the image adapter
and standalone regression matrices with normal/sanitized readback and BIOS boots.
Root loader conflicts and mismatched replacements fail; unrelated extracted
content survives. Source-backed helper lifecycle/progress/cleanup and Qt image
eligibility remain the next integration work; other versions and broader native
geometry qualification are still pending. See the
[checked adapter record](linux-syslinux-ntfs.md#cp20o--checked-ntfs-image-adapter).


CP20p connects the checked NTFS image adapter to the existing single authorized
helper, sharing source policy, extraction, progress and final cleanup. Both
native drivers pass normal/sanitized complete-job matrices and configured BIOS
boots; standalone NTFS and FAT image regressions also pass. This qualifies
BIOS-only image jobs at the helper level. Qt image choices remain unchanged.
EFI-capable Syslinux images on NTFS require UEFI:NTFS under Windows's shared rule;
that extra-partition integration and BIOS/UEFI qualification are CP20q, before
Qt enablement. See the [helper record](linux-syslinux-ntfs.md#cp20p--complete-ntfs-image-helper-lifecycle).

CP20q completes the required UEFI:NTFS layout for compatible NTFS Syslinux image
jobs through the existing helper and payload writer. Normal and sanitized
kernel/fallback runs pass 118 jobs and boot four outputs through both BIOS and
x64 UEFI; a 25-job BIOS-only regression retains its original single partition.
The helper rejects missing/unnecessary boot partitions before writing. CP20r
below records its Qt integration. Wider EFI architectures, source versions and
geometries remain qualification work.


CP20r exposes that qualified NTFS image route in Qt. Shared Rufus filesystem and
extra-partition rules retain FAT preference, ISO/DD choice and the appropriate
BIOS-only or dual-firmware layout. Image confirmation, copy percentages and
completion now distinguish this source-backed job from standalone UEFI:NTFS.
All ISO routes reuse the existing formatter availability check before
confirmation and again before authorization. Normal, scaled and sanitizer
Widgets tests exercise selection, immutable requests, missing tools, failures,
cancellation and READY only after helper exit. Native-write/boot qualification
remains CP20p/q; this checkpoint adds no new device writer or extraction engine.

Next bounded feature: CP21a, audit native GRUB2 installation and payload selection
against the Windows path, identifying reusable code/data before implementation.
Start with file-backed or simulated media and keep the GUI choice unavailable
until the adapter, helper and representative boot are qualified. Other Syslinux
versions, EFI architectures, Secure Boot and wider geometries remain backlog
items, not excluded scope.


CP21a completes that GRUB2 audit with an executable reuse proof. Rufus's original
MBR routine and unchanged prepared core boot generated FAT16/FAT32 emulated USB
media and read the expected files, normally and with the host harness sanitized.
The [audit](linux-grub2-reuse.md) recommends a bounded extension of existing
boot-record I/O, not a new host GRUB provider. It records standard PBR parity and
version/prefix/module selection as required integration work. No GRUB GUI route
is enabled yet. **Next: CP21b**, the regular-file adapter and failure tests, then
complete helper qualification and Qt integration in separate steps.


## CP21b update — GRUB2 file adapters (2026-09-29)

GRUB2 MBR/core placement and standard FAT16/FAT32 boot records now use the
existing production writers. Normal and sanitized file-only failure suites
and bare BIOS USB boots pass, including the FAT32 backup and filesystem reads.
FreeDOS shares the generalized FAT staging without changing its payloads.
No GRUB2 Qt choice or complete device job is enabled yet. CP21c is the standalone
authorized helper integration and simulated-device lifecycle; NTFS/ext and
source-backed modules/version handling remain required follow-ups. See the
[reuse audit](linux-grub2-reuse.md#cp21b--bounded-adapters-implemented).


## CP21c update — standalone GRUB2 device helper

The existing single-authorization helper now performs the complete standalone
GRUB2 MBR/FAT16/FAT32 operation using Rufus's embedded core and original standard
FAT boot records. Native formatter handoff, exclusive ownership, cancellation,
flush/readback and helper-exit completion reuse existing components. No mount,
normal.mod/config/OS copying or new installer is needed for this Windows option.
The simulated USB matrix and bare BIOS boot are recorded in the testing log.
CP21d is the advanced Qt selection; NTFS/ext, source-specific modules and broader
firmware/geometry qualification remain outstanding. This does not add a GRUB
ISO route or infer any host settings.


## CP21d update — advanced standalone GRUB2 Qt choice

The advanced Qt Widgets list now offers Rufus's `Grub 2.14` choice. MBR/BIOS
and native FAT16/FAT32 are the qualified subset; NTFS, ext and source-backed
GRUB remain unavailable. The source-free request excludes any previously
selected image and uses the existing one-authorization helper, confirmation,
progress, cancellation and READY-after-helper-exit behavior. Normal and
sanitized Widgets tests cover the UI route and failure cases. CP21b/c already
qualify the underlying simulated-device write and representative bare BIOS boot;
this checkpoint adds no new disk writer.


## CP21e update — GRUB2 NTFS layout only

The existing MBR/core adapters now accept standalone GRUB2 on an active NTFS
partition. Normal and sanitized regular-file tests validate the original MBR
bytes, type/offset/readback, bounded core placement and fault behavior. This is
an intermediate layout qualification, not an NTFS GRUB2 creation route: the
original NTFS partition boot record, helper lifecycle and bare BIOS boot still
need their own checks. The advanced Qt choice remains FAT16/FAT32-only.


## CP21f update — original NTFS PBR adapter

The original `write_ntfs_br` now runs through bounded native memory staging and
file I/O. It preserves the formatter BPB, writes/readbacks the exact 4,136-byte
primary record, and leaves the backup unchanged as Windows does. Native NTFS
geometry and metadata checks precede the write. File-only fault, cancellation,
whole-volume and metadata tests pass normally and under sanitizers; the shared
ms-sys memory callback refactor also passes FAT regressions. NTFS remains absent
from the advanced GRUB2 Qt choice pending the complete helper job and BIOS boot.


## CP21l update — standalone GRUB2 ext2/ext3 Qt selection

CP21k2 qualified complete ext2/ext3 virtual USB helper jobs, native readback and
bare BIOS boots. CP21l adds their advanced Qt choices, Win32's expansion/size
gate, **Default** cluster display, raw ext label handling and formatter preflight.
The qualified standalone GRUB2 set is now FAT16/FAT32/NTFS/ext2/ext3 on MBR.
Ext4, source-backed GRUB selection/configuration/modules and Grub4DOS remain
required work. CP21m first shares the image-specific core selection decision;
it must not silently use the bundled core for an incompatible or unknown image.


## CP21m update — shared GRUB2 core lookup and SBR precedence

The image-specific lookup gate, full/dot/dash candidate order and secondary
boot-record precedence now live in existing shared files and are consumed by
Windows. Normal, sanitizer and generated-ISO tests pass. This reduces future
Linux duplication without enabling source-backed GRUB prematurely. Native
cache reads, signed downloads, frozen selected-payload handling and compatible
image/module/configuration boot remain required. The exact cache/prompt/fallback
contract is recorded in `linux-grub2-reuse.md`; CP21n starts with the bounded
unprivileged cache reader.

## CP21n update — bounded full-name GRUB2 cache reader

The unprivileged source library now reads the full-version cache into an owned,
bounded byte snapshot with a SHA-256 fingerprint. It reuses shared lookup and
native identity checking, preserving the missing/unreadable distinction and
stopping on cancellation/input change. Its 48 tests and nearby regressions pass
normally and under sanitizers; GUI/helper builds pass. It is not wired into
image creation yet. Signed-download verification/selection, frozen helper
payload transfer and compatible image/module/configuration boot remain required.
CP21o begins with file-only signature verification reuse, without another host
installer, detection mechanism or disk writer.

## CP21o update — shared signing key and native verifier

Windows and Linux now consume one unchanged Rufus download key. A small C
adapter uses OpenSSL Crypto for the original detached RSA/SHA-256 format; it
accepts only verified bytes and has no trust override or alternate-algorithm
retry. An authentic server fixture and 36 offline scenarios pass normally and
under sanitizers; cache/inspection regressions and Linux GUI/helper builds pass.
The detailed audit is in `linux-download-signatures.md`. Native fetching,
warning/cache selection, frozen helper payload transfer and compatible image
boot remain required; CP21p begins with bounded fetching and local-server tests.


## CP21p update — bounded signed bootloader transport

One asynchronous Qt Network adapter now returns bounded, owned bytes only after
verification with the shared Rufus key. It retains the original payload/`.sig`
sequence and HTTP/length/timeouts, with a documented security difference:
HTTPS-to-HTTP redirects fail. Its 63 local TCP/TLS cases and signature/cache/
inspection regressions pass normally and under sanitizers; GUI/helper builds
pass. Image-backed GRUB remains unavailable until atomic cache installation,
Win32 warning/full-name/prefix selection, frozen helper transfer and a compatible
image/core/module/configuration boot are complete. CP21q starts with cache
installation; no new host detection or authorization mechanism is planned.


## CP21q update — atomic full-name cache writes

The existing cache component now atomically stores accepted downloaded cores
under the full detected version, sharing reader validation and preserving old
contents on failure/cancellation before commit. Its 45-case storage suite and
four adjacent CTests pass normally and under sanitizers; GUI/helper builds pass.
This storage API assumes an already verified download and makes no compatibility
claim. CP21r connects cache-first selection, Win32's warning and shared candidate
order. Frozen helper payload transfer and a compatible image boot still block
image-backed GRUB creation.


## CP21r update — connected GRUB core selection

The tested unprivileged selection component now connects full-name cache lookup,
Yes/No/Cancel decisions, shared candidate order, signed fetching and atomic
installation. It preserves unsigned existing caches and original fallback,
keeps detected/server/payload identities separate, and stops changed ISO or
substituted installed inputs. Its 53 cases and six adjacent CTests pass normally
and under sanitizers; GUI/helper builds pass. This component is not yet wired to
the main-window warning or authorized job. CP21s freezes selected payloads into
existing helper preflight; complete compatible-image boot still blocks enabling
image-backed GRUB creation.


## CP21s update — frozen GRUB core handoff

Optional selected bytes now pass through the existing private staging/request/
helper route, with ownership, bounds, size/hash and identity checks before target
access. The original writer remains shared. Eight focused suites pass normally
and under sanitizers; GUI/helper builds pass. Actual main-window warning/START
wiring, source-backed GRUB extraction/job integration and compatible-image boot
still block enabling image-backed creation. CP21t starts with GUI wiring.


CP21t is complete: GRUB download warning/START integration and all 18 new GUI
scenarios pass normally and under sanitizers. The full Widgets suite also passes
(647 cases, two optional real-ISO skips). Pure image-backed GRUB creation remains
disabled; CP21u extends the request/helper route before compatible-image boot
qualification. See `linux-port-plan.md`.


CP21u adds GRUB ISO request/read-only preflight using the existing source and
boot-policy code. Nine focused suites pass normally and under sanitizers. The
launcher/helper write route stays gated; image extraction/helper integration
and compatible-image boot qualification remain before GUI enablement. NTFS
images needing UEFI:NTFS and ext ISO jobs are still pending parity work.


CP21v joins GRUB ISO jobs to the existing FAT16/FAT32/NTFS helper extraction and
boot-record paths; it adds no second production writer or job layer. Normal and
sanitized helper/launcher regressions pass. GUI creation remains disabled until
CP21w proves one compatible image/core/module/configuration boot. Test details
and the simulated-mount qualification limits are in
[the CP21v record](testing-and-checkpoints.md#cp21v--grub-iso-helper-integration).


CP21w qualifies one compatible GRUB 2.12 BIOS/MBR/FAT32 ISO through production
helper creation, independent complete readback and Linux userspace boot, normally
and under sanitizers. It also fixes suppressed GRUB ISO copy progress. GUI
creation remains disabled until CP21x connects and qualifies START/READY; other
filesystem/firmware combinations remain pending. See [the test record](testing-and-checkpoints.md#cp21w--compatible-grub-iso-native-creation-and-boot).


CP21x enables the existing GRUB2 BIOS/MBR/FAT32 ISO route in Qt Widgets. It
preserves shared Syslinux precedence, core warning/cache/download/fallback choices,
ISO-recommended/optional-DD selection and unknown image versions. Retained core
bytes reach the existing helper handoff; copy progress uses Rufus's ISO weighting,
and READY waits for sync/cleanup and helper exit. Other GRUB ISO filesystems and
new firmware/module combinations remain pending. See the
[test record](testing-and-checkpoints.md#cp21x--grub-iso-qt-start-progress-and-ready).


CP21y qualifies the same compatible GRUB 2.12 ISO/core/module fixture on native
BIOS/MBR/FAT16 media without changing production code. Normal and sanitized
helper creation, all 308 file hashes/original boot-record readback and Linux
userspace USB boots pass; the FAT32 regression passes too. GUI ISO creation
still offers FAT32 until CP21z enables/test-checks the FAT16 selection. Other
GRUB ISO filesystem/firmware combinations remain pending. See the
[test record](testing-and-checkpoints.md#cp21y--native-fat16-grub-iso-creation-and-boot).


## CP21z — FAT16 GRUB ISO Qt selection (2026-10-01)

The existing GRUB ISO GUI route now offers FAT16 and FAT32 where both Rufus's
shared filesystem policy and native MBR geometry allow them. The eligibility
check also accepts small FAT16-only targets. This restores the original first
supported filesystem default for small BIOS targets; FAT16 is absent at the
4-GiB boundary. Image-specific preferences still apply. When a GRUB image has
no filesystem preference, rescanning it preserves a valid manual format choice,
as Win32 does. An explicit FAT32 choice therefore remains FAT32.

Both formats reuse the existing controller/helper lifecycle, selected-core
snapshot, shared copy progress and READY-after-sync/cleanup/helper-exit behavior.
No formatter, installer, compatibility layer or production dependency is added.
CP21w/y supply native creation, complete readback and BIOS boot evidence; this
checkpoint checks the Qt workflow using private D-Bus and file-backed fixtures.
NTFS/ext GRUB ISO choices remain pending qualification/enablement; arbitrary
core/module ABIs, physical media, real polkit and new UEFI combinations are not
certified by these fixtures.


## CP21aa — native GRUB ISO on NTFS (2026-10-01)

The existing helper now has native compatible-fixture qualification for plain
BIOS/MBR/NTFS GRUB ISO creation, with the preferred new kernel NTFS driver and
NTFS-3G fallback, normally and under ASan/UBSan/LSan. Production code is unchanged.
All 308 ordinary files, original MBR/core/PBR, NTFS geometry/clean metadata and
expected label patch pass independent readback. GRUB loads copied configuration
and boots copied Linux kernel/initramfs into userspace reading the USB marker.
One simulated authorization, sync/cleanup/helper-exit and resource audits pass.
The existing FAT32 recipe also passes after fixture extension.

NTFS GUI enablement follows in CP21ab. EFI/UEFI:NTFS and ext GRUB ISO combinations
remain pending; this does not certify arbitrary distribution payload ABIs,
physical media or actual polkit. The existing formatter/mount/extraction/boot
record paths are reused without a new production provider or abstraction. See
[the test record](../docs/testing-and-checkpoints.md#cp21aa--native-grub-iso-on-biosmbrntfs)
for evidence and [the VM recipe](../linux/tests/vm/README.md#grub-iso-on-ntfs-cp21aa)
for reproduction. Temporary builds/media/package trees are removed after export.


## CP21ab — BIOS/MBR/NTFS GRUB ISO Qt choice (2026-10-01)

The qualified plain BIOS GRUB ISO route now offers NTFS in Qt alongside native
FAT16/FAT32. It uses the existing controller/helper, Rufus filesystem policy,
NTFS formatting/mount providers and selected-core snapshot. When image policy
excludes FAT, NTFS becomes the default; eligible manual filesystem and cluster
choices pass unchanged into confirmation and the frozen helper request.

NTFS copy percentages use the original BT_IMAGE allocation: at 50.0% file copy
the tested GUI bar is 37027/65535, compared with 35061 for FAT. Successful READY
requires complete copy, the confirmed partition extent, complete NTFS format
bytes, sync/cleanup and helper exit. No new installer, writer, job abstraction
or production dependency is added. CP21aa retains the native creation/readback/
BIOS-boot evidence with the preferred kernel driver and NTFS-3G fallback.

GRUB ISO NTFS remains unavailable on MBR when the image is EFI-capable, since
Rufus requires its UEFI:NTFS combination there. Existing MBR/FAT and GPT/UEFI
choices remain available under their usual policy. Direct callers also reject
the unqualified combination before authorization. EFI/UEFI:NTFS and ext GRUB
ISO integration remain pending parity work; unknown versions stay unknown.


## CP21ac — GRUB/NTFS with UEFI:NTFS backend (2026-10-01)

The existing request/helper/MBR/raw-payload paths now support source-backed
GRUB/NTFS with Rufus's required UEFI:NTFS partition. The ISO remains retained,
shared partition policy and x64 fallback-loader validation run before writes,
and both bootloaders use one progress stage in the same authorized helper.
Regular-file integration checks compare the retained core, full bundled payload,
partition geometry and copied configuration, and cover payload failure,
cancellation and helper-exit ordering. No new production provider or abstraction
is added. Native combined creation/readback and BIOS/x64 UEFI boot qualification
remain CP21ad; the Qt EFI-capable GRUB/NTFS option remains gated until that passes.
See [the port plan](linux-port-plan.md#cp21ac--grub-iso-ntfs-with-uefintfs-backend-integration).


## CP21ad — native GRUB/NTFS + UEFI:NTFS qualification (2026-10-01)

The pinned compatible GRUB ISO passes production combined MBR/NTFS creation,
complete independent readback, BIOS boot into Linux userspace and x64
UEFI:NTFS handoff to the copied EFI test application. Both the preferred kernel
NTFS driver and absent-kernel NTFS-3G fallback pass normally and under
ASan/UBSan/LSan, with one simulated authorization and complete helper cleanup/
exit. All 309 files, original boot records and bundled payload match readback;
snapshot boot leaves backing media unchanged. The ordinary FAT32 BIOS recipe
also passes. Production hashes match CP21ac; no new production code is added.

Qt enablement follows in CP21ae; the EFI-capable GRUB/NTFS gate remains for now.
This fixture does not qualify arbitrary distribution modules/EFI loaders,
Secure Boot, other EFI architectures, physical media or real polkit. Compact
evidence is retained and disposable builds/media/package trees are removed.
See [the test record](testing-and-checkpoints.md#cp21ad--native-grub-ntfs-with-bios-and-uefintfs-handoffs).


## CP21ae — EFI-capable GRUB ISO MBR/NTFS Qt choice (2026-10-01)

The qualified GRUB ISO route now offers MBR/NTFS for EFI-capable images and
adds the UEFI:NTFS partition according to Rufus's existing shared policy.
Native cluster selection accounts for the actual reservation; plain BIOS
NTFS retains a single partition. Large-file defaults, manual choice/rescan,
GPT/MBR transitions, hybrid ISO/DD, original NTFS progress and READY after
helper exit are checked through the existing Widgets/helper fixtures.
CP21ad retains the native combined creation/readback/BIOS/EFI-handoff evidence.
No new production provider, job abstraction or dependency is added.

Earlier ext GRUB ISO backlog references were incorrect: the original Windows
`BT_IMAGE` filesystem selector offers FAT16/FAT32/NTFS, while ext choices belong
to standalone `BT_GRUB2`. An extra ext ISO route is not needed for parity.
Other image payloads/architectures, persistence, Windows BIOS/dual boot and
remaining standalone/filesystem features stay in scope. See
[the current support guide](linux-support.md) and
[the next checkpoint](linux-port-plan.md#cp21ae--efi-capable-grub-iso-mbrntfs-in-qt).


## CP22a — Windows BIOS/dual-boot reuse audit (2026-10-01)

The [source audit](linux-windows-bios-reuse.md) confirms that the native NTFS PBR
adapter already calls the original Windows routine and the MBR writer already
contains original Rufus/Windows 7 payloads. Shared filesystem/partition policy,
native NTFS providers, extraction/customization, progress and one-helper cleanup
are retained. Plain BIOS and EFI-capable dual mode need distinct frozen policy;
Alt+A/Alt+E and Windows target/default behavior are mapped for later Qt enablement.
No additional GUI route is enabled by this audit.

The next checkpoint extends the existing NTFS MBR layout gate. Explicit Windows
request/preflight, helper integration/native handoff, and Qt enablement follow
in small stages. FAT dual boot, legacy WinPE loader/config fixes and masquerading,
Windows 7 EFI fallback extraction, other architectures and Windows To Go remain
separate parity work. Component reuse checks pass normally and with sanitizers;
no Windows customization/installation qualification is repeated.


## CP22b — Original Windows MBRs on NTFS (2026-10-01)

The existing MBR planner/writer now accepts original Rufus and Windows 7 boot
code on NTFS, with or without trailing UEFI:NTFS. The boot payloads, shared
layout arithmetic and existing clearing/write/readback paths are reused.
Generated-file success/refusal/fault checks pass normally and under sanitizers.
This is component support; Windows BIOS/dual request, preflight, helper creation
and Qt enablement remain the following checkpoints. FAT dual mode and legacy
Windows routes remain separate parity work. See
[the plan](linux-port-plan.md#cp22b--original-windows-mbrs-on-ntfs).


## CP22c — Windows BIOS/dual request and preflight (2026-10-02)

Explicit Windows MBR/NTFS BIOS and dual operations now retain source identity,
original Rufus/Windows 7 bootstrap selection and actual enhanced-mode reservation.
Existing preflight shares Rufus filesystem/partition policy and Windows options
with GPT; known Windows 10/11 metadata and BOOTMGR are required, and only dual
mode checks the EFI fallback. Request, refusal and generated ISO/UDF checks pass
alongside existing GPT/Linux regressions, normally and under sanitizers.

The helper still refuses the new operation before target access and Qt does not
offer this creation route. CP22d integrates existing NTFS/PBR/payload/copy/cleanup
and qualifies native handoff; CP22e enables the Qt choices. FAT dual, legacy and
remaining Windows features stay separate parity work. No additional Windows
customization or installation qualification is added. See
[the plan](linux-port-plan.md#cp22c--windows-biosdual-request-and-preflight).


## CP22d1 — Windows BIOS/dual NTFS helper integration (2026-10-02)

The existing NTFS helper now handles explicit Windows BIOS/dual requests using
original MBR/PBR, optional original UEFI:NTFS and the shared copy/customization/
finalization flow. Shared Windows preflight runs before writes even for plain
BIOS without customization. The client recognizes its bootloader phase, requires
copied bytes and waits for helper exit before success.

Generated-file/fault/process checks pass normally and under sanitizers, reusing
the existing GRUB file-backed fixture. Native combined kernel/FUSE creation,
readback and BIOS/EFI handoff remain CP22d2; Qt dispatch/defaults/Alt+A/Alt+E stay
disabled until CP22e. Original Windows sources/payloads and native writers are
unchanged. FAT dual, legacy Windows and remaining parity work remain in scope;
no new real customization/installation qualification is added. See
[the plan](linux-port-plan.md#cp22d1--windows-biosdual-ntfs-helper-integration).


## CP22d2a — Native Windows BIOS/dual NTFS creation (2026-10-02)

The combined production helper now passes native simulated-USB creation and
independent readback with preferred kernel NTFS and absent-kernel NTFS-3G,
both original Rufus/Windows 7 MBR choices and plain/dual layouts. Four generated
Windows ISO/UDF combinations pass normally and under sanitizers. Original MBR,
NTFS PBR and optional UEFI:NTFS, exact copied files, clean NTFS metadata, one
simulated authorization, complete sync/private-mount cleanup and helper exit
are checked. No production code or dependency was added in this checkpoint.

CP22d2b still needs BIOS/EFI firmware handoff, including the original MBR prompt
with another bootable disk. Opaque BOOTMGR/generated WIM metadata do not prove
Windows Setup boot. Qt Windows BIOS/dual remains disabled until CP22e. FAT dual,
legacy and remaining Windows features remain separate parity work; no further
Windows customization/installation matrix is added. See
[the plan](linux-port-plan.md#cp22d2a--native-windows-biosdual-ntfs-creation).


## CP22d2b — Windows BIOS/dual firmware handoff (2026-10-02)

The qualified native Windows NTFS route now also passes generated BIOS handoff
through both original MBRs and the original NTFS PBR. Rufus's second-disk prompt
is observed: keypress chooses USB, timeout chooses the other disk. Dual media
launch the existing x64 EFI fixture through the unchanged UEFI:NTFS payload.
Four combinations pass normally and with sanitizers, with unchanged snapshot
backing media. Only test code and a VM-test-only CMake target were added.

Qt Windows BIOS/dual still awaits CP22e defaults/dispatch/Alt+A/Alt+E enablement.
Generated handoff is not Windows Setup qualification; physical/Secure Boot/
other-architecture behavior remains unqualified. No additional customization
matrix was run. FAT dual, legacy Windows and remaining feature parity stay in
scope. See [the plan](linux-port-plan.md#cp22d2b--windows-biosdual-firmware-handoff).


## CP22e — Windows BIOS/dual NTFS in Qt (2026-10-02)

The qualified modern Windows NTFS helper is now selectable through the existing
Qt UI. EFI-capable Windows images retain GPT/UEFI defaults; MBR names plain BIOS,
Alt+E defaults to MBR and names dual BIOS or UEFI when applicable, and Alt+A
persists the original Rufus/Windows 7 MBR choice. Shared Windows BIOS eligibility,
filesystem/extra-partition rules, actual native geometry and the existing single
helper are reused. Confirmation and status name the selected path; sync/cleanup
and helper exit still precede full READY. Seven suites pass normally and under
sanitizers, including read-only selection of the user's Windows 11 ISO.

FAT BIOS/dual remains the next separate adapter/helper qualification; GPT FAT32
WIM splitting remains selectable by explicitly choosing GPT after Alt+E. Older/
unknown Windows, mixed loaders, Windows To Go, standalone WIM/ESD and other
remaining features stay in scope. No additional customization installation test
was added. See [the plan](linux-port-plan.md#cp22e--windows-biosdual-ntfs-in-qt).


## CP22f — Original Windows FAT records (2026-10-02)

The existing FAT adapter now reuses the standard FAT16 and FAT32 BOOTMGR PE
writers unchanged. Both FAT32 prefixes and their separate sector-12/18 tails
match original payload bytes; BPB/serial, filesystem data and reserved gaps are
preserved. Both copies are staged before writing, bounded by reserved-sector
geometry, flushed and read back. Six file-only suites pass normally and under
ASan/UBSan/LSan. This is component qualification: Windows FAT BIOS/dual stays
gated pending request/helper integration and native/firmware checks. GPT FAT32
and qualified Windows NTFS behavior remain unchanged. See
[the plan](linux-port-plan.md#cp22f--original-windows-fat-boot-records).


## CP22g — Windows FAT32 request/preflight (2026-10-02)

Existing Windows BIOS protocol/preflight and cluster selection now accept a
frozen FAT32 request, with original Windows MBR choices and no UEFI:NTFS tail.
Original shared filesystem policy requires Alt+E; enhanced EFI-capable FAT sources
use a checked fallback EFI file on the main volume. GPT/BIOS share the existing
single-large-install.wim rule and extraction space/collision checks. Existing
FAT32 MBR writer/planner are reused unchanged. Helper execution and Qt FAT choices
remain gated pending record installation and native boot qualification. See
[the plan](linux-port-plan.md#cp22g--windows-fat32-layout-and-requestpreflight).


## CP22h — Windows FAT32 helper integration (2026-10-02)

The existing FAT helper now installs original Windows PE primary/backup records
before mounting/copying, reusing the FAT formatter/adapter, MBR/source checks,
split-WIM extraction and final cleanup. One production file changes by one net
line. Generated-file/fault tests cover both MBR choices, BIOS-only/dual sources,
record failures/cancellation, MBR/source mutation, sync and cleanup; a real small
WIM is split and reopened. The mount fixture uses a separate directory, so native
filesystem and firmware qualification remains CP22i. Qt still masks Windows
BIOS/dual to NTFS; FAT GUI enablement follows as CP22j. No further Windows
installation/customization matrix is scheduled. See
[the plan](linux-port-plan.md#cp22h--windows-fat32-helper-integration).


## CP22i — Native Windows FAT32 and firmware handoff (2026-10-02)

Both original Windows MBR choices now have native FAT32 BIOS-only/dual creation
and generated firmware handoff qualification using the unchanged production
helper. Independent FAT allocation/file/record readback, original MBR prompt
choices, direct FAT x64 EFI boot, single authorization and final sync/unmount/
exit checks pass. Only the VM test recipe changes. Native allocation updates
live FSInfo hints; the original boot-sector NO NAME label remains intentional.
Windows Setup/physical firmware/Secure Boot and larger geometry remain unqualified.
The Qt filesystem mask remains until CP22j; no new customization matrix is added.
See [the plan](linux-port-plan.md#cp22i--native-windows-fat32-creation-and-firmware-handoff).


CP22j exposes qualified modern Windows MBR/FAT32 BIOS/dual creation in Qt,
using shared Alt+E policy and the existing helper/split-WIM path. Both MBR choices
and GUI completion/failure behavior pass normal and sanitized tests. This closes
the modern Windows BIOS/dual FAT32 GUI gate; it does not qualify legacy Windows,
BIOS FAT16, physical Windows Setup or Secure Boot. CP22k audits legacy reuse
before selecting the next bounded implementation step.


CP22k's [legacy reuse audit](linux-legacy-windows-reuse.md) changes no production
code or feature availability. Known Windows 7/8/8.1 plain BIOS/NTFS needs bounded
eligibility/helper qualification through existing MBR/PBR/formatter/copy code.
Windows 7-style EFI needs the original first-install-image bootmgfw.efi operation;
XP-era WinPE needs shared loader/SIF transformations and original 0x81 disk
masquerading. FAT16's standard record and FAT32's NT/PE records need qualification
against the actual loader they search for. CP22l starts the older BOOTMGR NTFS
route, retaining separate GUI/EFI/FAT/legacy gates and unknown-version refusal.


CP22l1 admits known Windows 7/8/8.1 plain BIOS/NTFS through the existing backend
and generated-file helper; normal/sanitized tests pass. Qt remains gated pending
CP22l2 representative native NTFS readback/generated BIOS handoff, then CP22l3
plain-choice enablement. Older EFI/FAT and legacy WinPE remain work. See
[the plan](linux-port-plan.md#cp22l1--older-bootmgr-plain-biosntfs-preflight-and-helper-2026-10-02).

CP22l2 qualifies representative older-source native NTFS creation/readback and
generated BIOS handoff with both original MBRs, kernel/fallback mounting and
normal/sanitized builds; production code is unchanged. Qt enablement remains
CP22l3. Actual Windows Setup/BCD and older EFI/FAT/WinPE remain unqualified. See
[the plan](linux-port-plan.md#cp22l2--older-source-native-ntfs-creation-and-bios-handoff-2026-10-03).

CP22l3 exposes known Windows 7/8/8.1 plain BIOS/MBR/NTFS in Qt through the existing
helper. Both original MBR choices and shared customization refusal are retained.
Older EFI/FAT remains a temporary capability limit, with saved Alt+E ignored for
this request and preserved for other images. Next is source-content-driven
Windows 7-style EFI payload preflight, then installation/handoff before Qt
enablement. See
[the plan](linux-port-plan.md#cp22l3--older-bootmgr-plain-biosntfs-in-qt-2026-10-03).


CP22m1 implements the exact Windows 7-style EFI repair payload lookup through
retained ISO/UDF and upstream wimlib, sharing existing x64 application validation.
This is read-only component qualification; fallback installation, native EFI
handoff and older EFI/FAT Qt enablement remain work. No existing capability gate
or unknown-version classification changes. See
[the plan](linux-port-plan.md#cp22m1--windows-7-style-efi-payload-preflight-2026-10-03).


CP22m2 connects checked Windows EFI repair bytes to fresh helper preflight and
post-copy fallback installation, sharing the existing CA 2023 atomic writer.
Plain BIOS behavior, known-version and older EFI/FAT Qt gates remain unchanged.
Component/copy/error/cancel and request/process regressions pass normally and
sanitized; combined native USB creation/readback/EFI handoff remains CP22m3.
See [the plan](linux-port-plan.md#cp22m2--efi-repair-payload-installation-in-the-helper-2026-10-03).

CP22m3 qualifies complete native EFI repair creation, exact file readback and
generated x64 OVMF execution on FAT32/ISO and kernel NTFS/UDF, normally and
sanitized. All production code is unchanged. The checked WIM application supplies
the added fallback; root bootmgr.efi and all copied source files remain intact.
Known modern fixture metadata exercises the content trigger. Older EFI/FAT
admission and Qt choices remain work, as do actual Windows Setup/BCD and Secure
Boot; existing sole customization evidence is reused. See
[the plan](linux-port-plan.md#cp22m3--native-efi-repair-creationreadbackhandoff-2026-10-03).

CP22m4 admits known Windows 7/8/8.1 FAT32/NTFS enhanced/UEFI backend preflight,
reusing shared Rufus policy and the checked regular/repaired x64 fallback.
Unknown versions, legacy/mixed older loaders and older FAT16 remain refused.
Qt explicitly retains its current plain BIOS/NTFS limit until older native EFI
qualification and intentional target/filesystem/Alt+E exposure. All final
component and focused GUI regressions pass normally and sanitized. See
[the plan](linux-port-plan.md#cp22m4--bounded-older-efifat-eligibility-2026-10-03).

CP22m5 qualifies generated older native EFI repair creation/readback/handoff on
FAT32/ISO and kernel NTFS/UDF, normal and sanitized. Known Windows 7 metadata
uses the exact first-install-image payload, preserving root bootmgr.efi and all
other source files. Production code is unchanged. Intentional older EFI/FAT32
Qt choices remain next; actual Windows Setup/BCD/Secure Boot, older FAT16 and
legacy WinPE remain work. See
[the plan](linux-port-plan.md#cp22m5--older-native-efi-creationreadbackhandoff-2026-10-03).


CP22m6 exposes qualified known Windows 7/8/8.1 EFI/FAT32 choices in Qt through
the existing writer. The temporary version-only GUI guard/method are removed;
original `IS_EFI_BOOTABLE` governs root-only repair layouts as well as regular
fallback files, including modern sources with that layout. GPT defaults, Alt+E
MBR/dual choices, Alt+A, filesystem filtering, confirmation/request and READY
reuse existing policy/lifecycle. No new helper, provider, privileged action or
customization route is added. Known older FAT16 remains hidden for both targets;
unknown/legacy/mixed-source refusal remains in fresh backend preflight. Actual
older Windows Setup/BCD/Secure Boot and remaining legacy WinPE are still work.
See [the plan](linux-port-plan.md#cp22m6--older-efifat32-choices-in-qt-2026-10-03).


CP22n1 extracts WinPE layout/copy decisions and SETUPLDR transformations into
shared C called by Windows, with bounded short/truncated/logging behavior and
well-formed byte agreement against the original loops. Section insertion reuses
a shared prefix matcher while keeping native text/file operations. Component
checks pass normally/sanitized, existing config/ISO regressions pass, and Linux
GUI/helper builds succeed. WinPE native files/0x81 records/handoff and Qt exposure
remain work; no creation capability or additional customization qualification is
claimed. See [the plan](linux-port-plan.md#cp22n1--shared-legacy-winpe-transformations-2026-10-03).

CP22n2 adds a tested native WinPE root-file preparation component using those
shared primitives. Sources and existing destinations are preserved, text
encoding/newlines are explicit, mandatory-file/verification failures stop, and
publication is verified/flushed per file. Normal/sanitized component and existing
regressions pass; GUI/helper builds succeed. This is not yet a WinPE creation
route. Original 0x81 MBR/record policy, native source/preflight/helper integration,
generated BIOS handoff and Qt exposure remain, as do localized no-BOM SIF code
pages and actual Windows CRT validation. See
[the plan](linux-port-plan.md#cp22n2--native-winpe-root-file-preparation-component-2026-10-03).

CP22n3 implements legacy WinPE record components through existing providers:
shared no-/minint masquerading policy, original Rufus MBR with raw flag 0x81,
and original FAT32 NT record with its sector-12 tail/backup handling. Exact raw
readback handles libfdisk's 0x80-only bootable property without skipping layout
checks. All 18 focused suites are qualified normally/sanitized after one fixture
expectation correction; GUI/helper builds pass. Seven inventoried production
files change, with no new production files or I/O engine. This still does not
enable legacy WinPE creation. Generated BIOS handoff must resolve the audited
NTLDR/BOOTMGR filename discrepancy before helper/Qt integration, alongside the
tracked SIF encoding, older FAT16 and real Windows validation work. See
[the plan](linux-port-plan.md#cp22n3--legacy-winpe-boot-record-component-2026-10-03).

CP22n4 adds generated USB BIOS execution: six normal/sanitized cases distinguish
original NT FAT32 disk/NTLDR failures from successful fixture-corrected 0x81
and /minint 0x80 handoffs. Source patches and >64-KiB loading are verified.
All production files remain unchanged. Legacy WinPE creation stays gated.
Next qualify the existing NTFS BOOTMGR record with 0x81 and the legacy source
contract, then full helper creation/readback/handoff before Qt. FAT32 BPB-drive
handling, existing NTLDR validation/conditional installation and FSInfo
mount/remount lifecycle remain required work, alongside FAT16/localized SIF.
The generated fixture's alias/metadata corrections are not production fixes
or Windows Setup validation. See
[the plan](linux-port-plan.md#cp22n4--generated-legacy-bios-record-handoff-2026-10-03).

CP22n5 qualifies the unchanged original NTFS record with generated prepared
i386/amd64 loaders (DL=81) and /minint (DL=80), plus NTLDR-only and no-loader
controls. All five pass normally/sanitized after test-only parent-cache cleanup
was corrected. The NTFS payload does include UTF-16 NTLDR, correcting the
earlier incomplete ASCII-only audit. No production file or route changes.
Prefer this existing NTFS chain for the first WinPE helper lane. Source contract
and next preflight integration are recorded in
[the plan](linux-port-plan.md#cp22n5--original-ntfswinpe-bios-handoff-2026-10-03).
Full helper creation/readback/handoff must precede Qt; actual Windows runtime,
mixed/existing root loader handling and FAT16/FAT32/localized SIF remain.

CP22n6 implements the legacy source contract through a separate read-only
entry point in existing ISO preflight. Shared policy/setup/transformations,
retained ISO/UDF readers, extraction checks and partition-fit handling are
reused. Mandatory unique bounded files, known options, supported SIF insertion,
root collisions, source identity and plain BIOS/MBR/NTFS settings are checked.
Unknown version remains unknown; no modern installer metadata is invented.
Normal/sanitized checks pass (490 preflight, 74 root cases, 66,020 shared checks)
and GUI/helper build. Existing helper/Qt legacy creation remains gated. Next:
connect/qualify full single-helper NTFS creation/readback/BIOS handoff before Qt.
FAT16/FAT32 lifecycle, localized SIF, mixed/existing root loaders and actual
Windows runtime remain, as does Windows To Go. See
[the plan](linux-port-plan.md#cp22n6--retained-legacy-winpe-source-preflight-2026-10-03).

CP22n7 connects and qualifies plain legacy WinPE BIOS/MBR/NTFS helper creation.
It reuses source preflight, NTFS records, extraction, shared setup transforms,
native root preparation and one-helper finalization. Generated i386/AMD64/minint
jobs pass full file/record/clean-metadata readback and BIOS drive/tail handoff,
including NTFS-3G fallback and sanitizer coverage. Injected root-write failure
leaves FAILED with clean teardown; existing modern NTFS regression passes.
Kernel DOS parsing requires 0x80 while creating: final original Rufus/0x81 is
written/read back/fsynced after unmount using the existing writer. NTFS-3G's
unsupported no-replace rename uses atomic hard-link publication rather than
overwriting rename. Original Windows/shared source and request fields remain
unchanged. Qt exposure is next, with FAT16/FAT32 lifecycle, localized SIF,
mixed/existing root loaders, Windows To Go and actual Windows Setup still work.
See [the plan](linux-port-plan.md#cp22n7--complete-legacy-winpe-ntfs-helper-job-2026-10-03).

CP22n8 exposes the qualified legacy BIOS/MBR/NTFS path in Qt for supported
i386/AMD64/minint layouts. It reuses the shared setup/report checks, filesystem
defaults, fresh preflight, existing Windows BIOS request and ISO/DD dialog.
Legacy version stays uninferred; modern options are hidden. Normal/sanitized
Widgets cases and complete generated GUI/helper/readback/BIOS jobs pass.
Completion waits for the held launcher, sync/unmount and released access before
full READY; unplug preserves success. The small guest models caller UID and
authorization, so actual Windows Setup/host polkit are still unqualified.
FAT16/FAT32 remain gated pending their legacy records/root-loader handoff audit;
localized SIF, mixed/existing loaders and Windows To Go remain in the backlog.
See [the plan](linux-port-plan.md#cp22n8--qualified-legacy-winpe-qt-creation-2026-10-03).

CP22n9 audits the remaining legacy FAT records without changing production or
opening filesystem choices. FAT16's DOS provider cannot load prepared BOOTMGR.
For the complete BOOTMGR-only legacy contract, existing FAT32 PE code is the
reuse candidate, requiring the shared 0x80/0x81 mapping and valid FSInfo hints;
unchanged PE arrays otherwise overwrite the free-cluster summary. Normal and
sanitized generated BIOS/metadata checks prove those requirements. Next is the
bounded FAT32 record adaptation, then complete helper lifecycle qualification
before Qt/advanced FAT selection. NTFS stays the default; FAT16, mixed/existing
loaders, localized SIF and Windows To Go remain work. Actual Windows Setup is
still unverified. See
[the plan](linux-port-plan.md#cp22n9--legacy-fat-record-and-metadata-audit-2026-10-03).

CP22n10 implements/qualifies the regular-file legacy FAT32 BOOTMGR component.
It reuses original PE code and the existing record pipeline, preserving hints
and taking the shared 0x80/0x81 mapping. Existing providers stay byte-for-byte
unchanged. 152 adaptation cases, all provider/shared/root regressions and four
generated BIOS/metadata handoffs per build pass normally/sanitized. No fixture
drive/hint repair or loader alias is needed. Full retained-device helper
lifecycle is next; Qt legacy FAT choices remain gated and NTFS stays default.
FAT16, mixed/existing loaders, localized SIF, Windows To Go and actual Windows
Setup remain work. See
[the plan](linux-port-plan.md#cp22n10--native-legacy-fat32-bootmgr-record-adaptation-2026-10-03).


CP22n11 qualifies the complete retained-device legacy FAT32 helper path using
Rufus's advanced filesystem policy and shared setup plan. Existing record writer,
source checks, copy, private mount and cleanup are reused; adapted PE records
and final masquerading MBR are written only after successful unmount. Generated
i386/AMD64-UDF/minint jobs pass exact readback/fsck/BIOS handoff; normal/sanitized
root-write failures remain FAILED without final records. Existing NTFS legacy
and modern FAT32 regressions pass. Qt FAT selection remains gated for CP22n12;
NTFS stays default and legacy UEFI remains unsupported. FAT16, localized SIF,
mixed/existing loaders, Windows To Go and actual Windows Setup remain work. See
[the plan](linux-port-plan.md#cp22n11--complete-legacy-winpe-fat32-helper-job-2026-10-03).


CP22n12 exposes supported legacy WinPE FAT32 in Qt through the existing Alt+E
policy, retaining ordinary NTFS default and BIOS-only target/confirmation.
Selection refresh/persistence, fresh root-loader refusal, ISO/DD behavior and
full generated Qt/helper/readback/BIOS jobs pass normally/sanitized. READY waits
for sync/unmount and helper/launcher exit; unplug retains success. No helper,
protocol or shared Windows changes are needed. Next audit remaining mixed/root
loader dispatch before extending support; FAT16, localized no-BOM SIF, Windows
To Go and actual Windows Setup remain work. See
[the plan](linux-port-plan.md#cp22n12--qt-advanced-legacy-winpe-fat32-creation-2026-10-03).


CP22n13 audits existing/mixed legacy root loaders without production changes.
Windows retains an existing detector and may patch existing root SIF/BOOTMGR;
these require different source contracts, not a blanket collision exception.
Unused minint root SIF is already supported and preserved. Original provider/
combined-loader precedence is recorded; FAT16's DOS handoff remains unresolved.
Normal/sanitized source/root/shared checks pass, including 60 new root cases.
Next CP22n14 admits a verified identical root detector through existing preflight/
preparation and qualifies one complete job. Remaining root SIF/BOOTMGR, localized
SIF, FAT16, mixed dispatch and Windows To Go remain required work. See
[the plan](linux-port-plan.md#cp22n13--existingmixed-legacy-root-loader-audit-2026-10-03).


CP22n14 admits an identical existing root NTDETECT.COM for supported legacy
ISO/UDF NTFS/FAT32 jobs, retaining its bytes/name/inode/times through the existing
root component. Different detectors, required root SIF/BOOTMGR and other mixed
layouts remain gated. Source/root/shared normal and sanitizer checks pass, and
one complete generated Qt/helper FAT32 job passes readback, filesystem cleanup,
READY and BIOS handoff. No actual Windows Setup/customization matrix is added.
Next CP22n15 selects and safely edits the existing root SIF's own content; see
[the plan](linux-port-plan.md#cp22n14--retain-an-identical-legacy-root-detector-2026-10-03).


CP22n15 supports existing required root TXTSETUP.SIF content for supported legacy
ISO/UDF NTFS/FAT32 routes. It edits the root input itself through the shared
section matcher/native text adapter and verified atomic publication, preserving
directory sources, root spelling/ownership/permissions and unrelated settings.
Preflight reserves full edit staging space, with near-full regressions proving
ENOSPC refusal before writes. Source/root/shared normal and sanitizer checks,
focused Widgets refusals and a complete generated Qt/helper FAT32 readback/BIOS
job pass. Actual Windows Setup remains unqualified. Next audit existing root
BOOTMGR/SETUPLDR identity and original report/provider priority; localized no-BOM
SIF, differing detectors, FAT16, combined loaders and Windows To Go still remain.
See [the plan](linux-port-plan.md#cp22n15--edit-an-existing-legacy-root-sif-2026-10-03).


CP22n16 establishes the next existing root BOOTMGR contract: initially require
full equality with the selected unmodified SETUPLDR and preserve the original
filename report/FAT32 BOOTMGR-provider priority. Current source/Qt gates remain;
this audit adds no supported route. Native component implementation is CP22n17,
then fresh source admission/Qt/full helper qualification CP22n18. Normal and
sanitizer source checks pass with all production hashes unchanged. See
[the plan](linux-port-plan.md#cp22n16--existing-root-bootmgr-sourcerecord-contract-audit-2026-10-04).


CP22n17 implements identical existing root BOOTMGR inside the native component,
reusing its retained-input checks, shared loader patcher and SIF atomic writer.
Keep-minint retains the loader without staging or metadata changes. Component
success/error/race tests pass normally/sanitized; fresh source and GUI gates
remain unchanged. CP22n18 adds bounded source equality and staging space checks,
preserves the original BOOTMGR report/provider priority, and qualifies Qt/helper
integration. See
[the plan](linux-port-plan.md#cp22n17--native-identical-existing-root-bootmgr-2026-10-04).


CP22n18 exposes the identical existing root BOOTMGR legacy route in Qt through
fresh bounded ISO/UDF equality checks and full atomic-patch staging reservation.
The original report/provider priority and unknown version remain. A scoped Qt
pending-version allowance fixes the tested empty-choice integration bug; modern/
EFI/mixed and incompatible roots stay gated. Normal/sanitizer, focused/full GUI
and one generated full helper/readback/BIOS job pass. Next audit localized no-BOM
SIF input encoding; differing detectors/loaders, FAT16, mixed dispatch and Windows
To Go remain work. See
[the plan](linux-port-plan.md#cp22n18--existing-root-loader-source-admission-and-qt-integration-2026-10-04).


CP22n19 audits localized no-BOM SIF input without extending production admission.
Explicit encoding remains required; NLS hints/UTF-8 validity do not justify
host-independent inference. Existing Unicode/BOM edits and localized `/minint`
retention pass focused normal/sanitizer/locale checks. Windows CRT round trips
remain deferred. Next extend the retained root NTDETECT.COM contract to different
bounded regular content using the original no-replace-copy behavior, leaving
loader equality and file/error gates intact. See
[the checkpoint](linux-port-plan.md#cp22n19--localized-sif-encoding-contract-audit-2026-10-04).


CP22n20 extends retained root NTDETECT.COM to differing bounded regular content,
matching original no-replace copy behavior. Existing identity/file safety checks
and loader equality remain; no new production framework. Source/native/shared
normal and sanitizer checks plus 58 focused Widgets cases pass. Exact retained
bytes/metadata and absence of detector staging are checked. Next complete the
existing helper/readback recipe with differing detector content; actual Windows
Setup and executable compatibility are not newly qualified. See
[the checkpoint](linux-port-plan.md#cp22n20--retain-a-differing-root-ntdetectcom-2026-10-04).


CP22n21 completes the existing Qt/helper i386 FAT32 qualification for a differing
retained root detector plus existing root SIF/loader. Exact file hashes, original
records/FSInfo and expected-label-only fsck, one simulated authorization, sync/
unmount/exit before READY, unplug and generated BIOS pass. Production is unchanged;
Windows Setup/detector execution remains deferred. Next address prepatched/
nonidentical root-loader inputs through the shared transformation. See
[the checkpoint](linux-port-plan.md#cp22n21--complete-differing-detector-qthelper-readback-2026-10-04).


CP22n22 admits ordinary fully/partially prepatched root BOOTMGR through matching
one-pass shared transformations, preserving original reporting/providers and
unknown Windows version. Keep-minint still requires raw equality. Normal and
sanitizer native/source/shared tests and 70 focused Widgets cases pass; complete
prepatched-loader guest readback is next. Nonconverging roots, explicit localized
no-BOM SIF encoding, compatible FAT16 handoff, mixed dispatch and Windows To Go
remain required work. See
[the plan](linux-port-plan.md#cp22n22--existing-loader-admission-through-one-shared-patch-pass-2026-10-04).


CP22n23 completes prepatched-root-loader Qt/helper i386/FAT32 qualification:
exact seven-file readback, retained root bytes and directory sources, root SIF
edit and differing detector preservation, original records/FSInfo, one simulated
authorization, durable READY/unplug and generated BIOS handoff. Production is
unchanged. Actual Windows Setup remains deferred. Next address remaining mixed
legacy dispatch while keeping nonconverging roots, localized no-BOM encoding,
compatible FAT16 and Windows To Go in scope. See
[the plan](linux-port-plan.md#cp22n23--complete-prepatched-loader-qthelper-readback-2026-10-04).


CP22n24 removes the redundant source/Qt rejection of partial minint files beside
a complete i386/AMD64 setup with known /minint, using the existing shared/native
keep-loader plan. Normal/sanitizer and 94 focused Widgets tests pass; a complete
generated Qt/helper job is next. Syslinux/GRUB/EFI mixtures remain gated until
original MBR/SBR/PBR and post-copy dispatch is shared, including the Windows
Alt+E exception. Localized no-BOM, compatible FAT16, nonconverging roots and
Windows To Go remain required work. See
[the checkpoint](linux-port-plan.md#cp22n24--follow-the-shared-plan-for-mixed-setup-directories-2026-10-04).


CP22n25–32 complete partial-minint keep-loader qualification, share the original
independent boot-provider/PBR choices, and admit the qualified WinPE/Syslinux
Alt+E FAT32/NTFS mixture in Qt. Retain provider/root flags and unknown detected
versions. Loading with Alt+E follows shared FAT32 preference; toggling retains an
allowed selection. Alt+E-off ISO mixtures remain gated while valid hybrids retain
DD. Complete FAT32/NTFS backend jobs and one mixed NTFS Widgets READY/unplug job
pass independent file/boot-record/generated-BIOS readback. Normal/sanitizer
source/Widgets checks pass. Actual Windows Setup/detector remains deferred.
Remaining WinPE/GRUB/EFI/modern mixtures need an original-dispatch/native-sequence
audit; localized no-BOM SIF, compatible FAT16, nonconverging roots and Windows To
Go remain required work. See
[the current checkpoint](linux-port-plan.md#cp22n32--mixed-winpesyslinux-qt-admission-and-durable-ready-2026-10-04).


CP22n33 admits GRUB2/GRUB4DOS data alongside the qualified WinPE/Syslinux Alt+E
mixture using the original absent-SBR precedence. Preserve report flags,
filesystem restrictions and the original GRUB version prompt before writing.
Normal/sanitizer source/Widgets and a complete combined NTFS Qt/BIOS/READY job
pass. GRUB-only WinPE still requires its independent SBR and final MBR/active-byte
sequence; it remains gated. GRUB4DOS SBR and the other legacy/Windows To Go work
above remain required. See
[the checkpoint](linux-port-plan.md#cp22n33--reuse-the-sbr-free-winpesyslinuxgrub-path-2026-10-04).


CP22n34 qualifies the existing GRUB2 SBR writer beside original Windows/Rufus/
masqueraded MBR records on disposable files. It preserves exact MBR/active bytes,
payload boundaries, sync/readback and descriptor ownership, with normal and
sanitizer fault coverage. This removes the low-level writer restriction only;
WinPE/GRUB SBR image routes remain gated until fresh policy, payload selection
and helper sequencing are connected and qualified. GRUB4DOS SBR and the legacy/
Windows To Go backlog above remain required. See
[the checkpoint](linux-port-plan.md#cp22n34--independent-grub2-sbr-with-original-windows-mbrs-2026-10-04).


CP22n35 connects the retained GRUB2 SBR to the existing Windows NTFS backend for
Alt+E/shared keep-loader WinPE. Reuse core staging/identity and native helper
operations, with fresh independent MBR/SBR/PBR policy and no new installer flag.
Source/request/helper regular-file and sanitizer checks pass. This does not yet
enable Qt or qualify a complete simulated/physical USB boot. The next step is
the original Qt lookup/preflight/confirmation flow and full helper/READY job.
FAT32, patch/masquerade and GRUB4DOS SBR routes remain required alongside the
legacy/Windows To Go backlog. See
[the checkpoint](linux-port-plan.md#cp22n35--reuse-windows-requests-and-retained-grub2-sbr-payloads-2026-10-04).

CP22n36 resolves source-preflight ordering before the GRUB payload lookup by
reusing validation through a read-only source entry. Final helper preflight still
requires the frozen core; no versions or installers are guessed. Source/request
and focused Qt regression checks pass normally and under sanitizers. Qt admission
still awaits payload freezing and a complete simulated USB job in CP22n37. See
[the checkpoint](linux-port-plan.md#cp22n36--source-checks-before-grub-payload-selection-2026-10-04).

CP22n37 enables NTFS/Alt+E/shared keep-loader WinPE with a GRUB2 SBR in Qt. Reuse
original lookup, shared embedded bytes, owned cache/download snapshot, existing
staging and one helper. Normal/sanitizer component and Widgets checks plus one
complete kernel-ntfs simulated USB/READY/generated BIOS handoff pass. Exact
Windows MBR/PBR, frozen SBR and prepared-file readback are checked. Actual Setup,
host polkit and GRUB core execution/ABI remain unverified. Unknown directory-only
GRUB2 still follows original scanning/dispatch and does not become a guessed
provider. FAT32/patch/masquerade and GRUB4DOS SBR remain required, with FAT32
keep-loader next. See
[the checkpoint](linux-port-plan.md#cp22n37--qt-windows-ntfs-with-an-independent-frozen-grub2-sbr-2026-10-04).


CP22n38 enables the same frozen GRUB2 SBR for advanced FAT32 keep-loader WinPE.
Reuse four existing production entry points; shared policy/payload/writer and
final WinPE boot-record operations are unchanged. Qt restores normal FAT32/NTFS
choices and shared defaults. Normal/sanitizer source/request/helper/Widgets cases
and one complete vfat simulated USB/READY/generated BIOS handoff pass, checking
original Windows records, exact core/gap, clean allocation hints and eleven files.
Competing NTLDR/IO.SYS/MSDOS.SYS refuse FAT32; supported root BOOTMGR remains allowed.
Actual Setup, GRUB core execution/ABI, host polkit and physical USB remain unverified.
Patch/masquerade SBR sequencing is next; GRUB4DOS SBR and the legacy/Windows To Go
backlog remain required. See
[the checkpoint](linux-port-plan.md#cp22n38--advanced-fat32-winpe-with-the-same-independent-grub2-sbr-2026-10-04).


CP22n39 enables shared patch/masquerade WinPE with the same independent GRUB2 SBR.
Only the existing classifier changes; complete helper/Qt operations and original
Windows/shared transformations/writers are reused. Normal/sanitizer source,
request, helper and Widgets coverage plus a complete generated AMD64/vfat Qt USB
job pass. Verify the core survives final Rufus MBR/0x81 publication after unmount,
exact loader/SIF/boot records, clean volume, one authorization and durable READY.
Generated BIOS drive-0x81 handoff passes; actual Setup/detector, GRUB core execution,
host polkit and physical USB remain unverified. GRUB4DOS SBR investigation is next;
the remaining legacy/FAT16/Windows To Go backlog remains required. See
[the checkpoint](linux-port-plan.md#cp22n39--shared-winpe-patchmasquerade-with-an-independent-grub2-sbr-2026-10-04).

## CP22n40 update — original GRUB4DOS boot-record reuse (2026-10-04)

Original GRUB4DOS MBR/resource now use the existing native partition adapter and
shared GRUB gap writer. Normal/sanitizer functional and fault tests qualify exact
placement/preservation, including final WinPE masquerade rewriting. Windows
policy/code/resources are unchanged. Complete GRUB4DOS image/standalone helper
and Qt routes, loader compatibility/download/copy and execution remain required.
See [the reuse audit](linux-grub4dos-reuse.md) and
[CP22n40](linux-port-plan.md#cp22n40--original-grub4dos-boot-records-through-the-existing-gap-writer-2026-10-04).

## CP22n41 update — WinPE GRUB4DOS SBR helper qualification (2026-10-04)

The advanced WinPE FAT32/NTFS helper reuses the original immutable GRUB4DOS SBR,
shared precedence and existing copy/WinPE/sync/final-MBR flow. Normal/sanitizer
component checks and a complete FAT32 simulated USB creation/readback/Windows
loader handoff pass. Qt SBR admission remains gated; standalone and general ISO
GRUB4DOS routes and actual execution remain required. See
[CP22n41](linux-port-plan.md#cp22n41--bundled-grub4dos-sbr-in-the-existing-winpe-helper-2026-10-04).


## CP22n42 update — advanced WinPE GRUB4DOS SBR Qt route (2026-10-04)

Qt now admits the qualified Alt+E FAT32/NTFS route using original filesystem/MBR
and SBR precedence. Normal/sanitizer Widgets tests and one complete FAT32 GUI
simulated USB creation/readback pass, including confirmation, one simulated
authorization and full READY after sync/cleanup/launcher exit; unplug preserves it.
Unknown GRUB2 stays unknown and GRUB4DOS does not select a GRUB2 download/core.
One production file changes in four lines. Standalone signed grldr download/copy,
actual GRUB4DOS execution and general ISO routes remain required. See
[CP22n42](linux-port-plan.md#cp22n42--qt-winpe-selection-with-the-original-grub4dos-sbr-2026-10-04).


## CP22n43 update — standalone GRUB4DOS loader acquisition (2026-10-04)

Fixed-version grldr now uses existing owned cache/signed-download/atomic-readback
operations. Normal/sanitizer cases qualify the real signature, failures,
cancellation and provider/object reuse. This is the acquisition foundation;
standalone request/helper/copy, complete native creation, Qt exposure and actual
execution remain required. No image version inference or GRUB2 substitution.
See [CP22n43](linux-port-plan.md#cp22n43--standalone-grub4dos-loader-selection-through-existing-cachedownload-code-2026-10-04).


## CP22n44 update — standalone GRUB4DOS native FAT32 helper (2026-10-05)

The retained standalone grldr now reaches an explicit FAT32 helper job through
existing staging/metadata/source checks. Shared original MBR/SBR/FAT writers and
existing mounted-file copy/sync/cleanup produce a complete native simulated USB
with exact loader and record readback, one authorization and completion after exit.
Required loader errors prevent READY. Nine existing production files change; no new
production library/file. Standalone Qt exposure and actual BIOS execution are the
next checkpoints; FAT16/NTFS and general image GRUB4DOS routes remain required.
See [CP22n44](linux-port-plan.md#cp22n44--standalone-grub4dos-frozen-loader-and-native-fat32-helper-2026-10-05).


## CP22n45 update — standalone GRUB4DOS BIOS execution (2026-10-05)

The helper-created original FAT32 GRUB4DOS stick now reaches its own BIOS command
prompt, identifies its root, reads the complete grldr through FAT and executes a
command. A missing-loader copy is rejected with its original error. Exact creation
readback/sync/cleanup remain qualified, and snapshot boot leaves backing media
unchanged. No production change. Standalone Qt admission is next; FAT16/NTFS,
general image GRUB4DOS and OS chainloading remain required/separate.
See [CP22n45](linux-port-plan.md#cp22n45--actual-standalone-grub4dos-bios-execution-2026-10-05).


## CP22n46 update — standalone GRUB4DOS FAT32 Qt integration (2026-10-05)

Advanced Grub4DOS 0.4.6a now selects the qualified MBR/FAT32/BIOS route without an
ISO. Reuse acquisition/signature/cache, private frozen-loader staging, shared
progress policy and single job helper. No/Cancel, malformed cache and download
failures stop before authorization; the dialog accurately states Linux's missing-
loader stop. Held child/result, exact loader copy, cleanup/failure/cancellation and
unplug behavior are covered by normal/sanitized Widgets functional fixtures.
CP22n47 combines this UI with the unchanged native helper, virtual USB and existing
BIOS/CRC qualifier. FAT16/NTFS, general GRUB4DOS images and remaining Windows boot
features are still required. No extra Windows customization qualification.


## CP22n47 update — native standalone GRUB4DOS Qt/USB/BIOS route (2026-10-05)

The Qt FAT32/MBR/BIOS choice creates independently verified native virtual USB media
with the original grldr and boot records, despite post-selection cache replacement.
One guest-only authorization, original progress, sync/cleanup/held launcher exit,
private staging/descendant/claim removal, full READY and stable unplug are checked.
BIOS execution verifies original version/root/listing/whole-loader CRC and an ECHO
command; removing only grldr makes the same qualifier fail with its original error.
All 695 production hashes remain unchanged. Physical desktop/polkit remains separate.
Next qualify standalone FAT16 before Qt admission; NTFS/general images and full
Windows parity remain required. No extra Windows customization qualification.


## CP22n48 update — standalone GRUB4DOS FAT16 helper/BIOS (2026-10-05)

Source-free MBR/FAT16 now reuses and qualifies the same original GRUB4DOS boot
records, native FAT formatter and retained root loader copy. Only protocol admission
changes; native creation/readback/BIOS/CRC/ECHO and missing-loader controls pass for
FAT16 and FAT32. One simulated authorization covers cleanup and joined exit.
FAT16 Qt selection and its complete native GUI qualification are next, CP22n49;
Qt remains FAT32-only today. NTFS, general images and OS chainloading remain required.


## CP22n49 update — standalone GRUB4DOS FAT16 Qt/native route (2026-10-05)

Advanced Grub4DOS 0.4.6a now offers qualified FAT16/FAT32 for MBR/BIOS, using the
existing original cluster/default policy, frozen loader and one job helper. Both
FAT variants pass Widgets failures/cancel/completion tests and native Qt virtual-USB
creation/readback/BIOS/CRC/ECHO/missing-loader controls. Only the native cluster gate
changes in production. Standalone NTFS backend qualification is next, before NTFS
Qt exposure; general image and OS chainloading work remain required.


## CP22n50 update — standalone GRUB4DOS NTFS backend/BIOS (2026-10-05)

Source-free MBR/NTFS now reuses the existing native formatter, original NTFS boot
adapter, GRUB4DOS MBR/SBR and retained loader copy. The preferred new kernel driver,
independent NTFS/loader readback, one guest authorization, joined sync/unmount/exit,
actual original BIOS root/CRC/ECHO and missing-loader control pass. Affected normal
and ASan/UBSan/leak-detection suites pass. No new production module; Qt stays on
qualified FAT16/FAT32. NTFS-3G fallback qualification is CP22n51, followed by NTFS
Qt admission/qualification. General images, OS chainloading and full parity remain.


## CP22n51 update — standalone GRUB4DOS NTFS fallback (2026-10-05)

The unchanged NTFS-3G fallback creates the original GRUB4DOS loader/boot records
without new kernel NTFS support. Normal and sanitized native creation/readback/
BIOS/CRC/ECHO/missing-loader checks pass. A held FUSE exit verifies that completion
waits for unmount and the owned process; one authorization covers the entire job.
The preferred kernel route also passes. All 695 production hashes stay unchanged.
Qt remains FAT16/FAT32 until CP22n52 NTFS selection/Widgets qualification, followed
by CP22n53 native Qt completion. General image and OS chainloading work remains.


## CP22n52 update — standalone GRUB4DOS NTFS Qt selection (2026-10-05)

The advanced standalone choice now offers FAT16/FAT32/NTFS using existing Rufus
cluster/default/label and selection rules. One native cluster gate changes; the
qualified kernel/FUSE backend, frozen loader, BIOS/MBR semantics and one helper
remain unchanged. Focused normal/sanitizer GUI tests cover failures/cancellation,
frozen choices, exact original NTFS progress, joined full READY/one notification
and stable unplug. CP22n53 still needs the combined native Qt NTFS route; general
GRUB4DOS images and OS chainloading remain required. See
[CP22n52](linux-port-plan.md#cp22n52--standalone-grub4dos-ntfs-qt-admission-2026-10-05).


## CP22n53 update — native Qt GRUB4DOS NTFS creation (2026-10-05)

The existing Qt/native helper path creates standalone original GRUB4DOS NTFS
media through the preferred kernel and absent-kernel NTFS-3G routes. Reuse frozen
loader/label/geometry, formatter-owned serial, BIOS/MBR records, one authorization,
exact original progress and joined READY/one notification/stable unplug. Native
readback and real BIOS root/CRC/ECHO plus missing-loader controls qualify each
route; the existing FAT32 GUI form remains a regression control. All production
hashes stay unchanged. This is virtual USB/guest authorization evidence; general
GRUB4DOS ISO routes, OS chainloading, host polkit and physical-media durability
remain separate. See
[CP22n53](linux-port-plan.md#cp22n53--native-standalone-grub4dos-ntfs-qt-route-2026-10-05).


## CP22n54 update — general GRUB4DOS ISO backend (2026-10-05)

The first plain BIOS/MBR/FAT32 GRUB4DOS image backend reuses the existing source,
format, boot-record and extraction pipeline. Preserve the image's own root grldr
and original provider precedence; reject standalone payload substitution and
unsupported image combinations. Source/protocol completion checks are covered.
Qt image admission remains gated until representative native creation/readback/
BIOS qualification in CP22n55, followed by ordinary Qt qualification in CP22n56.
FAT16/NTFS/EFI image paths and OS chainloading remain required. See
[CP22n54](linux-port-plan.md#cp22n54--general-grub4dos-biosfat32-iso-backend-2026-10-05).


## CP22n55 update — native GRUB4DOS ISO qualification (2026-10-05)

Plain BIOS/MBR/FAT32 ISO creation now passes native client/helper progress,
authorization/sync/cleanup/exit checks, independent original-record and four-file
readback, and actual GRUB4DOS loader/menu execution normally and under sanitizers.
Missing-loader controls and the old standalone FAT32 command also pass. All 695
production hashes remain unchanged. Ordinary Qt image admission is CP22n56;
other image filesystems/EFI, arbitrary-loader compatibility and OS chainloads
remain required. See
[CP22n55](linux-port-plan.md#cp22n55--native-grub4dos-iso-creationreadbackbios-2026-10-05).


## CP22n56 update — ordinary Qt GRUB4DOS ISO route (2026-10-05)

Ordinary Qt selection now admits the qualified plain BIOS/MBR/FAT32 GRUB4DOS ISO
backend, using shared provider precedence and existing ISO/DD policies. The image
owns its loader/menu; no standalone cache or inferred version is used. ISO copy
percentages, original MBR/SBR caption and joined READY behavior have focused GUI
coverage. Four existing Qt production files change; no new module or Windows
source change. Combined native Qt/readback/BIOS qualification is CP22n57. FAT16/
NTFS/EFI BIOS combinations, arbitrary compatibility and OS chainloads remain
required. See [CP22n56](linux-port-plan.md#cp22n56--ordinary-qt-grub4dos-iso-admission-2026-10-05).


## CP22n57 update — combined native Qt GRUB4DOS ISO qualification (2026-10-05)

The existing Widgets guest now loads the generated GRUB4DOS ISO normally and
checks its native helper creation, independent FAT/MBR readback and original
loader/menu BIOS execution, including cleanup-before-READY and single simulated
authorization. No production changes. FAT16 image admission/backend qualification
is next; NTFS/EFI BIOS combinations, arbitrary image-loader compatibility, OS
chainloads, host polkit and physical USB durability remain required. See
[CP22n57](linux-port-plan.md#cp22n57--native-qt-grub4dos-iso-creationreadbackbios-2026-10-05).


## CP22n58 update — GRUB4DOS FAT16 ISO backend contract (2026-10-06)

The existing GRUB4DOS request/helper contract now accepts source-backed FAT16
without a standalone loader. Two guards in one existing production file change;
native formatting/boot records/extraction and original Windows sources are
unchanged. Focused source and complete request/process checks cover the admitted
combination and existing refusals. Native FAT16 image/readback/BIOS qualification
is CP22n59; ordinary Qt selection remains FAT32-only until then. NTFS/EFI image
combinations and remaining loader/OS compatibility stay required. See
[CP22n58](linux-port-plan.md#cp22n58--grub4dos-fat16-iso-backend-contract-2026-10-06).


## CP22n59 update — native GRUB4DOS FAT16 ISO qualification (2026-10-06)

FAT16 image creation passes native helper/source/progress/cleanup checks,
independent original boot-record and four-file readback, actual original loader/
menu BIOS execution and missing-loader controls normally and under sanitizers.
FAT32 ISO remains a passing regression. No production changes. Ordinary Qt FAT16
admission and combined native GUI qualification are CP22n60; source-backed NTFS/
EFI and remaining compatibility/OS boot work stay required. See
[CP22n59](linux-port-plan.md#cp22n59--native-grub4dos-fat16-iso-creationreadbackbios-2026-10-06).


## CP22n60 update — Qt GRUB4DOS FAT16 ISO route (2026-10-06)

Ordinary Qt GRUB4DOS images now offer qualified FAT16/FAT32 using original small-
device defaults, ISO/DD policy and shared progress. Focused component and native
Qt creation/readback/BIOS checks cover the FAT16 route and FAT32 regression,
including durable joined READY and single authorization. Only two lines in the
existing selector change; no new production module or backend. Source-backed
NTFS is next, followed by remaining EFI/loader/OS compatibility and delivery
work. See [CP22n60](linux-port-plan.md#cp22n60--qt-grub4dos-fat16-iso-admission-and-native-qualification-2026-10-06).


## CP22n61 update — GRUB4DOS NTFS ISO backend contract (2026-10-06)

The existing NTFS operation now accepts source-backed GRUB4DOS requests, retaining
image-owned loader/source checks and native formatter/boot/extraction reuse.
Normal and sanitizer suites pass 1414 source and 302 request checks each. One
existing production validator changes; no new backend or provider. Native NTFS
image readback/BIOS qualification is CP22n62; Qt NTFS remains gated. FAT16/FAT32
GUI qualification remains CP22n60. See
[CP22n61](linux-port-plan.md#cp22n61--grub4dos-ntfs-iso-contractbackend-admission-2026-10-06).


## CP22n62 update — native GRUB4DOS NTFS ISO proof (2026-10-06)

Source-backed NTFS creation/readback/BIOS passes normally and under sanitizers
with kernel ntfs and lowntfs-3g. Original records, all four source files, actual
loader/menu execution, missing-loader refusal and joined cleanup are checked.
Standalone fallback and FAT32 ISO remain passing regressions. No production
change. Ordinary Qt NTFS admission and combined GUI qualification are CP22n63;
image/OS compatibility, EFI, host polkit and physical hardware remain required.
See [CP22n62](linux-port-plan.md#cp22n62--native-grub4dos-ntfs-iso-creationreadbackbios-2026-10-06).


## CP22n63 update — ordinary Qt GRUB4DOS NTFS ISO (2026-10-06)

Qt now offers FAT16/FAT32/NTFS for qualified plain BIOS GRUB4DOS images, preserving
shared FAT16 defaults and manual choice reload. Both NTFS providers pass native
Qt/readback/BIOS normally and with sanitizers; FAT32 ISO and standalone fallback
remain native regressions. Focused GUI/window checks pass 173/77 each normally
and with sanitizers. Fix a demonstrated GUI omission that accepted an incomplete
NTFS format report. Two existing production files change; no new backend/module.
Combined BIOS/UEFI source contract starts with FAT32 in CP22n64. See
[CP22n63](linux-port-plan.md#cp22n63--ordinary-qt-grub4dos-ntfs-iso-admissionqualification-2026-10-06).


## CP22n64 update — GRUB4DOS FAT32 combined EFI contract (2026-10-06)

The existing source validator now accepts an image-owned x64 EFI fallback for
combined FAT32 BIOS/UEFI GRUB4DOS requests, reusing shared policy and bounded EFI
validation. Normal/sanitizer suites pass 1440 source and 302 request cases each,
including malformed loaders, source errors/cancellation/staleness and no target
access. One production file changes. Native BIOS/UEFI creation/readback/boot is
CP22n65; combined MBR Qt admission remains gated. FAT16, NTFS UEFI:NTFS pairing
and other EFI architectures remain required. See
[CP22n64](linux-port-plan.md#cp22n64--grub4dos-combined-fat32-biosuefi-source-contract-2026-10-06).


## CP22n65 update — native GRUB4DOS FAT32 BIOS/UEFI proof (2026-10-07)

Combined FAT32 source-backed creation/readback passes normally and under
sanitizers. The same verified disk executes the original GRUB4DOS BIOS menu and
source-owned x64 EFI application directly from FAT32. Exact five-file/boot-record
readback, missing BIOS loader refusal, joined cleanup and unchanged snapshot
media pass; plain BIOS remains a passing regression. No production change.
Ordinary combined MBR Qt admission/qualification is CP22n66. FAT16, NTFS paired
EFI, other architectures, OS compatibility and physical/host authorization
qualification remain required. See
[CP22n65](linux-port-plan.md#cp22n65--native-grub4dos-fat32-biosuefi-creationreadbackboot-2026-10-07).


## CP22n66 update — ordinary Qt combined GRUB4DOS FAT32 (2026-10-07)

Qt now offers qualified FAT32 MBR BIOS/UEFI while preserving GPT/UEFI behavior,
shared defaults and plain BIOS formats. Focused GUI/window checks pass 189/77
normally and with sanitizers. Native Qt normal/sanitizer combined creation,
five-file/original-record readback, both firmware boots, joined READY and
missing-loader controls pass; plain BIOS remains a regression. One existing
selector changes, with no new module/backend. Combined FAT16 contract/native
proof is next CP22n67, then its GUI; NTFS paired EFI and other parity work remain
required. See [CP22n66](linux-port-plan.md#cp22n66--ordinary-qt-grub4dos-fat32-biosuefi-route-2026-10-07).


## CP22n67 update — combined FAT16 backend/native proof (2026-10-07)

The existing GRUB4DOS combined source/backend now accepts FAT16/FAT32 through the
same EFI validator. Normal/sanitizer source/request/GUI-gate checks pass 1446/
302/94 each. Native FAT16 five-file/original-record readback, BIOS menu execution,
direct x64 UEFI handoff and joined cleanup pass normally and with sanitizers;
FAT32 remains a native regression. One production validator changes. Ordinary
combined FAT16 GUI admission is CP22n68; NTFS paired EFI and other parity work
remain required. See
[CP22n67](linux-port-plan.md#cp22n67--combined-grub4dos-fat16-sourcenative-biosuefi-proof-2026-10-07).


## CP22n68 update — ordinary Qt combined FAT16/FAT32 (2026-10-07)

Combined MBR GRUB4DOS now offers FAT16/FAT32 with shared FAT16 defaults/manual
retention. GUI/window checks pass 199/77 each normally and with sanitizers;
native Qt FAT16 creation/readback/BIOS/UEFI passes normally and with sanitizers,
with FAT32 regression. One selector changes. Test-only default assumptions and
injected-key timing issues are documented with failed/final evidence. Required
NTFS UEFI:NTFS request/backend pairing is next CP22n69 before GUI admission;
other parity/qualification work remains. See
[CP22n68](linux-port-plan.md#cp22n68--ordinary-qt-combined-grub4dos-fat16fat32-2026-10-07).


## CP22n69 update — GRUB4DOS NTFS paired backend contract (2026-10-07)

Source-backed GRUB4DOS NTFS now has the existing GRUB-style UEFI:NTFS operation/
source pairing, shared payload layout and bounded EFI validation. Normal and
sanitizer suites pass 1458 source/305 request cases each. Two existing production
files change; helper, resources and GUI selector do not. Native kernel/fallback
paired readback/BIOS/UEFI proof is CP22n70 before ordinary GUI admission. Other
parity/qualification work remains. See
[CP22n69](linux-port-plan.md#cp22n69--grub4dos-ntfs-uefintfs-requestsource-pairing-2026-10-07).


## CP22n70 update — paired NTFS native BIOS/UEFI proof (2026-10-07)

Source-owned GRUB4DOS NTFS paired creation/readback passes with kernel ntfs and
lowntfs-3g normally and under sanitizers. Original records, five source files,
exact trailing bundled payload/table geometry, actual BIOS menu and original
UEFI:NTFS-to-source x64 handoff, joined cleanup and unchanged snapshot media
pass. Direct FAT32 remains a native regression. No production changes. Ordinary
combined NTFS Qt admission/qualification is CP22n71. Other parity/qualification
work remains. See
[CP22n70](linux-port-plan.md#cp22n70--native-grub4dos-ntfs-uefintfs-biosuefi-proof-2026-10-07).


## CP23a update — image checksum foundation (2026-10-18)

Share the unchanged original digest algorithms/context and CPU detection with
Windows. The native adapter hashes one retained ordinary file with default MD5,
SHA-1 and SHA-256, optional SHA-512, bounded I/O and final source validation before
publishing. It preserves source offsets, original progress identity and cancellation,
without targets or privileges. Normal/sanitizer vector, fault and retained-source
checks pass; emulated SHA-1/SHA-256 instructions also match all 96 reference cases.
The Qt toolbar remains gated pending worker/dialog, Alt+H/settings and lifecycle
qualification. Windows algorithm threads remain a native performance-comparison
item. Evidence and reuse audit: `.rufus-test-artifacts/cp23a`; no Windows execution,
other-host qualification or complete checksum GUI is claimed.


## CP23b update — ordinary Qt image checksums (2026-10-18)

The checksum icon now operates on accepted idle raw/ISO images without a selected
USB or helper. Default MD5/SHA1/SHA256 and Alt+H's persisted SHA512 choice are
frozen with source identity. The read-only worker reports tagged progress and
publishes results only after joined cleanup; pending success is discarded on
Cancel. The GUI-thread result dialog preserves original selectable monospaced
fields, basename, optional-hash hint and OK/Escape, with full-value clipboard copy.
Full READY and one notification follow joined success. Focused normal/sanitizer,
analysis/window/scaled and simulated raw-write regressions pass; screenshots are
inspected. Per-algorithm pipeline performance remains follow-up work. See CP23b's
checkpoint record for exact evidence, original Windows comparisons and the Linux
read-only cancellation-notification difference.


## CP23c update — native checksum parallelism (2026-10-18)

One reader and per-algorithm pthread consumers now share three bounded 64-KiB
slots, preserving the Windows pipeline's parallel intent with the original core.
All workers join and resources are released before final source validation/results.
Read-ahead is bounded by the slowest consumer; reporting/cancellation stay on the
calling Qt worker. Native startup failures/cancellation, 100 independent vectors,
GUI/scaling and ASan/UBSan/leak/TSan checks pass, including single-CPU and emulated
SHA instruction cases. Optimized 256-MiB warm-input medians improve from 112.45 to
220.09 MiB/s for default hashes and 81.97 to 215.99 MiB/s with SHA512. This compares
the retained serial and native pipeline, not actual Windows/desktop/storage runs.
Linux retains caller CPU affinity and cooperative joined cleanup. Evidence and
exact limits are in CP23c's checkpoint record. Continue compressed-image work and
all remaining parity groups.


CP27a qualifies the original LZMA-Alone retained-source foundation: both builds
pass 360 differential/property vectors, 30892 source checks, actual >4-GiB output/
limit and existing source/writer/80-analysis/81-Widgets regressions. Original
Windows default/FAST decoder preprocessing is identical. Native known-size and
history refusals are documented intentional differences. Writer/helper/Qt LZMA
admission is next; enabled ordinary compression remains gzip/XZ/BZIP2.


CP27b qualifies LZMA frozen plans/native writer/strict helper on private files
and simulated-native targets. Both builds pass 145 writer cases, 60 helper jobs,
33 protocol results, all 81 analysis results and all admitted format regressions
(13 targets). Valid same-fact stream substitutions prove decoded SHA/readback
defenses without a container CRC. Actual LZMA guest/helper/controller and ordinary
SELECT remain next; original algorithms and current gzip/XZ/BZIP2 UI are unchanged.


CP27c adds both 38-job LZMA guest qualifications, including eight real source/
claim/fault observations, four mounted jobs and a fully poisoned 64-MiB
production-helper restore with raw/guest/host hashes, independent files and
BIOS boot. Production and both helper binaries remain unchanged from CP27b.
Ordinary LZMA SELECT/controller/READY is next; all other parity gaps remain.


CP27d enables ordinary LZMA SELECT/confirmed creation through the existing
controller. Both normal/sanitizer runs pass 96 analysis/99 focused Widgets
results across all four formats and twelve directed replacement pairs, plus
the full-size 11-job LZMA controller/helper/readback/BIOS guest. Both production
helpers are unchanged from CP27c's 38-job qualification. Enabled compression
is gzip/XZ/BZIP2/LZMA; all other image/provider/advanced/legacy gaps remain.


CP28a adds original Unix-compress/LZW to the retained read-only source foundation.
Native final callback failures propagate and saturated nine-bit resets avoid the
original's silent output loss; disabled Windows preprocessing remains identical.
Normal passes 387 corpus cases, 37197 source checks and actual 4294967808-byte
counting; the final ASan/UBSan source/corpus/large qualification also passes. `.Z` creation and `.gz` fallback remain next,
followed by all other providers, containers, saving and advanced/legacy work.


CP28b adds LZW frozen plans/native writing and strict helper/protocol support on
owned regular and simulated-native targets. A late original invalid-code error
can return a positive previous write count; native diagnostics now abort EBADMSG
and clear publication. Healthy core/I/O behavior and Windows preprocessing are
preserved. Normal passes all 17 targets, including 390 source cases, 188 writer
cases, 57 helper jobs, 33 protocol and 97 analysis results.
The complete ASan/UBSan 17-target qualification also passes. Actual LZW guest/controller/READY and original
`.gz` fallback are next; all remaining Windows feature groups stay in scope.


CP28c qualifies the unchanged production LZW helper in both complete 50-job
guests: 36 direct, four mounted and ten required seed/raw jobs. Twenty independent
stream fixtures and eleven actual claim/fault observations cover source I/O,
late invalid code and valid no-CRC source-data changes. Fully poisoned 64-MiB
restores pass raw/LZW/guest/host hashes, independent files and BIOS boot. All
730 production hashes and both helper binaries remain unchanged. Ordinary
LZW SELECT/controller/READY and `.gz` fallback are next; full scope stays active.


CP28d enables ordinary LZW SELECT/picker/confirmed DD and original initial Unix-
compress magic fallback inside `.gz`, retaining filename association 3 and LZW's
no-checksum/EOF semantics. Both normal/sanitizer 23-target host suites, 130
analysis/135 focused Widgets results and complete 17-job real guests pass,
including two full controller restores, five actual fallback faults, held99/
joined100, independent readback and BIOS. Five production files change; 725
remain unchanged. Current compression is gzip/XZ/BZIP2/LZMA/LZW. Original ZIP
source foundation is next; all containers/saving/advanced/legacy gaps remain.


CP29a qualifies the internal ZIP source reader for stored and raw-deflate first
central-directory entries, including ZIP64 and actual >4-GiB output. Its
bounded parser corrects an original short-record overread; original Windows
container preprocessing is byte-identical. Both 28-target normal/sanitizer
host suites and focused 140-case ZIP rechecks pass. ZIP remains absent from
ordinary GUI creation until remaining methods and writer/helper/guest/READY
qualify. Full Windows parity remains active.


CP29b qualifies all five methods named by the original ZIP dispatcher as
internal source readers: stored, deflate, BZIP2, LZMA and XZ. Both 28-target
normal/sanitizer host suites and 256-case focused ZIP rechecks pass. The
original Windows container and decoder cores are unchanged. ZIP writer,
helper, real guest and ordinary GUI creation remain gated; full parity stays
active.


CP29c admits ZIP to frozen decoded plans, the native writer and strict helper
packet. Normal host checks pass 30 sandbox targets plus 135 Widgets cases
outside the sandbox; the sanitizer suite passes 31/31. All five methods pass
owned-file writes and simulated native helper jobs. Real guest and ordinary
GUI gates remain.


CP29d qualifies the unchanged ZIP helper on real disposable guest block media:
all five original entry methods write, reject damaged CRCs, and preserve the
claim/cleanup rules. Both normal and sanitizer guests pass 23 ZIP direct jobs,
eight claim observations, a poisoned full 64-MiB restore, independent readback
and BIOS boot. All 733 prior production hashes stay identical. ZIP ordinary
SELECT/confirmed single-helper DD/joined READY and remaining parity groups
stay in scope.


CP29e enables ordinary ZIP picker/worker selection, frozen decoded plans and
confirmed one-helper DD with held-99/joined-READY. Both normal and sanitizer
Qt targets and full 64-MiB controller guests pass independent disk/FAT-file
readback and BIOS boot. Three of 733 production files change; 730 remain
identical. ZIP first-entry creation is available; Zstd/VTSI, VHD/VHDX/FFU,
device-image saving, force-marker UI, remaining selection/drag-drop and all
other Windows parity gaps remain active.


CP30a/b reuses Rufus's bundled Zstd 1.5.7 decoder in the Linux build. `.zst`
now has retained-source validation, actual 64-bit decoded sizing, checksum and
concatenated/skippable-frame handling, and a 128-MiB window limit. Frozen
decoded SHA256 plans, padded native writing/readback, strict helper packets
and simulated one-helper claim/cleanup/fault cases pass. At CP30a/b, the ordinary Qt
picker/analysis gate stayed closed pending real disposable guest block-device
qualification; after that, qualify SELECT, confirmation, held-99/joined-READY
and bootable independent readback. VTSI, containers, saving and all other
Windows parity gaps remain in scope.


CP30c/d qualifies Zstd on a disposable real guest block device, then admits
`.zst`/`.ZST` to the ordinary picker and asynchronous worker. Normal and
sanitizer guests each pass 13 direct Zstd jobs, eight real claim observations,
the complete 64-MiB poisoned restore, independent disk/FAT-file readback and
BIOS boot. Both ordinary SELECT/controller guests pass one confirmed helper,
held 99% until exit and joined 100% READY with the same full readback/boot.
The Qt analysis/Widgets suites pass in both builds. Zstd creation is now
available through the bounded original decoder policy; VTSI, VHD/VHDX/FFU,
image saving, force-marker UI and other Windows parity gaps remain active.

## CP31a — VTSI retained sparse source (2026-10-18)

`.vtsi` now has a bounded read-only source provider. It validates Ventoy footer
and segment checksums, version, table/data bounds and ordered disk extents,
then reconstructs the declared logical image with zero-filled sparse gaps for
analysis. Thirty-seven independent vectors and normal/sanitizer focused source
regressions pass, including faults and output over 4 GiB. The original Windows
writer consumes segment data consecutively and seeks the target for each disk
extent; CP31a uses that data interpretation but does not enable VTSI writing or
ordinary GUI selection. Frozen VTSI write plans, sparse target semantics,
helper, real guest and Qt qualification are next. Containers, image saving and
all other Windows parity gaps remain active.

## CP31b — VTSI frozen sparse writer and helper (2026-10-18)

The native plan now freezes both the logical zero-gap SHA256 and an ordered
segment-map SHA256. The strict helper packet carries both. Native writes seek
only to listed, sector-aligned extents and preserve all target gaps as Windows
Rufus does; sync and readback cover the written data. Twenty-six regular-file
and nineteen simulated helper cases pass, along with normal/sanitizer 11-target
focused suites. Real guest block-device qualification and ordinary Qt
SELECT/confirmation/READY remain required before VTSI creation is offered.
VHD/VHDX/FFU, image saving and the rest of Windows parity remain active.

## CP31c — real guest VTSI sparse write (2026-10-18)

The disposable guest block device now passes frozen-map refusal, two separated
64-KiB VTSI extents with poisoned gaps preserved, real exclusive-claim and
readback observations, and a full 64-MiB bootable production-helper restore.
Both normal and sanitizer guests pass host disk/FAT-file readback and BIOS
boot. The client checks logical VTSI progress separately from physical write
and verify counters; eight normal and eight sanitizer helper-process suites
pass, including sparse short and overcount refusals. Ordinary Qt selection,
confirmation and joined READY remain the next VTSI gate. All other Windows
parity gaps remain active.

## CP31d — ordinary VTSI Qt creation (2026-10-18)

The ordinary picker/worker now select `.vtsi`/`.VTSI` and freeze both logical
and segment-map hashes. Confirmed Qt creation uses the selected target's
geometry, one helper and joined READY. Normal and sanitizer Widgets suites
and QEMU guests pass sparse gap preservation, physical segment verification,
a complete 64-MiB Qt restore, host disk/FAT-file readback and BIOS boot. The
remaining image-format, image-saving and other Windows parity gaps stay active.

## CP31e — fixed and dynamic VHD native source/writer (2026-10-19)

The retained Linux image source now validates legacy VHD footers, dynamic
headers, BAT entries and sector bitmaps. It streams the logical disk from fixed
or dynamic `.vhd`, including zero sectors and unallocated blocks, without
moving the caller's file offset. The native write plan freezes the decoded
hash and uses the existing bounded copy/sync/verify path for regular files.
Independent fixtures, faults, malformed overlaps, QEMU-created fixed/dynamic
VHDs and normal/sanitizer writer readback pass. Differencing VHD parent chains,
VHDX, FFU, device-helper/Qt admission and image saving remain required.

## CP31f/g — VHD device and ordinary Qt creation (2026-10-19)

Fixed and dynamic VHD now pass strict helper validation, simulated device
faults and claims, normal/sanitizer real guest write/readback and full BIOS
boot. The ordinary picker accepts `.vhd`/`.VHD`; normal full Widgets,
sanitizer focused Widgets and both Qt guests pass frozen decoded identity,
confirmation, one helper per write, joined READY and independent disk/FAT-file
readback. Differencing VHD parent chains, VHDX/FFU, device-image saving and
the other work groups remain in scope.

## CP38 — parent images and VHDX replay (2026-10-19)

Preview 7 adds native VHD/VHDX differencing chains and version-0 VHDX log replay
through ordinary SELECT, confirmation, one helper, verified DD and joined READY.
Parents remain open through the helper job, are checked for linkage/geometry
and source changes, and take part in native protected-backing checks. Log replay
uses an immutable overlay before metadata/BAT parsing. Portable relative parent
paths are supported; Windows absolute/volume/UNC relocation, parent replacement
UI and the documented replay/chain bounds remain explicit limitations. See CP38
in the testing record and the support guide. FFU's remaining layouts/codecs,
force-marker UI and all other parity groups remain active.

## CP39 — FFU codecs, descriptor runs and capture names (2026-10-19)

Preview 8 adds LZNT1 and Huffman-XPRESS decoding, bounded grouped compressed
payloads and multi-block end-relative runs to the existing full single-store
FFU route. Native data reconstruction still checks every SHA256 chunk before
output and shares frozen DD plans, target protection, one helper and joined
READY. FFU capture accepts unnamed/Unicode GPT names while retaining exact GPT
bytes; optional manifest names remain ASCII. Normal/sanitizer grouped vectors,
one combined guest per build and one Qt gate qualify the batch; independent
wimlib codecs, LZNT1 decoding and Img2Ffu capture readback provide comparisons.
Target-aware portable resizing, partial-update base validation, multi-store
selection/deployment, SFU and catalog signer trust remain in the FFU backlog.
The support guide records capture/manifest and buffer bounds. Other complete
Windows parity groups remain active.

## CP40 — target-aware full FFU deployment (2026-10-20)

Preview 9 closes ordinary full-image deployment on larger disks: final sparse
extents preserve gaps, end-relative runs use actual capacity, partial grid
overlaps retain overwrite order, and validated GPT metadata moves to the new
end without changing partition entries/filesystem sizes. SELECT and the helper
freeze the authenticated FFU layout as well as decoded content; native writes
and readback retain shared claim/sync/cleanup/joined READY. Qualification and
bounds are recorded in the support/testing records.

Optimized portable resizing, partial base validation, multi-store deployment,
signer trust, remaining capture layouts and other Windows parity stay active.
Audit correction: Windows Rufus passes only `/Apply-Ffu /ApplyDrive /ImageFile`
and does not expose DISM's `/SFUFile` split-set workflow. The speculative SFU
item added at CP39 is not required for Windows Rufus parity.

## CP41 — exact-sector FFU capture and adaptive sizing (2026-10-20)

Preview 10 removes the capture requirement for a whole number of 128-KiB
blocks and the fixed 128-GiB ceiling. The encoder uses full end-relative tail
blocks, hashes each source byte once and chooses larger chunks within native
reader memory budgets. The reader reconstructs exact-capacity overlaps across
both grids and supports chunks up to 16 MiB. One grouped normal/sanitizer corpus,
one combined device capture/Qt restore/boot route per build and one Qt gate
qualify this delivery. The support guide distinguishes arithmetic large-size
planning from actual capture coverage and records the remaining dense budget,
manifest/GPT and compression limits. Other FFU and complete Windows parity work
remain active.

## CP43 — conditional partial FFU deployment (2026-10-20)

Preview 12 supports partial single-store V1/V2 and every existing compressed
codec. It validates every declared base comparison before writing, preserves
unlisted bytes and verifies new mapped data. Partial metadata is applied exactly
as declared, including GPT bytes for a larger existing base. Validated FFUs need
no MBR marker, matching Windows Rufus. One grouped native normal/sanitizer corpus,
one combined guest per build, one Qt gate and two inspected packages form the
batch. Optimized resizing, multiple stores, signer trust and remaining capture
layouts remain unfinished. See the support guide for exact boundaries.

## CP44 — image entry and marker override, preview 13 (2026-10-20)

Close saved Alt+M override, raw signature recognition regardless of suffix,
`.usb`/`.wic`/`.bzip2`, local first-file drops, startup `-i`/`--iso` or positional
paths and desktop file arguments in one batch. Frozen typed requests and native
preflight retain source/geometry/target checks. Focused GUI/protocol checks,
one combined normal/sanitizer route, one Qt gate and two inspected packages
qualify delivery. The support guide records exact limits; the other Windows
parity groups remain active.

## CP46 — compressed FFU saving, preview 15 (2026-10-20)

Full single-store capture now writes LZNT1 payloads with bounded literal fallback
and explicit mappings for zero-filled regions. Exact dirty-disk restores retain
every original byte. Opaque GPT names stay in the payload without entering the
text manifest. Existing read-only acquisition, source consistency, verified
atomic publication, one authorization and joined READY remain in force.
One batched normal/sanitizer corpus, combined guests, one Qt gate and inspected
packages qualify this delivery. Optimized resizing, multiple stores, catalog
signer trust and the other Windows parity groups remain active.

## CP47 — full/quick formatting, preview 16 (2026-10-20)

The Quick format control now reaches the frozen helper job, native formatters
and persistence journals. Full FAT clears/verifies the exact volume; NTFS/exFAT
use native full modes; ext3/ext4 fully initialize the journal. ext2 is equivalent
to quick as in Windows. Large FAT32 stays quick-only. Long full jobs remain
cancelable and retain joined READY. One grouped native/sanitizer corpus, combined
guests, one Qt gate and inspected packages qualify delivery. See the support
guide for the deliberate FAT I/O refusal and qualification bounds. Bad-block /
fake-capacity modes, UDF/ReFS providers and the remaining parity groups stay open.


## CP48 — bad-block and fake-capacity testing, preview 17 (2026-10-20)

The existing device-test controls now run all five shared Windows pattern sets,
including DD creation. Default fake detection stamps first-pass block numbers;
Alt+B is an explicit saved toggle. One authorized helper retains its exclusive
claim through testing, report review, retry, metadata cleanup and creation.
Reports retain bad blocks and first error types; clean logs are removed. Abort,
Retry and Ignore use the same job. Partial FFUs with base comparisons refuse this
incompatible destructive option before target access. Full FFUs remain eligible.

One grouped native/sanitizer corpus, focused protocol/Widgets checks, combined
guests, one Qt gate and inspected packages qualify this batch. Exact coverage
and deliberate final-sector/TLC retry behavior are in the support guide.
Optimized FFU resizing, multi-store deployment, signer trust, UDF/ReFS and the
other Windows parity groups remain unfinished.


## CP49 — extended labels and icons, preview 18 (2026-10-20)

Enable the default-checked extended label/icon control. Keep its preference while
DD/ext selection clears and disables the effective choice. Freeze the original
user label before filesystem normalization. The same authorized job writes the
original Rufus icon and UTF-16LE autorun metadata, or preserves an existing
case-insensitive autorun entry. Native FAT16/FAT32/NTFS/exFAT and whole-device SFD
mounts retain provider, sync, cleanup and joined READY checks. Existing copied
byte counters remain image/DOS counters; a separate result proves appearance
completion. Metadata write/verification failures remain FAILED.

One 30-case normal/sanitizer file corpus, focused protocol/Widgets checks, combined
guests with eight filesystem/layout cases, a default Qt job, real kernel/FUSE NTFS,
image-owned metadata preservation and one representative restore/boot qualify the
batch. One Qt gate and inspected packages complete delivery. Existing autorun-label
discovery remains label work; FFU resizing/multi-store/trust and all other Windows
parity groups remain active.


## CP50 — extended FFU capture layouts, preview 19 (2026-10-20)

FFU saving now supports manifests spanning multiple chunks, empty GPT disks,
and GPT entry tables up to 4 MiB without separate entry-count/width caps.
Capture and larger-target relocation share the table budget. Independent
manifest/store alignment retains every metadata hash; pre-acquisition space
planning covers every accepted manifest without staging the source twice.
A grouped normal/sanitizer corpus checks independent reconstruction and equal/
larger dirty-target restores. Combined guests capture a read-only 8192-entry GPT
source, restore its multi-chunk manifest through ordinary Qt and perform one
representative full restore/readback/BIOS boot. One Qt gate and inspected packages
complete the batch. Optimized resizing, multi-store deployment, signer trust and
the complete Windows parity backlog remain active. See the support/testing records
for bounds and interoperability limits.


## CP51 — old-BIOS compatibility, preview 20 (2026-10-20)

The advanced drive option **Add fixes for old BIOSes** now works for eligible
MBR/BIOS jobs. It starts unchecked and retains the user's session choice while
unsupported modes clear/disable its effective value. The frozen job carries the
choice through one authorization, native formatting, bootloader writes and
joined READY. Shared Rufus placement doubles the track/cluster-aligned prefix.
An inactive final one-track 0xEA partition supplies compatibility unless the
required UEFI:NTFS partition takes priority; persistence precedes either.
The planner, GUI filesystem/cluster choices, boot records, payload checks and
readback use the selected geometry. Oversized GRUB cores refuse before writes.
A grouped native/sanitizer corpus, combined guests, one Qt gate and inspected
packages qualify delivery. Full Windows parity remains active; see the support
and testing records for exact coverage and limits.


## CP52 — ISO → ESP, preview 21 (2026-10-20)

Eligible small regular-EFI images can now use **ISO → ESP** on GPT/FAT16 or
GPT/FAT32. ISO remains the recommended default. The shared policy includes the
separate ESP question for non-hybrid images and persistence, as Windows does.
The helper revalidates source eligibility, writes the main ESP type/name and
retains optional persistence. Partition layout uses the selected allocation
unit; ESP formatting uses the original default-unit rules. FAT32 ESP retains
Windows' quick large-FAT32 behavior. Confirmation shows the selected mode and
actual formatter default. One authorization and READY after sync/cleanup/exit
remain required. One grouped component corpus, combined normal/sanitizer guests,
one Qt gate and inspected packages qualify delivery; full Windows parity remains
active. See the support/testing records for exact limits.


## CP53 — explicit FFU stores, preview 22 (2026-10-20)

V2 FFUs with multiple stores now offer an explicit store chooser. The list shows
one-based store numbers, capacities and source UEFI device paths. START stays
disabled until the chosen store passes complete analysis. The retained source
identity is checked again after choosing; cancellation or a changed source stops
selection. Confirmation identifies the store. The strict optional `store` field
freezes it through native preflight and the single authorized helper. Legacy
requests may omit it for a single-store image; multiple stores never default to
one. The native layout digest binds the chosen store even when two stores have
identical decoded content.

All store headers/descriptors are bounded and parsed in metadata order; payloads
follow all metadata. Metadata hashes authenticate the chooser, and every store's
payload hashes are checked before any logical output or target writes. Only the
selected store contributes writes, end-relative placement, GPT relocation and
partial-update base comparisons. Other stores' ranges are never applied to the
selected device. Existing sector/chunk/map/table bounds, source protection,
claims, sync/readback, cleanup and READY after helper/launcher exit remain.

Windows Rufus delegates FFU deployment to DISM with the selected physical drive.
This Linux route makes the store explicit for its one selected target. Source
UEFI device paths are displayed for identification; they are not resolved into
host devices or used to write additional disks. Automatic multi-target/UEFI-LUN
mapping, optimized partition/filesystem resizing and catalog signer trust remain
unqualified and pending. Full Windows parity remains active.


## CP54 — FFU catalog signature policy, preview 23 (2026-10-20)

FFU input now validates a present catalog as a bounded PKCS#7 Microsoft CTL.
Unsigned catalogs remain accepted. Linux also retains its existing catalog-free
FFU input compatibility: these sources have no signer identity and receive the
same chunk-integrity checks as unsigned CTLs. Windows normally parses a CTL
even for unsigned input; keeping already-supported catalog-free images usable
is an explicit Linux compatibility difference. For a signed
catalog, the first cryptographically valid signer is selected; invalid signatures,
changed signed content and missing signer certificates refuse before logical
output, store listing or target writes. As Windows desktop DISM does, the reader
accepts test/self-signed and expired certificates and rejects its two known
production-chain SHA-1 thumbprints. It does not consult the host trust store,
fetch certificates, apply host clock/revocation policy or claim publisher trust.
A fixed public Microsoft Root Certificate Authority 2010 certificate lets an
included production intermediate reach the same root without host PKI state.

The policy was checked against the static FFU provider shipped in the user's
Windows 11 25H2 ISO. No Windows application was executed. Catalog signatures
cover the CTL contents; payload and metadata chunk hashes are still checked
separately. This preserves the observed desktop policy and does not add a new
requirement to match a CTL member to the FFU hash table. The entire catalog's
SHA-256 is additionally bound into the frozen Linux layout identity, so changing
to another valid catalog cannot retain the selected job's identity. Catalogs
are limited to 16 MiB. FFU saving continues to produce unsigned catalogs.

Qualification groups native normal/sanitizer signature and file-writer cases,
independent osslsigncode signing/verification, a genuine Windows production CAT
refusal, combined normal/sanitizer Qt guests, one Qt gate and inspected native/
generic packages. Each guest applies selected stores from signed FFUs, verifies
readback and source retention, then performs one full restore and BIOS boot.
Existing shared helper faults are reused. Evidence is in
`../.rufus-test-artifacts/cp54/`; disposable disks and package stages are removed.

Optimized partition/filesystem resizing and automatic multi-target/UEFI-LUN
mapping remain pending. Static format findings for the next resizing batch:
`[FullFlash] Optimized=True` identifies a single non-first `UseAllSpace=True`
partition. Windows shifts following begin-relative descriptors in 128-KiB units,
aligns the following partition to 1 MiB, updates GPT geometry and extends the
selected filesystem, with recovery metadata updated afterward. Implementation
must handle the filesystem and recovery consequences together; descriptor
relocation alone does not complete this behavior. Full Windows parity remains
active.

## CP55 — optimized FFU deployment (2026-10-20)

Close optimized target sizing, shifted descriptor/GPT deployment, NTFS growth
and recovery XML repair together. The single authorized job completes all
maintenance before READY. See the [implementation and qualification scope](linux-port-plan.md#cp55--optimized-ffu-deployment-preview-24-2026-10-20).
CP56 supersedes this planned mapping batch: the [FFU audit](linux-ffu-parity.md)
removes the incorrect desktop requirement and closes FFU feature implementation.
Other Windows parity backlog items remain active.


## CP59 — desktop storage, startup and logging (Preview 28)

The [desktop batch](linux-desktop-settings.md) groups adjacent portable INI,
cache/report storage, extra-device/filesystem startup preferences, instance wait,
remembered advanced panels, automatic/persistent logs and the original Ctrl+L/P,
Alt+R/U actions. Existing expert/writing preferences use the selected backend.
A bounded visible tail retains the full session in a private temporary spool.

Device-writing behavior is unchanged; CP58/earlier claim, helper, polkit, sync,
cleanup and READY evidence is reused. One focused normal/sanitizer desktop group
and one full Qt gate qualify the batch. Localization, native updates/download
selection and all other unresolved Windows features remain in the active goal.


## CP61 — runtime UEFI validation and checksum maintenance (Preview 30)

One [runtime validation batch](linux-runtime-validation.md) shares the original
Windows eligibility predicate, exposes the session checkbox and freezes it in
the strict helper request. Streaming source MD5s precede config edits; existing
source lists only rehash explicit Rufus changes. Generated SWMs and Windows
file replacements report their paths, and original EFI loaders receive
no-replace backups before unchanged bundled validators are installed.

The normal/sanitizer integration groups each pass 203 focused real cases. One
combined guest per build covers ordinary FAT16/FAT32/NTFS jobs, independent
list/config/resource/original-loader readback, released claims, one authorization
and joined READY. The final NTFS media boots a bare native EFI application
through UEFI:NTFS and the validator. No Windows program or installation runs.
The existing shared fault/polkit matrix and earlier customization qualification
are reused. Secure Boot/revocation and all other unresolved groups remain in the
active full Windows parity goal.


## CP62 — bundled Secure Boot policy analysis and START warning (Preview 31)

The [Secure Boot batch](linux-secure-boot.md) reuses unchanged fallback text and
DBX resources, applies the original five-step revocation order and expert-only
certificate rule, and replaces unchecked PE/resource/DBX pointer reads with a
bounded native adapter. The ordinary worker checks reported EFI files through
its retained ISO/UDF view; unreadable/unsupported entries remain pending.
START warns before mode selection or authorization and ties the response to the
image selection generation. Windows's distinction between issuer metadata and
publisher/firmware trust remains explicit.

The grouped normal/sanitizer evidence covers 206 real component/analysis/UI
cases each. Combined guests reuse the unchanged FAT16/FAT32/NTFS writer, claims,
joined READY and clean/damaged native EFI checks. One full Qt gate and native/TGZ
package inspection accompany delivery. Remote policy feeds/DBX cache refresh
remain in the download/update group; the complete Windows parity goal stays
active, including other firmware/architecture/legacy/filesystem work.


## CP63 — remote Secure Boot policy and atomic DBX cache (Preview 32)

One [policy update batch](linux-policy-updates.md) closes session SBAT/active/
revoked list refresh, timestamp-selected seven-architecture DBX checks,
one download consent, validated atomic caching, original intervals, manual
checks and cancellation. The shared HTTP transport also preserves detached
signature verification for existing signed downloads. Policy snapshots apply
to the next image selection. The native single-file DBX envelope deliberately
replaces Windows's raw-file/separate-registry timestamp pair to prevent torn
updates; invalid/offline responses retain validated policy and bundled fallback.

One focused normal/sanitizer group, one combined guest per build and one full
Qt gate qualify the batch. Existing writer/helper faults, polkit and agreed
customization evidence are reused. Application binary updates, image download
selection, localization and all other unresolved rows remain in the active
complete Windows parity goal.


## CP64 — standalone MS-DOS and ReactOS (Preview 33)

One [legacy boot batch](linux-legacy-boot.md) groups the fixed Microsoft resource
and consent/cache, shared WinME patches, explicit original MS-DOS locale choices,
private source staging, strict helper modes and ordinary creation with advanced
ReactOS MBR/FAT16/FAT32 records. ReactOS copies no invented OS payload. Existing
FAT partitioning, copy/readback, one authorization and joined READY are reused.
Native early bad-resource refusal protects existing target data; file birth
time follows the native filesystem while source access/mtime/attributes are set.

Focused normal/sanitizer tests, one combined guest per build and one Qt gate
qualify the batch. Existing helper-fault/polkit/customization evidence is reused;
no Microsoft or ReactOS application/installation is executed. Other dedicated
payload versions, architectures, legacy routes and all unresolved rows remain
in the complete Windows parity goal.


## CP65 — device discovery and extended labels (Preview 34)

The [device-discovery batch](linux-device-discovery.md) combines running-app
backing-disk exclusion, mounted extended-label reads and Multiple Volumes
captions. Shared Windows label-token rules and an asynchronous native metadata
reader preserve physical selection identity and immediate read-only/removal
updates. Discovery never mounts media or requests authorization. Positive
partition/slave/loop/Btrfs application backings are excluded from the list.

One normal/sanitizer component/private-D-Bus/UI group, combined real-filesystem
networkless guests, one nine-suite Qt gate and inspected native/TGZ packages
reuse the existing writer/helper/boot/fault/customization qualification. Native
metadata bounds and Unicode/link safeguards are documented. Remaining download,
architecture/legacy, filesystem, localization and distribution groups keep the
full Windows parity goal active.


## CP66 — native image download workflow (Preview 35)

The [native download batch](linux-image-downloads.md) adds the split
SELECT/DOWNLOAD button, shared Fido selector and signed provider/data parser,
Windows/UEFI Shell chooser and native service sequence, browser handoff, bounded
large-file streaming, cancellation and automatic image analysis. It asks for
explicit choices and requests no device authorization. Existing writer/helper/
boot/polkit and agreed customization evidence are reused.

The grouped normal/sanitizer checks include an actual 5-GiB+17-byte transfer and
full-file readback. Live official metadata returns the signed provider and
Windows download choices without an ISO transfer. Combined networkless guests
qualify UID1000 native NTFS saving and the real unsupported-exchange fallback.
Retained backups, no-replace publication and sync/rollback preserve existing or
concurrent destination data. One full nine-suite Qt gate and inspected native/
TGZ packages complete the batch. Native on-demand loading, explicit lists,
bounded data parsing and NTFS publication differences are documented. Other
payload, architecture/legacy, filesystem, localization, application-update and
distribution work stays in the active complete Windows parity goal.


## CP69 — ISO read options and timestamps (Preview 38)

The [ISO controls batch](linux-iso-options.md) groups Alt+I/J/K/S/T: session
ISO/Joliet/Rock Ridge and size checks, saved timestamp preservation, coherent
selected-source analysis/preflight/extraction and the explicit MS-DOS 64 KiB
cluster override. Physical capacity and actual filesystem bounds remain enforced.
Ordinary ISO9660/UDF copies preserve access/modification times when enabled;
creation time uses native filesystem behavior. Directory times are applied after
children so copying cannot immediately overwrite them. See the guide for the
Windows behavior, Linux differences and qualification limits.


## CP70 — NTFS compression and indexing (Preview 39)

The [NTFS options batch](linux-ntfs-options.md) groups session Alt+N compression,
saved Alt+Q indexing, frozen NTFS-only options, native formatter defaults and
boot-file compression exceptions. Larger clusters continue uncompressed with
a diagnostic. Compressed file creation preselects the qualified NTFS-3G provider
before writes; ordinary NTFS creation keeps the kernel provider. No write or
corruption error switches drivers. The guide records native directory-default
restoration, provider evidence and remaining UDF/ReFS indexing-hint work.
