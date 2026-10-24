# GRUB4DOS reuse on Linux

Use Rufus's original boot records and shared selection policy. GRUB4DOS does
not require a new Linux installer to write its MBR/SBR. Installation of the
filesystem loader is a separate requirement; boot records alone are insufficient.

## Original behavior

| Operation | Existing Rufus implementation | Linux reuse |
| --- | --- | --- |
| MBR | `src/format.c:WriteMBR` calls `write_grub4dos_mbr`. `src/ms-sys/br.c` writes the 423-byte `mbr_grub_0x0` array and boot marker. | Include the original `src/ms-sys/inc/mbr_grub.h` in the existing MBR writer. Preserve signature, table and bytes outside its bootstrap. |
| SBR | `WriteSBR` reads `res/grub/grldr.mbr`, skips its first 512 bytes and writes the remainder at byte 512. Reject missing/too-small resources and payloads extending into the first partition. | Reuse the bounded GRUB2 I/O implementation with this resource slicing. Retain target/layout validation, cancellation, flushing and exact readback. |
| Standalone loader | `src/rufus.c` checks `grub4dos-0.4.6a/grldr` in the download cache and offers a signed download if absent. `src/format.c` copies it to root `grldr`. A copy failure is logged without failing that Windows branch. | Reuse the existing signed-download/cache and authorized copy operations when integrating this choice. A missing or failed required loader must prevent successful completion; document that divergence at integration. |
| Image loader | The shared image scanner recognizes root `grldr` as GRUB4DOS. It does not establish its version. ISO extraction supplies the source file. | Preserve source metadata and original precedence. Do not infer a version from a filename or substitute a standalone downloaded loader for an image's file. Qualify the loader contract before admitting an image route. |
| Precedence | Shared `GetMasterBootRecord` and `GetSecondaryBootType` select Syslinux ahead of GRUB, and GRUB2 ahead of GRUB4DOS. Windows Alt+E can keep a Windows MBR while independently selecting a GRUB SBR. | Reuse these policies; the low-level writer does not decide image eligibility. |
| Filesystems | Shared standalone policy permits FAT16, FAT32 and NTFS; ext is specific to GRUB2. | The GRUB4DOS MBR planner accepts those three filesystems and rejects exFAT/ext. Formatting and complete boot qualification remain separate. |

The resource readme identifies the bundled payload as the official 2024-02-26
0.4.6a release. Its full size is 8,192 bytes and its SBR is 7,680 bytes. The
original MBR array equals its first 423 bytes, but the writer deliberately uses
that array rather than copying the resource's whole first sector.

- Full resource SHA256: `a2a8769371eb57c5e79b3cea4a911275af4743f10f420ed6d26e4eec5c865059`.
- SBR SHA256: `3a75885af21afe187723f848f388ecab52cea3b9630b33358b63302dbddcb8a1`.

## CP22n40 qualification

The existing partition adapter now supports the original GRUB4DOS MBR and a
`WriteGrub4dosSbrImage` entry point borrowing the complete selected resource.
Both GRUB providers use one private I/O implementation. The public GRUB2 API
retains its prior behavior. Provider-specific MBRs require the matching provider;
Windows/Rufus/masqueraded MBRs allow the independently selected SBR. Syslinux and
zero-code requests remain rejected. No new installer, library or compatibility
framework; original Windows code and resources remain unchanged.

Normal and ASan/UBSan with leak detection pass 700 GRUB4DOS and 658 GRUB2 SBR
cases, six GRUB4DOS MBR creation/preparation suites and the existing GRUB2 ext
layout suite. Full Qt GUI/helper builds pass. Verify exact bytes outside the
SBR, resource immutability, header skipping, minimum/gap bounds, short/interrupted
I/O, cancellation, failed/secondary sync, corrupt/failed readback, changed media,
wrong MBR and descriptor ownership. Original WinPE's final masquerade rewrite
preserves the installed GRUB4DOS SBR. A private previous adapter rejects three
new positives while GRUB2 controls pass; a private wrong-slice mutation is caught
by the final functional test.

These are regular-file/native-adapter tests, not GRUB4DOS execution, a complete
helper/GUI USB job or physical media qualification. Keep GUI/backend image gates
until resource loading, fresh-source checks, helper sequencing and a coherent
creation route are connected and qualified. Standalone loader download/copy and
representative execution remain required work.

## CP22n41 helper integration

