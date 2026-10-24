# Linux delivery finish lines

## Current release work — CP78 / Preview47 (2026-10-24)

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

The user requested GitHub publication at `mcirsta/rufus`, tag `linux-preview.47`.
The AUR recipe must use the checksum of that actual downloaded tag archive.
Arch/CachyOS, Debian, Fedora and generic install trees depend on system libraries;
the generic archive is not self-contained.

Full Windows parity remains incomplete. UDF 2.50/2.60, ReFS formatting and Dev
Drive detection are user-deferred. Existing UDF 1.02–2.01 remains supported; the
actual Windows UDF indexing attribute is unavailable. Non-x64 To Go creation,
complete i686/aarch64 application hosts and unavailable physical USB/unplug/4Kn
checks remain explicitly unqualified. No additional installed-Windows or
customization matrix is scheduled.

### Useful-release review — 2026-10-24

| Area | Release disposition |
| --- | --- |
| UDF 2.50/2.60 (I14/B6) | Explicitly deferred; keep UDF 1.02–2.01 |
| ReFS formatting (I15) | Excluded from first-release requirements by the user |
| Dev Drive recognition (I10) | Subsequently also deferred by the user after provider review |
| Existing-UDF indexing (A03) | CP77 qualifies the saved choice and optional unavailable diagnostic; actual Windows attribute remains unimplemented |
| Native localization (I05) | Reuse original translations; Linux-specific English fallback accepted for this release. Full new translations remain a follow-up |
| Q01–Q04 | Active bounded evidence, host/desktop, package and release/update qualification |

The generic archive is a system-dependent install tree, not a self-contained
portable application. Preview47 packages contain current limitations. Unavailable
physical hardware stays explicitly unqualified under Q02. Preserve the existing
one-Debian/one-RPM plan. Publish the qualified source tag and release assets as requested by the user. One final candidate gate reuses unchanged media evidence;
fix concrete failures without reopening delivered families or extra OS-install tests.

### B1 result

[CP72](linux-desktop-controls.md) delivers cache clearing, saved native worker
priority, USB diagnostics, visible-log selection, explicit light/dark appearance,
percentage/rate/remaining-time progress and advanced native release checks.
Normal and sanitizer focused groups pass 242 substantive cases each. One full
nine-suite Qt gate passes 2,207 substantive cases with five existing optional
skips and zero failures. Prior media, claim/fault and boot evidence is reused.
The final log-filter key correction and log-window shortcut context are covered
by focused normal/sanitizer desktop checks. The unchanged legacy media gate is
reused.

### B2 result

[CP73](linux-device-source-operations.md) delivers I06–I09, I11, A01 and A04.
The focused corpus passes 185 substantive cases in each normal/sanitizer build.
One combined guest route per build covers the distinct native device/source
operations and one representative mapped-parent restore with full readback and
BIOS handoff. One nine-suite Qt gate passes 2,210 substantive cases with five
existing optional skips and zero failures. Preview42 packages contain the matching
GUI/helper, guide and completion register. Physical-device and Windows execution
qualification remain bounded by B8 and the existing agreed testing scope.

### B3 result

[CP74](linux-iso-efi-resources.md) delivers I16 and I20–I25 as one batch.
Grouped normal/sanitizer source/resource corpus: eight suites, 1,536 substantive Qt cases plus 244 existing extraction cases and ten B3 corpus checks, no failures/skips. One combined native route per build qualifies 5.10 FAT32, 6.03 NTFS and old families 3 FAT32/4 NTFS, exact EFI/COM32/checksum/link readback, Windows reparse metadata, one authorization and joined READY, with representative original Syslinux BIOS and embedded EFI/UEFI:NTFS handoffs. Aarch64 v4 provider cross-build/version/accessor execution is qualified under emulation. One full nine-suite Qt run plus a grouped normal/sanitizer repair check qualifies 2,213 substantive cases with 2 existing optional skips and no outstanding failures. The full run initially found two failures; a retained-report confirmation fix and the updated version-eligibility test pass 15 focused cases in each build. Existing unchanged helper/writer fault evidence reused.
Preview43 includes matching GUI/helper/native providers, the guide and register.
Physical aarch64 media writes, physical USB and original Windows execution remain
unqualified. No per-release/distribution or installed-Windows matrix was added.