The advanced WinPE helper now selects the bundled SBR from its fresh shared
report, retains the complete resource before target access and uses the same
load/install sequence as GRUB2. The image's unversioned grldr is copied unchanged.
A separate candidate opt-in keeps Qt gated until its complete-flow qualification.
No new protocol field/download/provider library; original Windows policies and
payloads remain unchanged. Syslinux and selected GRUB2 retain original precedence.

Normal/sanitizer source/resource/request/helper tests and one complete FAT32
simulated USB job pass. Verify exact SBR/gap, final masqueraded MBR, Windows FAT
records/hints, nine target files, one simulated authorization and cleanup/helper
exit. The generated Windows loader boots with drive 0x81. This does not execute
GRUB4DOS or qualify its standalone/general ISO routes. Next is Qt admission and
completion qualification; signed standalone grldr download/copy remains required.


## CP22n42 Qt integration

The four existing Qt candidate/admission checks now allow the qualified advanced
WinPE GRUB4DOS SBR. Alt+E/default and selection-refresh behavior use existing
shared policy; no new dialog or download/payload field is needed. Source grldr
is unchanged and unversioned. Unknown GRUB2 remains unknown and original scanning
may select GRUB4DOS without a GRUB2 prompt. Other providers retain precedence.

Normal and ASan/UBSan/leak checks pass 306 focused Widgets cases, including 52 new
rows and eight updated unknown-version rows. A complete FAT32 GUI simulated USB
job verifies original SBR/gap, nine files, final 0x81 boot records, clean filesystem,
confirmation/one simulated authorization and full READY after sync/cleanup/launcher
exit. Unplug preserves READY. The generated Windows loader boots with drive 0x81;
actual GRUB4DOS execution and standalone/general ISO routes remain required.
Only selection_controller.cpp changes in production (four lines); 694/695 hashes
match. No new production file, helper operation, dependency or protocol field.
Next is signed standalone grldr selection/copy and native route qualification.


## CP22n43 standalone loader selection

Reuse the existing cache source reader/atomic storage, owned snapshots and signed
download state machine for fixed grub4dos-0.4.6a/grldr. No ISO dependency, version
inference, prefix retries or embedded loader fallback. Normal/sanitizer functional
checks qualify the real server signature, I/O/identity/atomic errors, cancellation,
readback replacement and object/provider reuse. No/Cancel or a failed required
loader cannot produce empty success. This diverges from Windows's No/failed-copy
branch, which can finish without its required boot loader; Linux stops selection
before target writes and must fail on a later required copy error.

Four existing production files change; no new production file/cache/downloader,
request/helper operation or standalone GUI choice. The original 322,965-byte server
loader identifies release 0.4.6a 2024-02-26; its unchanged signature verifies with
Rufus's original key. Compact offline fixtures record exact hashes and provenance.
These checks qualify acquisition/storage, not actual installation/execution or
an ISO's grldr version. Next is frozen loader staging, authorized copy and complete
native FAT32 qualification; standalone Qt, FAT16/NTFS and actual GRUB4DOS boot follow.


## CP22n44 retained loader and FAT32 creation

Standalone source-free MBR/FAT32 now has an explicit helper operation with required
frozen grldr metadata. Reuse existing process staging, bounded identity/ownership/
hash checks and owned bytes; GRUB2 retains its gap checks, while filesystem grldr
uses the memory limit. Preflight loader and original SBR before target access,
keep their snapshots separate, and reuse shared MBR policy and existing boot writers.
Copy grldr with the existing FreeDOS module's bounded exclusive writer/readback and
sync, then reuse filesystem/device finalization and private-mount cleanup. Required
copy errors prevent READY. Nine existing production files change; no new production
file/library/privilege flow. Source-backed WinPE behavior remains unchanged.

433 normal/sanitizer cases qualify request/source/copy/helper and FreeDOS regressions;
419 source-backed helper and 159 focused normal Widgets regressions pass. A private
zero-copy-success mutation is caught. One complete native simulated USB FAT32 job
passes with real kernel mount/copy, one simulated authorization, held launcher exit,
cleanup/claim/descriptor audit and independent original-record / exact grldr readback.
Standalone Qt exposure, actual GRUB4DOS execution, FAT16/NTFS and other image routes
remain required. See CP22n44 in the plan and compact checkpoint evidence.


## CP22n45 actual standalone BIOS loader execution