### B4 result

[CP75](linux-legacy-iso-routes.md) delivers I17–I19 as one batch.
Six focused component suites pass in normal and sanitizer builds: 1,897 substantive Qt cases, seven existing optional Microsoft-resource skips, plus 244 ISO/UDF extraction cases. All fourteen new legacy resource/protocol/source cases pass. One combined native route per build creates and reads back five jobs: legacy PE FAT16, unknown-version Windows FAT16 dual, ReactOS FAT32, legacy PE NTFS with EFI/UEFI:NTFS, and Kolibri FAT32. Exact MBR/PBR/source/prepared bytes, actual hidden/system FAT attributes, final NTFS dual layout/clean flags, one authorization and joined READY pass. Three representative firmware handoffs pass: native FAT16 EFI, original Syslinux/mboot ReactOS and Kolibri PBR. One full nine-suite Qt run plus focused normal/sanitizer repairs qualifies 2,214 substantive cases with 2 existing optional skips and no outstanding failures. The full run found twelve failures in one obsolete root-loader-refusal group (including one fixture startup failure); all twelve rows pass after its acceptance expectations are updated. Product sources remain unchanged after the full gate. The earlier focused UI group had two failures; one fixture expectation and the Kolibri phase contract were repaired, and all six B4 repair rows pass in normal and sanitizer builds. Existing unchanged helper faults and installed-Windows/customization evidence are reused.
Preview44 packages the matching GUI/helper, guide and register. Original Windows
CheckDisk is documented as a native difference; full Windows API and installed
WinPE/AIK execution remain unqualified. No extra OS-installation matrix was added.

### B5 result

[CP76](linux-sector-geometry.md) delivers I12, I13 and A02 together.
Grouped normal/sanitizer components qualify 13 unique suites and 778 substantive cases, no skips or outstanding failures. Initial checks found three obsolete/fault-interaction failures; focused repairs preserve the initial receipts and pass. One combined native route per build creates and independently reads back 21 ordinary Qt/helper jobs: seven formats at 512/512, 512/4096 and 4096/4096 across MBR/GPT/SFD, six stale-geometry refusals before authorization, one FAT32 full format, exact payloads where originally eligible and clean ext roots/superblocks, one authorization per job and joined READY. A representative native FAT32 EFI handoff passes. Sparse component fixtures cover capacity/cluster/provider boundaries including the actual maximum native 4-KiB UDF volume. The single full nine-suite Qt run plus a focused normal/sanitizer native-ioctl fixture and cluster-choice repairs qualifies 2,215 substantive cases with 2 existing optional skips and zero outstanding failures. The full run exposed an obsolete ioctl fixture that omitted BLKPBSZGET and one cluster-choice assertion expecting four options before 64 KiB was restored; all 53 cases in the affected device suite and the single cluster row pass after repair. Prior unchanged writer faults and installed-Windows/customization evidence are reused.
Preview45 contains matching GUI/helper/providers, the guide and register. Native
BIOS/bundled-payload limits and Windows/native UDF provider bounds are explicit.
No additional installed-Windows matrix was added.

### Next work and testing

- Finish B8 release assets and public-source/AUR preparation. Retain explicit
  non-x64, additional-host and unavailable-hardware qualification gaps.
- CP77 resolves supported-UDF release behavior with an explicit provider
  limitation. Its actual indexing-metadata parity remains open; do not claim it.
- Do not resume B6, ReFS formatting or Dev Drive detection implicitly. User
  deferrals remain visible in the register and support guide.
- Use focused checks during changes, one full Qt gate per implementation batch,
  one final candidate gate and existing shared writer/helper/boot evidence.
  Preserve default Ninja parallelism, one authorization and joined READY.
- Original Windows execution and extra customization/installation tests remain
  deferred; CP11n already fulfills the agreed representative creation/readback scope.

### Full completion

This describes eventual full parity, not a requirement to implement deferred B6
before preparing a useful release. Deferred is not delivered.

Every I/A row must close through qualified implementation or a demonstrated
native difference under AGENTS.md. I05 includes the native-only UI/diagnostic
translations still missing after catalog reuse. Q01 reconciles eligible choices
to retained evidence without multiplying invalid Cartesian combinations. Q02
uses bounded physical/host/desktop cases; unavailable hardware remains visibly
unqualified. Q03 limits extra packaging to one Debian-family and one RPM-family
environment alongside Arch/generic delivery. Q04 prepares actual release/update
evidence and requires authorization for publication.

Keep all remaining source-specific acceptance conditions in the
[register](linux-parity-register.json). A new issue needs a concrete offered
Windows path and native counter-evidence assigned to its owning batch. Source
families already delivered, dormant enums, hypothetical devices and optional
per-version matrices cannot generate duplicate checkpoint projects.

## Historical delivery record — through CP71

Everything below is retained checkpoint history. Earlier unchecked work groups
and “remaining FFU/To Go/parent chains” statements describe their date of writing;
the register above is the current completion plan.

## Preview 1: installable application

- [x] Versioned Release GUI and matching helper/polkit policy.
- [x] Selected native Syslinux installers, desktop entry and icons.
- [x] Working About, original license and credits, headless help/version.
- [x] Native Arch/CachyOS package, generic install archive and support guide.
- [x] Focused desktop checks and inspection of staged package paths/dependencies.

This milestone delivers the existing qualified media workflows. It does not
claim complete Windows feature parity or universal hardware compatibility.

## Feature completion batches

- [x] FFU feature implementation: capture, full/partial/optimized deployment and
  desktop catalog policy; [CP56 scope audit and qualification limits](linux-ffu-parity.md).
- [x] Supplementary ZIP archives: Ctrl+SELECT, source protection, final file
  overlay and one-helper joined READY; [CP57](linux-supplementary-zip.md).
- [x] Windows To Go and standalone WIM/ESD creation: native metadata, boot files,
  installed policy, ordinary chooser/helper and joined READY; [CP58](linux-windows-to-go.md).
- [x] Runtime UEFI media validation and checksum maintenance: shared Windows
  eligibility, session checkbox, original-loader preservation and modified-file/
  split-WIM checksums; [CP61](linux-runtime-validation.md).
- [ ] Remaining image operations: Windows-specific parent relocation and
  remaining source workflows.
- [ ] Installer workflows: remaining Windows/Linux architectures and eligible
  legacy boot routes.
- [ ] Device and filesystem operations: remaining format/layout combinations,
  advanced device handling and remaining firmware qualification. CP63 closes remote Secure Boot policy refresh. CP45/47/48
  already qualify zeroing, full native formatting and bad-block/fake-capacity jobs.
  CP60 adds UDF 1.02–2.01 and non-bootable ext2/ext3 on MBR/GPT/SFD;
  [later UDF revisions and ReFS remain open](linux-filesystem-formatting.md).
- [x] Desktop storage/startup/logging: portable INI, saved panels, startup
  preferences/instance wait and automatic/persistent logs; [CP59](linux-desktop-settings.md).
- [ ] Application completion: remaining downloads/settings, localization,
  update behavior, labels/icons and advanced controls/shortcuts.

CP33 completes ordinary clean fixed/dynamic VHDX selection and creation in one
batch. FFU, parent chains and VHDX log replay remain in the image-operations
batch together with saving and the remaining selection controls.

CP34 adds full single-store FFU selection and creation, with uncompressed and
plain XPRESS payloads and the captured disk layout. Remaining FFU work includes
LZNT1/Huffman-XPRESS, portable resizing, multi-store/partial deployment and
catalog trust. These gaps remain in scope.

CP35 / Preview 4 adds device saving as fixed VHD, dynamic VHDX and full
uncompressed FFU in one delivery batch. ISO export remains in the image
operations batch: Windows exports a mounted filesystem tree as UDF, which is
different from capturing the device bytes. Automatic source unmount, read-only
device discovery and further FFU capture layouts remain device/image work.

Each batch can contain several related implementations. Qualify shared code
once and add format-specific checks for distinct behavior. A new feature that
only changes parsing does not require repeating the complete claim/fault/boot
matrix already qualified for its writer. Record one completed batch and its
supported boundary; retain prior shared evidence.

## Full parity and release signoff

- [ ] Audit the original Windows controls and dispatch against the implemented
  Linux routes; resolve functional gaps in every backlog group.
- [ ] Preserve the explicitly agreed Linux differences, including the disabled
  Windows regional-settings copy option.
- [ ] Run a consolidated regression and representative native media/desktop
  qualification for the final build, reusing completed evidence where the
  implementation is unchanged.