The original helper-created MBR/SBR/FAT32/grldr now boots through BIOS USB without
adding a menu/config or alternative payload. Its preset menu naturally reaches
its command prompt. Verify original 0.4.6a 2024-02-26, (hd0,0)/FAT32/0x0C, root
listing, CRC32 of the entire loader through its own FAT reader (2b53370c) and an
executed ECHO response; snapshot boot leaves the backing disk unchanged. A private
missing-grldr control is rejected with the original missing-loader error. Reuse
existing QMP/VGA/firmware/screen tools; all 695 production hashes remain unchanged.
Standalone Qt integration is next. Other filesystems, image loaders and OS chainload
remain separate qualifications; no extra Windows customization test.


## CP22n46 standalone Qt choice

The existing advanced boot combo now offers Grub4DOS 0.4.6a between Grub 2.14
and UEFI:NTFS. Native cluster policy admits only the qualified MBR/FAT32 route;
normal BIOS target/confirmation and retained ISO behavior are preserved. Reuse
the existing acquisition provider, shared progress weighting and private staging
with the complete-job helper; no new production module or privilege mechanism.
Required loader bytes are retained through authorization and process lifetime,
and success checks their exact count. Cache replacement cannot replace those bytes.
No/Cancel and acquisition/preflight failures precede authorization; result receipt
alone cannot show READY. The No text explicitly describes CP22n43/44's justified
Linux stop, instead of promising Windows's successful but loader-less media.

Private-bus Widgets lifecycle and selection tests cover the new UI integration
normally and under ASan/UBSan with leak checking. Original shared/Windows/helper
code and boot payloads remain unchanged. CP22n47 will combine this Qt flow with the
native virtual USB helper and the existing actual BIOS/CRC boot qualifier. Separate
native helper/BIOS evidence from CP22n44/45 remains valid; do not label the protocol
fixture test a physical USB, desktop-polkit or complete native GUI boot qualification.


## CP22n47 complete native Qt creation and BIOS execution

The ordinary Qt advanced choice now has a complete native virtual-USB qualification:
original cached/frozen grldr, cache replacement control, one guest-only authorization,
actual format/boot records/private FAT mount/copy/sync/cleanup and held launcher exit.
The UI waits with original discrete progress and no notification; release produces
full READY once, after descendants/mount/staging/claim cleanup. Stable unplug and
one notification are checked. Independent readback includes the frozen random Qt
serial and explicit label, original records and exact root loader bytes. Direct
SeaBIOS execution reads the entire grldr through FAT32 with its expected CRC and
executes the marker. Missing-loader control fails with the original diagnostic.
No production change, host polkit, physical USB or additional customization test.
Standalone FAT16 is next; NTFS and general GRUB4DOS images remain required.


## CP22n48 standalone FAT16 helper and BIOS execution

The existing native formatter, standard FAT PBR adapter, original MBR/SBR and root
loader copy now qualify FAT16 as well as FAT32. Only request admission changes in
production; the new exact FAT16 operation retains the same required frozen loader
and source-free MBR checks. Native creation uses 2-KiB clusters on a 64-MiB virtual
USB and finishes after sync/private cleanup/held launcher exit. Independent readback
checks geometry, original records and exact root-only grldr. Bare BIOS boots report
FAT16/0x0E, read the whole loader with CRC32 2b53370c and execute ECHO. Missing-loader
copies fail the same qualifier on both filesystems; backing hashes remain unchanged.
Normal and sanitized request/source/helper regressions cover both FAT variants.
Qt still admits only FAT32; FAT16 GUI selection/qualification is CP22n49. Standalone
NTFS and general GRUB4DOS images remain required. No new formatting/copy implementation.


## CP22n49 FAT16 Qt integration and native qualification

The advanced standalone Qt choice now exposes FAT16/FAT32 through the original
shared filesystem/cluster policy. Only one native cluster-admission line changes;
the existing selection, frozen loader, helper and progress code remain unchanged.
Normal and sanitized Widgets scenarios cover both FAT variants, including refusals,
cache replacement, failures, cancellation and joined READY. Native Qt creation,
random serial/label readback, original records, held exit/cleanup, notification/unplug,
bare BIOS/CRC/ECHO and missing-loader controls pass for both filesystems. The common
FAT16 oracle accepts explicitly frozen GUI serial/label; helper recipes retain their
old fixed defaults. This is virtual USB/guest authorization evidence, not host polkit
or physical USB qualification. Standalone NTFS helper reuse/qualification is next;
NTFS GUI and general image GRUB4DOS remain required.