- [ ] Publish the supported platforms, remaining qualification limits and
  installable packages with versioned release notes.

Validation is bounded by the agreed representative cases. It is not an
open-ended requirement to test every USB model, firmware, OS image or Windows
customization combination. Original Windows application execution remains
deferred under the existing user instruction.

CP36 / Preview 5 adds UDF-only ISO filesystem export and automatic source
unmount in the ordinary save flow. The batch shares the existing transactional
destination and uses one authorized source helper for unmount and private
read-only source access, while encoding and destination writes stay with the
desktop user. FAT and NTFS-3G source routes share one combined qualification
and one representative saved VHDX restore/boot. Parent/log handling, remaining
FFU/capture variants, read-only discovery and the other finish-line batches
remain in scope.

CP37 / Preview 6 closes read-only device discovery and saving. Eligible media
remain selectable for all four capture formats, with explicit read access in
source checks and default write refusal in creation checks. The batch reuses
unchanged encoders, transactional output, source cleanup and prior fault
qualification, adds one combined normal/sanitizer route and one Qt gate, and
retains one representative saved VHDX restore/boot. Remaining capture layouts,
container operations and the other feature-completion batches remain in scope.

CP38 / Preview 7 closes portable VHD/VHDX differencing chains and immutable
VHDX log replay as one delivery batch. Ordinary confirmed DD uses the existing
writer and one helper, retains all parent sources, and protects their backing
storage. One combined normal/sanitizer guest route covers both formats and one
full parent-plus-log restore/boot. The native corpus, one Qt gate and prior
shared writer fault evidence form the batch gate. Windows-specific parent
relocation paths, remaining FFU/capture variants and the other completion work
stay in scope.

CP39 / Preview 8 adds LZNT1/Huffman-XPRESS, grouped compressed FFU buffers,
multi-block end-relative runs and unnamed/Unicode GPT captures. They share the
existing verified writer and acquisition/transaction paths. One grouped native
normal/sanitizer corpus, one combined guest per build, one representative
full restore and one Qt gate close this batch. Remaining FFU target-dependent
deployment features are tracked explicitly; full parity is not yet signed off.

CP40 / Preview 9 closes ordinary full FFU deployment on equal/larger targets,
including sparse preservation, end-relative placement and GPT relocation. It
preserves partition/filesystem sizes. Optimized portable resizing,
partial/multi-store deployment, signer trust and remaining capture layouts stay
in the image-operations batch. The separate DISM SFUFile workflow is not exposed
by Windows Rufus and is not an additional parity requirement.

CP41 / Preview 10 adds exact-sector FFU capture/restore and adaptive chunk
sizing, replacing the fixed 128-GiB cap with explicit native resource budgets.
The full parity objective still includes optimized FFU resizing, partial/multi-
store deployment, signer trust, remaining capture/selection operations and the
other feature completion batches.

CP42 / Preview 11 closes the persistence controls, frozen request, MBR/GPT
layout, ext2/3/4 formatting and Debian/Casper boot configuration in one batch.
Shared helper tests are reused; the combined normal/sanitizer guest gives each
ext version a native write/readback and one representative USB two Linux boots
with a surviving file. Distribution-specific conventions and the rest of the
Windows parity backlog remain active.

CP43 / Preview 12 closes conditional partial FFU deployment across the supported
single-store versions/codecs. All declared base comparisons pass before writes;
unlisted bytes survive and readback verifies the updated extents. One grouped
normal/sanitizer corpus, combined guest per build, one Qt gate and two inspected
packages complete the delivery batch. Optimized FFU resizing, multiple stores,
catalog trust and the other feature-completion groups remain active.

## CP44 — image entry and marker override, preview 13 (2026-10-20)

Close saved Alt+M override, raw signature recognition regardless of suffix,
`.usb`/`.wic`/`.bzip2`, local first-file drops, startup `-i`/`--iso` or positional
paths and desktop file arguments in one batch. Frozen typed requests and native
preflight retain source/geometry/target checks. Focused GUI/protocol checks,
one combined normal/sanitizer route, one Qt gate and two inspected packages
qualify delivery. The support guide records exact limits; the other Windows
parity groups remain active.

## CP45 — device zeroing, preview 14 (2026-10-20)

Alt+Z and Ctrl+Alt+Z now perform source-free full and fast clearing through one
confirmed/authorized helper. Exact Windows fast-zero semantics, full readback,
zero-write blank jobs and joined READY share one native normal/sanitizer corpus,
one combined guest per build and one Qt gate. The support guide records the
geometry and memory boundaries. Full formatting and bad-block/fake-capacity
tests remain separate parity work, together with the other completion groups.

## CP46 — compressed FFU capture, preview 15 (2026-10-20)

Preview 15 replaces dense uncompressed FFU capture with bounded LZNT1 payloads,
literal fallback and explicit zero-region restoration. Preserve opaque GPT names
without unsafe manifest text. Deliver one native/sanitizer corpus, one combined
guest per build, one Qt gate and inspected Arch/CachyOS/TGZ artifacts. Reuse shared
capture authorization/publication and writer fault matrices. Full formatting
qualification and the remaining FFU/full Windows parity groups stay active.

## CP47 — full/quick formatting, preview 16 (2026-10-20)

Enable the Quick format choice for supported native filesystems and persistence,
with Windows large-FAT32/DD rules, bounded FAT clearing/readback, provider-correct
NTFS/exFAT full modes, ext journal initialization and cancelable long formatting.
Deliver one normal/sanitizer corpus, combined guests, one Qt gate and inspected
Arch/CachyOS/TGZ packages. Bad-block/fake-capacity checks, native UDF/ReFS, remaining
FFU work and complete Windows parity stay active.


## CP48 — bad-block testing, preview 17 (2026-10-20)

Five Windows pattern modes and default fake-capacity detection now run before
creation, including DD. Alt+B saves the explicit detector preference. Reports
and Abort/Retry/Ignore stay within one authorized job and joined READY.
Partial FFUs needing existing base bytes refuse destructive testing before target
access. Qualification groups native/sanitizer cases, combined guests, one Qt gate
and inspected packages. See the support guide for exact coverage and remaining
Windows parity work.


## CP49 — extended labels and icons, preview 18 (2026-10-20)

The default-checked appearance option now writes the original Rufus icon and
Windows UTF-16LE autorun label metadata, preserving image-owned autorun files.
The job freezes original Unicode text independently of filesystem normalization.
FAT16/FAT32/NTFS/exFAT and SFD use checked private mounts, one authorization and
joined READY. DD/ext disable the effective option and retain the user's choice.
One normal/sanitizer corpus, focused GUI checks, combined guests, one Qt gate
and inspected packages qualify delivery. See the support guide for exact limits;
CP65 later adds existing mounted extended-label discovery; other parity groups remain active.


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

## CP56 — FFU feature completion, preview 25

The [final FFU audit](linux-ffu-parity.md) closes capture/deployment feature work.
Capture now emits desktop-compatible plain XPRESS. Automatic multi-drive mapping
was an erroneous inferred requirement and is removed from the current backlog.
Further FFU changes require a concrete defect or named qualification need;
remaining image/application/installer/device groups continue toward full parity.

## CP58 — Windows To Go and WIM/ESD, preview 27 (2026-10-21)

Preview 27 closes Windows To Go and standalone WIM/ESD as one batch: ordinary
archive/edition selection, native NTFS metadata including EA/EFS, GPT ESP/MSR or
MBR BIOS/UEFI, BCD/boot files, installed SAN/S-mode and one-helper joined READY.
Normal/sanitizer components cover 284 cases per build. Combined guests cover all
four source forms and one native EFI handoff per build; one full Qt gate passes.
Windows execution/installation and decryption remain deferred. See the
[To Go guide](linux-windows-to-go.md) for precise coverage and evidence. Full
Windows parity remains active outside this completed feature group.


CP59 / Preview 28 groups desktop storage, startup controls and logging as one
batch. No writer or helper protocol changes are needed; their completed device
qualification is reused. Localization, updates/download selection and the other
feature-completion groups remain open.

CP60 / Preview 29 groups native UDF 1.02/1.50/2.00/2.01 data formatting and
non-bootable ext2/ext3 MBR/GPT/SFD layouts. It shares existing partitioning,
claim, zeroing, mount, ZIP/appearance and helper completion code. One grouped
component corpus, one combined normal/sanitizer guest route and one full Qt
gate qualify the batch. Later UDF metadata-partition revisions, larger UDF
geometry, ReFS and the other completion groups remain open; see the
[formatting guide](linux-filesystem-formatting.md).