## CP22n50 NTFS backend and native BIOS qualification

Standalone source-free MBR/NTFS uses the existing NTFS formatter/boot adapter,
original MBR/SBR and retained grldr writer. The exact NTFS operation does not infer
UEFI:NTFS, and the client expects loader-copy phases before finalization. Extend
existing native target guards and the writer's private filesystem check; preserve
FreeDOS's FAT-only attribute requirement. No new production module or privilege path.

The new kernel ntfs driver writes the original 322965-byte loader. Independent
readers check NTFS 3.1, clean flags, 4096-byte clusters, label, original primary boot
code/backup BPB, exact MBR/SBR/gap and only grldr as user content. Actual BIOS execution
checks NTFS/0x07, original version, CRC32 2b53370c and ECHO; a library-based removal
of only grldr produces the original BIOS error with metadata still readable.
Native FAT32 and all affected normal/sanitized backend/copy gates pass. Qt remains
FAT16/FAT32 until NTFS fallback and GUI qualification. See CP22n50 in the plan.

The removal fixture is test-only and never mounts host filesystems or runs as root.
Upstream documents that [ntfs_delete closes its child inode even on failure](https://github.com/tuxera/ntfs-3g/blob/edge/libntfs-3g/dir.c);
clear that pointer before normal cleanup and retain the parent for its own close.
Sanitized put/get/remove/missing-file and metadata readback qualify this ownership.
Production copying remains POSIX I/O through the selected native mount provider.


## CP22n51 existing NTFS-3G fallback qualification

No production change is needed. A guest with new kernel NTFS support absent uses
the existing lowntfs-3g provider and single job helper. Normal and sanitized native
creation, independent original-loader/record/clean-volume readback and actual
BIOS root/CRC/ECHO/missing-loader controls pass. A test-only held foreground-provider
exit proves unmount precedes its join and completion waits for the owned process;
staging, descendants and exclusive claim are released before the final result.
The preferred kernel route remains green. Qt NTFS admission is CP22n52; native Qt
qualification follows in CP22n53. See the plan for exact checkpoints and limits.


## CP22n52 NTFS Qt admission

Only the existing standalone cluster gate changes to admit NTFS with FAT16/FAT32.
Reuse original NTFS choices/defaults, existing filesystem preselection/manual
preference, shared label policy, formatter-owned serial, source-free confirmation
and frozen loader, one helper and original progress weighting. NTFS finalization
reaches 62414/65535 before joined completion; FAT remains 60853/65535. Normal and
sanitized format/window/focused Widgets cases pass for all three filesystems,
including retained ISO exclusion, cache replacement, authorization/cancellation,
sync/cleanup failures and full READY/one notification/stable unplug. No Windows
code/resource or backend module changes. The combined native Qt NTFS route remains
CP22n53; prior helper/BIOS qualification does not substitute for that check.


## CP22n53 native Qt NTFS qualification

Reuse the same Qt/native driver and independent NTFS/BIOS oracles with no new
production code. Kernel ntfs and absent-kernel lowntfs-3g create the frozen original
loader on NTFS; readback checks case-sensitive Unicode label, native serial and
original MBR/SBR/PBR. Hold real FUSE/native cleanup and authorization launcher exit
before joined full READY/one notification/stable unplug. Preserve the existing
mount handoff: unmount/join FUSE, reacquire/revalidate/sync whole disk, then finish.
Actual BIOS root/CRC/ECHO and missing-loader controls extend the prior helper
qualification to Qt. General image-loader compatibility and OS chainloads remain
required; standalone loader identity does not establish an arbitrary image version.


## CP22n54 general ISO contract and backend

For an ordinary image Windows uses the bundled GRUB4DOS MBR/SBR and extracts the
image's root grldr unchanged. Root detection and shared Syslinux/GRUB2/GRUB4DOS
precedence remain the source of eligibility; no image version is inferred and no
standalone downloaded payload is substituted. This is the original selection
contract, with representative native boot qualification still needed.

Reuse the existing request source metadata and helper pipeline for plain
BIOS/MBR/FAT32 images. Exactly one loader source is allowed: frozen standalone
payload or retained ISO. Fresh analysis and bounded SBR loading occur before
target access; image extraction and completion use the ordinary ISO path.
Standalone FAT16/FAT32/NTFS remain unchanged. Source/protocol tests cover rejection
and completion; Qt admission remains gated until CP22n55 native helper/readback/
BIOS qualification, then CP22n56 ordinary Qt qualification. Other image
filesystems, EFI combinations and OS chainloading remain required work.


## CP22n55 native ISO qualification

The plain BIOS/FAT32 backend creates an ISO-owned original grldr/menu/payload
through the unchanged production client/helper, without standalone staging or
cache. One simulated authorization, full monotonic ISO progress and native sync/
unmount/joined cleanup pass; readback verifies original MBR/SBR/gap/FAT records
and all four files. SeaBIOS executes original grldr, reads loader/menu CRCs and
confirms the variable set by the ISO's menu. Removing only grldr fails boot and
snapshot backing hashes remain unchanged. Normal/sanitizer routes and the old
standalone FAT32 native/BIOS control pass; production remains unchanged. This
qualifies one representative original loader/menu, while source-loader versions
remain unknown to image analysis. Ordinary Qt admission follows in CP22n56;
other filesystems/EFI combinations, arbitrary compatibility and OS chainloading
remain required.


## CP22n56 ordinary Qt ISO admission

Extend the existing GRUB eligibility predicate and image controller for the
qualified plain BIOS/MBR/FAT32 ISO backend. Preserve shared provider precedence,
ISO/DD choices and target defaults; filter unavailable ISO mode on undersized
targets while retaining valid DD. EFI-bearing images retain the existing GPT/UEFI
copy route, with the new BIOS route gated. Use the ISO-owned loader/menu without
standalone staging or version inference. Source-backed GRUB4DOS uses original ISO
progress and byte-count completion, with "Writing master boot record..." through
MBR/SBR installation and joined full READY/one notification/stable unplug.
Standalone behavior remains unchanged. Component GUI checks extend CP22n55's
native helper/readback/BIOS coverage; combined ordinary Qt/native qualification
follows in CP22n57. Other filesystems/EFI BIOS combinations, arbitrary loader
compatibility and OS chainloading remain required.


## CP22n57 combined native Qt ISO qualification

Reuse the existing Widgets guest with mbr.py --grub4dos-image --grub4dos-image-gui.
Ordinary image loading selects MBR/BIOS/FAT32 using shared policy and keeps the
ISO-owned loader/menu without standalone staging or version inference. Native
copy progress and cleanup-before-joined-READY extend the prior component GUI
checks. Independent FAT/MBR readback and actual BIOS loader/menu execution reuse
CP22n55's oracles, including the missing-loader control and unchanged snapshot
backing files. Keep the existing standalone Qt FAT32 route as a regression control.
No production change or new driver. FAT16 image qualification is next; NTFS/EFI
BIOS combinations, arbitrary compatibility and OS chainloading remain required.


## CP22n58 FAT16 image contract

Admit FAT16 retained-image requests through the existing operation with no
standalone loader metadata, changing only two protocol guards. The native FAT16
formatter and original standard FAT/MBR/SBR writers plus ISO extraction are
reused unchanged. Keep source-backed NTFS, EFI and incompatible provider/options
refusals. Extend the ISO/UDF source and request/process checks, including missing
root loader, original provider precedence and unchanged unknown version. Native
FAT16 image/readback/BIOS qualification follows in CP22n59; Qt remains FAT32-only
until that proof. This is backend admission, not a new native boot qualification.


## CP22n59 native FAT16 image qualification

The existing native ISO guest now qualifies FAT16 with its unchanged image-owned
loader/menu. Original MBR/SBR/gap/FAT16 records, 2048-byte clusters, label/serial
and all four files pass independent readback. Normal and sanitizer native jobs
preserve source, single authorization and cleanup/exit-before-completion. BIOS
executes the original loader/menu and missing-loader controls fail; snapshot
backing files remain unchanged. FAT32 ISO remains a passing regression. No
production change. Ordinary Qt FAT16 admission/native GUI proof follows in
CP22n60. NTFS/EFI image combinations and arbitrary loader/OS compatibility remain
required; a booted representative loader does not infer an image analysis version.


## CP22n60 ordinary Qt FAT16 image route

Extend the existing selector's qualified mask and candidate filter to FAT16,
retaining source-backed NTFS refusal. Shared Windows defaults offer FAT16/FAT32
on small eligible targets, retain manual FAT32 across reload and preserve valid
DD when no native FAT geometry fits. Existing component and native Qt/readback/
BIOS tests cover both FAT choices, image-owned loader/menu, source identity,
ISO progress, held launcher exit and joined full READY/one notification/unplug.
No new module, protocol or helper code. NTFS image contract/backend work follows
in CP22n61; EFI image and arbitrary loader/OS compatibility remain required.


## CP22n61 — image-backed NTFS contract

Reuse prepare-grub4dos-ntfs-device-v1 for a retained source ISO instead of a
standalone frozen loader. Existing native NTFS formatting, original shared
MBR/SBR/standard NTFS records, source checks and extraction already support this
combination; only its request admission guards change. Source/core exclusivity,
formatter-owned serial and plain BIOS policy remain enforced. Normal/sanitizer
source and request checks pass. This does not yet qualify native NTFS image
creation or BIOS boot: prove both kernel ntfs and explicit lowntfs-3g routes in
CP22n62 before enabling ordinary Qt NTFS selection. No image-version inference
or downloaded loader substitution is introduced.


## CP22n62 — native NTFS ISO qualification

The CP22n61 contract now passes native kernel ntfs and explicit lowntfs-3g ISO
creation, independent original-record/four-file readback and actual BIOS loader/
menu execution normally and with ASan/UBSan/leak detection. Missing-loader
controls reject altered media and snapshot backing disks stay unchanged. Reuse
standalone FUSE join audits for image jobs, retaining no standalone payload or
snapshot. Test-only filenames are added for readback; no production code changes.
This is representative original 0.4.6a image-loader proof, not general GRUB4DOS/
OS compatibility. Qt NTFS admission and native GUI qualification follow in
CP22n63. Preserve unknown versions and image-owned loaders.


## CP22n63 — ordinary Qt NTFS ISO route

Share the same FAT16/FAT32/NTFS selector mask as GRUB2 after CP22n62 native proof,
retaining Rufus's defaults, ISO/DD policy and source-owned loader. Both NTFS
providers pass ordinary native Qt creation, independent readback and actual BIOS
menu execution normally and with sanitizers, including provider/launcher joins
before full READY and one notification. Fix the existing completion predicate
so incomplete NTFS reports cannot complete either standalone or ISO GRUB4DOS.
No additional production module or adapter. Representative original-loader proof
does not establish every OS/loader compatibility. Next audit combined BIOS/UEFI
FAT32 source contracts before GUI admission, with NTFS UEFI:NTFS pairing later.


## CP22n64 — combined FAT32 source/backend contract

Reuse the existing x64 EFI fallback validator for image-backed GRUB4DOS FAT32
requests instead of rejecting every EFI-capable image. Shared policy already
supplies the ordinary zero-extra-partition layout; the helper retains original
MBR/SBR/standard FAT records and ISO extraction. Valid original/mixed-case ISO/
UDF fallbacks pass; malformed paths/PE headers, read failure, cancellation and
changed sources fail before target access. One production validator changes,
with no new module, protocol or Windows change. Native BIOS/UEFI qualification
is CP22n65 before combined MBR Qt admission. FAT16 and NTFS paired EFI routes and
other architectures remain qualification work; image versions stay unknown and
source loaders are not replaced.


## CP22n65 — native combined FAT32 BIOS/UEFI qualification

Reuse the existing helper and original records with an ISO-owned x64 EFI
fallback. Normal/sanitizer native jobs pass five-file/record readback, actual
GRUB4DOS BIOS loader/menu execution and direct FAT32 OVMF/application handoff on
the same disk, with no UEFI:NTFS or changed snapshot backing media. Keep source
identity, one simulated authorization and joined cleanup; plain BIOS remains a
regression. No production change. The existing shared EFI test application and
firmware oracle suffice; no new installer, reader or module. Ordinary combined
MBR Qt admission is CP22n66. Other architectures, FAT16 and NTFS paired EFI and
OS compatibility remain qualification work.


## CP22n66 — ordinary combined Qt FAT32

Expose the CP22n65-qualified combined MBR FAT32 path through the existing
selector. Shared target/confirmation rendering and the existing image writer
suffice. Preserve GPT default/manual choices and GPT NTFS availability; plain
BIOS formats remain unchanged. Source-owned loader/fallback, progress, joined
READY/one notification and native BIOS/UEFI/readback pass normally and under
sanitizers. One selector file changes; no new backend/UI/protocol/module. Both
firmware boots leave snapshot media unchanged. Combined FAT16 contract/native
proof is next, with NTFS UEFI:NTFS pairing and other architecture/OS/hardware
qualification still required. Do not infer image-loader versions.


## CP22n67 — combined FAT16 source/native qualification

Broaden the same FAT/EFI source predicate to FAT16 without a new installer or
validator. Reuse original FAT16 and GRUB4DOS records, ordinary extraction and
native lifecycle. Normal/sanitizer source checks and actual FAT16 BIOS/UEFI boots
pass, including exact five-file readback, missing BIOS loader refusal and
unchanged snapshot media. FAT32 is a regression. Only one source validator
changes; Qt still offers FAT32 alone for combined MBR pending CP22n68. Required
NTFS paired EFI, other architectures, OS/Secure Boot and host/hardware
qualification remain. Image-owned loaders stay intact and versions unknown.


## CP22n68 — ordinary combined FAT16/FAT32 Qt

Broaden the existing combined MBR candidate/mask to both qualified FAT choices.
Shared policy supplies FAT16 (Default)/2048-byte clusters on the small target;
manual choices and GPT behavior remain intact. Normal/sanitizer Qt creation,
exact five-file/original-record readback, BIOS menu and direct x64 FAT EFI boot
pass, with joined READY/one notification and unchanged snapshots. One selector
changes; no new production module. Keep source versions unknown. GRUB4DOS-only
emulated key pacing corrects demonstrated lost test input; other callers retain
the shared helper's old defaults. NTFS bundled UEFI:NTFS pairing is next before
its combined GUI admission; other architecture/OS/hardware proofs remain.


## CP22n69 — NTFS UEFI:NTFS request/source pairing

Reuse the GRUB2 paired request pattern for image-owned GRUB4DOS using one new
operation discriminator and no new field/module/provider. Shared partition
policy refuses missing or gratuitous payloads; the existing helper's bundled
payload/native format/original records/extraction path already handles valid
requests. Source/architecture/geometry/metadata/core exclusivity and one-launch
process checks pass normally and with sanitizers. Two validators change, without
Windows/helper/resource/GUI changes. Native kernel/fallback paired proof is next
CP22n70; combined NTFS remains hidden in Qt until qualified. Unknown source
versions remain unknown and standalone loaders are not substituted.


## CP22n70 — paired NTFS native firmware qualification

Reuse the unchanged helper/original GRUB4DOS and NTFS records with the bundled
trailing UEFI:NTFS payload. Kernel/fallback normal/sanitizer jobs pass exact
five-file/two-reader/payload/table readback, actual BIOS menu and payload-to-
source x64 EFI execution, owned-provider/launcher joins and unchanged snapshots.
Direct FAT32 is a regression. No production change. Bound negative BIOS control
to the main extent; nested test readback remains read-only and mutations retain
the parent cache rule. Ordinary combined NTFS Qt admission is CP22n71 before
wider architecture/OS/hardware qualification. Versions stay unknown and source
loaders/fallbacks are never substituted.


## CP22n71 — ordinary Qt paired NTFS

Remove the remaining combined NTFS selector exclusions after native pairing
proof. Ordinary Qt MBR selection now offers FAT16/FAT32/NTFS using shared
geometry and extra-partition rules; EFI/NTFS automatically freezes the existing
paired operation and trailing UEFI:NTFS payload. Preserve GPT defaults, manual
format choices, unknown source versions and image-owned loader/fallback files.
An NTFS-only image remains eligible for MBR. No standalone download is involved.

Existing normal/sanitizer Widgets checks cover selection, 4/8-KiB requests,
failure/cancellation, original copy/finalization progress and READY after helper
exit. Native Qt kernel/fallback jobs and FAT32 regression check every source
file, original records and payload geometry, BIOS menu/missing-loader controls
and x64 EFI execution with unchanged snapshot media. The minimal GUI guest now
publishes its required second partition link. The NTFS mutation utility releases
both inode handles consumed by the
[upstream deletion implementation](https://github.com/tuxera/ntfs-3g/blob/2026.7.7/libntfs-3g/dir.c).
Captured host fsync stalls
remain distinct from guest creation; no test deadline is increased. See CP22n71
in the checkpoint log for retained failures, sanitizer evidence and limits.