## CP61 — runtime UEFI validation (Preview 30)

One runtime batch combines the checkbox, source checksum streaming and repair,
original EFI preservation, bundled validators and actual Windows/SWM change
reporting. One normal/sanitizer device route and one full Qt gate qualify it;
packaging includes the unchanged validator assets' license and attribution.
The [guide](linux-runtime-validation.md) records supported behavior and the
separate remaining Secure Boot/trust work. Full Windows parity remains active.


## CP62 — bundled Secure Boot policy checks (Preview 31)

The [Secure Boot batch](linux-secure-boot.md) delivers native issuer metadata,
original DBX/SSP/SBAT/SVN/expert-certificate classification and the START warning
with the same bundled policy as Windows. One focused normal/sanitizer group,
one combined guest per build and one full Qt gate qualify it. The existing
OpenSSL/digest dependencies and helper/write contracts are reused.
Remote policy-list and timestamp-selected DBX cache updates remain part of
the download/update backlog; this release does not close the full parity goal.


## CP63 — remote policy refresh and DBX caching (Preview 32)

The [policy update batch](linux-policy-updates.md) closes the remote-policy
portion of the download/update group: shared bounded HTTP, original session
lists and interval behavior, DBX timestamp selection, one download consent,
atomic validated cache, frozen image-analysis snapshots and Settings controls.
The original helper/write contract is unchanged. One grouped component/UI
qualification, combined normal/sanitizer guests and one Qt gate supply evidence.
Application binary updates, image download selection and the other remaining
feature groups keep the complete Windows parity goal active.


## CP64 — standalone MS-DOS and ReactOS (Preview 33)

One [legacy boot batch](linux-legacy-boot.md) combines Microsoft resource consent/
cache and shared original patches, explicit MS-DOS locale/startup, private source
staging, strict helper modes and advanced ReactOS MBR/FAT16/FAT32 records. It
reuses the FAT writer and complete authorization/sync/cleanup/READY contract.
One grouped normal/sanitizer qualification, combined guests and one Qt gate
reuse unchanged shared faults and the agreed customization evidence. Microsoft/
ReactOS application execution and installation are not part of qualification.
Remaining download, architecture/legacy, filesystem, localization and delivery
rows keep the complete Windows feature parity goal active.


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


## CP67 — application language selection and catalog reuse (Preview 36)

The [language batch](linux-localization.md) reuses the unchanged original
38-language catalog and shares quoted-argument parsing with Windows. The native
selector, saved `Locale`, name/numeric CLI choice, base fallback, checked menu,
explicit uppercase and RTL layout work together. Live switching retains the
controller, device/image/filesystem, user labels and complete session log.
Unmatched native-only text remains English and translation coverage stays open.

One grouped normal/sanitizer parser/catalog/UI qualification, one combined
networkless diskless guest route as UID1000, one full nine-suite Qt gate and
inspected native/TGZ packages use default Ninja parallelism. Existing writer,
helper, boot, fault/polkit and agreed customization evidence is reused; no host
media is mounted or written. Strict external-catalog transaction/bounds and
Qt relabeling differences are documented. The complete Windows parity goal
remains active across the other unresolved groups.


## CP68 — native application release checks (Preview 37)

The [application-update batch](linux-application-updates.md) groups native release
catalog parsing, numeric/channel/architecture eligibility, scheduled and manual
checks, beta/forced discovery, cancellation, deferred notifications and explicit
release-page/package-manager handoff. It preserves existing Secure Boot consent
and timestamps, keeps notes as plain text and reports an empty native catalog
separately from an up-to-date release. Publishing and distribution installation
remain delivery work; Windows executables are excluded from native offers.
The stale-translation diagnostic and redundant-language-event dialog lifetime
repair are included in the same application-controls batch.

One grouped normal/sanitizer qualification, combined diskless loopback-only
guests as UID1000, live public metadata, one full nine-suite Qt gate and inspected
native/TGZ packages reuse existing writer/helper/boot/fault/polkit/customization
evidence. No host media or installation is touched. Complete Windows parity
remains active across the remaining feature and distribution groups.


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
